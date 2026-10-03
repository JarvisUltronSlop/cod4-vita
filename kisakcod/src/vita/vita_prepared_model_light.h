#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace kisak::vita {
// Source-owned per-entity preparation. Preserve float operation order, shell
// interpolation and fallback; share only the identical normal scale and sun
// constants. Texel positions are clamped positive, so truncation equals floor.
template<class Sample, class Colors>
inline bool PreparedModelLightGrid(const Sample& sample, const float normal[3],
                                   float inverseNormalLength, float output[3],
                                   const Colors& directionalColors) {
    if (!output)
        return false;
    output[0] = output[1] = output[2] = 1.0f;
    if (!sample.valid || !sample.hasDirectionalColors || !normal)
        return false;

    const float normalized[3] = {
        normal[0] * inverseNormalLength,
        normal[1] * inverseNormalLength,
        normal[2] * inverseNormalLength,
    };
    const float maximumComponent = std::max({std::fabs(normalized[0]),
                                             std::fabs(normalized[1]),
                                             std::fabs(normalized[2])});
    if (!std::isfinite(maximumComponent) || maximumComponent <= 0.000001f)
        return false;

    const float texelPosition[3] = {
        std::clamp(2.0f + 1.5f * normalized[0] / maximumComponent,
                   0.5f, 3.5f),
        std::clamp(2.0f + 1.5f * normalized[1] / maximumComponent,
                   0.5f, 3.5f),
        std::clamp(2.0f + 1.5f * normalized[2] / maximumComponent,
                   0.5f, 3.5f),
    };
    const int base[3] = {
        static_cast<int>(texelPosition[0]),
        static_cast<int>(texelPosition[1]),
        static_cast<int>(texelPosition[2]),
    };
    const float fraction[3] = {
        texelPosition[0] - static_cast<float>(base[0]),
        texelPosition[1] - static_cast<float>(base[1]),
        texelPosition[2] - static_cast<float>(base[2]),
    };
    // [z][y][x], copied from the writes in R_SetLightGridColors. The six
    // faces account for 56 texels; entries at the 8 corners are shared by the
    // adjoining faces in the source texture.
    static constexpr uint8_t kShellIndex[4][4][4] = {
        {
            {0, 1, 2, 3},
            {4, 5, 6, 7},
            {8, 9, 10, 11},
            {12, 13, 14, 15},
        },
        {
            {16, 17, 18, 19},
            {20, 0, 3, 21},
            {22, 12, 15, 23},
            {24, 25, 26, 27},
        },
        {
            {28, 29, 30, 31},
            {32, 40, 43, 33},
            {34, 52, 55, 35},
            {36, 37, 38, 39},
        },
        {
            {40, 41, 42, 43},
            {44, 45, 46, 47},
            {48, 49, 50, 51},
            {52, 53, 54, 55},
        },
    };
    float accumulated[3] = {0.0f, 0.0f, 0.0f};
    float totalWeight = 0.0f;
    // The stencil is always eight corners. Resolve each axis endpoint once,
    // and share only the identical first (x*y) multiplication between z slices.
    // Keep x-fastest source accumulation, (x*y)*z, weight*color/255 and the
    // final division unchanged: reassociation or pre-normalizing colors would
    // change rounding. Explicit corners remove three nested loop controls and
    // repeated endpoint clamps from every uncached vertex on the source owner.
    const int x0 = std::clamp(base[0], 0, 3), x1 = std::clamp(base[0] + 1, 0, 3);
    const int y0 = std::clamp(base[1], 0, 3), y1 = std::clamp(base[1] + 1, 0, 3);
    const int z0 = std::clamp(base[2], 0, 3), z1 = std::clamp(base[2] + 1, 0, 3);
    const float wx0 = 1.0f - fraction[0], wy0 = 1.0f - fraction[1];
    const float wz0 = 1.0f - fraction[2];
    const float xy00 = wx0 * wy0, xy10 = fraction[0] * wy0;
    const float xy01 = wx0 * fraction[1], xy11 = fraction[0] * fraction[1];
    const auto accumulate = [&](int x, int y, int z, float weight)
#if defined(__GNUC__)
        __attribute__((always_inline))
#endif
    {
        if (weight <= 0.0f) return;
        const uint32_t offset = kShellIndex[z][y][x] * 3u;
        totalWeight += weight;
        for (uint32_t channel = 0; channel < 3u; ++channel)
            accumulated[channel] += weight
                * static_cast<float>(directionalColors[offset + channel]) / 255.0f;
    };
    accumulate(x0, y0, z0, xy00 * wz0);
    accumulate(x1, y0, z0, xy10 * wz0);
    accumulate(x0, y1, z0, xy01 * wz0);
    accumulate(x1, y1, z0, xy11 * wz0);
    accumulate(x0, y0, z1, xy00 * fraction[2]);
    accumulate(x1, y0, z1, xy10 * fraction[2]);
    accumulate(x0, y1, z1, xy01 * fraction[2]);
    accumulate(x1, y1, z1, xy11 * fraction[2]);
    if (!std::isfinite(totalWeight) || totalWeight <= 0.000001f)
        return false;
    for (uint32_t channel = 0; channel < 3u; ++channel) {
        const float value = accumulated[channel] / totalWeight;
        if (!std::isfinite(value))
            return false;
        output[channel] = std::clamp(value, 0.0f, 1.35f);
    }
    return true;
}

template<class Sample, class Lighting> class PreparedModelLight {
    const Sample& sample_;
    const Lighting& lighting_;
    float sun_[3]{}, alpha_=0;
    // Integers 0..255 convert exactly. Convert once per entity sample, retaining
    // weight * color / 255 in the original vertex loop: pre-dividing colors
    // would change float rounding. This storage lives only for this sample.
    float directionalColors_[56 * 3];
    bool valid_=false;
public:
    PreparedModelLight(const Sample& sample, const Lighting& lighting)
        : sample_(sample), lighting_(lighting) {
        if (!sample.valid || !lighting.valid || !std::isfinite(sample.primaryLightWeight)) return;
        std::memcpy(sun_, lighting.sunDirection, sizeof(sun_));
        const float length = sun_[0]*sun_[0] + sun_[1]*sun_[1] + sun_[2]*sun_[2];
        if (!std::isfinite(length) || length <= 0.000001f) return;
        const float scale = 1.0f / std::sqrt(length);
        for (auto& v : sun_) v *= scale;
        alpha_ = std::clamp(sample.primaryLightWeight, 0.0f, 1.0f);
        if (sample.hasDirectionalColors) {
            for (uint32_t i = 0; i < 56 * 3; ++i)
                directionalColors_[i] = static_cast<float>(sample.directionalColors[i]);
        }
        valid_ = true;
    }
    bool Evaluate(const float* normal, float* output) const {
        if (!valid_ || !normal || !output) return false;
        const float length = normal[0]*normal[0] + normal[1]*normal[1] + normal[2]*normal[2];
        if (!std::isfinite(length) || length <= 0.000001f) return false;
        const float scale = 1.0f / std::sqrt(length);
        float rgb[3]{};
        if (!PreparedModelLightGrid(sample_, normal, scale, rgb, directionalColors_))
            std::memcpy(rgb, sample_.color, sizeof(rgb));
        const float dot = std::clamp(sun_[0]*normal[0]*scale
            + sun_[1]*normal[1]*scale + sun_[2]*normal[2]*scale, 0.0f, 1.0f);
        for (int i=0; i<3; ++i) {
            const float color = lighting_.sunLightColor[i];
            if (!std::isfinite(rgb[i]) || !std::isfinite(color)) return false;
            output[i] = 2.0f*rgb[i] + alpha_*color*dot;
            if (!std::isfinite(output[i])) return false;
        }
        return true;
    }
};
}
