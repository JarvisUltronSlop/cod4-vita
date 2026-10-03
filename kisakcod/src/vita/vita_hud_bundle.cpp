#include <vita/vita_hud_bundle.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace kisak::vita {
namespace {

constexpr uint32_t kMagic = 0x48544956; // "VITH"
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxRecords = 64;
constexpr uint32_t kMaxRoleBytes = 256;
constexpr uint32_t kMaxImageBytes = 4096;
constexpr uint64_t kMaxAggregateBytes = 64ull * 1024ull;
constexpr size_t kHeaderBytes = 20;

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
                uint32_t length, uint32_t maximum, std::string* value) {
    if (!offset || !value || length == 0 || length > maximum
        || *offset > data.size() || length > data.size() - *offset)
        return false;
    value->assign(reinterpret_cast<const char*>(data.data() + *offset), length);
    *offset += length;
    return value->find('\0') == std::string::npos;
}

} // namespace

void VitaHudBundle::Clear() {
    bindings_.clear();
    loaded_ = false;
}

bool VitaHudBundle::Load(const VitaAssetDatabase& database, uint64_t assetId) {
    Clear();
    const KvapChunk* requested = assetId
        ? database.Find(VitaAssetType::Hud, assetId) : nullptr;
    if (assetId && !requested)
        return false;
    for (uint32_t index = 0; index < database.ChunkCount(); ++index) {
        const KvapChunk* chunk = database.Chunk(index);
        if (!chunk || chunk->type != static_cast<uint32_t>(VitaAssetType::Hud)
            || (assetId && chunk != requested)
            || chunk->rawSize < kHeaderBytes || chunk->rawSize > kMaxAggregateBytes)
            continue;
        std::vector<uint8_t> data(static_cast<size_t>(chunk->rawSize));
        if (database.Read(chunk, data.data(), data.size()) != KvapError::Ok
            || ReadU32(data.data()) != kMagic || ReadU32(data.data() + 4) != kVersion)
            continue;
        const uint32_t recordCount = ReadU32(data.data() + 8);
        const uint64_t declaredBytes = ReadU64(data.data() + 12);
        if (recordCount == 0 || recordCount > kMaxRecords
            || declaredBytes > kMaxAggregateBytes)
            continue;
        size_t offset = kHeaderBytes;
        uint64_t aggregateBytes = 0;
        std::vector<VitaHudBinding> parsed;
        parsed.reserve(recordCount);
        bool valid = true;
        for (uint32_t recordIndex = 0; recordIndex < recordCount; ++recordIndex) {
            if (offset > data.size() || data.size() - offset < 8) {
                valid = false;
                break;
            }
            const uint32_t roleBytes = ReadU32(data.data() + offset);
            const uint32_t imageBytes = ReadU32(data.data() + offset + 4);
            offset += 8;
            VitaHudBinding binding;
            if (!ReadString(data, &offset, roleBytes, kMaxRoleBytes, &binding.role)
                || !ReadString(data, &offset, imageBytes, kMaxImageBytes, &binding.image)) {
                valid = false;
                break;
            }
            aggregateBytes += 8ull + roleBytes + imageBytes;
            if (aggregateBytes > kMaxAggregateBytes)
                valid = false;
            parsed.push_back(std::move(binding));
            if (!valid)
                break;
        }
        if (!valid || offset != data.size() || aggregateBytes != declaredBytes)
            continue;
        std::sort(parsed.begin(), parsed.end(),
                  [](const VitaHudBinding& left, const VitaHudBinding& right) {
                      return left.role < right.role;
                  });
        for (size_t bindingIndex = 1; bindingIndex < parsed.size(); ++bindingIndex) {
            if (parsed[bindingIndex - 1].role == parsed[bindingIndex].role) {
                valid = false;
                break;
            }
        }
        if (!valid)
            continue;
        bindings_ = std::move(parsed);
        loaded_ = true;
        return true;
    }
    return false;
}

const VitaHudBinding* VitaHudBundle::Find(const char* role) const {
    if (!role)
        return nullptr;
    const auto found = std::find_if(bindings_.begin(), bindings_.end(),
                                    [role](const VitaHudBinding& binding) {
                                        return binding.role == role;
                                    });
    return found == bindings_.end() ? nullptr : &*found;
}

const char* VitaHudBundle::ImageFor(const char* role) const {
    const VitaHudBinding* binding = Find(role);
    return binding ? binding->image.c_str() : nullptr;
}

} // namespace kisak::vita
