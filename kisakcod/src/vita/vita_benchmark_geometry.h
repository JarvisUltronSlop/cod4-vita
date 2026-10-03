#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace kisak::vita {
// Diagnostic category v1: placed XModels in the source foliage_ family.
// Ground materials, fences and particle effects are intentionally outside it.
inline bool VitaBenchmarkFoliageModel(std::string_view name) {
    return name.substr(0, 8) == "foliage_";
}

struct VitaBenchmarkGeometry {
    uint32_t mode=0; // 0 unchanged, 1 omit foliage models, 2 omit all static models
    uint32_t archivePasses=0, failures=0, staticModelUs=0;
    std::array<uint32_t,2> world{}, viewmodel{}; // [actual draws, triangles]
    // Arrays are [foliage, other static models], after the SAME frustum/LOD tests.
    std::array<uint32_t,2> candidates{}, candidateTriangles{}, candidateInstances{};
    std::array<uint32_t,2> skipped{}, skippedTriangles{};
    std::array<uint32_t,2> submitted{}, submittedTriangles{};

    bool Admit(bool foliage, uint32_t indices, uint32_t instances) {
        const unsigned category=foliage?0u:1u;
        const uint32_t triangles=(indices/3u)*instances;
        ++candidates[category]; candidateTriangles[category]+=triangles;
        candidateInstances[category]+=instances;
        if (mode==2 || (mode==1 && foliage)) {
            ++skipped[category]; skippedTriangles[category]+=triangles;
            return false;
        }
        return true;
    }
    void Submitted(bool foliage, uint32_t indices, uint32_t instances, bool success) {
        if (!success) { ++failures; return; }
        const unsigned category=foliage?0u:1u;
        ++submitted[category]; submittedTriangles[category]+=(indices/3u)*instances;
    }
};
}
