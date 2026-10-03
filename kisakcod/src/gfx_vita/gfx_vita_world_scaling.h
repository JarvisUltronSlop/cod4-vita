#pragma once
#include "gfx_vita_material_family.h"
#include <vita/vita_render_pass_probe.h>

// Diagnostic only: keep colour sampling, source alpha/discard and fog, but
// replace detailed normal/specular/directional lighting with an inexpensive
// approximation from the same authored lightmap textures.
inline bool GfxVitaBuildCheapWorld(const char* original,bool alpha,std::string& out) {
    out.clear();if(!original)return false;
    if(!(alpha?GfxVitaBuildWorldAlpha(original,out):GfxVitaBuildMaterialFamily(original,1,out)))return false;
    const auto normal=out.find("    float normalLength = dot(v_normal, v_normal);");
    const auto cutout=out.find(alpha?"    if (u_alpha_test_enabled > 0.5) {":"    vec3 lit = material.rgb;");
    const auto lit=out.find("    vec3 lit = material.rgb;");
    const auto fog=out.find("#ifndef VITA_NO_FOG");
    if(normal==std::string::npos || cutout==std::string::npos || lit==std::string::npos || fog==std::string::npos
        || !(normal<cutout && cutout<=lit && lit<fog)) {out.clear();return false;}
    out.replace(lit,fog-lit,R"CHEAP(    vec3 lit = material.rgb;
    vec3 illumination = vec3(0.0);
    if (u_use_primary_lightmap > 0.5)
        illumination += texture2D(u_lightmap_primary, v_lightmapcoord).r * u_sun_color;
    if (u_use_secondary_lightmap > 0.5) {
        vec2 topCoord = vec2(v_lightmapcoord.x, v_lightmapcoord.y * 0.5);
        vec2 bottomCoord = vec2(v_lightmapcoord.x, v_lightmapcoord.y * 0.5 + 0.5);
        illumination += 0.5 * (texture2D(u_lightmap_secondary, topCoord).rgb
            + texture2D(u_lightmap_secondary, bottomCoord).rgb);
    }
    lit *= clamp(illumination, vec3(0.0), vec3(1.35));
)CHEAP");
    out.erase(normal,cutout-normal);
    const auto main=out.find("void main() {"),material=out.find("    vec4 material = v_color;");
    if(main==std::string::npos || material==std::string::npos || main>=material){out.clear();return false;}
    out.erase(main+13,material-(main+13));return true;
}

struct GfxVitaWorldScalingState {
    unsigned phase=0,skyPrepared=0,skySuppressed=0,worldPrepared=0;
    unsigned eligible=0,actual=0,invalid=0,targetEligible=0,targetActual=0;
    void Begin(unsigned tag) {*this={};if(kisak::vita::VitaWorldScalingMode(tag))phase=tag&255;}
    bool Draw(bool sky,bool world,unsigned lane,unsigned bound,unsigned ready,unsigned surface) {
        if(!phase)return true;
        skyPrepared+=sky;worldPrepared+=world;
        if(lane){++eligible;targetEligible|=surface==261?1:surface==507?2:surface==588?4:0;}
        if(bound){++actual;targetActual|=surface==261?1:surface==507?2:surface==588?4:0;}
        if(bound!=(phase==4 && ready==3?lane:0))++invalid;
        if(sky && phase==2){++skySuppressed;return false;}return true;
    }
};

// Swaps preallocated target descriptors only. Call on the renderer before
// BeginFrame, after the previous frame's completion; never reallocates or waits.
template<class Target>
bool GfxVitaWorldScalingTarget(unsigned tag,Target& active,Target& spare,bool& low) {
    const bool wanted=kisak::vita::VitaWorldScalingMode(tag) && (tag&255)==6;
    if(wanted==low)return true;
    if(wanted && (!spare.framebuffer || spare.width!=480 || spare.height!=272
        || active.width!=720 || active.height!=408))return false;
    std::swap(active,spare);low=wanted;return true;
}
