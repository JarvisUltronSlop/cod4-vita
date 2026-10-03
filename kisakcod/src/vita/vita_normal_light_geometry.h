#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace kisak::vita {
// Pure normal-derived geometry: no light colours, asset pointers or generation
// references. Preserve normalization, interpolation and accumulation order.
struct NormalLightGeometry {
    float inverseNormalLength = 0, weights[8]{}, totalWeight = 0;
    uint8_t shellIndices[8]{};
    bool normalValid = false, gridValid = false;
    void Prepare(const float* normal) {
        normalValid = gridValid = false;
        if (!normal) return;
        const float length = normal[0]*normal[0] + normal[1]*normal[1] + normal[2]*normal[2];
        if (!std::isfinite(length) || length <= 0.000001f) return;
        inverseNormalLength = 1.0f / std::sqrt(length);
        normalValid = true;
    const float normalized[3] = {
        normal[0] * inverseNormalLength,
        normal[1] * inverseNormalLength,
        normal[2] * inverseNormalLength,
    };
    const float maximumComponent = std::max({std::fabs(normalized[0]),
                                             std::fabs(normalized[1]),
                                             std::fabs(normalized[2])});
    if (!std::isfinite(maximumComponent) || maximumComponent <= 0.000001f)
        return;

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
    totalWeight = 0.0f;
    for (int z = 0; z <= 1; ++z) {
        for (int y = 0; y <= 1; ++y) {
            for (int x = 0; x <= 1; ++x) {
                const int sampleX = std::clamp(base[0] + x, 0, 3);
                const int sampleY = std::clamp(base[1] + y, 0, 3);
                const int sampleZ = std::clamp(base[2] + z, 0, 3);
                const uint32_t shellIndex =
                    kShellIndex[sampleZ][sampleY][sampleX];
                const float weight = (x ? fraction[0] : 1.0f - fraction[0])
                    * (y ? fraction[1] : 1.0f - fraction[1])
                    * (z ? fraction[2] : 1.0f - fraction[2]);
                const unsigned slot = z * 4 + y * 2 + x;
                weights[slot] = weight;
                shellIndices[slot] = static_cast<uint8_t>(shellIndex);
                if (weight > 0.0f) totalWeight += weight;
            }
        }
    }
    gridValid = std::isfinite(totalWeight) && totalWeight > 0.000001f;
    }
};

// Source-owned, fixed-size memoization across lighting samples and map loads.
// Exact input bits; collisions replace entries, affecting only work avoided.
// Invalid normal results are also deterministic. No heap/arena ownership.
template<size_t Capacity = 1024> class NormalLightGeometryCache {
    static_assert(Capacity && !(Capacity & (Capacity - 1)));
    struct Entry { uint32_t bits[3]{}; NormalLightGeometry geometry; bool occupied = false; };
    std::array<Entry, Capacity> entries_{};
public:
    uint32_t hits = 0, misses = 0;
    void Clear() {
        for (auto& entry : entries_) entry.occupied = false;
        hits = misses = 0;
    }
    const NormalLightGeometry& Resolve(const float* normal) {
        uint32_t bits[3]; std::memcpy(bits, normal, sizeof(bits));
        uint32_t hash = bits[0] * 0x9e3779b1u;
        hash ^= bits[1] * 0x85ebca6bu;
        hash ^= bits[2] * 0xc2b2ae35u;
        hash ^= hash >> 16;
        auto& entry = entries_[hash & (Capacity - 1)];
        if (entry.occupied && std::memcmp(entry.bits, bits, sizeof(bits)) == 0) {
            ++hits; return entry.geometry;
        }
        ++misses;
        entry.geometry.Prepare(normal);
        std::memcpy(entry.bits, bits, sizeof(bits));
        entry.occupied = true;
        return entry.geometry;
    }
};
}
