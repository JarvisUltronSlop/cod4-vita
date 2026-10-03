#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>

namespace kisak::vita {
enum class VitaWorldResolution : uint8_t { Native, Balanced, Performance };
struct VitaRenderSettings {
    VitaWorldResolution resolution = VitaWorldResolution::Balanced;
    bool balancedGrass = true;
    bool adaptiveLighting = true;
#if KISAK_VITA_CPU_RUNTIME
    bool simpleWorldLighting = true; // CPU candidate: appearance awaits hardware review.
#else
    bool simpleWorldLighting = false;
#endif
    uint32_t Width() const { return resolution==VitaWorldResolution::Native ? 960 : resolution==VitaWorldResolution::Performance ? 480 : 720; }
    uint32_t Height() const { return Width()*544/960; }
};
inline bool VitaReducedGrassModel(const char* name) {
    return name && std::strncmp(name,"foliage_",8)==0 && std::strstr(name,"grass");
}
// Source record ordinal is stable across loads, material surfaces and LODs.
// Mix before selecting one of four equiprobable groups; never hash pointers.
inline bool VitaOptionalGrassPlacement(uint32_t sourceOrdinal) {
    uint32_t h=sourceOrdinal+0x9e3779b9u;
    h=(h^(h>>16))*0x85ebca6bu; h=(h^(h>>13))*0xc2b2ae35u;
    return ((h^(h>>16))&3u)==0;
}
inline bool VitaOmitDistantGrass(bool omitted, float minimumBatchDistance) {
    if (!std::isfinite(minimumBatchDistance) || minimumBatchDistance<0) return false;
    return omitted ? minimumBatchDistance>=896.0f : minimumBatchDistance>1024.0f;
}
}
