#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <gfx/gfx_backend.h>
#include <vita/vita_asset_db.h>

namespace kisak::vita {

enum VitaEffectPlacementFlags : uint32_t {
    VITA_EFFECT_ONESHOT = 1u << 0,
    VITA_EFFECT_LOOP = 1u << 1,
    VITA_EFFECT_EXPLODER = 1u << 2,
};

// Coarse visual families are retained for v1 compatibility and for effects
// whose source graph has not been exported yet. VFXA v2 prefers the exact
// element type and material state carried by VitaEffectElement.
enum class VitaEffectVisualClass : uint8_t {
    Other = 0,
    Smoke,
    Dust,
    Fire,
    Paper,
    Water,
    Foliage,
    Light,
    Weather,
};

// VFXA is deliberately made of fixed-width records plus a bounded string
// table. These runtime values never expose a raw GSC entity, XAsset, or
// desktop pointer to the renderer.
struct VitaEffectVisualSample {
    uint8_t baseColor[4] = {0, 0, 0, 0};
    float baseRotationDelta = 0.0f;
    float baseRotationTotal = 0.0f;
    float baseSize[2] = {0.0f, 0.0f};
    float baseScale = 0.0f;
    uint8_t amplitudeColor[4] = {0, 0, 0, 0};
    float amplitudeRotationDelta = 0.0f;
    float amplitudeRotationTotal = 0.0f;
    float amplitudeSize[2] = {0.0f, 0.0f};
    float amplitudeScale = 0.0f;
};

struct VitaEffectElement {
    uint32_t flags = 0;
    uint32_t elemType = 0;
    uint32_t visualCount = 0;
    uint32_t velIntervalCount = 0;
    uint32_t visStateIntervalCount = 0;
    float spawnRange[2] = {0.0f, 0.0f};
    float fadeInRange[2] = {0.0f, 0.0f};
    float fadeOutRange[2] = {0.0f, 0.0f};
    float spawnFrustumCullRadius = 0.0f;
    int32_t spawnDelayMsec[2] = {0, 0};
    int32_t lifeSpanMsec[2] = {0, 0};
    float spawnOrigin[3][2] = {{0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}};
    float spawnOffsetRadius[2] = {0.0f, 0.0f};
    float spawnOffsetHeight[2] = {0.0f, 0.0f};
    float spawnAngles[3][2] = {{0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}};
    float angularVelocity[3][2] = {{0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}};
    float initialRotation[2] = {0.0f, 0.0f};
    float gravity[2] = {0.0f, 0.0f};
    float reflectionFactor[2] = {0.0f, 0.0f};
    uint8_t atlasBehavior = 0;
    uint8_t atlasIndex = 0;
    uint8_t atlasFps = 0;
    uint8_t atlasLoopCount = 0;
    uint8_t atlasColIndexBits = 0;
    uint8_t atlasRowIndexBits = 0;
    int32_t atlasEntryCount = 0;
    float collMins[3] = {0.0f, 0.0f, 0.0f};
    float collMaxs[3] = {0.0f, 0.0f, 0.0f};
    float emitDist[2] = {0.0f, 0.0f};
    float emitDistVariance[2] = {0.0f, 0.0f};
    int32_t spawnDef[2] = {0, 0};
    uint32_t sortOrder = 0;
    uint32_t useItemClip = 0;
    float lightingFrac = 0.0f;
    std::string effectOnImpact;
    std::string effectOnDeath;
    std::string effectEmitted;
    std::vector<std::string> visualNames;
    std::vector<std::array<float, 24>> velocitySamples;
    std::vector<VitaEffectVisualSample> visualSamples;
    std::vector<std::array<float, 5>> trailVertices;
    std::vector<uint16_t> trailIndices;
    int32_t trailScrollTimeMsec = 0;
    int32_t trailRepeatDist = 0;
    int32_t trailSplitDist = 0;
    bool trailPresent = false;
};

struct VitaEffectRecord {
    std::string name;
    std::string path;
    bool graphPresent = false;
    uint32_t flags = 0;
    int32_t msecLoopingLife = 0;
    uint32_t elemDefCountLooping = 0;
    uint32_t elemDefCountOneShot = 0;
    uint32_t elemDefCountEmission = 0;
    std::vector<VitaEffectElement> elements;
};

struct VitaEffectPlacement {
    uint32_t effectIndex = 0;
    uint32_t flags = 0;
    float origin[3] = {0.0f, 0.0f, 0.0f};
    float angles[3] = {0.0f, 0.0f, 0.0f};
    float delay = 0.0f;
};

struct VitaLoopSoundPlacement {
    std::string soundAlias;
    float origin[3] = {0.0f, 0.0f, 0.0f};
    float angles[3] = {0.0f, 0.0f, 0.0f};
};

class VitaEffectBundle {
public:
    bool Load(const VitaAssetDatabase& database, uint64_t assetId = 0);
    void Clear();

    bool Loaded() const { return loaded_; }
    uint32_t WireVersion() const { return wireVersion_; }
    bool SourceComplete() const;
    uint32_t MapIndex() const { return mapIndex_; }
    uint32_t Count() const {
        return static_cast<uint32_t>(effects_.size());
    }
    uint32_t PlacementCount() const {
        return static_cast<uint32_t>(placements_.size());
    }
    uint32_t LoopSoundCount() const {
        return static_cast<uint32_t>(loopSounds_.size());
    }
    const VitaEffectRecord* Record(uint32_t index) const;
    const VitaEffectPlacement* Placement(uint32_t index) const;
    const VitaLoopSoundPlacement* LoopSound(uint32_t index) const;
    const VitaEffectRecord* Find(const char* name) const;
    // Runner elements serialize a child effect path while older map scripts
    // usually expose an alias/name. Resolve both forms without handing a
    // native pointer to the renderer; UINT32_MAX means the source reference
    // is not present in this bounded map closure.
    uint32_t FindReferenceIndex(const char* reference) const;
    VitaEffectVisualClass VisualClass(uint32_t index) const;

    // Rain is a normal authored loadfx/createfx effect, not a synthetic map
    // tint. These helpers keep the renderer-facing weather decision tied to
    // the portable VFXA records while retaining a zero-valued compatibility
    // result for packages that predate the sidecar.
    const VitaEffectRecord* WeatherEffect() const;
    uint32_t WeatherPlacementCount() const;
    const VitaEffectPlacement* WeatherPlacement(uint32_t ordinal) const;
    uint32_t WeatherSeed() const;

private:
    uint32_t mapIndex_ = 0;
    std::vector<VitaEffectRecord> effects_;
    std::vector<VitaEffectPlacement> placements_;
    std::vector<VitaLoopSoundPlacement> loopSounds_;
    bool loaded_ = false;
    uint32_t wireVersion_ = 0;
};

} // namespace kisak::vita
