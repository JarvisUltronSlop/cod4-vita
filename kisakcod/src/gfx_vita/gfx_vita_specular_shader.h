#pragma once
#include "gfx_vita_opaque_shader.h"
// Algebraic rewrite only. Preserve texture samples, lobe constants, clamping,
// reflection direction normalization, precision and the degenerate-view guard.
// These exact anchors deliberately reject source drift instead of guessing.
struct GfxVitaSpecularRewrite { const char* before; const char* after; };
inline const GfxVitaSpecularRewrite* GfxVitaSpecularRewrites(unsigned& count) {
    static const GfxVitaSpecularRewrite edits[] = {
        {"            vec3 reflection = vec3(0.0);",
         "            vec3 reflection = vec3(0.0);\n            float viewNormal = 0.0;"},
        {"                viewVector = normalize(viewVector);",
         "                viewVector = normalize(viewVector);\n                viewNormal = dot(normal, viewVector);"},
        {"reflection = viewVector - 2.0 * dot(normal, viewVector) * normal;",
         "reflection = viewVector - 2.0 * viewNormal * normal;"},
        {"                float fresnel = pow(1.0 - max(dot(normal, viewVector), 0.0), 5.0);",
         "                float fresnelBase = 1.0 - max(viewNormal, 0.0);\n"
         "                float fresnelSquare = fresnelBase * fresnelBase;\n"
         "                float fresnel = (fresnelSquare * fresnelSquare) * fresnelBase;"}
    };
    count=sizeof(edits)/sizeof(edits[0]);return edits;
}
inline bool GfxVitaBuildSpecularShader(const char* original,bool sunOnly,std::string& result) {
    if(!GfxVitaBuildOpaqueShader(original,sunOnly,result))return false;
    unsigned count=0;const auto* edits=GfxVitaSpecularRewrites(count);
    for(unsigned i=0;i<count;++i) {
        const std::string from=edits[i].before;const auto at=result.find(from);
        if(at==std::string::npos || result.find(from,at+from.size())!=std::string::npos){result.clear();return false;}
        result.replace(at,from.size(),edits[i].after);
    }
    return true;
}
