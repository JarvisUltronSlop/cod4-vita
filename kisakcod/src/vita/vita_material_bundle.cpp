#include <vita/vita_material_bundle.h>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <utility>

namespace kisak::vita {
namespace {

constexpr uint32_t kMagic = 0x4C544956; // "VITL"
constexpr uint32_t kVersionV1 = 1;
constexpr uint32_t kVersionV2 = 2;
constexpr uint32_t kVersionV3 = 3;
constexpr uint32_t kVersionV4 = 4;
constexpr uint32_t kVersionV5 = 5;
constexpr uint32_t kMaxRecords = 4096;
constexpr uint32_t kMaxStringBytes = 4096;
constexpr uint64_t kMaxAggregateBytes = 16ull * 1024ull * 1024ull;
constexpr size_t kHeaderBytes = 20;
constexpr size_t kV1RecordHeaderBytes = 8;
// VITL v2: five variable string lengths, source material flags/state, and
// sampler-state bytes for the three authored texture semantic slots.
constexpr size_t kV2RecordHeaderBytes = 32;
// VITL v3 appends the source lit-pass custom sampler bits. The string layout
// remains unchanged so v2 packages stay readable through the compatibility
// branch above.
constexpr size_t kV3RecordHeaderBytes = 33;
// VITL v4 appends the first source lit-pass GfxStateBits pair and a validity
// byte. Older packages remain readable through the compatibility branches.
constexpr size_t kV4RecordHeaderBytes = 42;
constexpr size_t kV5RecordHeaderBytes = 52;

uint32_t ReadU32(const uint8_t* bytes) {
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

uint64_t ReadU64(const uint8_t* bytes) {
    uint64_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

bool ReadString(const std::vector<uint8_t>& data, size_t* offset,
                uint32_t length, std::string* value, bool allowEmpty) {
    if (!offset || !value || (!allowEmpty && length == 0) || length > kMaxStringBytes
        || *offset > data.size() || length > data.size() - *offset)
        return false;
    value->assign(reinterpret_cast<const char*>(data.data() + *offset), length);
    *offset += length;
    return true;
}

} // namespace

void VitaMaterialBundle::Clear() {
    materials_.clear();
    loaded_ = false;
}

bool VitaMaterialBundle::Load(const VitaAssetDatabase& database, uint64_t assetId) {
    Clear();
    const KvapChunk* requested = assetId
        ? database.Find(VitaAssetType::Material, assetId) : nullptr;
    if (assetId && !requested)
        return false;
    for (uint32_t index = 0; index < database.ChunkCount(); ++index) {
        const KvapChunk* chunk = database.Chunk(index);
        if (!chunk || chunk->type != static_cast<uint32_t>(VitaAssetType::Material)
            || (assetId && chunk != requested)
            || chunk->rawSize < kHeaderBytes || chunk->rawSize > kMaxAggregateBytes)
            continue;
        std::vector<uint8_t> data(static_cast<size_t>(chunk->rawSize));
        if (database.Read(chunk, data.data(), data.size()) != KvapError::Ok
            || ReadU32(data.data()) != kMagic)
            continue;
        const uint32_t version = ReadU32(data.data() + 4);
        if (version != kVersionV1 && version != kVersionV2
            && version != kVersionV3 && version != kVersionV4 && version != kVersionV5)
            continue;
        const uint32_t recordCount = ReadU32(data.data() + 8);
        const uint64_t declaredBytes = ReadU64(data.data() + 12);
        if (recordCount > kMaxRecords || declaredBytes > kMaxAggregateBytes)
            continue;
        size_t offset = kHeaderBytes;
        uint64_t aggregateBytes = 0;
        std::vector<VitaMaterialRecord> parsed;
        parsed.reserve(recordCount);
        bool valid = true;
        for (uint32_t recordIndex = 0; recordIndex < recordCount; ++recordIndex) {
            const size_t recordHeaderBytes = version == kVersionV1
                ? kV1RecordHeaderBytes
                : (version == kVersionV2 ? kV2RecordHeaderBytes
                    : (version == kVersionV3 ? kV3RecordHeaderBytes
                    : (version == kVersionV4 ? kV4RecordHeaderBytes : kV5RecordHeaderBytes)));
            if (offset > data.size() || data.size() - offset < recordHeaderBytes) {
                valid = false;
                break;
            }
            VitaMaterialRecord material;
            const uint32_t nameBytes = ReadU32(data.data() + offset);
            const uint32_t colorBytes = ReadU32(data.data() + offset + 4);
            uint32_t normalBytes = 0;
            uint32_t specularBytes = 0;
            uint32_t techniqueBytes = 0;
            if (version == kVersionV2 || version == kVersionV3
                || version == kVersionV4 || version == kVersionV5) {
                normalBytes = ReadU32(data.data() + offset + 8);
                specularBytes = ReadU32(data.data() + offset + 12);
                techniqueBytes = ReadU32(data.data() + offset + 16);
                material.surfaceTypeBits = ReadU32(data.data() + offset + 20);
                material.gameFlags = data[offset + 24];
                material.sortKey = data[offset + 25];
                material.stateFlags = data[offset + 26];
                material.textureCount = data[offset + 27];
                material.colorSamplerState = data[offset + 28];
                material.normalSamplerState = data[offset + 29];
                material.specularSamplerState = data[offset + 30];
                material.cameraRegion = data[offset + 31];
                if (version >= kVersionV3)
                    material.lightmapSamplerFlags = data[offset + 32];
                if (version >= kVersionV4) {
                    material.litStateBits0 = ReadU32(data.data() + offset + 33);
                    material.litStateBits1 = ReadU32(data.data() + offset + 37);
                    material.litStateValid = data[offset + 41] != 0 ? 1u : 0u;
                }
                if (version == kVersionV5) {
                    material.unlitStateBits0 = ReadU32(data.data() + offset + 42);
                    material.unlitStateBits1 = ReadU32(data.data() + offset + 46);
                    material.unlitStateValid = data[offset + 50] != 0 ? 1u : 0u;
                    material.colorSemantic = data[offset + 51];
                    if (material.colorSemantic != 0 && material.colorSemantic != 2) {
                        valid = false;
                        break;
                    }
                }
            }
            offset += recordHeaderBytes;
            if (!ReadString(data, &offset, nameBytes, &material.name, false)
                || !ReadString(data, &offset, colorBytes, &material.colorMap, true)
                || !ReadString(data, &offset, normalBytes, &material.normalMap, true)
                || !ReadString(data, &offset, specularBytes, &material.specularMap, true)
                || !ReadString(data, &offset, techniqueBytes, &material.techniqueSet, true)) {
                valid = false;
                break;
            }
            aggregateBytes += static_cast<uint64_t>(nameBytes) + colorBytes
                + normalBytes + specularBytes + techniqueBytes;
            if (aggregateBytes > kMaxAggregateBytes) {
                valid = false;
                break;
            }
            parsed.push_back(std::move(material));
        }
        if (!valid || offset != data.size() || aggregateBytes != declaredBytes)
            continue;
        materials_ = std::move(parsed);
        std::sort(materials_.begin(), materials_.end(),
                  [](const VitaMaterialRecord& left, const VitaMaterialRecord& right) {
                      return left.name < right.name;
                  });
        loaded_ = true;
        return true;
    }
    return false;
}

const VitaMaterialRecord* VitaMaterialBundle::Find(const char* name) const {
    if (!name)
        return nullptr;
    // Load already sorts these immutable records. Keep the first equal record
    // (including duplicate names), without rescanning the full bundle for
    // every surface. No auxiliary index, allocation or retained pointer.
    const std::string_view key(name);
    const auto found = std::lower_bound(materials_.begin(), materials_.end(), key,
        [](const VitaMaterialRecord& material, std::string_view key) {
            return std::string_view(material.name).compare(key) < 0;
        });
    return found == materials_.end() || std::string_view(found->name) != key ? nullptr : &*found;
}

const VitaMaterialRecord* VitaMaterialBundle::Record(uint32_t index) const {
    return index < materials_.size() ? &materials_[index] : nullptr;
}

} // namespace kisak::vita
