#pragma once

#include <gfx/gfx_backend.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

namespace kisak::vita {

// Keep the source plane equations and evaluation order. Only their camera-only
// terms move out of the surface loop; invalid bounds still remain visible.
class VitaWorldFrustum {
    struct Plane {
        float normal[3], absolute[3], distance;
    };
    Plane planes_[6];

public:
    template<class View> explicit VitaWorldFrustum(const View& view) {
        const auto set = [&](unsigned index, float x, float y, float z, float d) {
            planes_[index] = {{x, y, z}, {std::fabs(x), std::fabs(y), std::fabs(z)}, d};
        };
        set(0, view.forward[0], view.forward[1], view.forward[2],
            -view.eyeForward - view.nearDistance);
        set(1, -view.forward[0], -view.forward[1], -view.forward[2],
            view.eyeForward + view.farDistance);
        const float side[4][3] = {
            {view.forward[0] + view.right[0] * view.horizontalScale,
             view.forward[1] + view.right[1] * view.horizontalScale,
             view.forward[2] + view.right[2] * view.horizontalScale},
            {view.forward[0] - view.right[0] * view.horizontalScale,
             view.forward[1] - view.right[1] * view.horizontalScale,
             view.forward[2] - view.right[2] * view.horizontalScale},
            {view.forward[0] + view.up[0] * view.verticalScale,
             view.forward[1] + view.up[1] * view.verticalScale,
             view.forward[2] + view.up[2] * view.verticalScale},
            {view.forward[0] - view.up[0] * view.verticalScale,
             view.forward[1] - view.up[1] * view.verticalScale,
             view.forward[2] - view.up[2] * view.verticalScale},
        };
        for (unsigned i = 0; i < 4; ++i) {
            const auto& n = side[i];
            const float dot = n[0] * view.eye[0] + n[1] * view.eye[1] + n[2] * view.eye[2];
            set(i + 2, n[0], n[1], n[2], -dot);
        }
    }

    template<class Bounds> bool Visible(const Bounds& bounds) const {
        if (!bounds.valid) return true;
        const float center[3] = {
            (bounds.minimum[0] + bounds.maximum[0]) * 0.5f,
            (bounds.minimum[1] + bounds.maximum[1]) * 0.5f,
            (bounds.minimum[2] + bounds.maximum[2]) * 0.5f,
        };
        const float extent[3] = {
            (bounds.maximum[0] - bounds.minimum[0]) * 0.5f,
            (bounds.maximum[1] - bounds.minimum[1]) * 0.5f,
            (bounds.maximum[2] - bounds.minimum[2]) * 0.5f,
        };
        for (const auto& plane : planes_) {
            const float distance = plane.normal[0] * center[0] + plane.normal[1] * center[1]
                + plane.normal[2] * center[2] + plane.distance;
            const float radius = plane.absolute[0] * extent[0] + plane.absolute[1] * extent[1]
                + plane.absolute[2] * extent[2];
            if (distance + radius < 0.0f) return false;
        }
        return true;
    }
};

// This is the existing world order, including its source-index tie breaker.
// Adding other material fields here could change blended or coplanar output.
template<class Surface>
bool VitaWorldSurfaceLess(const Surface* surfaces, uint32_t leftIndex, uint32_t rightIndex) {
    const auto& left = surfaces[leftIndex];
    const auto& right = surfaces[rightIndex];
    if (left.mode != right.mode)
        return static_cast<uint8_t>(left.mode) < static_cast<uint8_t>(right.mode);
    if (left.sourceTechniqueFlags0 != right.sourceTechniqueFlags0)
        return left.sourceTechniqueFlags0 < right.sourceTechniqueFlags0;
    if (left.sourceStateBits0 != right.sourceStateBits0)
        return left.sourceStateBits0 < right.sourceStateBits0;
    if (left.sourceStateBits1 != right.sourceStateBits1)
        return left.sourceStateBits1 < right.sourceStateBits1;
    if (left.texture.value != right.texture.value)
        return left.texture.value < right.texture.value;
    if (left.lightmapTexture.value != right.lightmapTexture.value)
        return left.lightmapTexture.value < right.lightmapTexture.value;
    if (left.secondaryLightmapTexture.value != right.secondaryLightmapTexture.value)
        return left.secondaryLightmapTexture.value < right.secondaryLightmapTexture.value;
    return leftIndex < rightIndex;
}

// Source-owned map metadata. No asset pointers or GPU resources are retained.
// Invalidate when world materials/texture leases change. Visibility and draw
// eligibility are evaluated anew even when the order can be reused.
class VitaWorldDrawOrder {
    std::unique_ptr<uint32_t[]> order_;
    size_t capacity_ = 0, count_ = 0;
    bool dirty_ = true, ready_ = false;

public:
    using Allocator = uint32_t* (*)(size_t);
    void Invalidate() { dirty_ = true; ready_ = false; }
    void Clear() { order_.reset(); capacity_ = count_ = 0; Invalidate(); }
    size_t Bytes() const { return capacity_ * sizeof(uint32_t); }

    template<class Surface>
    bool Prepare(const Surface* surfaces, size_t count, Allocator allocate = nullptr) {
        if (!dirty_ && count == count_) return ready_;
        dirty_ = false;
        ready_ = false;
        count_ = count;
        if (count > UINT32_MAX || count > SIZE_MAX / sizeof(uint32_t)) return false;
        if (count > capacity_) {
            std::unique_ptr<uint32_t[]> next(allocate ? allocate(count)
                : new(std::nothrow) uint32_t[count]);
            // Use the uncached path after failure. Retry at the next explicit
            // invalidation, not on every gameplay frame under memory pressure.
            if (!next) return false;
            order_ = std::move(next);
            capacity_ = count;
        }
        for (size_t i = 0; i < count; ++i) order_[i] = static_cast<uint32_t>(i);
        if (count > 1)
            std::sort(order_.get(), order_.get() + count,
                [&](uint32_t a, uint32_t b) { return VitaWorldSurfaceLess(surfaces, a, b); });
        ready_ = true;
        return true;
    }

    template<class Surface, class Bounds>
    void Collect(const std::vector<Surface>& surfaces, const std::vector<Bounds>& bounds,
                 const VitaWorldFrustum& frustum, std::vector<uint32_t>& visible) {
        const bool cached = Prepare(surfaces.data(), surfaces.size());
        visible.clear();
        for (size_t i = 0; i < surfaces.size(); ++i) {
            const uint32_t index = cached ? order_[i] : static_cast<uint32_t>(i);
            const auto& surface = surfaces[index];
            if (surface.cameraRegion == 3u || surface.mode == GfxSurfaceMode::Sky
                || surface.vertexCount == 0 || surface.triangleCount == 0)
                continue;
            if (index < bounds.size() && !frustum.Visible(bounds[index])) continue;
            visible.push_back(index);
        }
        if (!cached)
            std::sort(visible.begin(), visible.end(),
                [&](uint32_t a, uint32_t b) { return VitaWorldSurfaceLess(surfaces.data(), a, b); });
    }
};

} // namespace kisak::vita
