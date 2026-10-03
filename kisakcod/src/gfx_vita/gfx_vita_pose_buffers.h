#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

// The pinned VitaGL SubData implementation retires buffers for four swaps.
// Six physical buffers let a once-per-frame writer return after five swaps.
// We still call SubData: its own reader protection remains authoritative.
class GfxVitaPoseBuffers {
    struct Slot {uint32_t handle=0;uint64_t drawnAt=0;bool drawn=false;};
    struct Ring {
        uint32_t root=0,bytes=0,current=0;
        std::array<Slot,6> slots{};
        std::unique_ptr<uint8_t[]> shadow;
    };
    std::array<Ring,16> rings_{};
    uint64_t extraBytes_=0,shadowBytes_=0;
public:
    struct Upload {uint32_t handle,bytes;const void* data;bool rotated=false,fallback=false;};
    static constexpr uint64_t Budget=6u*1024u*1024u;
    template<class Create,class Release>
    bool Reserve(uint32_t root,uint32_t bytes,Create create,Release release) {
        if(!root || !bytes || uint64_t(bytes)*5>Budget-extraBytes_)return false;
        Ring* target=nullptr;
        for(auto& r:rings_) {if(r.root==root)return false;if(!r.root && !target)target=&r;}
        if(!target)return false;
        Ring pending;pending.root=root;pending.bytes=bytes;pending.slots[0].handle=root;
        pending.shadow.reset(new(std::nothrow) uint8_t[bytes]{});
        if(!pending.shadow)return false;
        for(unsigned i=1;i<6;++i) {
            pending.slots[i].handle=create(bytes);
            if(!pending.slots[i].handle) {
                for(unsigned j=1;j<i;++j)release(pending.slots[j].handle);
                return false;
            }
        }
        extraBytes_+=uint64_t(bytes)*5;shadowBytes_+=bytes;*target=std::move(pending);return true;
    }
    Upload Prepare(uint32_t root,const void* data,uint32_t bytes,uint64_t swaps) {
        for(auto& r:rings_)if(r.root==root) {
            if(!data || bytes>r.bytes)return {0,0,nullptr};
            // Preserve bytes outside a partial update across physical slots.
            std::memcpy(r.shadow.get(),data,bytes);
            const auto previous=r.current;
            bool found=false;
            for(unsigned i=0;i<6;++i) {
                const unsigned candidate=(previous+i)%6;const auto& s=r.slots[candidate];
                if(!s.drawn || (swaps>=s.drawnAt && swaps-s.drawnAt>4)) {r.current=candidate;found=true;break;}
            }
            // Unexpected pressure uses the driver's safe replacement path,
            // never overwrites mapped storage or drops a simulation command.
            return {r.slots[r.current].handle,r.bytes,r.shadow.get(),r.current!=previous,!found};
        }
        return {root,bytes,data,false,true};
    }
    uint32_t Draw(uint32_t root,uint64_t swaps) {
        for(auto& r:rings_)if(r.root==root) {
            auto& s=r.slots[r.current];s.drawn=true;s.drawnAt=swaps;return s.handle;
        }
        return root;
    }
    template<class Release>void ReleaseExtra(uint32_t root,Release release) {
        for(auto& r:rings_)if(r.root==root) {
            for(unsigned i=1;i<6;++i)release(r.slots[i].handle);
            extraBytes_-=uint64_t(r.bytes)*5;shadowBytes_-=r.bytes;r={};return;
        }
    }
    template<class Release>void Clear(Release release) {
        for(auto& r:rings_)if(r.root)ReleaseExtra(r.root,release);
    }
    uint64_t ExtraBytes()const{return extraBytes_;}
    uint64_t ShadowBytes()const{return shadowBytes_;}
};
