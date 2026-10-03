#include <vita/vita_rawfile_bundle.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace kisak::vita {
namespace {

constexpr uint32_t kMagic = 0x52544956; // "VITR"
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxRecords = 4096;
constexpr uint32_t kMaxNameBytes = 4096;
constexpr uint32_t kMaxPayloadBytes = 32u * 1024u * 1024u;
constexpr uint64_t kMaxAggregateBytes = 256ull * 1024ull * 1024ull;

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

} // namespace

void VitaRawFileBundle::Clear() {
    records_.clear();
    loaded_ = false;
}

bool VitaRawFileBundle::Load(const VitaAssetDatabase& database) {
    Clear();
    for (uint32_t index = 0; index < database.ChunkCount(); ++index) {
        const KvapChunk* chunk = database.Chunk(index);
        if (!chunk || chunk->type != static_cast<uint32_t>(VitaAssetType::Script)
            || chunk->rawSize < 20 || chunk->rawSize > kMaxAggregateBytes)
            continue;
        std::vector<uint8_t> data(static_cast<size_t>(chunk->rawSize));
        if (database.Read(chunk, data.data(), data.size()) != KvapError::Ok
            || ReadU32(data.data()) != kMagic || ReadU32(data.data() + 4) != kVersion)
            continue;
        const uint32_t recordCount = ReadU32(data.data() + 8);
        const uint64_t declaredDataBytes = ReadU64(data.data() + 12);
        if (recordCount > kMaxRecords || declaredDataBytes > kMaxAggregateBytes)
            continue;
        size_t offset = 20;
        uint64_t aggregateBytes = 0;
        std::vector<VitaRawFileRecord> parsed;
        parsed.reserve(recordCount);
        bool valid = true;
        for (uint32_t recordIndex = 0; recordIndex < recordCount; ++recordIndex) {
            if (offset + 8 > data.size()) {
                valid = false;
                break;
            }
            const uint32_t nameBytes = ReadU32(data.data() + offset);
            const uint32_t payloadBytes = ReadU32(data.data() + offset + 4);
            offset += 8;
            if (nameBytes == 0 || nameBytes > kMaxNameBytes
                || payloadBytes > kMaxPayloadBytes
                || offset + static_cast<size_t>(nameBytes) + payloadBytes > data.size()) {
                valid = false;
                break;
            }
            VitaRawFileRecord record;
            record.name.assign(reinterpret_cast<const char*>(data.data() + offset), nameBytes);
            offset += nameBytes;
            record.bytes.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
                                data.begin() + static_cast<std::ptrdiff_t>(offset + payloadBytes));
            offset += payloadBytes;
            aggregateBytes += payloadBytes;
            if (aggregateBytes > kMaxAggregateBytes) {
                valid = false;
                break;
            }
            parsed.push_back(std::move(record));
        }
        if (!valid || offset != data.size() || aggregateBytes != declaredDataBytes)
            continue;
        records_ = std::move(parsed);
        std::sort(records_.begin(), records_.end(),
                  [](const VitaRawFileRecord& left, const VitaRawFileRecord& right) {
                      return left.name < right.name;
                  });
        loaded_ = true;
        return true;
    }
    return false;
}

const VitaRawFileRecord* VitaRawFileBundle::Find(const char* name) const {
    if (!name)
        return nullptr;
    const auto found = std::find_if(records_.begin(), records_.end(),
                                    [name](const VitaRawFileRecord& record) {
                                        return record.name == name;
                                    });
    return found == records_.end() ? nullptr : &*found;
}

const VitaRawFileRecord* VitaRawFileBundle::Record(uint32_t index) const {
    return index < records_.size() ? &records_[index] : nullptr;
}

} // namespace kisak::vita
