#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <vita/vita_asset_db.h>

namespace kisak::vita {

constexpr uint32_t kVitaImpactTypeCount = 12;
constexpr uint32_t kVitaImpactNonFleshCount = 29;
constexpr uint32_t kVitaImpactFleshCount = 4;

struct VitaImpactFxEntryRecord {
    std::array<std::string, kVitaImpactNonFleshCount> nonFlesh;
    std::array<std::string, kVitaImpactFleshCount> flesh;
};

struct VitaImpactFxTableRecord {
    std::string name;
    std::array<VitaImpactFxEntryRecord, kVitaImpactTypeCount> entries;
};

// Pointer-free representation of the source FxImpactTable fastfile asset.
// VIFX stores effect names only; VitaAssetRegistry resolves those names to
// arena-owned FxEffectDef records after the VFXA graph has been rehydrated.
class VitaImpactFxBundle {
public:
    bool Load(const VitaAssetDatabase& database);
    void Clear();

    bool Loaded() const { return loaded_; }
    bool SourceComplete() const;
    uint32_t WireVersion() const { return wireVersion_; }
    uint32_t Count() const { return static_cast<uint32_t>(tables_.size()); }
    const VitaImpactFxTableRecord* Table(uint32_t index) const;
    const VitaImpactFxTableRecord* Find(const char* name) const;

private:
    std::vector<VitaImpactFxTableRecord> tables_;
    bool loaded_ = false;
    uint32_t wireVersion_ = 0;
};

} // namespace kisak::vita
