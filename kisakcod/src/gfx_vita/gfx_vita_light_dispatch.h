#pragma once
#include <cstddef>
#include <cmath>
#include <gfx/gfx_backend.h>

// Skip only exact zero-colour local inputs, including the reserved black
// primary-light slot. Any nonzero or nonfinite value retains the full shader.
// The sun is handled separately in both programs, using the source sun index.
template<class Lighting>
bool GfxVitaNeedsLocalLightLoops(const Lighting& lighting) {
    if ((lighting.primaryLightCount && lighting.sunPrimaryLightIndex >= lighting.primaryLightCount)
        || lighting.primaryLightCount > sizeof(lighting.primaryLights)/sizeof(lighting.primaryLights[0])
        || lighting.dynamicLightCount > sizeof(lighting.dynamicLights)/sizeof(lighting.dynamicLights[0]))
        return true;
    const auto nonzero=[](const auto& light) {
        if(light.color[0]!=0.0f || light.color[1]!=0.0f || light.color[2]!=0.0f) return true;
        for(float v:light.direction) if(!std::isfinite(v)) return true;
        for(float v:light.origin) if(!std::isfinite(v)) return true;
        return !std::isfinite(light.radius) || light.radius<0
            || !std::isfinite(light.cosHalfFovOuter) || !std::isfinite(light.cosHalfFovInner)
            || !std::isfinite(light.cosHalfFovExpanded) || !std::isfinite(light.rotationLimit)
            || !std::isfinite(light.translationLimit) || !std::isfinite(light.shadowVisibility);
    };
    for (size_t i=0;i<lighting.primaryLightCount;++i)
        if (i!=lighting.sunPrimaryLightIndex && nonzero(lighting.primaryLights[i])) return true;
    for (size_t i=0;i<lighting.dynamicLightCount;++i)
        if (nonzero(lighting.dynamicLights[i])) return true;
    return false;
}

// Only prove an exact zero local contribution. Do not approximate spot cones,
// sample at the camera, or cull against an individual instance's bounds when
// the submitted batch contains more than one instance. A one-unit guard keeps
// float/packed-transform rounding away from the sphere boundary.
template<class Lighting>
bool GfxVitaBoundsNeedLocalLights(const Lighting& lighting, const GfxDrawLightBounds& bounds) {
    if (!bounds.valid) return true;
    for (unsigned axis=0;axis<3;++axis)
        if (!std::isfinite(bounds.minimum[axis]) || !std::isfinite(bounds.maximum[axis])
            || bounds.minimum[axis]>bounds.maximum[axis]) return true;
    if ((lighting.primaryLightCount && lighting.sunPrimaryLightIndex>=lighting.primaryLightCount)
        || lighting.primaryLightCount>sizeof(lighting.primaryLights)/sizeof(lighting.primaryLights[0])
        || lighting.dynamicLightCount>sizeof(lighting.dynamicLights)/sizeof(lighting.dynamicLights[0])) return true;
    const auto overlaps=[&](const auto& light, bool dynamic) {
        for(float v:light.color) if(!std::isfinite(v))return true;
        for(float v:light.direction) if(!std::isfinite(v))return true;
        for(float v:light.origin) if(!std::isfinite(v))return true;
        if(!std::isfinite(light.radius) || light.radius<0
            || !std::isfinite(light.cosHalfFovOuter) || !std::isfinite(light.cosHalfFovInner)
            || !std::isfinite(light.cosHalfFovExpanded) || !std::isfinite(light.rotationLimit)
            || !std::isfinite(light.translationLimit) || !std::isfinite(light.shadowVisibility))return true;
        if(light.color[0]==0 && light.color[1]==0 && light.color[2]==0)return false;
        // The primary shader treats types 0/1/4 as directional. Unknown types
        // remain conservative. Dynamic descriptors use distance in all cases.
        if(!dynamic && light.type!=2 && light.type!=3 && light.type!=5 && light.type!=6)return true;
        double distanceSquared=0;
        for(unsigned axis=0;axis<3;++axis) {
            const double p=light.origin[axis];
            const double d=p<bounds.minimum[axis]?bounds.minimum[axis]-p:
                p>bounds.maximum[axis]?p-bounds.maximum[axis]:0.0;
            distanceSquared+=d*d;
        }
        const double radius=(light.radius>0.0001f?light.radius:0.0001f)+1.0;
        return distanceSquared<=radius*radius;
    };
    for(size_t i=0;i<lighting.primaryLightCount;++i)
        if(i!=lighting.sunPrimaryLightIndex && overlaps(lighting.primaryLights[i],false))return true;
    for(size_t i=0;i<lighting.dynamicLightCount;++i)
        if(overlaps(lighting.dynamicLights[i],true))return true;
    return false;
}

// Scope restoration prevents one culled batch from suppressing lights on the
// next batch, transient FX, or the weapon. Consume bounds even on early return.
struct GfxVitaDrawLocalLightScope {
    bool& active; bool previous;
    template<class Lighting>
    GfxVitaDrawLocalLightScope(bool& loops,GfxDrawLightBounds& pending,const Lighting& lighting,bool eligible)
        :active(loops),previous(loops) {
        if(eligible && loops)loops=GfxVitaBoundsNeedLocalLights(lighting,pending);
        pending={};
    }
    ~GfxVitaDrawLocalLightScope(){active=previous;}
};
