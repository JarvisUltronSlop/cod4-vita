#pragma once

#include <cstdint>
#include <vector>

#include <vita/vita_asset_db.h>
#include <vita/vita_weapon_record.h>

namespace kisak::vita {

using ShipmentWeaponDefinition = VitaWeaponRecordV5;

class ShipmentWeaponTable {
public:
    bool Load(const VitaAssetDatabase& database);
    void Clear();
    bool Loaded() const { return loaded_; }
    bool SourceComplete() const;
    uint32_t WireVersion() const { return wireVersion_; }
    uint32_t Count() const { return static_cast<uint32_t>(records_.size()); }
    const ShipmentWeaponDefinition* Default() const;
    const ShipmentWeaponDefinition* Find(const char* name) const;
    const ShipmentWeaponDefinition* ForSlot(uint32_t slot) const;
    const std::vector<ShipmentWeaponDefinition>& Records() const { return records_; }

private:
    std::vector<ShipmentWeaponDefinition> records_;
    bool loaded_ = false;
    uint32_t wireVersion_ = 0;
};

} // namespace kisak::vita
