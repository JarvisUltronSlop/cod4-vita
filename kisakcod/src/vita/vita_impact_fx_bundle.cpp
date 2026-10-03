#include <vita/vita_impact_fx_bundle.h>

#include <cstring>
#include <utility>
#include <vector>

namespace kisak::vita {
namespace {

constexpr uint32_t kMagic = 0x58464956; // "VIFX"
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxTables = 4;
constexpr uint32_t kMaxStringBytes = 1024u * 1024u;
constexpr uint32_t kMaxNameBytes = 1024;
constexpr size_t kHeaderBytes = 32;
constexpr size_t kTableRecordBytes = 8;
constexpr uint32_t kSlotsPerEntry = kVitaImpactNonFleshCount
    + kVitaImpactFleshCount;
constexpr uint32_t kSlotsPerTable = kVitaImpactTypeCount * kSlotsPerEntry;

uint32_t ReadU32(const uint8_t* bytes) {
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

bool ReadString(const std::vector<uint8_t>& data, size_t stringBase,
                uint32_t stringBytes, uint32_t offset, bool allowEmpty,
                std::string* output) {
    if (!output || offset >= stringBytes || stringBase > data.size()
        || stringBytes > data.size() - stringBase)
        return false;
    const uint8_t* begin = data.data() + stringBase + offset;
    const size_t remaining = stringBytes - offset;
    size_t length = 0;
    while (length < remaining && begin[length] != 0)
        ++length;
    if (length == remaining || length > kMaxNameBytes
        || (!allowEmpty && length == 0))
        return false;
    output->assign(reinterpret_cast<const char*>(begin), length);
    return true;
}

} // namespace

void VitaImpactFxBundle::Clear() {
    tables_.clear();
    loaded_ = false;
    wireVersion_ = 0;
}

bool VitaImpactFxBundle::Load(const VitaAssetDatabase& database) {
    Clear();
    for (uint32_t chunkIndex = 0; chunkIndex < database.ChunkCount();
         ++chunkIndex) {
        const KvapChunk* chunk = database.Chunk(chunkIndex);
        if (!chunk
            || chunk->type != static_cast<uint32_t>(VitaAssetType::Effects)
            || chunk->rawSize < kHeaderBytes
            || chunk->rawSize > kMaxStringBytes + 65536u)
            continue;
        std::vector<uint8_t> data(static_cast<size_t>(chunk->rawSize));
        if (database.Read(chunk, data.data(), data.size()) != KvapError::Ok
            || ReadU32(data.data()) != kMagic)
            continue;

        const uint32_t version = ReadU32(data.data() + 4);
        const uint32_t tableCount = ReadU32(data.data() + 8);
        const uint32_t entryCount = ReadU32(data.data() + 12);
        const uint32_t nonFleshCount = ReadU32(data.data() + 16);
        const uint32_t fleshCount = ReadU32(data.data() + 20);
        const uint32_t stringBytes = ReadU32(data.data() + 24);
        const uint32_t reserved = ReadU32(data.data() + 28);
        if (version != kVersion || tableCount == 0
            || tableCount > kMaxTables
            || entryCount != kVitaImpactTypeCount
            || nonFleshCount != kVitaImpactNonFleshCount
            || fleshCount != kVitaImpactFleshCount
            || stringBytes == 0 || stringBytes > kMaxStringBytes
            || reserved != 0)
            continue;

        const uint64_t slotCount = static_cast<uint64_t>(tableCount)
            * kSlotsPerTable;
        const uint64_t stringBase64 = kHeaderBytes
            + static_cast<uint64_t>(tableCount) * kTableRecordBytes
            + slotCount * sizeof(uint32_t);
        if (stringBase64 > data.size()
            || stringBytes != data.size() - stringBase64
            || data[static_cast<size_t>(stringBase64)] != 0)
            continue;
        const size_t tableBase = kHeaderBytes;
        const size_t slotBase = tableBase
            + static_cast<size_t>(tableCount) * kTableRecordBytes;
        const size_t stringBase = static_cast<size_t>(stringBase64);

        std::vector<VitaImpactFxTableRecord> tables;
        tables.reserve(tableCount);
        bool valid = true;
        for (uint32_t tableIndex = 0; tableIndex < tableCount && valid;
             ++tableIndex) {
            const uint8_t* tableRecord = data.data() + tableBase
                + static_cast<size_t>(tableIndex) * kTableRecordBytes;
            const uint32_t nameOffset = ReadU32(tableRecord);
            const uint32_t firstSlot = ReadU32(tableRecord + 4);
            if (firstSlot != tableIndex * kSlotsPerTable)
                break;
            VitaImpactFxTableRecord table;
            valid = ReadString(data, stringBase, stringBytes, nameOffset,
                               true, &table.name);
            for (uint32_t prior = 0; prior < tables.size() && valid; ++prior)
                valid = tables[prior].name != table.name;
            for (uint32_t impactType = 0;
                 impactType < kVitaImpactTypeCount && valid; ++impactType) {
                VitaImpactFxEntryRecord& entry = table.entries[impactType];
                const uint32_t entrySlot = firstSlot
                    + impactType * kSlotsPerEntry;
                for (uint32_t index = 0;
                     index < kVitaImpactNonFleshCount && valid; ++index) {
                    const uint32_t offset = ReadU32(
                        data.data() + slotBase
                        + static_cast<size_t>(entrySlot + index)
                            * sizeof(uint32_t));
                    valid = ReadString(data, stringBase, stringBytes, offset,
                                       true, &entry.nonFlesh[index]);
                }
                for (uint32_t index = 0;
                     index < kVitaImpactFleshCount && valid; ++index) {
                    const uint32_t offset = ReadU32(
                        data.data() + slotBase
                        + static_cast<size_t>(entrySlot
                            + kVitaImpactNonFleshCount + index)
                            * sizeof(uint32_t));
                    valid = ReadString(data, stringBase, stringBytes, offset,
                                       true, &entry.flesh[index]);
                }
            }
            if (valid)
                tables.push_back(std::move(table));
        }
        if (!valid || tables.size() != tableCount)
            continue;
        tables_ = std::move(tables);
        loaded_ = true;
        wireVersion_ = version;
        return true;
    }
    return false;
}

bool VitaImpactFxBundle::SourceComplete() const {
    if (!loaded_ || wireVersion_ != kVersion || tables_.empty())
        return false;
    bool hasDefault = false;
    for (const VitaImpactFxTableRecord& table : tables_) {
        hasDefault = hasDefault || table.name.empty();
        // Null slots are authored and consumed defensively by cgame event
        // paths. Empty strings preserve those null pointers exactly.
    }
    return hasDefault;
}

const VitaImpactFxTableRecord* VitaImpactFxBundle::Table(
    uint32_t index) const {
    return index < tables_.size() ? &tables_[index] : nullptr;
}

const VitaImpactFxTableRecord* VitaImpactFxBundle::Find(
    const char* name) const {
    if (!name)
        return nullptr;
    for (const VitaImpactFxTableRecord& table : tables_) {
        if (table.name == name)
            return &table;
    }
    return nullptr;
}

} // namespace kisak::vita
