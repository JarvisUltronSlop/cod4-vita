#pragma once
#include "gfx_vita_opaque_shader.h"
// Diagnostic variants only: 0 original opaque, 1 no normal-map sampling and
// reconstruction, 2 no specular/reflection lobe, 3 base color with original fog.
inline unsigned GfxVitaMaterialProbeVariant(unsigned phase) {
    return phase==2?1u:phase==4?2u:phase==6?3u:0u;
}
inline bool GfxVitaBuildMaterialProbeShader(const char* original,bool sunOnly,unsigned variant,std::string& result) {
    if(variant>3 || !GfxVitaBuildOpaqueShader(original,sunOnly,result))return false;
    if(!variant)return true;
    if(variant<=2) {
        const char* name=variant==1?"VITA_NO_NORMAL_MAP":"VITA_NO_SPECULAR";
        const std::string guard=std::string("#if !defined(")+name+")";
        const auto g=result.find(guard);
        if(g==std::string::npos || result.find(guard,g+guard.size())!=std::string::npos)return false;
        const std::string version="#version 100\n";const auto at=result.find(version);
        if(at==std::string::npos)return false;
        result.insert(at+version.size(),std::string("#define ")+name+" 1\n");return true;
    }
    const std::string start="    float normalLength = dot(v_normal, v_normal);";
    const std::string end="#ifndef VITA_NO_FOG";
    const auto a=result.find(start),b=result.find(end);
    if(a==std::string::npos || b==std::string::npos || b<=a
        || result.find(start,a+start.size())!=std::string::npos || result.find(end,b+end.size())!=std::string::npos)return false;
    result.replace(a,b-a,"    vec3 lit = material.rgb;\n");
    return true;
}
