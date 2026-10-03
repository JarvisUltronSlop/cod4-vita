#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <gfx/gfx_backend.h>

namespace kisak::vita {

// Rigid source props use the instance light grid, with constant (0, 0)
// lightmap coordinates and visibility 1. Keep positions/UVs as source floats;
// the color/basis encoding matches the existing native world vertex format.
struct VitaStaticModelVertex {
    float worldPosition[3];
    float texCoord[2];
    int8_t normal[4];
    int8_t tangent[4];
    uint8_t color[4];
};
static_assert(sizeof(VitaStaticModelVertex) == 32);

inline bool VitaPackStaticModelVertex(const GfxDrawVertex& source,
                                      VitaStaticModelVertex* output) {
    if (!output || source.lightmapCoord[0] != 0.0f
        || source.lightmapCoord[1] != 0.0f || source.shadowVisibility != 1.0f)
        return false;
    std::memcpy(output->worldPosition, source.worldPosition,
                sizeof(output->worldPosition));
    std::memcpy(output->texCoord, source.texCoord, sizeof(output->texCoord));
    const auto packSnorm = [](float value) -> int8_t {
        return static_cast<int8_t>(std::lround(std::clamp(
            std::isfinite(value) ? value : 0.0f, -1.0f, 1.0f) * 127.0f));
    };
    for (uint32_t component = 0; component < 3; ++component) {
        output->normal[component] = packSnorm(source.normal[component]);
        output->tangent[component] = packSnorm(source.tangent[component]);
    }
    output->normal[3] = output->tangent[3] = 0;
    for (uint32_t component = 0; component < 4; ++component) {
        const float color = source.color[component];
        output->color[component] = static_cast<uint8_t>(std::lround(std::clamp(
            std::isfinite(color) ? color : 0.0f, 0.0f, 1.0f) * 255.0f));
    }
    return true;
}

} // namespace kisak::vita
