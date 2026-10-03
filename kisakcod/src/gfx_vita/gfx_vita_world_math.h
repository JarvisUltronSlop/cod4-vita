#pragma once
#include "gfx_vita_material_family.h"

struct GfxVitaWorldMathEdit {const char* before;const char* after;};
// Keep Gram-Schmidt, DXT5nm Z reconstruction and every degenerate-basis
// fallback intact. These edits need no assumption about mesh orthogonality.
inline const GfxVitaWorldMathEdit* GfxVitaWorldMathEdits(unsigned& count) {
    static const GfxVitaWorldMathEdit edits[]={
        {"    vec4 material = v_color;", "    vec3 sun = vec3(0.0, 0.0, 1.0);\n    if (u_frame_valid > 0.5) sun = normalize(u_sun_direction);\n    vec4 material = v_color;"},
        {"    vec3 sourceLightmapNormal = normal;", "    float sourceLightmapSun = 0.0;"},
        {"        sourceLightmapNormal = normalize(normal\n            + tangent * sourceLightmapNormalXY.x\n            + bitangent * sourceLightmapNormalXY.y);",
         "        vec3 lightmapVector = normal\n            + tangent * sourceLightmapNormalXY.x\n            + bitangent * sourceLightmapNormalXY.y;\n        sourceLightmapSun = u_frame_valid > 0.5\n            ? max(dot(lightmapVector, sun)\n                * inversesqrt(dot(lightmapVector, lightmapVector)), 0.0) : 0.0;"},
        {"    vec3 sun = vec3(0.0, 0.0, 1.0);\n    float sunDot = 0.0;", "    float sunDot = 0.0;"},
        {"        sun = normalize(u_sun_direction);\n        sunDot", "        sunDot"},
        {"        float sourceSunDot = u_frame_valid > 0.5\n            ? max(dot(sourceLightmapNormal, sun), 0.0) : 0.0;", "        float sourceSunDot = sourceLightmapSun;"},
        {"            float sourceSunDot = u_frame_valid > 0.5\n                ? max(dot(sourceLightmapNormal, sun), 0.0) : 0.0;", "            float sourceSunDot = sourceLightmapSun;"},
        {"                float fresnel = pow(1.0 - max(dot(normal, viewVector), 0.0), 5.0);",
         "                float fresnelBase = 1.0 - max(dot(normal, viewVector), 0.0);\n                float fresnelSquared = fresnelBase * fresnelBase;\n                float fresnel = fresnelSquared * fresnelSquared * fresnelBase;"},
        // Cube coordinates describe a direction: positive scaling cancels
        // in face selection and projection. Keep the zero-view guard above.
        {"                    u_reflection_probe, normalize(reflection)).rgb;",
         "                    u_reflection_probe, reflection).rgb;"}
    };
    count=sizeof(edits)/sizeof(edits[0]);return edits;
}
inline bool GfxVitaBuildWorldMath(const char* original,bool transparent,std::string& result) {
    result.clear();if(!original)return false;
    std::string source;
    if(!(transparent?GfxVitaBuildWorldAlpha(original,source):GfxVitaBuildMaterialFamily(original,1,source)))return false;
    unsigned count=0;const auto* edits=GfxVitaWorldMathEdits(count);
    for(unsigned i=0;i<count;++i) {
        const std::string from=edits[i].before;const auto at=source.find(from);
        if(at==std::string::npos || source.find(from,at+from.size())!=std::string::npos)return false;
        source.replace(at,from.size(),edits[i].after);
    }
    result.swap(source);return true;
}
inline unsigned GfxVitaWorldMathLane(bool staticWorld,bool viewmodel,bool instanced,bool opaqueFamily,bool alphaFamily) {
    if(!staticWorld || viewmodel || instanced || opaqueFamily==alphaFamily)return 0;
    return opaqueFamily?1u:2u;
}
