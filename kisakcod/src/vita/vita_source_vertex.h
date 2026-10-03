#pragma once
#include <array>
#include <cmath>
#include <gfx/gfx_backend.h>
#include <vita/vita_skin_math.h>

namespace kisak::vita {
// Exact byte / 255.0f values, computed once in read-only storage. The complete
// table is checked against the original runtime expression by portable tests.
inline constexpr auto sourceVertexColors = [] {
    std::array<float, 256> result{};
    for (unsigned i=0; i<256; ++i) result[i] = static_cast<float>(i) / 255.0f;
    return result;
}();

template<class PackedVertex, class Project>
inline bool ConvertSourceVertex(const PackedVertex& input, GfxDrawVertex& dest, Project project) {
    // All 21 floats are explicitly written on success. Only the two unused
    // lightmap coordinates require zeroing; avoid clearing overwritten data.
    static_assert(sizeof(GfxDrawVertex) == 21 * sizeof(float));
    dest.lightmapCoord[0] = dest.lightmapCoord[1] = 0.0f;
    dest.shadowVisibility = 1.0f;
    project(input.xyz, dest.position, dest.worldPosition);
    for (float component : dest.position) {
        if (!std::isfinite(component)) {
            // Match the original zero-initialize-then-project failure output.
            for (auto& value : dest.color) value = 0.0f;
            for (auto& value : dest.texCoord) value = 0.0f;
            for (auto& value : dest.normal) value = 0.0f;
            for (auto& value : dest.tangent) value = 0.0f;
            return false;
        }
    }
    skinmath::Vec2UnpackTexCoords(input.texCoord, dest.texCoord);
    skinmath::Vec3UnpackUnitVec(input.normal, dest.normal);
    skinmath::Vec3UnpackUnitVec(input.tangent, dest.tangent);
    for (unsigned component=0; component<4; ++component)
        dest.color[component] = sourceVertexColors[input.color.array[component]];
    return true;
}
}
