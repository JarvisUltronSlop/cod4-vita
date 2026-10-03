#include <vita/vita_model_bundle.h>
#include <vita/vita_model_format.h>
#include <vita/vita_chunk_window.h>
#include <vita/vita_cpu_profile.h>
#include <new>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace kisak::vita {
namespace {

constexpr uint32_t kMagic = 0x58544956; // "VITX"
constexpr uint32_t kVersionV1 = 1;
constexpr uint32_t kVersionV2 = 2;
constexpr uint32_t kVersionV3 = 3;
constexpr uint32_t kVersionV4 = 4;
constexpr uint32_t kVersionV5 = 5;
constexpr uint32_t kRenderLodMagic = 0x444F4C56; // "VLOD"
constexpr uint32_t kRenderLodVersion = 1;
// Map-placed static-model closures can contain over two hundred unique
// XModels. The old weapon-only limit silently rejected those VITX chunks and
// made foliage, clutter, and container dressing disappear from non-Shipment
// maps even though the geometry was exported correctly.
// The source closure reserves map dressing, FX models, and viewmodels in one
// bounded VITX stream. Keep the reader envelope in sync with the exporter so
// valid post-FX closures are not rejected at runtime.
constexpr uint32_t kMaxModels = kVitaModelMaxRecords;
constexpr uint32_t kMaxSurfaces = 64;
constexpr uint32_t kMaxVertices = 65535;
constexpr uint32_t kMaxIndices = 300000;
constexpr uint32_t kMaxBones = 128;
constexpr uint64_t kMaxChunkBytes = kVitaModelMaxPayloadBytes + 20;

#pragma pack(push, 1)
struct VitaModelBoneLegacy {
    float quat[4];
    float trans[3];
    float transWeight;
};
#pragma pack(pop)

static_assert(sizeof(VitaModelBoneLegacy) == 32);

uint32_t ReadU32(const VitaChunkWindow& bytes, size_t offset) {
    uint32_t value = 0;
    bytes.Read(offset,&value,sizeof(value));
    return value;
}

float ReadFloat(const VitaChunkWindow& bytes, size_t offset) {
    float value = 0.0f;
    bytes.Read(offset,&value,sizeof(value));
    return value;
}

bool ReadString(const VitaChunkWindow& bytes, size_t* offset,
                uint32_t length, std::string* output, uint32_t maximum) {
    if (!offset || !output || length == 0 || length > maximum
        || *offset > bytes.size() || length > bytes.size() - *offset)
        return false;
    output->resize(length);
    if(!bytes.Read(*offset,output->data(),length))return false;
    *offset += length;
    return std::find(output->begin(), output->end(), '\0') == output->end();
}

struct ParsedRenderLodModel {
    std::string name;
    uint32_t lodCount = 1;
    uint32_t lodRampType = 0;
    float distances[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<VitaModelRenderLod> lods;
};

bool ParseRenderLodChunk(const VitaAssetDatabase& database,
                         const KvapChunk* chunk,
                         std::vector<VitaModelRecord>* models) {
    if (!chunk || !models || chunk->rawSize < 20
        || chunk->rawSize > kMaxChunkBytes)
        return false;
    VitaChunkWindow bytes(database,chunk);
    const uint32_t magic = ReadU32(bytes, 0);
    const uint32_t version = ReadU32(bytes, 4);
    const uint32_t modelCount = ReadU32(bytes, 8);
    const uint64_t dataBytes = static_cast<uint64_t>(ReadU32(bytes, 12))
        | (static_cast<uint64_t>(ReadU32(bytes, 16)) << 32);
    if (magic != kRenderLodMagic || version != kRenderLodVersion
        || modelCount == 0 || modelCount > kMaxModels
        || dataBytes != bytes.size() - 20u)
        return false;

    size_t offset = 20u;
    std::vector<ParsedRenderLodModel> parsed;
    parsed.reserve(modelCount);
    for (uint32_t modelIndex = 0; modelIndex < modelCount; ++modelIndex) {
        // nameBytes, source lod count, stored lower-lod count, ramp type,
        // followed by all four authored XModel distance thresholds.
        if (offset + 32u > bytes.size())
            return false;
        const uint32_t nameBytes = ReadU32(bytes, offset + 0u);
        const uint32_t lodCount = ReadU32(bytes, offset + 4u);
        const uint32_t storedLodCount = ReadU32(bytes, offset + 8u);
        const uint32_t lodRampType = ReadU32(bytes, offset + 12u);
        if (nameBytes == 0u || nameBytes > 256u
            || lodCount < 2u || lodCount > 4u
            || storedLodCount == 0u || storedLodCount >= lodCount
            || lodRampType > 1u)
            return false;
        ParsedRenderLodModel model{};
        model.lodCount = lodCount;
        model.lodRampType = lodRampType;
        float previousDistance = 0.0f;
        bool terminalDistance = false;
        for (uint32_t lod = 0; lod < 4u; ++lod) {
            model.distances[lod] = ReadFloat(bytes, offset + 16u + lod * 4u);
            if (!std::isfinite(model.distances[lod])
                || model.distances[lod] < 0.0f)
                return false;
            if (lod >= lodCount) {
                if (model.distances[lod] != 0.0f)
                    return false;
                continue;
            }
            if (terminalDistance
                || (model.distances[lod] != 0.0f
                    && model.distances[lod] <= previousDistance))
                return false;
            if (model.distances[lod] == 0.0f)
                terminalDistance = true;
            else
                previousDistance = model.distances[lod];
        }
        offset += 32u;
        if (!ReadString(bytes, &offset, nameBytes, &model.name, 256u))
            return false;
        if (std::any_of(parsed.begin(), parsed.end(), [&](const auto& prior) {
                return prior.name == model.name;
            }))
            return false;

        model.lods.reserve(storedLodCount);
        uint32_t previousSourceLod = 0u;
        for (uint32_t storedLod = 0; storedLod < storedLodCount;
             ++storedLod) {
            if (offset + 16u > bytes.size())
                return false;
            VitaModelRenderLod lod{};
            lod.sourceLod = ReadU32(bytes, offset + 0u);
            const uint32_t surfaceCount = ReadU32(bytes, offset + 4u);
            const uint32_t vertexCount = ReadU32(bytes, offset + 8u);
            const uint32_t indexCount = ReadU32(bytes, offset + 12u);
            offset += 16u;
            if (lod.sourceLod == 0u || lod.sourceLod >= lodCount
                || lod.sourceLod <= previousSourceLod
                || surfaceCount == 0u || surfaceCount > kMaxSurfaces
                || vertexCount == 0u || vertexCount > kMaxVertices
                || indexCount < 3u || indexCount > kMaxIndices
                || indexCount % 3u != 0u)
                return false;
            previousSourceLod = lod.sourceLod;
            lod.distance = model.distances[lod.sourceLod];
            lod.surfaces.reserve(surfaceCount);
            for (uint32_t surfaceIndex = 0; surfaceIndex < surfaceCount;
                 ++surfaceIndex) {
                if (offset + 24u > bytes.size())
                    return false;
                const uint32_t materialBytes = ReadU32(bytes, offset + 0u);
                VitaModelSurface surface{};
                surface.vertexStart = ReadU32(bytes, offset + 4u);
                surface.vertexCount = ReadU32(bytes, offset + 8u);
                surface.indexStart = ReadU32(bytes, offset + 12u);
                surface.indexCount = ReadU32(bytes, offset + 16u);
                const uint32_t reserved = ReadU32(bytes, offset + 20u);
                offset += 24u;
                if (materialBytes > 256u || reserved != 0u
                    || surface.vertexCount == 0u
                    || surface.indexCount == 0u
                    || surface.indexCount % 3u != 0u
                    || static_cast<uint64_t>(surface.vertexStart)
                            + surface.vertexCount > vertexCount
                    || static_cast<uint64_t>(surface.indexStart)
                            + surface.indexCount > indexCount)
                    return false;
                if (materialBytes != 0u
                    && !ReadString(bytes, &offset, materialBytes,
                                   &surface.materialName, 256u))
                    return false;
                lod.surfaces.push_back(std::move(surface));
            }
            const size_t vertexBytes = static_cast<size_t>(vertexCount)
                * sizeof(VitaModelVertex);
            const size_t indexBytes = static_cast<size_t>(indexCount)
                * sizeof(uint16_t);
            if (offset > bytes.size()
                || vertexBytes > bytes.size() - offset
                || indexBytes > bytes.size() - offset - vertexBytes)
                return false;
            lod.vertices.resize(vertexCount);
            if(!bytes.Read(offset,lod.vertices.data(),vertexBytes))return false;
            offset += vertexBytes;
            lod.indices.resize(indexCount);
            if(!bytes.Read(offset,lod.indices.data(),indexBytes))return false;
            offset += indexBytes;
            if (std::any_of(lod.indices.begin(), lod.indices.end(),
                            [vertexCount](uint16_t index) {
                                return index >= vertexCount;
                            }))
                return false;
            for (const VitaModelSurface& surface : lod.surfaces) {
                for (uint32_t surfaceIndex = 0u;
                     surfaceIndex < surface.indexCount; ++surfaceIndex) {
                    const uint16_t index = lod.indices[
                        surface.indexStart + surfaceIndex];
                    if (index < surface.vertexStart
                        || index >= surface.vertexStart
                                + surface.vertexCount)
                        return false;
                }
            }
            model.lods.push_back(std::move(lod));
        }
        parsed.push_back(std::move(model));
    }
    if (offset != bytes.size() || !bytes.Finish())
        return false;

    // Commit only after the complete companion validates. A corrupt VLOD can
    // therefore never leave a half-updated model bundle behind.
    uint32_t attachedModels = 0u;
    for (ParsedRenderLodModel& input : parsed) {
        const auto found = std::find_if(
            models->begin(), models->end(), [&](const VitaModelRecord& model) {
                return model.name == input.name;
            });
        if (found == models->end())
            return false;
        found->lodCount = input.lodCount;
        found->lodRampType = input.lodRampType;
        std::memcpy(found->lodDistances, input.distances,
                    sizeof(found->lodDistances));
        found->renderLods = std::move(input.lods);
        ++attachedModels;
    }
    return attachedModels == modelCount;
}

} // namespace

void VitaModelBundle::Clear() {
    models_.clear();
    wireVersion_ = 0;
    loaded_ = false;
    renderLodsLoaded_ = false;
}

bool VitaModelBundle::Load(const VitaAssetDatabase& database, uint64_t assetId,
                           uint64_t renderLodAssetId) try {
    Clear();
    const KvapChunk* requested = assetId
        ? database.Find(VitaAssetType::Model, assetId) : nullptr;
    if (assetId && !requested)
        return false;
    for (uint32_t chunkIndex = 0; chunkIndex < database.ChunkCount(); ++chunkIndex) {
        const KvapChunk* chunk = database.Chunk(chunkIndex);
        if (!chunk || chunk->type != static_cast<uint32_t>(VitaAssetType::Model)
            || (assetId && chunk != requested)
            || chunk->rawSize < 20 || chunk->rawSize > kMaxChunkBytes)
            continue;
        VitaChunkWindow bytes(database,chunk);
        const uint32_t magic = ReadU32(bytes, 0);
        const uint32_t version = ReadU32(bytes, 4);
        const uint32_t modelCount = ReadU32(bytes, 8);
        const uint64_t dataBytes = static_cast<uint64_t>(ReadU32(bytes, 12))
            | (static_cast<uint64_t>(ReadU32(bytes, 16)) << 32);
        if (magic != kMagic || (version != kVersionV1 && version != kVersionV2
                                && version != kVersionV3 && version != kVersionV4
                                && version != kVersionV5)
            || modelCount == 0
            || modelCount > kMaxModels || dataBytes != bytes.size() - 20)
            continue;

        size_t offset = 20;
        std::vector<VitaModelRecord> parsed;
        parsed.reserve(modelCount);
        bool valid = true;
        for (uint32_t modelIndex = 0; modelIndex < modelCount && valid; ++modelIndex) {
            if (offset + 40 > bytes.size()) {
                valid = false;
                break;
            }
            const uint32_t nameBytes = ReadU32(bytes, offset + 0);
            const uint32_t surfaceCount = ReadU32(bytes, offset + 4);
            const uint32_t vertexCount = ReadU32(bytes, offset + 8);
            const uint32_t indexCount = ReadU32(bytes, offset + 12);
            if (nameBytes == 0 || nameBytes > 256 || surfaceCount == 0
                || surfaceCount > kMaxSurfaces || vertexCount == 0
                || vertexCount > kMaxVertices || indexCount < 3
                || indexCount > kMaxIndices || indexCount % 3 != 0) {
                valid = false;
                break;
            }
            VitaModelRecord model{};
            for (uint32_t component = 0; component < 3; ++component) {
                model.mins[component] = ReadFloat(bytes, offset + 16 + component * 4);
                model.maxs[component] = ReadFloat(bytes, offset + 28 + component * 4);
            }
            offset += 40;
            if (!ReadString(bytes, &offset, nameBytes, &model.name, 256)) {
                valid = false;
                break;
            }
            if (version == kVersionV2 || version == kVersionV3
                || version == kVersionV4 || version == kVersionV5) {
                if (offset + 16 > bytes.size()) {
                    valid = false;
                    break;
                }
                const uint32_t boneCount = ReadU32(bytes, offset + 0);
                const uint32_t rootBoneCount = ReadU32(bytes, offset + 4);
                const uint32_t parentBytes = ReadU32(bytes, offset + 8);
                const uint32_t reserved = ReadU32(bytes, offset + 12);
                const size_t boneBytesPerRecord = version == kVersionV3
                    || version == kVersionV4 || version == kVersionV5
                    ? sizeof(VitaModelBone) : sizeof(VitaModelBoneLegacy);
                const uint32_t weaponTag = reserved & 0xFFFFu;
                const uint32_t cameraTag = reserved >> 16;
                uint32_t flashTag = 0;
                uint32_t boneNameBytes = 0;
                const size_t skinHeaderBytes = version == kVersionV5 ? 24u
                    : (version == kVersionV4 ? 20u : 16u);
                if (offset + skinHeaderBytes > bytes.size()) {
                    valid = false;
                    break;
                }
                if (version == kVersionV4 || version == kVersionV5)
                    flashTag = ReadU32(bytes, offset + 16);
                if (version == kVersionV5)
                    boneNameBytes = ReadU32(bytes, offset + 20);
                if (boneCount > kMaxBones
                    || rootBoneCount > boneCount || parentBytes != boneCount
                    // VITX v3-v5 use this formerly-unused field for one-based
                    // tag indices: tag_weapon in the low half and tag_camera
                    // in the high half. Older versions retain zero; v4/v5 add
                    // tag_flash and v5 adds one fixed name record per bone.
                    || (version != kVersionV3 && version != kVersionV4
                        && version != kVersionV5
                        && reserved != 0)
                    || ((version == kVersionV3 || version == kVersionV4
                         || version == kVersionV5)
                        && (weaponTag > boneCount || cameraTag > boneCount))
                    || ((version == kVersionV4 || version == kVersionV5)
                        && flashTag > boneCount)
                    || (version == kVersionV5
                        && boneNameBytes != boneCount * 64u)
                    || offset + skinHeaderBytes + parentBytes + boneNameBytes
                        + static_cast<uint64_t>(boneCount) * boneBytesPerRecord
                        > bytes.size()) {
                    valid = false;
                    break;
                }
                offset += skinHeaderBytes;
                model.parents.resize(parentBytes);
                if(!bytes.Read(offset,model.parents.data(),parentBytes)){valid=false;break;}
                offset += parentBytes;
                for (uint8_t parent : model.parents) {
                    if (parent != 0xFFu && parent >= boneCount) {
                        valid = false;
                        break;
                    }
                }
                if (!valid)
                    break;
                if (version == kVersionV5) {
                    model.boneNames.reserve(boneCount);
                    for (uint32_t boneIndex = 0; boneIndex < boneCount; ++boneIndex) {
                        char fixedName[64]{};
                        if(!bytes.Read(offset+static_cast<size_t>(boneIndex)*64u,fixedName,sizeof(fixedName))){valid=false;break;}
                        const void* terminator = std::memchr(fixedName, '\0', 64u);
                        // An empty record is the authored anonymous-root name
                        // (source string-table id 1), not a missing name.
                        if (!terminator) {
                            valid = false;
                            break;
                        }
                        const size_t length = static_cast<const char*>(terminator)
                            - fixedName;
                        model.boneNames.emplace_back(fixedName, length);
                    }
                    if (!valid)
                        break;
                    offset += boneNameBytes;
                }
                model.bones.resize(boneCount);
                if (version == kVersionV3 || version == kVersionV4
                    || version == kVersionV5)
                    model.weaponAttachBone = weaponTag != 0
                        ? static_cast<int16_t>(weaponTag - 1) : -1;
                model.cameraTagBone = (version == kVersionV3 || version == kVersionV4
                                       || version == kVersionV5)
                    && cameraTag != 0
                    ? static_cast<int16_t>(cameraTag - 1) : -1;
                model.flashTagBone = (version == kVersionV4 || version == kVersionV5)
                    && flashTag != 0
                    ? static_cast<int16_t>(flashTag - 1) : -1;
                const size_t boneBytes = static_cast<size_t>(boneCount)
                    * boneBytesPerRecord;
                if (version == kVersionV3 || version == kVersionV4
                    || version == kVersionV5) {
                    if(!bytes.Read(offset,model.bones.data(),boneBytes)){valid=false;break;}
                    model.localSkeleton = true;
                } else {
                    for (uint32_t boneIndex = 0; boneIndex < boneCount; ++boneIndex) {
                        VitaModelBoneLegacy legacy{};
                        if(!bytes.Read(offset+static_cast<size_t>(boneIndex)*sizeof(legacy),&legacy,sizeof(legacy))){valid=false;break;}
                        VitaModelBone& bone = model.bones[boneIndex];
                        std::memcpy(bone.quat, legacy.quat, sizeof(bone.quat));
                        std::memcpy(bone.trans, legacy.trans, sizeof(bone.trans));
                        bone.transWeight = legacy.transWeight;
                        std::memcpy(bone.bindQuat, legacy.quat,
                                    sizeof(bone.bindQuat));
                        std::memcpy(bone.bindTrans, legacy.trans,
                                    sizeof(bone.bindTrans));
                        bone.bindTransWeight = legacy.transWeight;
                    }
                }
                offset += boneBytes;
                for (const VitaModelBone& bone : model.bones) {
                    for (float component : bone.quat) {
                        if (!std::isfinite(component)) {
                            valid = false;
                            break;
                        }
                    }
                    for (float component : bone.trans) {
                        if (!std::isfinite(component)) {
                            valid = false;
                            break;
                        }
                    }
                    for (float component : bone.bindQuat) {
                        if ((version == kVersionV3 || version == kVersionV4
                             || version == kVersionV5)
                            && !std::isfinite(component)) {
                            valid = false;
                            break;
                        }
                    }
                    for (float component : bone.bindTrans) {
                        if ((version == kVersionV3 || version == kVersionV4
                             || version == kVersionV5)
                            && !std::isfinite(component)) {
                            valid = false;
                            break;
                        }
                    }
                    if (!std::isfinite(bone.transWeight)
                        || ((version == kVersionV3 || version == kVersionV4
                             || version == kVersionV5)
                            && !std::isfinite(bone.bindTransWeight)))
                        valid = false;
                    if (!valid)
                        break;
                }
                if (!valid)
                    break;
            }
            model.surfaces.reserve(surfaceCount);
            for (uint32_t surfaceIndex = 0; surfaceIndex < surfaceCount; ++surfaceIndex) {
                if (offset + 24 > bytes.size()) {
                    valid = false;
                    break;
                }
                VitaModelSurface surface{};
                const uint32_t materialBytes = ReadU32(bytes, offset + 0);
                surface.vertexStart = ReadU32(bytes, offset + 4);
                surface.vertexCount = ReadU32(bytes, offset + 8);
                surface.indexStart = ReadU32(bytes, offset + 12);
                surface.indexCount = ReadU32(bytes, offset + 16);
                if (materialBytes > 256 || surface.vertexCount == 0
                    || surface.indexCount == 0 || surface.indexCount % 3 != 0
                    || static_cast<uint64_t>(surface.vertexStart) + surface.vertexCount > vertexCount
                    || static_cast<uint64_t>(surface.indexStart) + surface.indexCount > indexCount) {
                    valid = false;
                    break;
                }
                offset += 24;
                if (materialBytes != 0
                    && !ReadString(bytes, &offset, materialBytes, &surface.materialName, 256)) {
                    valid = false;
                    break;
                }
                model.surfaces.push_back(std::move(surface));
            }
            if (!valid)
                break;
            const size_t vertexBytes = static_cast<size_t>(vertexCount) * sizeof(VitaModelVertex);
            const size_t indexBytes = static_cast<size_t>(indexCount) * sizeof(uint16_t);
            if (offset + vertexBytes + indexBytes > bytes.size()) {
                valid = false;
                break;
            }
            model.vertices.resize(vertexCount);
            if(!bytes.Read(offset,model.vertices.data(),vertexBytes)){valid=false;break;}
            offset += vertexBytes;
            if (version == kVersionV2 || version == kVersionV3
                || version == kVersionV4 || version == kVersionV5) {
                const size_t weightBytes = static_cast<size_t>(vertexCount)
                    * sizeof(VitaModelVertexWeight);
                if (offset + weightBytes > bytes.size()) {
                    valid = false;
                    break;
                }
                model.weights.resize(vertexCount);
                if(!bytes.Read(offset,model.weights.data(),weightBytes)){valid=false;break;}
                offset += weightBytes;
                for (const VitaModelVertexWeight& weight : model.weights) {
                    uint32_t total = 0;
                    for (uint32_t slot = 0; slot < 4; ++slot) {
                        if (weight.weight[slot] != 0 && weight.bone[slot] >= model.bones.size()) {
                            valid = false;
                            break;
                        }
                        total += weight.weight[slot];
                    }
                    if (!valid || total > 65535u) {
                        valid = false;
                        break;
                    }
                }
                if (!valid)
                    break;
            }
            model.indices.resize(indexCount);
            if(!bytes.Read(offset,model.indices.data(),indexBytes)){valid=false;break;}
            offset += indexBytes;
            for (uint16_t index : model.indices) {
                if (index >= vertexCount) {
                    valid = false;
                    break;
                }
            }
            if (valid)
                parsed.push_back(std::move(model));
        }
        if (valid && offset == bytes.size() && parsed.size() == modelCount && bytes.Finish()) {
            models_ = std::move(parsed);
            std::sort(models_.begin(), models_.end(), [](const VitaModelRecord& left,
                                                         const VitaModelRecord& right) {
                return left.name < right.name;
            });
            loaded_ = true;
            wireVersion_ = version;
            break;
        }
    }
    if (!loaded_)
        return false;

    const KvapChunk* requestedRenderLod = renderLodAssetId
        ? database.Find(VitaAssetType::Model, renderLodAssetId) : nullptr;
    if (requestedRenderLod) {
        if (!ParseRenderLodChunk(database, requestedRenderLod, &models_)) {
            Clear();
            return false;
        }
        renderLodsLoaded_ = true;
        return true;
    }

    // Generic/single-map development packages do not have stable numeric map
    // ids. Discover their optional VLOD by magic while keeping an explicitly
    // requested (but absent) companion backward-compatible with VITX-only
    // packages produced before the render archive existed.
    if (renderLodAssetId == 0u) {
        for (uint32_t chunkIndex = 0; chunkIndex < database.ChunkCount();
             ++chunkIndex) {
            const KvapChunk* chunk = database.Chunk(chunkIndex);
            if (!chunk
                || chunk->type != static_cast<uint32_t>(VitaAssetType::Model)
                || chunk->rawSize < 20u || chunk->rawSize > kMaxChunkBytes)
                continue;
            uint32_t magic = 0u;
            if (database.ReadRange(chunk, 0u, &magic, sizeof(magic))
                    != KvapError::Ok
                || magic != kRenderLodMagic)
                continue;
            if (!ParseRenderLodChunk(database, chunk, &models_)) {
                Clear();
                return false;
            }
            renderLodsLoaded_ = true;
            break;
        }
    }
    return true;
} catch(const std::bad_alloc&) {
    Clear();
    VITA_CPU_LOADING("model-load-allocation-failed");
    return false;
}

const VitaModelRenderLod* VitaModelRecord::FindRenderLod(
    uint32_t sourceLod) const {
    const auto found = std::find_if(
        renderLods.begin(), renderLods.end(), [sourceLod](const auto& lod) {
            return lod.sourceLod == sourceLod;
        });
    return found == renderLods.end() ? nullptr : &*found;
}

uint32_t VitaModelBundle::RenderLodCount() const {
    uint32_t count = 0u;
    for (const VitaModelRecord& model : models_)
        count += static_cast<uint32_t>(model.renderLods.size());
    return count;
}

void VitaModelBundle::ReleaseRenderLods() {
    for (VitaModelRecord& model : models_)
        std::vector<VitaModelRenderLod>().swap(model.renderLods);
    renderLodsLoaded_ = false;
}

const VitaModelRecord* VitaModelBundle::Find(const char* name) const {
    if (!name)
        return nullptr;
    const auto found = std::find_if(models_.begin(), models_.end(),
                                    [name](const VitaModelRecord& model) {
                                        return model.name == name;
                                    });
    return found == models_.end() ? nullptr : &*found;
}

const VitaModelRecord* VitaModelBundle::FindContains(const char* name) const {
    if (!name)
        return nullptr;
    const auto found = std::find_if(models_.begin(), models_.end(),
                                    [name](const VitaModelRecord& model) {
                                        return model.name.find(name) != std::string::npos;
                                    });
    return found == models_.end() ? nullptr : &*found;
}

void VitaModelBundle::PruneToNames(const std::vector<std::string>& names) {
    if (!loaded_ || names.empty())
        return;
    std::vector<VitaModelRecord> retained;
    retained.reserve(std::min(models_.size(), names.size()));
    for (VitaModelRecord& model : models_) {
        if (std::find(names.begin(), names.end(), model.name) == names.end())
            continue;
        retained.push_back(std::move(model));
    }
    if (!retained.empty()) {
        models_.swap(retained);
        return;
    }
    // Never turn a valid model bundle into an empty one because a legacy
    // package did not expose names in the entity/weapon sidecars.
}

} // namespace kisak::vita
