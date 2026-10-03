#pragma once
#include <gfx/gfx_backend.h>
#include <array>
#include <cmath>

// Renderer-owned bounds for the most recently uploaded prefix of each logical
// pose buffer. Physical GPU ring rotation does not change its logical identity.
// No allocation, source pointer, or stale partial-update tail is retained.
template<size_t Capacity=16>
class GfxVitaPoseLightBounds {
    struct Entry {uint32_t handle=0,count=0;GfxDrawLightBounds bounds{};};
    std::array<Entry,Capacity> entries_{};
public:
    void Forget(uint32_t handle){for(auto& e:entries_)if(e.handle==handle)e={};}
    void Clear(){for(auto& e:entries_)e={};}
    template<class Vertex>
    bool Store(uint32_t handle,const Vertex* vertices,uint32_t count) {
        Forget(handle);
        if(!handle || !vertices || !count)return false;
        Entry* target=nullptr;
        for(auto& e:entries_)if(!e.handle){target=&e;break;}
        if(!target)return false; // Unknown bounds retain full lighting.
        GfxDrawLightBounds bounds{};
        for(uint32_t i=0;i<count;++i)for(unsigned axis=0;axis<3;++axis) {
            const float value=vertices[i].worldPosition[axis];
            if(!std::isfinite(value))return false;
            if(!i || value<bounds.minimum[axis])bounds.minimum[axis]=value;
            if(!i || value>bounds.maximum[axis])bounds.maximum[axis]=value;
        }
        bounds.valid=true;*target={handle,count,bounds};return true;
    }
    GfxDrawLightBounds Find(uint32_t handle,uint32_t first,uint32_t count)const {
        for(const auto& e:entries_)if(handle && e.handle==handle) {
            if(count && first<=e.count && count<=e.count-first)return e.bounds;
            return {};
        }
        return {};
    }
};
