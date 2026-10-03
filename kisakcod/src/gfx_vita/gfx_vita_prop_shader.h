#pragma once
#include <string>

// Inputs are resolved from the actual draw, not from the preceding material.
// Local-light eligibility is the existing pre-sanitization exact-zero check.
inline bool GfxVitaPropShaderEligible(bool instanced, bool worldSpace, bool viewmodel,
    bool baked, bool primary, bool secondary, bool color, bool localLoops,
    bool adaptive, bool hud, bool sky, unsigned benchmarkMode) {
    return instanced && worldSpace && !viewmodel && baked && !primary && !secondary
        && color && !localLoops && adaptive && !hud && !sky && benchmarkMode==0;
}

// Keep shader expressions intact. Only uniforms proved constant by the draw
// gate become compile-time constants, allowing the pinned compiler to remove
// unreachable geometry/lightmap branches and unused frame-light calculations.
inline bool GfxVitaBuildPropShaders(const char* originalVertex, const char* originalFragment,
                                  std::string& vertex, std::string& fragment) {
    vertex=originalVertex;fragment=originalFragment;
    const auto fix=[](std::string& source,const char* name,const char* value) {
        const std::string uniform=std::string("uniform float ")+name+";";
        const auto at=source.find(uniform);
        if(at==std::string::npos || source.find(uniform,at+uniform.size())!=std::string::npos)return false;
        source.replace(at,uniform.size(),std::string("const float ")+name+" = "+value+";");return true;
    };
    if(!fix(vertex,"u_static_instanced","1.0") || !fix(vertex,"u_world_space","1.0")
        || !fix(vertex,"u_viewmodel_space","0.0") || !fix(fragment,"u_baked_lighting","1.0")
        || !fix(fragment,"u_use_primary_lightmap","0.0") || !fix(fragment,"u_use_secondary_lightmap","0.0")
        || !fix(fragment,"u_has_color","1.0"))return false;
    const auto version=fragment.find("#version 100\n");
    if(version==std::string::npos)return false;
    fragment.insert(version+sizeof("#version 100\n")-1,"#define VITA_SUN_ONLY 1\n");
    return true;
}
