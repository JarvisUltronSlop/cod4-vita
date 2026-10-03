#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vita/vita_asset_db.h>

namespace kisak::vita {

struct ShipmentSpawnPoint {
    float origin[3] = {0.0f, 0.0f, 0.0f};
    float angles[3] = {0.0f, 0.0f, 0.0f};
};

// Pointer-free map entity data recovered by the Windows exporter.  The native
// GSC bridge only needs the common fields that desktop MP exposes through its
// entity script class; the full text remains available in the package for
// future spawn-function parity.
struct ShipmentMapEntity {
    std::string classname;
    std::string target;
    std::string targetname;
    std::string model;
    std::string scriptLinkName;
    std::string scriptNoteworthy;
    int32_t spawnflags = 0;
    int32_t damage = 0;
    int32_t count = 0;
    int32_t health = 0;
    float origin[3] = {0.0f, 0.0f, 0.0f};
    float angles[3] = {0.0f, 0.0f, 0.0f};
    float modelScale = 1.0f;
    // Static XModels without an authored gndLt still receive the source
    // instance primary-light index from R_CalcModelLighting. Preserve that
    // selection separately from the packed gndLt colour payload so the Vita
    // light-grid sampler can apply the same non-sun light-region override.
    uint8_t primaryLightIndex = 0;
    // KisakCOD's R_DecodeGroundLighting stores this as a packed RGBA model
    // light sample plus the source primary-light index. Keep the bytes
    // pointer-free so static XModels use the authored per-instance lighting
    // on both Vita backends.
    uint32_t groundLighting = 0;
    uint8_t groundPrimaryLightIndex = 0;
    bool hasOrigin = false;
    bool hasAngles = false;
    bool hasModelScale = false;
    bool hasGroundLighting = false;
    bool hasPrimaryLightIndex = false;
    bool hasCount = false;
    bool hasHealth = false;
    // Runtime-only placement for a source FX model element. VFXA model
    // elements carry an oriented quaternion/axis rather than map-entity
    // Euler angles; this field never participates in the pointer-free entity
    // sidecar and is consumed only by the frame draw pass.
    bool hasAxis = false;
    float axis[3][3] = {};
};

class ShipmentMapEntities {
public:
    bool Load(const VitaAssetDatabase& database, uint64_t assetId = 0);
    void Clear();
    bool Loaded() const { return loaded_; }
    uint32_t ByteCount() const { return static_cast<uint32_t>(text_.size()); }
    const std::string& Text() const { return text_; }
    const std::vector<ShipmentMapEntity>& Records() const { return records_; }
    const std::vector<ShipmentSpawnPoint>& DeathmatchSpawns() const { return dmSpawns_; }
    const std::vector<ShipmentSpawnPoint>& TeamSpawns() const { return teamSpawns_; }

private:
    std::string text_;
    std::vector<ShipmentMapEntity> records_;
    std::vector<ShipmentSpawnPoint> dmSpawns_;
    std::vector<ShipmentSpawnPoint> teamSpawns_;
    bool loaded_ = false;
};

} // namespace kisak::vita
