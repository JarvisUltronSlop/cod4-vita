#pragma once

#include <cstdint>
#include <string>

#include <vita/vita_asset_db.h>

namespace kisak::vita {

enum VitaEnvironmentFlags : uint32_t {
    VITA_ENV_HAS_FOG = 1u << 0,
    VITA_ENV_HAS_VISION = 1u << 1,
    VITA_ENV_HAS_RAIN = 1u << 2,
    VITA_ENV_HAS_LIGHTNING = 1u << 3,
    VITA_ENV_HAS_WATER_FX = 1u << 4,
    VITA_ENV_HAS_CLIENT_FX = 1u << 5,
    VITA_ENV_HAS_GLOW = 1u << 6,
};

// Portable values from the source map's createart/vision/FX closure. This
// contains no native GfxWorld or Material pointers.
struct VitaEnvironmentState {
    bool valid = false;
    uint32_t mapIndex = 0;
    uint32_t flags = 0;
    float fogColor[3] = {0.0f, 0.0f, 0.0f};
    float fogStart = 0.0f;
    float fogDensity = 0.0f;
    float fogHalfway = 0.0f;
    float fogTransition = 0.0f;
    float filmLightTint[3] = {1.0f, 1.0f, 1.0f};
    float filmDarkTint[3] = {1.0f, 1.0f, 1.0f};
    float filmContrast = 1.0f;
    float filmBrightness = 0.0f;
    float filmDesaturation = 0.0f;
    float filmInvert = 0.0f;
    float rainRate = 0.0f;
    float rainSlant = 0.0f;
    float rainCoverage = 0.0f;
    float wetness = 0.0f;
    // Source R_SetGlowInfo values.  The block is kept separate from film so
    // the scalar/native post chains can preserve the desktop pass ordering.
    bool glowEnabled = false;
    float glowRadius0 = 0.0f;
    float glowRadius1 = 0.0f;
    float glowBloomCutoff = 0.0f;
    float glowBloomDesaturation = 0.0f;
    float glowBloomIntensity0 = 0.0f;
    float glowBloomIntensity1 = 0.0f;
    float glowSkyBleedIntensity0 = 0.0f;
    float glowSkyBleedIntensity1 = 0.0f;
    // Source map-script r_specularColorScale, used when R_SetSunConstants
    // builds the authored sun-specular input. V1/V2 sidecars default to 1.
    float specularColorScale = 1.0f;
    uint32_t effectCount = 0;
    std::string visionName;
    std::string weatherEffect;
};

bool LoadVitaEnvironment(const VitaAssetDatabase& database, uint64_t assetId,
                         VitaEnvironmentState* output);

} // namespace kisak::vita
