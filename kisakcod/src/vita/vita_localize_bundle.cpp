#include <vita/vita_localize_bundle.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace kisak::vita {
namespace {

constexpr uint32_t kMagic = 0x5A544956; // "VITZ"
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxRecords = 32768;
constexpr uint32_t kMaxStringBytes = 4096;
constexpr uint64_t kMaxAggregateBytes = 8ull * 1024ull * 1024ull;

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

void VitaLocalizeBundle::Clear() {
#if KISAK_VITA_CPU_RUNTIME
    catalogKey_={};
#endif
    records_.clear();
    loaded_ = false;
}

bool VitaLocalizeBundle::Load(const VitaAssetDatabase& database) {
#if KISAK_VITA_CPU_RUNTIME
    VitaCpuCatalogKey key;key.Capture(database,VitaAssetType::Strings);
    if(loaded_ && key==catalogKey_)return true;
#endif
    Clear();
    for (uint32_t index = 0; index < database.ChunkCount(); ++index) {
        const KvapChunk* chunk = database.Chunk(index);
        if (!chunk || chunk->type != static_cast<uint32_t>(VitaAssetType::Strings)
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
        std::vector<VitaLocalizedString> parsed;
        parsed.reserve(recordCount);
        bool valid = true;
        for (uint32_t recordIndex = 0; recordIndex < recordCount; ++recordIndex) {
            if (offset + 8 > data.size()) {
                valid = false;
                break;
            }
            const uint32_t nameBytes = ReadU32(data.data() + offset);
            const uint32_t valueBytes = ReadU32(data.data() + offset + 4);
            offset += 8;
            if (nameBytes == 0 || nameBytes > kMaxStringBytes
                || valueBytes > kMaxStringBytes
                || offset + static_cast<size_t>(nameBytes) + valueBytes > data.size()) {
                valid = false;
                break;
            }
            VitaLocalizedString record;
            record.name.assign(reinterpret_cast<const char*>(data.data() + offset), nameBytes);
            offset += nameBytes;
            record.value.assign(reinterpret_cast<const char*>(data.data() + offset), valueBytes);
            offset += valueBytes;
            aggregateBytes += 8ull + nameBytes + valueBytes;
            if (aggregateBytes > kMaxAggregateBytes
                || record.name.find('\0') != std::string::npos
                || record.value.find('\0') != std::string::npos) {
                valid = false;
                break;
            }
            parsed.push_back(std::move(record));
        }
        if (!valid || offset != data.size() || aggregateBytes != declaredDataBytes)
            continue;
        records_ = std::move(parsed);
        std::sort(records_.begin(), records_.end(),
                  [](const VitaLocalizedString& left, const VitaLocalizedString& right) {
                      return left.name < right.name;
                  });
        loaded_ = true;
#if KISAK_VITA_CPU_RUNTIME
        catalogKey_=key;
#endif
        return true;
    }
    return false;
}

const VitaLocalizedString* VitaLocalizeBundle::Find(const char* name) const {
    if (!name)
        return nullptr;
    if (name[0] == '@')
        ++name;
    const auto found = std::find_if(records_.begin(), records_.end(),
                                    [name](const VitaLocalizedString& record) {
                                        return record.name == name;
                                    });
    return found == records_.end() ? nullptr : &*found;
}

const VitaLocalizedString* VitaLocalizeBundle::Record(uint32_t index) const {
    return index < records_.size() ? &records_[index] : nullptr;
}

} // namespace kisak::vita
