#include <vita/vita_shipment_weapons.h>
#include <vita/vita_slice_capabilities.h>

#include <cmath>
#include <cstring>

namespace kisak::vita {
namespace {

constexpr uint32_t kMagic = 0x50544956; // "VITP"
constexpr uint32_t kVersionV1 = 1;
constexpr uint32_t kVersionV2 = 2;
constexpr uint32_t kVersionV3 = 3;
constexpr uint32_t kVersionV4 = 4;
constexpr uint32_t kVersionV5 = kVitaWeaponVersionSource;
constexpr uint32_t kRecordBytesV1 = 160;
constexpr uint32_t kRecordBytesV2 = 244;

#pragma pack(push, 1)
struct VitaWeaponRecordV2 {
    char name[64];
    uint32_t weaponType;
    uint32_t weaponClass;
    uint32_t fireType;
    uint32_t inventoryType;
    uint32_t stance;
    int32_t startAmmo;
    int32_t maxAmmo;
    int32_t clipSize;
    int32_t damage;
    int32_t playerDamage;
    int32_t fireDelay;
    int32_t fireTime;
    int32_t rechamberTime;
    int32_t reloadTime;
    int32_t reloadEmptyTime;
    int32_t adsTransInTime;
    int32_t adsTransOutTime;
    float moveSpeedScale;
    float adsMoveSpeedScale;
    float sprintDurationScale;
    float adsZoomFov;
    float hipSpreadStandMin;
    float hipSpreadStandMax;
    float adsSwayMaxAngle;
    char viewModelName[64];
    uint64_t hideSurfaceMask;
    uint32_t viewModelSurfaceCount;
    uint32_t hideTagCount;
    uint32_t hideTagResolvedCount;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct VitaWeaponRecordV3 {
    char name[64];
    uint32_t weaponType;
    uint32_t weaponClass;
    uint32_t fireType;
    uint32_t inventoryType;
    uint32_t stance;
    int32_t startAmmo;
    int32_t maxAmmo;
    int32_t clipSize;
    int32_t damage;
    int32_t playerDamage;
    int32_t fireDelay;
    int32_t fireTime;
    int32_t rechamberTime;
    int32_t reloadTime;
    int32_t reloadEmptyTime;
    int32_t adsTransInTime;
    int32_t adsTransOutTime;
    float moveSpeedScale;
    float adsMoveSpeedScale;
    float sprintDurationScale;
    float adsZoomFov;
    float hipSpreadStandMin;
    float hipSpreadStandMax;
    float adsSwayMaxAngle;
    char viewModelName[64];
    uint64_t hideSurfaceMask;
    uint32_t viewModelSurfaceCount;
    uint32_t hideTagCount;
    uint32_t hideTagResolvedCount;
    char viewAnimationNames[33][64];
};
#pragma pack(pop)

#pragma pack(push, 1)
struct VitaWeaponRecordV4 {
    char name[64];
    uint32_t weaponType;
    uint32_t weaponClass;
    uint32_t fireType;
    uint32_t inventoryType;
    uint32_t stance;
    int32_t startAmmo;
    int32_t maxAmmo;
    int32_t clipSize;
    int32_t damage;
    int32_t playerDamage;
    int32_t fireDelay;
    int32_t fireTime;
    int32_t rechamberTime;
    int32_t reloadTime;
    int32_t reloadEmptyTime;
    int32_t adsTransInTime;
    int32_t adsTransOutTime;
    float moveSpeedScale;
    float adsMoveSpeedScale;
    float sprintDurationScale;
    float adsZoomFov;
    float hipSpreadStandMin;
    float hipSpreadStandMax;
    float adsSwayMaxAngle;
    char viewModelName[64];
    uint64_t hideSurfaceMask;
    uint32_t viewModelSurfaceCount;
    uint32_t hideTagCount;
    uint32_t hideTagResolvedCount;
    char viewAnimationNames[33][64];
    int32_t freezeMovementWhenFiring;
};
#pragma pack(pop)

// The first 160 bytes are the original VITP v1 payload. Keep a separate
// packed prefix so old reference packages remain readable after the v2 tail
// adds source-resolved viewmodel visibility metadata.
#pragma pack(push, 1)
struct VitaWeaponRecordV1 {
    char name[64];
    uint32_t weaponType;
    uint32_t weaponClass;
    uint32_t fireType;
    uint32_t inventoryType;
    uint32_t stance;
    int32_t startAmmo;
    int32_t maxAmmo;
    int32_t clipSize;
    int32_t damage;
    int32_t playerDamage;
    int32_t fireDelay;
    int32_t fireTime;
    int32_t rechamberTime;
    int32_t reloadTime;
    int32_t reloadEmptyTime;
    int32_t adsTransInTime;
    int32_t adsTransOutTime;
    float moveSpeedScale;
    float adsMoveSpeedScale;
    float sprintDurationScale;
    float adsZoomFov;
    float hipSpreadStandMin;
    float hipSpreadStandMax;
    float adsSwayMaxAngle;
};
#pragma pack(pop)

static_assert(sizeof(VitaWeaponRecordV1) == kRecordBytesV1);
static_assert(sizeof(VitaWeaponRecordV2) == kRecordBytesV2);
static_assert(sizeof(VitaWeaponRecordV3) == 2356);
static_assert(sizeof(VitaWeaponRecordV4) == 2360);
static_assert(sizeof(ShipmentWeaponDefinition) == kVitaWeaponRecordV5Bytes);

ShipmentWeaponDefinition Convert(const VitaWeaponRecordV2& source) {
    ShipmentWeaponDefinition destination{};
    std::memcpy(&destination, &source, sizeof(source));
    return destination;
}

ShipmentWeaponDefinition Convert(const VitaWeaponRecordV3& source) {
    ShipmentWeaponDefinition destination{};
    std::memcpy(&destination, &source, sizeof(source));
    return destination;
}

ShipmentWeaponDefinition Convert(const VitaWeaponRecordV4& source) {
    ShipmentWeaponDefinition destination{};
    std::memcpy(&destination, &source, sizeof(source));
    return destination;
}

ShipmentWeaponDefinition Convert(const VitaWeaponRecordV1& source) {
    ShipmentWeaponDefinition destination{};
    std::memcpy(&destination, &source, sizeof(source));
    return destination;
}

ShipmentWeaponDefinition Convert(const VitaWeaponRecordV5& source) {
    return source;
}

template <size_t N>
bool IsTerminated(const char (&text)[N]) {
    return std::memchr(text, '\0', N) != nullptr;
}

template <size_t Count, size_t Width>
bool AreTerminated(const char (&text)[Count][Width]) {
    for (size_t index = 0; index < Count; ++index) {
        if (!IsTerminated(text[index]))
            return false;
    }
    return true;
}

bool IsFiniteArray(const float* values, size_t count) {
    if (!values)
        return false;
    for (size_t index = 0; index < count; ++index) {
        if (!std::isfinite(values[index]))
            return false;
    }
    return true;
}

bool ValidateSourceRecord(const VitaWeaponRecordV5& record) {
    if (!record.SourceComplete() || !record.name[0]
        || !IsTerminated(record.name) || !IsTerminated(record.viewModelName)
        || !AreTerminated(record.viewAnimationNames)
        || !AreTerminated(record.stringNames)
        || !AreTerminated(record.gunModelNames)
        || !IsTerminated(record.handModelName)
        || !AreTerminated(record.worldModelNames)
        || !IsTerminated(record.worldClipModelName)
        || !IsTerminated(record.rocketModelName)
        || !IsTerminated(record.knifeModelName)
        || !IsTerminated(record.worldKnifeModelName)
        || !IsTerminated(record.projectileModelName)
        || !AreTerminated(record.hideTagNames)
        || !AreTerminated(record.notetrackSoundKeyNames)
        || !AreTerminated(record.notetrackSoundValueNames)
        || !AreTerminated(record.materialNames)
        || !AreTerminated(record.effectNames)
        || !AreTerminated(record.soundNames)
        || !AreTerminated(record.bounceSoundNames)
        || !AreTerminated(record.accuracyGraphNames))
        return false;

    const auto integer = [&record](VitaWeaponIntField field) {
        return record.integerFields[static_cast<uint32_t>(field)];
    };
    if (record.weaponType != static_cast<uint32_t>(
            integer(VitaWeaponIntField::WeaponType))
        || record.weaponClass != static_cast<uint32_t>(
            integer(VitaWeaponIntField::WeaponClass))
        || record.fireType != static_cast<uint32_t>(
            integer(VitaWeaponIntField::FireType))
        || record.inventoryType != static_cast<uint32_t>(
            integer(VitaWeaponIntField::InventoryType))
        || record.stance != static_cast<uint32_t>(
            integer(VitaWeaponIntField::Stance)))
        return false;

    return IsFiniteArray(record.floatFields,
                         static_cast<uint32_t>(VitaWeaponFloatField::Count))
        && IsFiniteArray(&record.movementVectors[0][0], 8u * 3u)
        && IsFiniteArray(record.parallelBounce, kVitaWeaponBounceCurveCount)
        && IsFiniteArray(record.perpendicularBounce,
                         kVitaWeaponBounceCurveCount)
        && IsFiniteArray(record.projectileColor, 3)
        && IsFiniteArray(record.locationDamageMultipliers,
                         kVitaWeaponLocationDamageCount);
}

} // namespace

void ShipmentWeaponTable::Clear() {
    records_.clear();
    loaded_ = false;
    wireVersion_ = 0;
}

bool ShipmentWeaponTable::Load(const VitaAssetDatabase& database) {
    Clear();
    for (uint32_t index = 0; index < database.ChunkCount(); ++index) {
        const auto* chunk = database.Chunk(index);
        if (!chunk || chunk->type != static_cast<uint32_t>(VitaAssetType::Weapon)
            || chunk->rawSize < 16
            || chunk->rawSize > 16 + 256 * kVitaWeaponRecordV5Bytes)
            continue;
        std::vector<uint8_t> bytes(static_cast<size_t>(chunk->rawSize));
        if (database.Read(chunk, bytes.data(), bytes.size()) != KvapError::Ok)
            continue;
        uint32_t header[4]{};
        std::memcpy(header, bytes.data(), sizeof(header));
    const bool versionV1 = header[0] == kMagic && header[1] == kVersionV1
            && header[3] == kRecordBytesV1;
    const bool versionV2 = header[0] == kMagic && header[1] == kVersionV2
            && header[3] == kRecordBytesV2;
        const bool versionV3 = header[0] == kMagic && header[1] == kVersionV3
            && header[3] == sizeof(VitaWeaponRecordV3);
        const bool versionV4 = header[0] == kMagic && header[1] == kVersionV4
            && header[3] == sizeof(VitaWeaponRecordV4);
        const bool versionV5 = header[0] == kMagic && header[1] == kVersionV5
            && header[3] == kVitaWeaponRecordV5Bytes;
        if ((!versionV1 && !versionV2 && !versionV3 && !versionV4
             && !versionV5) || header[2] == 0
            || 16ull + static_cast<uint64_t>(header[2]) * header[3] != bytes.size())
            continue;
        records_.reserve(header[2]);
        const uint8_t* cursor = bytes.data() + 16;
        for (uint32_t recordIndex = 0; recordIndex < header[2]; ++recordIndex) {
            if (versionV5) {
                VitaWeaponRecordV5 source{};
                std::memcpy(&source, cursor, sizeof(source));
                if (!ValidateSourceRecord(source)) {
                    records_.clear();
                    break;
                }
                records_.push_back(Convert(source));
            } else if (versionV4) {
                VitaWeaponRecordV4 source{};
                std::memcpy(&source, cursor, sizeof(source));
                records_.push_back(Convert(source));
            } else if (versionV3) {
                VitaWeaponRecordV3 source{};
                std::memcpy(&source, cursor, sizeof(source));
                records_.push_back(Convert(source));
            } else if (versionV2) {
                VitaWeaponRecordV2 source{};
                std::memcpy(&source, cursor, sizeof(source));
                records_.push_back(Convert(source));
            } else {
                VitaWeaponRecordV1 source{};
                std::memcpy(&source, cursor, sizeof(source));
                records_.push_back(Convert(source));
            }
            cursor += header[3];
        }
        if (!records_.empty()) {
            loaded_ = true;
            wireVersion_ = header[1];
            return true;
        }
    }
    return false;
}

const ShipmentWeaponDefinition* ShipmentWeaponTable::Default() const {
    if (records_.empty())
        return nullptr;
    for (const auto& record : records_) {
        if (std::strcmp(record.name, "none") == 0)
            return &record;
    }
    if (const auto* mp5 = Find("mp5_mp"))
        return mp5;
    return &records_.front();
}

const ShipmentWeaponDefinition* ShipmentWeaponTable::Find(
    const char* name) const {
    if (!name || !name[0])
        return nullptr;
    for (const auto& record : records_) {
        if (std::strcmp(record.name, name) == 0)
            return &record;
    }
    return nullptr;
}

bool ShipmentWeaponTable::SourceComplete() const {
    if (!loaded_ || wireVersion_ != kVitaWeaponVersionSource)
        return false;
    for (const char* name : kVitaSourceWeaponNames) {
        const ShipmentWeaponDefinition* record = Find(name);
        if (!record || !record->SourceComplete())
            return false;
    }
    return true;
}

const ShipmentWeaponDefinition* ShipmentWeaponTable::ForSlot(uint32_t slot) const {
    static constexpr const char* kNames[] = {"m4_mp", "mp5_mp", "usp_mp"};
    if (slot >= 1 && slot <= 3) {
        const char* wanted = kNames[slot - 1];
        for (const auto& record : records_) {
            if (std::strcmp(record.name, wanted) == 0)
                return &record;
        }
        // The scalar exporter may use the single-player-neutral aliases for
        // the first three records; accept those only for their matching slot.
        static constexpr const char* kAliases[] = {"m4", "mp5", "usp"};
        for (const auto& record : records_) {
            if (std::strcmp(record.name, kAliases[slot - 1]) == 0)
                return &record;
        }
    }
    return Default();
}

} // namespace kisak::vita
