#include <vita/vita_menu_bundle.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

namespace kisak::vita {
namespace {

constexpr uint32_t kMagic = 0x4D544956; // "VITM"
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxMenus = 1024;
constexpr uint32_t kMaxItemsPerMenu = 256;
constexpr uint32_t kMaxStringBytes = 4096;
constexpr uint64_t kMaxAggregateBytes = 16ull * 1024ull * 1024ull;
constexpr size_t kHeaderBytes = 20;
// 16-byte size/name/item header + style + five vec4 fields.
constexpr size_t kMenuFixedBytes = 100;
constexpr size_t kItemFixedBytes = 68;

uint32_t ReadU32(const uint8_t* bytes) {
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

int32_t ReadI32(const uint8_t* bytes) {
    int32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

float ReadF32(const uint8_t* bytes) {
    float value = 0.0f;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

bool ReadString(const std::vector<uint8_t>& data, size_t* offset,
                uint32_t length, std::string* value) {
    if (!offset || !value || length > kMaxStringBytes
        || *offset > data.size()
        || length > data.size() - *offset)
        return false;
    value->assign(reinterpret_cast<const char*>(data.data() + *offset), length);
    *offset += length;
    return true;
}

bool ReadFloats(const std::vector<uint8_t>& data, size_t* offset, float* values,
                size_t count) {
    if (!offset || !values || *offset > data.size()
        || count > (data.size() - *offset) / sizeof(float))
        return false;
    for (size_t index = 0; index < count; ++index) {
        values[index] = ReadF32(data.data() + *offset);
        *offset += sizeof(float);
    }
    return true;
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

bool IsRuntimeMenu(const std::string& name) {
    // The Vita route uses the authored menu graph after the map loads as well
    // as at boot. Keep every VITM record so changemap, pause, options, and
    // create-a-class retain the retail command/layout relationship.
    (void)name;
    return true;
}

} // namespace

void VitaMenuBundle::Clear() {
#if KISAK_VITA_CPU_RUNTIME
    catalogKey_={};
#endif
    menus_.clear();
    loaded_ = false;
}

bool VitaMenuBundle::Load(const VitaAssetDatabase& database) {
#if KISAK_VITA_CPU_RUNTIME
    VitaCpuCatalogKey key;key.Capture(database,VitaAssetType::Menu);
    if(loaded_ && key==catalogKey_)return true;
#endif
    Clear();
    for (uint32_t index = 0; index < database.ChunkCount(); ++index) {
        const KvapChunk* chunk = database.Chunk(index);
        if (!chunk || chunk->type != static_cast<uint32_t>(VitaAssetType::Menu)
            || chunk->rawSize < kHeaderBytes || chunk->rawSize > kMaxAggregateBytes)
            continue;
        std::vector<uint8_t> data(static_cast<size_t>(chunk->rawSize));
        if (database.Read(chunk, data.data(), data.size()) != KvapError::Ok
            || ReadU32(data.data()) != kMagic || ReadU32(data.data() + 4) != kVersion)
            continue;
        const uint32_t menuCount = ReadU32(data.data() + 8);
        const uint64_t declaredBytes = [&data]() {
            uint64_t value = 0;
            std::memcpy(&value, data.data() + 12, sizeof(value));
            return value;
        }();
        if (menuCount > kMaxMenus || declaredBytes > kMaxAggregateBytes)
            continue;

        size_t offset = kHeaderBytes;
        uint64_t aggregateBytes = 0;
        std::vector<VitaMenuRecord> parsed;
        parsed.reserve(menuCount);
        bool valid = true;
        for (uint32_t menuIndex = 0; menuIndex < menuCount; ++menuIndex) {
            if (offset > data.size() || data.size() - offset < kMenuFixedBytes) {
                valid = false;
                break;
            }
            const uint32_t nameBytes = ReadU32(data.data() + offset);
            const uint32_t fontBytes = ReadU32(data.data() + offset + 4);
            const uint32_t itemCount = ReadU32(data.data() + offset + 8);
            const uint32_t fullScreen = ReadU32(data.data() + offset + 12);
            offset += 16;
            VitaMenuRecord menu;
            menu.fullScreen = fullScreen;
            menu.style = ReadI32(data.data() + offset);
            offset += sizeof(int32_t);
            if (!ReadFloats(data, &offset, menu.rect, 4)
                || !ReadFloats(data, &offset, menu.foreColor, 4)
                || !ReadFloats(data, &offset, menu.backColor, 4)
                || !ReadFloats(data, &offset, menu.focusColor, 4)
                || !ReadFloats(data, &offset, menu.disableColor, 4)
                || !ReadString(data, &offset, nameBytes, &menu.name)
                || !ReadString(data, &offset, fontBytes, &menu.font)
                || itemCount > kMaxItemsPerMenu) {
                valid = false;
                break;
            }
            const bool keepMenu = IsRuntimeMenu(menu.name);
            if (keepMenu)
                menu.items.reserve(itemCount);
            for (uint32_t itemIndex = 0; itemIndex < itemCount; ++itemIndex) {
                if (offset > data.size() || data.size() - offset < kItemFixedBytes) {
                    valid = false;
                    break;
                }
                if (!keepMenu) {
                    const uint32_t itemNameBytes = ReadU32(data.data() + offset);
                    const uint32_t textBytes = ReadU32(data.data() + offset + 4);
                    const uint32_t actionBytes = ReadU32(data.data() + offset + 8);
                    const uint32_t acceptBytes = ReadU32(data.data() + offset + 12);
                    const uint64_t itemBytes = static_cast<uint64_t>(itemNameBytes)
                        + textBytes + actionBytes + acceptBytes;
                    offset += kItemFixedBytes;
                    if (itemNameBytes > kMaxStringBytes || textBytes > kMaxStringBytes
                        || actionBytes > kMaxStringBytes || acceptBytes > kMaxStringBytes
                        || itemBytes > data.size() - offset
                        || itemBytes > kMaxStringBytes * 4ull) {
                        valid = false;
                        break;
                    }
                    offset += static_cast<size_t>(itemBytes);
                    continue;
                }
                const uint32_t itemNameBytes = ReadU32(data.data() + offset);
                const uint32_t textBytes = ReadU32(data.data() + offset + 4);
                const uint32_t actionBytes = ReadU32(data.data() + offset + 8);
                const uint32_t acceptBytes = ReadU32(data.data() + offset + 12);
                offset += 16;
                VitaMenuItem item;
                item.type = ReadI32(data.data() + offset);
                item.alignment = ReadI32(data.data() + offset + 4);
                item.textStyle = ReadI32(data.data() + offset + 8);
                item.flags = ReadU32(data.data() + offset + 12);
                offset += 16;
                if (!ReadFloats(data, &offset, item.rect, 4)
                    || !ReadFloats(data, &offset, item.textRect, 4)) {
                    valid = false;
                    break;
                }
                if (offset > data.size() || data.size() - offset < sizeof(float)) {
                    valid = false;
                    break;
                }
                item.textScale = ReadF32(data.data() + offset);
                offset += sizeof(float);
                if (!ReadString(data, &offset, itemNameBytes, &item.name)
                    || !ReadString(data, &offset, textBytes, &item.text)
                    || !ReadString(data, &offset, actionBytes, &item.action)
                    || !ReadString(data, &offset, acceptBytes, &item.onAccept)) {
                    valid = false;
                    break;
                }
                menu.items.push_back(std::move(item));
            }
            if (!valid)
                break;
            if (keepMenu)
                parsed.push_back(std::move(menu));
        }
        aggregateBytes = offset >= kHeaderBytes ? offset - kHeaderBytes : 0;
        if (!valid || offset != data.size() || aggregateBytes != declaredBytes)
            continue;
        menus_ = std::move(parsed);
        std::sort(menus_.begin(), menus_.end(),
                  [](const VitaMenuRecord& left, const VitaMenuRecord& right) {
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

const VitaMenuRecord* VitaMenuBundle::Find(const char* name) const {
    if (!name)
        return nullptr;
    const auto found = std::find_if(menus_.begin(), menus_.end(),
                                    [name](const VitaMenuRecord& menu) {
                                        return EqualIgnoreCase(menu.name, name);
                                    });
    return found == menus_.end() ? nullptr : &*found;
}

const VitaMenuItem* VitaMenuBundle::FindItem(const char* menuName,
                                              const char* itemName) const {
    const VitaMenuRecord* menu = Find(menuName);
    if (!menu || !itemName)
        return nullptr;
    const auto found = std::find_if(menu->items.begin(), menu->items.end(),
                                    [itemName](const VitaMenuItem& item) {
                                        return item.name == itemName;
                                    });
    return found == menu->items.end() ? nullptr : &*found;
}

const VitaMenuRecord* VitaMenuBundle::Record(uint32_t index) const {
    return index < menus_.size() ? &menus_[index] : nullptr;
}

} // namespace kisak::vita
