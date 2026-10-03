#pragma once
#include "gfx_vita_material_family.h"

// Native VitaGL translates GLSL to Cg and erases mediump/lowp. Explicit
// half types below survive that translator. Do not enable global fastprecision:
// positions, UVs, normal reconstruction, shadow-byte unpacking, specular mask
// and exponents must remain float. Only bounded lighting colours are rounded.
struct GfxVitaWorldPrecisionEdit { const char* before; const char* after; unsigned count; };
inline const GfxVitaWorldPrecisionEdit* GfxVitaWorldPrecisionEdits(unsigned& count) {
    static const GfxVitaWorldPrecisionEdit edits[]={
        {"vec3 lit = material.rgb;", "half3 lit = half3(material.rgb);",1},
        {"vec3 environment = textureCube(", "half3 environment = textureCube(",1},
        {"float primary = texture2D(", "half primary = texture2D(",2},
        {"vec4 secondaryTop = texture2D(", "half4 secondaryTop = texture2D(",2},
        {"vec4 secondaryBottom = texture2D(", "half4 secondaryBottom = texture2D(",2},
        {"vec3 secondaryRgb = secondaryTop.rgb * normalInverseLength\n            + secondaryBottom.rgb * alignment;",
         "half3 secondaryRgb = secondaryTop.rgb * half(normalInverseLength)\n            + secondaryBottom.rgb * half(alignment);",1},
        {"vec3 staticLight = secondaryRgb\n            + primary * u_sun_color * sourceSunDot * shadowVisibility;",
         "half3 staticLight = secondaryRgb\n            + primary * half3(u_sun_color) * half(sourceSunDot) * half(shadowVisibility);",1},
        {"clamp(staticLight, vec3(0.0), vec3(1.35))", "clamp(staticLight, half3(0.0), half3(1.35))",1},
        {"vec3 secondary = secondaryTop.rgb * normalInverseLength\n                + secondaryBottom.rgb * alignment;",
         "half3 secondary = secondaryTop.rgb * half(normalInverseLength)\n                + secondaryBottom.rgb * half(alignment);",1},
        // VitaGL maps mix to Cg lerp. Resolve the half/float overload at the
        // fog boundary without reducing fog precision or changing its equation.
        {"lit = mix(lit, u_fog_color, clamp(fog, 0.0, 1.0));",
         "lit = half3(mix(vec3(lit), u_fog_color, clamp(fog, 0.0, 1.0)));",1},
    };
    count=sizeof(edits)/sizeof(edits[0]);return edits;
}
inline bool GfxVitaBuildWorldPrecision(const char* original,bool transparent,std::string& result) {
    result.clear();if(!original)return false;
    if(!(transparent?GfxVitaBuildWorldAlpha(original,result):GfxVitaBuildMaterialFamily(original,1,result))){result.clear();return false;}
    unsigned count=0;const auto* edits=GfxVitaWorldPrecisionEdits(count);
    for(unsigned i=0;i<count;++i) {
        const auto& e=edits[i];const std::string from=e.before,to=e.after;unsigned found=0;
        for(size_t at=0;(at=result.find(from,at))!=std::string::npos;at+=to.size()) {result.replace(at,from.size(),to);++found;}
        if(found!=e.count){result.clear();return false;}
    }
    return true;
}
inline unsigned GfxVitaWorldPrecisionLane(bool staticWorld,bool viewmodel,bool instanced,bool opaque,bool transparent) {
    if(!staticWorld || viewmodel || instanced || opaque==transparent)return 0;
    return transparent?2u:1u;
}
