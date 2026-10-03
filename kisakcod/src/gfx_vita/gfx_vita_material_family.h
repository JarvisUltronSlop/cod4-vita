#pragma once
#include "gfx_vita_opaque_shader.h"
// Only known exact texture configurations are specialized. Unknown/mixed
// materials and any valid local-light workload retain the baseline program.
inline unsigned GfxVitaMaterialFamily(bool opaque,bool adaptive,bool localLights,
    bool color,bool normal,bool specular,bool reflection,bool primary,bool secondary,bool baked) {
    if(!opaque || !adaptive || localLights || !color || !normal || !specular)return 0;
    if(primary && secondary && reflection)return 1;
    if(!primary && !secondary && !reflection && baked)return 2;
    return 0;
}
inline bool GfxVitaBuildMaterialFamily(const char* original,unsigned family,std::string& result) {
    if(family<1 || family>2 || !GfxVitaBuildOpaqueShader(original,true,result))return false;
    const char* names[]={"u_has_color","u_has_normal","u_has_specular","u_has_reflection_probe",
                         "u_use_primary_lightmap","u_use_secondary_lightmap","u_baked_lighting"};
    // World baked flag remains dynamic. It does not participate in that gate.
    for(unsigned i=0;i<(family==2?7u:6u);++i) {
        const std::string from=std::string("uniform float ")+names[i]+";";
        const auto at=result.find(from);
        if(at==std::string::npos || result.find(from,at+from.size())!=std::string::npos){result.clear();return false;}
        const bool on=i<3 || i==6 || family==1;
        result.replace(at,from.size(),std::string("const float ")+names[i]+" = "+(on?"1.0;":"0.0;"));
    }
    return true;
}

// This authored weapon configuration has no normal texture. Preserve the
// original vertex-normal fallback and specular equation; do not remove a map.
inline bool GfxVitaWeaponSpecularEligible(bool weaponOpaque,bool adaptive,bool localLights,
    bool color,bool normal,bool specular,bool reflection,bool primary,bool secondary,bool baked) {
    return weaponOpaque && adaptive && !localLights && color && !normal && specular
        && !reflection && !primary && !secondary && baked;
}
inline bool GfxVitaBuildWeaponSpecular(const char* original,std::string& result) {
    if(!GfxVitaBuildMaterialFamily(original,2,result))return false;
    const std::string from="const float u_has_normal = 1.0;";
    const auto at=result.find(from);
    if(at==std::string::npos || result.find(from,at+from.size())!=std::string::npos){result.clear();return false;}
    result.replace(at,from.size(),"const float u_has_normal = 0.0;");
    return true;
}

inline bool GfxVitaWorldAlphaEligible(bool sourceLitWorld,bool transparent,bool adaptive,bool localLights,
    bool color,bool normal,bool specular,bool reflection,bool primary,bool secondary) {
    return sourceLitWorld && transparent && adaptive && !localLights
        && color && normal && specular && reflection && primary && secondary;
}
// Unlike the opaque family, keep every discard predicate and its position.
// Baked lighting, alpha thresholds, fog and material state remain dynamic.
inline bool GfxVitaBuildWorldAlpha(const char* original,std::string& result) {
    result.clear();if(!original)return false;
    std::string source(original);
    const std::string version="#version 100\n";
    auto at=source.find(version);
    if(at==std::string::npos || source.find(version,at+version.size())!=std::string::npos)return false;
    source.insert(at+version.size(),"#define VITA_SUN_ONLY 1\n");
    const char* names[]={"u_has_color","u_has_normal","u_has_specular","u_has_reflection_probe","u_use_primary_lightmap","u_use_secondary_lightmap"};
    for(const auto* name:names) {
        const std::string from=std::string("uniform float ")+name+";";
        at=source.find(from);
        if(at==std::string::npos || source.find(from,at+from.size())!=std::string::npos)return false;
        source.replace(at,from.size(),std::string("const float ")+name+" = 1.0;");
    }
    result.swap(source);return true;
}
