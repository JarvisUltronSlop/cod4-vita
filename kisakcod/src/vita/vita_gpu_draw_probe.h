#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace kisak::vita {
constexpr uint32_t VitaGpuDrawPhase(uint32_t ms) { return ms<20000?1:ms<160000?2:ms<180000?3:0; }
struct VitaGpuDrawKey { std::array<uint32_t,24> words{}; };
// Fullscreen programs use scene/dither bindings, not source material maps.
// Keep the observed GL draw/buffer identities; erase stale source state.
inline void VitaGpuDrawFullscreenInputs(VitaGpuDrawKey& key,uint32_t scene,uint32_t dither) {
    key.words[11]=UINT32_MAX;
    for(unsigned i=12;i<24;++i)key.words[i]=0;
    key.words[12]=scene;key.words[13]=dither;key.words[22]=256u;
}
// Renderer-owned, bounded storage. Keys contain values/immutable GL identities,
// never pointers into source sessions. Equality is checked after hash lookup.
struct VitaGpuDrawProbe {
    static constexpr uint32_t Capacity=2048, Batch=8;
    struct Entry { VitaGpuDrawKey key; const char* site=nullptr; };
    struct Sample {uint32_t id=0,ordinal=0;uint64_t submit=0,wait=0;};
    std::array<Entry,Capacity> entries{};
    std::array<uint32_t,Capacity*2> slots{};
    std::array<uint32_t,Capacity> order{};
    std::array<Sample,Batch> samples{};
    std::array<uint64_t,7> passWait{};
    uint32_t generation=0,phase=0,count=0,published=0,draws=0,previousDraws=0,step=0,first=0,sampleCount=0,invalid=0;
    uint64_t prefixWait=0,emptyWait=0;
    bool selected=false;
    static uint32_t Hash(const VitaGpuDrawKey& key) {
        uint32_t h=2166136261u;for(auto v:key.words){h^=v;h*=16777619u;}return h;
    }
    void Begin(uint32_t gen,uint32_t requestedPhase) {
        if(requestedPhase && (generation!=gen || !phase)) {
            slots.fill(0);count=published=previousDraws=step=invalid=0;generation=gen;
        }
        phase=requestedPhase;draws=sampleCount=0;selected=false;
        passWait.fill(0);prefixWait=emptyWait=0;
        const uint32_t groups=(previousDraws+Batch-1)/Batch;
        first=phase==2 && groups?(step++%groups)*Batch:0;
    }
    uint32_t Draw(const VitaGpuDrawKey& key,const char* site) {
        selected=false;
        if(!phase)return 0;
        if(draws==Capacity){invalid|=1;return 0;}
        uint32_t slot=Hash(key)&(slots.size()-1),id=0;
        for(uint32_t tries=0;tries<slots.size();++tries,slot=(slot+1)&(slots.size()-1)) {
            id=slots[slot];
            if(!id) {
                if(count==Capacity){invalid|=2;return 0;}
                entries[count]={key,site};id=slots[slot]=++count;break;
            }
            if(entries[id-1].key.words==key.words)break;
        }
        order[draws++]=id;
        selected=phase==2 && !invalid && draws-1>=first && draws-1<first+Batch;
        return id;
    }
    void End() {previousDraws=draws;selected=false;}
};
}
