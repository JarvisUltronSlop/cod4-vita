#pragma once
#include <array>
#include <cstdint>
#include <string_view>
namespace kisak::vita {
// Explicit source-name scope. Do not hide floor materials, foliage, intact
// pallets, barrels, cars, trash cans or dumpsters under a broad substring test.
inline bool VitaBenchmarkDebrisModel(std::string_view name) {
    constexpr std::string_view names[]={
        "com_pallet_destroyed", "com_junktire", "com_junktire2",
        "com_trashbag", "com_trashbag1_black", "com_trashbag1_white", "com_trashbag2_green",
        "com_cardboardbox01", "com_cardboardbox02", "com_cardboardbox03", "com_cardboardbox04",
        "com_cardboardbox06", "com_cardboardboxshortclosed_1", "com_cardboardboxshortclosed_2",
        "com_cardboardboxshortopen_2", "com_cardboardboxshortopen_3",
        "com_bottle1", "com_bottle2", "com_bottle3", "com_milk_carton",
        "ch_rubble_bible01", "ch_rubble_notebook01", "ch_rubble_woodplank01",
        "ch_rubble_particleboard01", "ch_rubble_chunk01", "ch_rubble_chunk02"
    };
    for (auto candidate:names) if(name==candidate) return true;
    return false;
}
inline bool VitaBenchmarkDebrisMaterial(std::string_view name) {
    return name=="wc/me_trash01";
}
struct VitaBenchmarkDebris {
    uint32_t mode=0, failures=0; // 0 normal, 1 suppress admitted debris geometry
    // [trash world surfaces, other world surfaces, clutter models, other models]
    std::array<uint32_t,4> candidates{}, candidateTriangles{}, candidateInstances{};
    std::array<uint32_t,4> skipped{}, skippedTriangles{}, submitted{}, submittedTriangles{};
    bool Admit(unsigned category, uint32_t indices, uint32_t instances=1) {
        const uint32_t triangles=indices/3u*instances;
        ++candidates[category]; candidateTriangles[category]+=triangles;
        candidateInstances[category]+=instances;
        if (mode==1 && (category==0 || category==2)) {
            ++skipped[category]; skippedTriangles[category]+=triangles; return false;
        }
        return true;
    }
    void Submitted(unsigned category, uint32_t indices, uint32_t instances, bool success) {
        if (!success) { ++failures; return; }
        ++submitted[category]; submittedTriangles[category]+=indices/3u*instances;
    }
};
}
