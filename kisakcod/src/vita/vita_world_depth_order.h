#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <new>
namespace kisak::vita {
struct VitaDepthRange {float nearDepth=0,farDepth=0;bool valid=false;};
class VitaDepthScratch {
    std::unique_ptr<VitaDepthRange[]> values;
    size_t capacity=0;
public:
    using Allocator=VitaDepthRange*(*)(size_t);
    bool Reserve(size_t count,Allocator allocate=nullptr) noexcept {
        if(count<=capacity)return true;
        if(count>SIZE_MAX/sizeof(VitaDepthRange))return false;
        std::unique_ptr<VitaDepthRange[]> next(allocate?allocate(count):new(std::nothrow) VitaDepthRange[count]);
        if(!next)return false;
        values=std::move(next);capacity=count;return true;
    }
    size_t size() const {return capacity;}
    VitaDepthRange* data(){return values.get();}
    VitaDepthRange& operator[](size_t index){return values[index];}
    void Clear(){values.reset();capacity=0;}
};
// Exact ordinary depth-write LESS/LEQUAL state only; stencil, bias, partial
// color writes, blend, cutout and unknown state are ordering barriers.
inline bool VitaDepthOrderState(bool valid,bool opaque,uint32_t bits0,uint32_t bits1) {
    return valid && opaque && (bits0&0x0800u) && !(bits0&0x07000700u)
        && (bits0&0x18000000u)==0x18000000u && (bits1==5u || bits1==13u);
}
inline VitaDepthRange VitaProjectDepthRange(const float* matrix,const float* lo,const float* hi) {
    VitaDepthRange result{};
    for(unsigned axis=0;axis<3;++axis)if(!std::isfinite(lo[axis]) || !std::isfinite(hi[axis]) || lo[axis]>hi[axis])return result;
    float near=1,far=-1;
    for(unsigned corner=0;corner<8;++corner) {
        const float x=corner&1?hi[0]:lo[0],y=corner&2?hi[1]:lo[1],z=corner&4?hi[2]:lo[2];
        const float w=matrix[3]*x+matrix[7]*y+matrix[11]*z+matrix[15];
        const float projected=matrix[2]*x+matrix[6]*y+matrix[10]*z+matrix[14];
        if(!std::isfinite(w) || !std::isfinite(projected) || w<=0)return result;
        const float depth=projected/w;
        if(!std::isfinite(depth) || depth<=-1 || depth>=1)return result;
        near=std::min(near,depth);far=std::max(far,depth);
    }
    return {near,far,true};
}
struct VitaDepthOrderStats {uint32_t swaps=0,comparisons=0;};
constexpr uint32_t kVitaDepthOrderComparisonLimit=32768;
// Adjacent swaps are safe only when the entire right surface is strictly
// nearer. Overlapping/coplanar ranges retain their original relative order;
// invalid ranges are barriers. Never use a non-transitive std::sort comparator.
inline VitaDepthOrderStats VitaOrderWorldDepth(uint32_t* order,size_t count,const VitaDepthRange* ranges) {
    VitaDepthOrderStats stats{};
    for(unsigned pass=0;pass<32 && count>1;++pass) {
        bool changed=false;
        for(size_t i=count-1;i>0;--i) {
            if(stats.comparisons==kVitaDepthOrderComparisonLimit)return stats;
            ++stats.comparisons;
            const auto& a=ranges[order[i-1]];const auto& b=ranges[order[i]];
            if(a.valid && b.valid && b.farDepth+0.0001f<a.nearDepth) {
                std::swap(order[i-1],order[i]);++stats.swaps;changed=true;
            }
        }
        if(!changed)break;
    }
    return stats;
}
inline uint32_t VitaDepthMembership(const uint32_t* values,size_t count) {
    uint32_t hash=0;for(size_t i=0;i<count;++i)hash+=(values[i]+1u)*2654435761u;return hash;
}
}
