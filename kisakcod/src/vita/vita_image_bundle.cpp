#include <vita/vita_image_bundle.h>
#include <vita/vita_map_assets.h>
#include <vita/vita_chunk_window.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <new>

namespace kisak::vita {
namespace {

constexpr uint32_t kMagic = 0x49544956; // "VITI"
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxRecords = 4096;
constexpr uint32_t kMaxNameBytes = 4096;
constexpr uint32_t kMaxPayloadBytes = 8u * 1024u * 1024u;
// The expanded retail map/model image closure can use an 80 MiB payload
// envelope plus its typed record headers. Keep parser headroom above that
// envelope while retaining a hard bound against accidental all-game dumps.
constexpr uint64_t kMaxAggregateBytes = 96ull * 1024ull * 1024ull;
constexpr size_t kHeaderBytes = 20;
constexpr size_t kRecordHeaderBytes = 32;

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

uint32_t Crc32Update(uint32_t crc, const uint8_t* bytes, size_t size) {
    if (!bytes)
        return crc;
    for (size_t index = 0; index < size; ++index) {
        crc ^= bytes[index];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & static_cast<uint32_t>(-(crc & 1u)));
    }
    return crc;
}

} // namespace

bool VitaImageIndex::AppendChunk(uint64_t assetId) {
    const auto* chunk=database_.Find(VitaAssetType::Image,assetId);
    if (!chunk || chunk->rawSize<kHeaderBytes || chunk->rawSize>kMaxAggregateBytes) return false;
    VitaChunkWindow window(database_,chunk);
    uint8_t header[kHeaderBytes]{};
    if (!window.Read(0,header,sizeof(header)) || ReadU32(header)!=kMagic
        || ReadU32(header+4)!=kVersion) return false;
    const uint32_t count=ReadU32(header+8);
    const uint64_t declared=ReadU64(header+12);
    if (count>kMaxRecords || entries_.size()>kMaxRecords-count || declared>kMaxAggregateBytes) return false;
    std::vector<Entry> parsed;
    parsed.reserve(count);
    size_t offset=kHeaderBytes;
    uint64_t aggregate=0;
    uint8_t scratch[65536];
    for (uint32_t i=0;i<count;++i) {
        uint8_t h[kRecordHeaderBytes]{};
        if (!window.Read(offset,h,sizeof(h))) return false;
        offset+=sizeof(h);
        const uint32_t nameBytes=ReadU32(h),payloadBytes=ReadU32(h+4);
        Entry e; auto& image=e.image;
        image.width=ReadU32(h+8);image.height=ReadU32(h+12);image.depth=ReadU32(h+16);
        image.levelCount=ReadU32(h+20);image.format=ReadU32(h+24);image.flags=ReadU32(h+28);
        if (!image.width || image.width>4096 || !image.height || image.height>4096
            || image.depth!=1 || !image.levelCount || image.levelCount>16
            || !payloadBytes || payloadBytes>kMaxPayloadBytes || !nameBytes || nameBytes>kMaxNameBytes
            || nameBytes_+nameBytes>512u*1024u) return false;
        image.name.resize(nameBytes);
        if (!window.Read(offset,image.name.data(),nameBytes)
            || image.name.find('\0')!=std::string::npos) return false;
        nameBytes_+=nameBytes;offset+=nameBytes;
        e.assetId=assetId;e.offset=offset;e.size=payloadBytes;e.chunkCrc=chunk->crc32;
        uint32_t crc=0xffffffffu;
        for (size_t remaining=payloadBytes;remaining;) {
            const size_t bytes=std::min(remaining,sizeof(scratch));
            if (!window.Read(offset,scratch,bytes)) return false;
            crc=KvapCrc32Update(crc,scratch,bytes);offset+=bytes;remaining-=bytes;
        }
        e.crc=~crc;aggregate+=payloadBytes;
        if (aggregate>kMaxAggregateBytes) return false;
        parsed.push_back(std::move(e));
    }
    if (offset!=chunk->rawSize || aggregate!=declared || !window.Finish()) return false;
    std::sort(parsed.begin(),parsed.end(),[](const Entry& a,const Entry& b){return a.image.name<b.image.name;});
    for (size_t i=1;i<parsed.size();++i)
        if (parsed[i-1].image.name==parsed[i].image.name) return false;
    // Base chunk wins over subsequent colour segments, as in LoadNamedMapColor.
    // Duplicate aliases inside any one chunk are always rejected.
    for (auto& e:parsed) {
        const auto at=std::lower_bound(entries_.begin(),entries_.end(),e.image.name,
            [](const Entry& a,const std::string& name){return a.image.name<name;});
        if (at==entries_.end() || at->image.name!=e.image.name) entries_.insert(at,std::move(e));
    }
    return true;
}

bool VitaImageIndex::Build(uint64_t assetId) try {
    entries_.clear();nameBytes_=0;
    if (AppendChunk(assetId)) return true;
    entries_.clear();nameBytes_=0;return false;
} catch (const std::bad_alloc&) {entries_.clear();nameBytes_=0;return false;}

bool VitaImageIndex::BuildMapColors(uint32_t mapIndex) try {
    entries_.clear();nameBytes_=0;
    if (!VitaMapIsEnabled(mapIndex)) return false;
    const auto base=VitaMapImagesAssetId(mapIndex);
    if (database_.Find(VitaAssetType::Image,base) && !AppendChunk(base)) {
        entries_.clear();return false;
    }
    for (uint32_t segment=0;segment<64;++segment) {
        const auto asset=VitaMapColorImagesAssetId(mapIndex,segment);
        if (!database_.Find(VitaAssetType::Image,asset)) break;
        if (!AppendChunk(asset)) {entries_.clear();return false;}
    }
    return !entries_.empty();
} catch (const std::bad_alloc&) {entries_.clear();nameBytes_=0;return false;}

bool VitaImageIndex::Contains(const char* name) const {
    if (!name || !*name) return false;
    const auto at=std::lower_bound(entries_.begin(),entries_.end(),name,
        [](const Entry& a,const char* b){return a.image.name<b;});
    return at!=entries_.end() && at->image.name==name;
}

bool VitaImageIndex::Read(const char* name,VitaImageRecord* output) const try {
    if (!name || !*name || !output) return false;
    const auto at=std::lower_bound(entries_.begin(),entries_.end(),name,
        [](const Entry& a,const char* b){return a.image.name<b;});
    if (at==entries_.end() || at->image.name!=name) return false;
    const auto* chunk=database_.Find(VitaAssetType::Image,at->assetId);
    if (!chunk || chunk->crc32!=at->chunkCrc) return false;
    VitaImageRecord image=at->image;
    image.bytes.resize(at->size);
    if (database_.ReadRange(chunk,at->offset,image.bytes.data(),image.bytes.size())!=KvapError::Ok
        || KvapCrc32(image.bytes.data(),image.bytes.size())!=at->crc) return false;
    *output=std::move(image);return true;
} catch (const std::bad_alloc&) {return false;}

bool VitaImageBundle::LoadIndexed(const VitaImageIndex& index,const char* name,bool append) try {
    if (!append) Clear();
    if (!name || images_.size()>=kMaxRecords || Find(name)) return false;
    VitaImageRecord image;
    if (!index.Read(name,&image)) return false;
    images_.push_back(std::move(image));loaded_=true;return true;
} catch (const std::bad_alloc&) {return false;}

void VitaImageBundle::Clear() {
    // Image payloads can occupy tens of MiB for one map.  Release the old
    // vector's capacity on map changes so a reload does not retain the prior
    // map's source-image envelope while the new one is being parsed.
    std::vector<VitaImageRecord>().swap(images_);
    loaded_ = false;
}

namespace {

bool ReadImageChunk(const VitaAssetDatabase& database, const KvapChunk* chunk,
                    std::vector<VitaImageRecord>* output,
                    const std::vector<std::string>* selectedNames = nullptr,
                    uint64_t maxSelectedPayloadBytes = UINT64_MAX,
                    bool metadataOnly = false, bool retainMetadata = false) {
    if (!chunk || !output
        || chunk->rawSize < kHeaderBytes || chunk->rawSize > kMaxAggregateBytes)
        return false;
    // VITI image chunks are large because they contain the source payload for
    // every map texture. Read the fixed stream in bounded ranges and append
    // directly to the destination transaction. Loading the main and optional
    // reflection chunks through the same transaction avoids a second bundle
    // and the late vector/heap transition that can destabilize large maps.
    uint8_t header[kHeaderBytes]{};
    const auto ReadAt = [&](uint64_t offset, void* destination, size_t size) {
        return database.ReadRange(chunk, offset, destination, size) == KvapError::Ok;
    };
    if (!ReadAt(0, header, sizeof(header))
        || ReadU32(header) != kMagic || ReadU32(header + 4) != kVersion)
        return false;
    const uint32_t recordCount = ReadU32(header + 8);
    const uint64_t declaredBytes = ReadU64(header + 12);
    if (recordCount > kMaxRecords || output->size() > kMaxRecords - recordCount
        || declaredBytes > kMaxAggregateBytes
        || declaredBytes > UINT64_MAX - kHeaderBytes)
        return false;
    const size_t originalSize = output->size();
    output->reserve(output->size() + recordCount);
    uint64_t offset = kHeaderBytes;
    uint64_t aggregateBytes = 0;
    uint64_t selectedPayloadBytes = 0;
    for (const auto& existing:*output) selectedPayloadBytes+=existing.bytes.size();
    bool valid = true;
    uint32_t crc = Crc32Update(0xFFFFFFFFu, header, sizeof(header));
    for (uint32_t recordIndex = 0; recordIndex < recordCount; ++recordIndex) {
        uint8_t recordHeader[kRecordHeaderBytes]{};
        if (offset > chunk->rawSize
            || kRecordHeaderBytes > chunk->rawSize - offset
            || !ReadAt(offset, recordHeader, sizeof(recordHeader))) {
            valid = false;
            break;
        }
        crc = Crc32Update(crc, recordHeader, sizeof(recordHeader));
        const uint32_t nameBytes = ReadU32(recordHeader);
        const uint32_t payloadBytes = ReadU32(recordHeader + 4);
        VitaImageRecord image;
        image.width = ReadU32(recordHeader + 8);
        image.height = ReadU32(recordHeader + 12);
        image.depth = ReadU32(recordHeader + 16);
        image.levelCount = ReadU32(recordHeader + 20);
        image.format = ReadU32(recordHeader + 24);
        image.flags = ReadU32(recordHeader + 28);
        offset += kRecordHeaderBytes;
        if (image.width == 0 || image.width > 4096 || image.height == 0
            || image.height > 4096 || image.depth == 0 || image.depth > 1
            || image.levelCount == 0 || image.levelCount > 16
            || payloadBytes == 0 || payloadBytes > kMaxPayloadBytes
            || nameBytes == 0 || nameBytes > kMaxNameBytes
            || offset > chunk->rawSize || nameBytes > chunk->rawSize - offset) {
            valid = false;
            break;
        }
        image.name.resize(nameBytes);
        if (!ReadAt(offset, image.name.data(), image.name.size())) {
            valid = false;
            break;
        }
        crc = Crc32Update(crc, reinterpret_cast<const uint8_t*>(image.name.data()),
                          image.name.size());
        offset += nameBytes;
        if (offset > chunk->rawSize || payloadBytes > chunk->rawSize - offset) {
            valid = false;
            break;
        }
        const bool requested = !selectedNames
            || std::find(selectedNames->begin(), selectedNames->end(), image.name)
                != selectedNames->end();
        const bool selected = !metadataOnly && requested
            && payloadBytes <= maxSelectedPayloadBytes
            && selectedPayloadBytes <= maxSelectedPayloadBytes - payloadBytes;
        if (selected) {
            image.bytes.resize(payloadBytes);
            if (!ReadAt(offset, image.bytes.data(), image.bytes.size())) {
                valid = false;
                break;
            }
            crc = Crc32Update(crc, image.bytes.data(), image.bytes.size());
            selectedPayloadBytes += payloadBytes;
        } else if (!selectedNames || metadataOnly || retainMetadata) {
            // Complete loads validate every payload. Filtered scalar model
            // loads intentionally skip unrelated bytes after their headers
            // have been bounds-checked; the host package audit validates the
            // complete companion CRC, and this keeps guest startup from
            // synchronously copying the entire sidecar through Vita3K.
            // Match the package reader's bounded window. Tiny 4 KiB jobs
            // incurred a separate owner/worker round trip for every CRC block.
            uint8_t scratch[65536];
            uint64_t remaining = payloadBytes;
            uint64_t payloadOffset = offset;
            while (remaining != 0) {
                const size_t request = static_cast<size_t>(std::min<uint64_t>(
                    remaining, sizeof(scratch)));
                if (!ReadAt(payloadOffset, scratch, request)) {
                    valid = false;
                    break;
                }
                crc = Crc32Update(crc, scratch, request);
                payloadOffset += request;
                remaining -= request;
            }
            if (!valid)
                break;
        }
        offset += payloadBytes;
        aggregateBytes += payloadBytes;
        if (aggregateBytes > kMaxAggregateBytes) {
            valid = false;
            break;
        }
        if (metadataOnly || retainMetadata || selected)
            output->push_back(std::move(image));
    }
    if (!valid || offset != chunk->rawSize || aggregateBytes != declaredBytes
        || ((!selectedNames || retainMetadata) && ~crc != chunk->crc32)) {
        output->resize(originalSize);
        return false;
    }
    return true;
}

bool ReadNamedImageChunk(const VitaAssetDatabase& database,
                         const KvapChunk* chunk, const char* requestedName,
                         VitaImageRecord* output) {
    if (!chunk || !requestedName || !*requestedName || !output
        || chunk->rawSize < kHeaderBytes || chunk->rawSize > kMaxAggregateBytes)
        return false;
    const size_t requestedNameBytes = std::strlen(requestedName);
    if (requestedNameBytes == 0 || requestedNameBytes > kMaxNameBytes)
        return false;
    const auto ReadAt = [&](uint64_t offset, void* destination, size_t size) {
        return database.ReadRange(chunk, offset, destination, size) == KvapError::Ok;
    };
    uint8_t header[kHeaderBytes]{};
    if (!ReadAt(0, header, sizeof(header))
        || ReadU32(header) != kMagic || ReadU32(header + 4) != kVersion)
        return false;
    const uint32_t recordCount = ReadU32(header + 8);
    const uint64_t declaredBytes = ReadU64(header + 12);
    if (recordCount > kMaxRecords || declaredBytes > kMaxAggregateBytes
        || declaredBytes > UINT64_MAX - kHeaderBytes)
        return false;

    // Names are bounded by the VITI wire format. Keeping this buffer on the
    // stack means an unsuccessful alias lookup performs no per-record heap
    // allocation while the guest is already carrying the world textures.
    char name[kMaxNameBytes + 1]{};
    uint64_t offset = kHeaderBytes;
    uint64_t aggregateBytes = 0;
    bool found = false;
    for (uint32_t recordIndex = 0; recordIndex < recordCount; ++recordIndex) {
        uint8_t recordHeader[kRecordHeaderBytes]{};
        if (offset > chunk->rawSize
            || kRecordHeaderBytes > chunk->rawSize - offset
            || !ReadAt(offset, recordHeader, sizeof(recordHeader)))
            return false;
        const uint32_t nameBytes = ReadU32(recordHeader);
        const uint32_t payloadBytes = ReadU32(recordHeader + 4);
        VitaImageRecord image;
        image.width = ReadU32(recordHeader + 8);
        image.height = ReadU32(recordHeader + 12);
        image.depth = ReadU32(recordHeader + 16);
        image.levelCount = ReadU32(recordHeader + 20);
        image.format = ReadU32(recordHeader + 24);
        image.flags = ReadU32(recordHeader + 28);
        offset += kRecordHeaderBytes;
        if (image.width == 0 || image.width > 4096 || image.height == 0
            || image.height > 4096 || image.depth == 0 || image.depth > 1
            || image.levelCount == 0 || image.levelCount > 16
            || payloadBytes == 0 || payloadBytes > kMaxPayloadBytes
            || nameBytes == 0 || nameBytes > kMaxNameBytes
            || offset > chunk->rawSize || nameBytes > chunk->rawSize - offset)
            return false;
        if (!ReadAt(offset, name, nameBytes))
            return false;
        name[nameBytes] = '\0';
        offset += nameBytes;
        if (offset > chunk->rawSize || payloadBytes > chunk->rawSize - offset)
            return false;
        if (!found && nameBytes == requestedNameBytes
            && std::memcmp(name, requestedName, nameBytes) == 0) {
            image.name.assign(name, nameBytes);
            image.bytes.resize(payloadBytes);
            if (!ReadAt(offset, image.bytes.data(), image.bytes.size()))
                return false;
            *output = std::move(image);
            found = true;
        } else if (found && nameBytes == requestedNameBytes
                   && std::memcmp(name, requestedName, nameBytes) == 0) {
            // A duplicate would make the selected alias ambiguous and is a
            // package-closure error, not a reason to upload the wrong layer.
            return false;
        }
        offset += payloadBytes;
        aggregateBytes += payloadBytes;
        if (aggregateBytes > kMaxAggregateBytes)
            return false;
    }
    return found && offset == chunk->rawSize && aggregateBytes == declaredBytes;
}

} // namespace

bool VitaImageBundle::Load(const VitaAssetDatabase& database, uint64_t assetId,
                           uint64_t additionalAssetId) {
    return LoadSelected(database, assetId, {}, additionalAssetId,
                        UINT64_MAX);
}

bool VitaImageBundle::LoadSelected(const VitaAssetDatabase& database,
                                   uint64_t assetId,
                                   const std::vector<std::string>& names,
                                   uint64_t additionalAssetId,
                                   uint64_t maxPayloadBytes, bool retainMetadata) try {
    Clear();
    const KvapChunk* requested = assetId
        ? database.Find(VitaAssetType::Image, assetId) : nullptr;
    const KvapChunk* additional = additionalAssetId
        ? database.Find(VitaAssetType::Image, additionalAssetId) : nullptr;
    if ((assetId && !requested) || (additionalAssetId && !additional))
        return false;
    const bool hasRequested = requested != nullptr;
    const bool hasAdditional = additional != nullptr && additional != requested;
    const uint32_t requiredChunks = (hasRequested ? 1u : 0u)
        + (hasAdditional ? 1u : 0u);
    std::vector<VitaImageRecord> parsed;
    parsed.reserve(requiredChunks > 1u ? 384u : 256u);
    uint32_t loadedChunks = 0;
    for (uint32_t index = 0; index < database.ChunkCount(); ++index) {
        const KvapChunk* chunk = database.Chunk(index);
        if (!chunk || chunk->type != static_cast<uint32_t>(VitaAssetType::Image))
            continue;
        if (requiredChunks != 0
            && chunk != requested && chunk != additional)
            continue;
        if (!ReadImageChunk(database, chunk, &parsed,
                            names.empty() ? nullptr : &names,
                            maxPayloadBytes, false, retainMetadata))
            continue;
        ++loadedChunks;
        if (requiredChunks == 0 || loadedChunks == requiredChunks)
            break;
    }
    if (loadedChunks != (requiredChunks != 0 ? requiredChunks : 1u))
        return false;
    images_ = std::move(parsed);
    std::sort(images_.begin(), images_.end(),
              [](const VitaImageRecord& left, const VitaImageRecord& right) {
                  return left.name < right.name;
              });
    loaded_ = true;
    return true;
}

catch (const std::bad_alloc&) { Clear(); return false; }

bool VitaImageBundle::LoadNamedMapColor(const VitaAssetDatabase& database,
    uint32_t mapIndex, const char* name, bool append) {
    if (!VitaMapIsEnabled(mapIndex)) return false;
    const auto ReadAsset = [&](uint64_t asset) {
        return append ? AppendNamed(database, asset, name) : LoadNamed(database, asset, name);
    };
    if (ReadAsset(VitaMapImagesAssetId(mapIndex))) return true;
    for (uint32_t segment = 0; segment < 64; ++segment) {
        const auto asset = VitaMapColorImagesAssetId(mapIndex, segment);
        if (!database.Find(VitaAssetType::Image, asset)) break;
        if (ReadAsset(asset)) return true;
    }
    return false;
}

bool VitaImageBundle::LoadNamed(const VitaAssetDatabase& database,
                                uint64_t assetId, const char* name) try {
    Clear();
    const KvapChunk* requested = assetId
        ? database.Find(VitaAssetType::Image, assetId) : nullptr;
    if (!requested || !name || !*name)
        return false;
    VitaImageRecord image;
    if (!ReadNamedImageChunk(database, requested, name, &image))
        return false;
    images_.reserve(1);
    images_.push_back(std::move(image));
    loaded_ = true;
    return true;
}

catch (const std::bad_alloc&) { Clear(); return false; }

bool VitaImageBundle::AppendNamed(const VitaAssetDatabase& database,
                                  uint64_t assetId, const char* name) try {
    if (assetId == 0 || !name || !*name || images_.size() >= kMaxRecords)
        return false;
    const KvapChunk* requested = database.Find(VitaAssetType::Image, assetId);
    if (!requested)
        return false;
    // Do not retain two source payloads for one authored alias. Callers that
    // already have a backend handle skip the read before reaching this path;
    // the duplicate check also keeps a malformed segment from changing the
    // first source binding nondeterministically.
    if (Find(name) != nullptr)
        return false;
    VitaImageRecord image;
    if (!ReadNamedImageChunk(database, requested, name, &image))
        return false;
    images_.push_back(std::move(image));
    loaded_ = true;
    return true;
}

catch (const std::bad_alloc&) { return false; }

bool VitaImageBundle::LoadMetadata(const VitaAssetDatabase& database,
                                   uint64_t assetId,
                                   uint64_t additionalAssetId) try {
    Clear();
    const KvapChunk* requested = assetId
        ? database.Find(VitaAssetType::Image, assetId) : nullptr;
    const KvapChunk* additional = additionalAssetId
        ? database.Find(VitaAssetType::Image, additionalAssetId) : nullptr;
    if ((assetId && !requested) || (additionalAssetId && !additional))
        return false;
    const bool hasRequested = requested != nullptr;
    const bool hasAdditional = additional != nullptr && additional != requested;
    const uint32_t requiredChunks = (hasRequested ? 1u : 0u)
        + (hasAdditional ? 1u : 0u);
    std::vector<VitaImageRecord> parsed;
    parsed.reserve(requiredChunks > 1u ? 384u : 256u);
    uint32_t loadedChunks = 0;
    for (uint32_t index = 0; index < database.ChunkCount(); ++index) {
        const KvapChunk* chunk = database.Chunk(index);
        if (!chunk || chunk->type != static_cast<uint32_t>(VitaAssetType::Image))
            continue;
        if (requiredChunks != 0
            && chunk != requested && chunk != additional)
            continue;
        if (!ReadImageChunk(database, chunk, &parsed, nullptr,
                            UINT64_MAX, true))
            continue;
        ++loadedChunks;
        if (requiredChunks == 0 || loadedChunks == requiredChunks)
            break;
    }
    if (loadedChunks != (requiredChunks != 0 ? requiredChunks : 1u))
        return false;
    images_ = std::move(parsed);
    std::sort(images_.begin(), images_.end(),
              [](const VitaImageRecord& left, const VitaImageRecord& right) {
                  return left.name < right.name;
              });
    loaded_ = true;
    return true;
}

catch (const std::bad_alloc&) { Clear(); return false; }

bool VitaImageBundle::Append(const VitaAssetDatabase& database,
                             uint64_t assetId) {
    return AppendSelected(database, assetId, {});
}

bool VitaImageBundle::AppendSelected(
    const VitaAssetDatabase& database, uint64_t assetId,
    const std::vector<std::string>& names, uint64_t maxPayloadBytes) try {
    if (!loaded_ || assetId == 0)
        return false;
    VitaImageBundle additional;
    if (!additional.LoadSelected(database, assetId, names, 0,
                                 maxPayloadBytes))
        return false;
    if (images_.size() + additional.images_.size() > kMaxRecords)
        return false;
    for (const VitaImageRecord& image : additional.images_) {
        const auto duplicate = std::find_if(
            images_.begin(), images_.end(), [&](const VitaImageRecord& value) {
                return value.name == image.name;
            });
        if (duplicate != images_.end())
            return false;
    }
    images_.reserve(images_.size() + additional.images_.size());
    for (VitaImageRecord& image : additional.images_)
        images_.push_back(std::move(image));
    std::sort(images_.begin(), images_.end(),
              [](const VitaImageRecord& left, const VitaImageRecord& right) {
                  return left.name < right.name;
              });
    return true;
}

catch (const std::bad_alloc&) { return false; }

const VitaImageRecord* VitaImageBundle::Find(const char* name) const {
    if (!name)
        return nullptr;
    const auto found = std::find_if(images_.begin(), images_.end(),
                                    [name](const VitaImageRecord& image) {
                                        return image.name == name;
                                    });
    return found == images_.end() ? nullptr : &*found;
}

const VitaImageRecord* VitaImageBundle::Record(uint32_t index) const {
    return index < images_.size() ? &images_[index] : nullptr;
}

void VitaImageBundle::ReleasePayload(const char* name) {
    if (!name || !*name)
        return;
    const auto found = std::find_if(images_.begin(), images_.end(),
                                    [name](VitaImageRecord& image) {
                                        return image.name == name;
                                    });
    if (found == images_.end() || found->bytes.empty())
        return;
    std::vector<uint8_t>().swap(found->bytes);
}

uint64_t VitaImageBundle::ReleaseAllPayloads() {
    uint64_t released = 0;
    for (VitaImageRecord& image : images_) {
        released += static_cast<uint64_t>(image.bytes.size());
        if (!image.bytes.empty())
            std::vector<uint8_t>().swap(image.bytes);
    }
    return released;
}

} // namespace kisak::vita
