#include <vita/vita_stringtable_bundle.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

namespace kisak::vita {
namespace {

constexpr uint32_t kMagic = 0x54544956; // "VITT"
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxRecords = 32768;
constexpr uint32_t kMaxStringBytes = 4096;
constexpr uint32_t kMaxRows = 4096;
constexpr uint32_t kMaxColumns = 4096;
constexpr uint64_t kMaxCells = 16ull * 1024ull * 1024ull;
constexpr uint64_t kMaxAggregateBytes = 16ull * 1024ull * 1024ull;
constexpr size_t kHeaderBytes = 20;
constexpr size_t kRecordHeaderBytes = 12;

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

bool EqualIgnoreCase(const std::string& left, const char* right) {
    if (!right || left.size() != std::strlen(right))
        return false;
    for (size_t index = 0; index < left.size(); ++index) {
        const unsigned char leftChar = static_cast<unsigned char>(left[index]);
        const unsigned char rightChar = static_cast<unsigned char>(right[index]);
        if (std::tolower(leftChar) != std::tolower(rightChar))
            return false;
    }
    return true;
}

bool ReadString(const std::vector<uint8_t>& data, size_t* offset,
                uint32_t length, std::string* value) {
    if (!offset || !value || length > kMaxStringBytes
        || *offset > data.size()
        || length > data.size() - *offset)
        return false;
    value->assign(reinterpret_cast<const char*>(data.data() + *offset), length);
    *offset += length;
    return value->find('\0') == std::string::npos;
}

} // namespace

void VitaStringTableBundle::Clear() {
#if KISAK_VITA_CPU_RUNTIME
    catalogKey_={};
#endif
    records_.clear();
    loaded_ = false;
}

bool VitaStringTableBundle::Load(const VitaAssetDatabase& database) {
#if KISAK_VITA_CPU_RUNTIME
    VitaCpuCatalogKey key;key.Capture(database,VitaAssetType::Strings);
    if(loaded_ && key==catalogKey_)return true;
#endif
    Clear();
    for (uint32_t index = 0; index < database.ChunkCount(); ++index) {
        const KvapChunk* chunk = database.Chunk(index);
        if (!chunk || chunk->type != static_cast<uint32_t>(VitaAssetType::Strings)
            || chunk->rawSize < kHeaderBytes || chunk->rawSize > kMaxAggregateBytes)
            continue;
        std::vector<uint8_t> data(static_cast<size_t>(chunk->rawSize));
        if (database.Read(chunk, data.data(), data.size()) != KvapError::Ok
            || ReadU32(data.data()) != kMagic || ReadU32(data.data() + 4) != kVersion)
            continue;
        const uint32_t recordCount = ReadU32(data.data() + 8);
        const uint64_t declaredDataBytes = ReadU64(data.data() + 12);
        if (recordCount > kMaxRecords || declaredDataBytes > kMaxAggregateBytes)
            continue;

        size_t offset = kHeaderBytes;
        uint64_t aggregateBytes = 0;
        std::vector<VitaStringTableRecord> parsed;
        parsed.reserve(recordCount);
        bool valid = true;
        for (uint32_t recordIndex = 0; recordIndex < recordCount; ++recordIndex) {
            if (offset > data.size() || data.size() - offset < kRecordHeaderBytes) {
                valid = false;
                break;
            }
            const uint32_t nameBytes = ReadU32(data.data() + offset);
            const uint32_t rowCount = ReadU32(data.data() + offset + 4);
            const uint32_t columnCount = ReadU32(data.data() + offset + 8);
            offset += kRecordHeaderBytes;
            if (nameBytes == 0 || nameBytes > kMaxStringBytes
                || rowCount > kMaxRows || columnCount > kMaxColumns
                || (columnCount != 0
                    && rowCount > kMaxCells / columnCount)) {
                valid = false;
                break;
            }
            VitaStringTableRecord record;
            if (!ReadString(data, &offset, nameBytes, &record.name)) {
                valid = false;
                break;
            }
            record.rowCount = rowCount;
            record.columnCount = columnCount;
            const uint64_t cellCount = static_cast<uint64_t>(rowCount) * columnCount;
            record.values.reserve(static_cast<size_t>(cellCount));
            for (uint64_t cell = 0; cell < cellCount; ++cell) {
                if (offset > data.size() || data.size() - offset < sizeof(uint32_t)) {
                    valid = false;
                    break;
                }
                const uint32_t cellBytes = ReadU32(data.data() + offset);
                offset += sizeof(uint32_t);
                std::string value;
                if (!ReadString(data, &offset, cellBytes, &value)) {
                    valid = false;
                    break;
                }
                record.values.push_back(std::move(value));
            }
            if (!valid)
                break;
            aggregateBytes += kRecordHeaderBytes + nameBytes
                + cellCount * sizeof(uint32_t);
            for (const std::string& value : record.values)
                aggregateBytes += value.size();
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
                  [](const VitaStringTableRecord& left,
                     const VitaStringTableRecord& right) {
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

const VitaStringTableRecord* VitaStringTableBundle::Find(const char* name) const {
    if (!name)
        return nullptr;
    const auto found = std::find_if(records_.begin(), records_.end(),
                                    [name](const VitaStringTableRecord& record) {
                                        return EqualIgnoreCase(record.name, name);
                                    });
    return found == records_.end() ? nullptr : &*found;
}

const VitaStringTableRecord* VitaStringTableBundle::Record(uint32_t index) const {
    return index < records_.size() ? &records_[index] : nullptr;
}

bool VitaStringTableBundle::Lookup(const char* name, int comparisonColumn,
                                   const char* value, int valueColumn,
                                   std::string* result) const {
    if (!result || !value)
        return false;
    const VitaStringTableRecord* table = Find(name);
    if (!table || comparisonColumn < 0 || valueColumn < 0
        || comparisonColumn >= static_cast<int>(table->columnCount)
        || valueColumn >= static_cast<int>(table->columnCount))
        return false;
    for (int row = static_cast<int>(table->rowCount) - 1; row >= 0; --row) {
        const size_t comparisonIndex = static_cast<size_t>(row)
            * table->columnCount + static_cast<size_t>(comparisonColumn);
        if (comparisonIndex >= table->values.size())
            return false;
        if (!EqualIgnoreCase(table->values[comparisonIndex], value))
            continue;
        const size_t resultIndex = static_cast<size_t>(row)
            * table->columnCount + static_cast<size_t>(valueColumn);
        if (resultIndex >= table->values.size())
            return false;
        *result = table->values[resultIndex];
        return true;
    }
    return false;
}

} // namespace kisak::vita
