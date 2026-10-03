#pragma once
#include "gfx_vita_material_family.h"

// All prefetched samplers are required by the exact family gate. Coordinates,
// filtering, formats, precision and shader equations are unchanged. Opaque
// programs contain no discard, and these coordinates have no dependent ALU.
inline bool GfxVitaBuildTexturePrefetch(const char* original,unsigned family,std::string& result) {
    if(!GfxVitaBuildMaterialFamily(original,family,result))return false;
    const auto replace=[&](const std::string& from,const std::string& to) {
        const auto at=result.find(from);
        if(at==std::string::npos || result.find(from,at+from.size())!=std::string::npos)return false;
        result.replace(at,from.size(),to);return true;
    };
    std::string reads="    vec4 earlySpecular = texture2D(u_specular, v_texcoord);\n";
    if(!replace("vec4 mask = texture2D(u_specular, v_texcoord);","vec4 mask = earlySpecular;")){result.clear();return false;}
    if(family==1) {
        reads+="    float earlyPrimary = texture2D(u_lightmap_primary, v_lightmapcoord).r;\n"
               "    vec4 earlySecondaryTop = texture2D(u_lightmap_secondary, vec2(v_lightmapcoord.x, v_lightmapcoord.y * 0.5));\n"
               "    vec4 earlySecondaryBottom = texture2D(u_lightmap_secondary, vec2(v_lightmapcoord.x, v_lightmapcoord.y * 0.5 + 0.5));\n";
        if(!replace("float primary = texture2D(u_lightmap_primary, v_lightmapcoord).r;","float primary = earlyPrimary;")
            || !replace("\n        vec4 secondaryTop = texture2D(u_lightmap_secondary, topCoord);","\n        vec4 secondaryTop = earlySecondaryTop;")
            || !replace("\n        vec4 secondaryBottom = texture2D(u_lightmap_secondary, bottomCoord);","\n        vec4 secondaryBottom = earlySecondaryBottom;")) {result.clear();return false;}
    }
    if(!replace("    vec4 material = v_color;",reads+"    vec4 material = v_color;")){result.clear();return false;}
    return true;
}
