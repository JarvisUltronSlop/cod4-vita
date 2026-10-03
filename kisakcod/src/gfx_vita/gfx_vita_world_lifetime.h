#pragma once
#include "gfx_vita_material_family.h"
struct GfxVitaWorldLifetimeEdit {const char* before;const char* after;};
// Resolve a lightmap's sun scalar while the tangent basis is live, instead
// of preserving its full bumped normal across specular/reflection work.
inline const GfxVitaWorldLifetimeEdit* GfxVitaWorldLifetimeEdits(unsigned& count) {
    static const GfxVitaWorldLifetimeEdit edits[]={
        {"    vec4 material = v_color;", "    vec3 sun = vec3(0.0, 0.0, 1.0);\n    if (u_frame_valid > 0.5) sun = normalize(u_sun_direction);\n    vec4 material = v_color;"},
        {"    vec3 sourceLightmapNormal = normal;", "    float sourceLightmapSun = 0.0;"},
        {"        sourceLightmapNormal = normalize(normal\n            + tangent * sourceLightmapNormalXY.x\n            + bitangent * sourceLightmapNormalXY.y);",
         "        sourceLightmapSun = u_frame_valid > 0.5\n            ? max(dot(normalize(normal\n                + tangent * sourceLightmapNormalXY.x\n                + bitangent * sourceLightmapNormalXY.y), sun), 0.0) : 0.0;"},
        {"    vec3 sun = vec3(0.0, 0.0, 1.0);\n    float sunDot = 0.0;", "    float sunDot = 0.0;"},
        {"        sun = normalize(u_sun_direction);\n        sunDot", "        sunDot"},
        {"        float sourceSunDot = u_frame_valid > 0.5\n            ? max(dot(sourceLightmapNormal, sun), 0.0) : 0.0;", "        float sourceSunDot = sourceLightmapSun;"},
        {"            float sourceSunDot = u_frame_valid > 0.5\n                ? max(dot(sourceLightmapNormal, sun), 0.0) : 0.0;", "            float sourceSunDot = sourceLightmapSun;"}
    };
    count=sizeof(edits)/sizeof(edits[0]);return edits;
}
inline bool GfxVitaBuildWorldLifetime(const char* original,std::string& result) {
    result.clear();std::string source;
    // Family1 guarantees the normal map and both lightmaps are present.
    if(!GfxVitaBuildMaterialFamily(original,1,source))return false;
    unsigned count=0;const auto* edits=GfxVitaWorldLifetimeEdits(count);
    for(unsigned i=0;i<count;++i) {
        const std::string from=edits[i].before;const auto at=source.find(from);
        if(at==std::string::npos || source.find(from,at+from.size())!=std::string::npos)return false;
        source.replace(at,from.size(),edits[i].after);
    }
    result.swap(source);return true;
}
inline bool GfxVitaWorldLifetimeEligible(bool staticWorld,bool viewmodel,bool instanced,bool opaque,unsigned family) {
    return staticWorld && !viewmodel && !instanced && opaque && family==1;
}
