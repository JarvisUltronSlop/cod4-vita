#pragma once
#include <vita/vita_viewmodel_packet.h>
#include <vita/vita_normal_response_cache.h>

namespace kisak::vita {
// Renderer-owned value cache. Reuse across snapshots only when every lighting
// input and owner identity is bit-identical. Projection/pose is not a light
// input; Resolve still checks all three decoded normal components exactly.
class VitaViewModelLightCache {
    NormalResponseCache<8192> cache_;
    VitaViewModelLightSample sample_{};
    VitaViewModelWorldLight world_{};
    uint32_t generation_=0, entity_=0;
    bool valid_=false;
    template<class T> static bool SameBits(const T& a,const T& b) {
        return std::memcmp(&a,&b,sizeof(a))==0;
    }
public:
    void Invalidate() { valid_=false; }
    bool Begin(uint32_t generation,uint32_t entity,const VitaViewModelInputs& input) {
        const auto& s=input.sample;const auto& w=input.world;
        const bool same=valid_ && generation_==generation && entity_==entity
            && sample_.valid==s.valid && sample_.hasDirectionalColors==s.hasDirectionalColors
            && SameBits(sample_.primaryLightWeight,s.primaryLightWeight)
            && SameBits(sample_.color,s.color) && sample_.directionalColors==s.directionalColors
            && world_.valid==w.valid && SameBits(world_.sunDirection,w.sunDirection)
            && SameBits(world_.sunLightColor,w.sunLightColor);
        if(!same) {
            cache_.BeginSample();sample_=s;world_=w;generation_=generation;entity_=entity;valid_=true;
        }
        cache_.hits=cache_.misses=0;
        return !same;
    }
    template<class Evaluate> bool Resolve(const float* normal,float* response,Evaluate evaluate) {
        if(!valid_) return evaluate(normal,response);
        return cache_.Resolve(normal,response,evaluate);
    }
    uint32_t Hits() const { return cache_.hits; }
    uint32_t Misses() const { return cache_.misses; }
};
static_assert(sizeof(VitaViewModelLightCache)<256*1024);
}
