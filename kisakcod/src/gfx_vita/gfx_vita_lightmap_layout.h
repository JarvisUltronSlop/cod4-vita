#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

inline bool GfxVitaSamplerUnitValid(bool software,uint32_t unit) {return unit<(software?4u:5u);}
inline bool GfxVitaLightmapDimensions(uint32_t w,uint32_t h,uint32_t bpp) {
    return w>=8 && h>=8 && w<=4096 && h<=4096 && !(w&(w-1)) && !(h&(h-1)) && (bpp==1 || bpp==4);
}
// GXM Morton order: Y occupies even bits, X odd bits. Rectangles contain
// square Morton blocks along their longer dimension (matching pinned VitaGL).
inline size_t GfxVitaLightmapTexel(uint32_t x,uint32_t y,uint32_t w,uint32_t h) {
    const uint32_t side=w<h?w:h;
    size_t index=size_t(y/side)*(w/side)*side*side+size_t(x/side)*side*side;
    x%=side;y%=side;
    for(uint32_t bit=1,shift=0;bit<side;bit<<=1,shift+=2) {
        index|=size_t((y&bit)!=0)<<shift;
        index|=size_t((x&bit)!=0)<<(shift+1);
    }
    return index;
}
inline bool GfxVitaSwizzleLightmap(void* dst,size_t dstBytes,const void* src,size_t srcBytes,
                                  uint32_t w,uint32_t h,uint32_t bpp,uint32_t stride) {
    if(!dst || !src || !GfxVitaLightmapDimensions(w,h,bpp) || stride<w*bpp)return false;
    const size_t size=size_t(w)*h*bpp,readSize=size_t(stride)*(h-1)+size_t(w)*bpp;
    if(dstBytes<size || srcBytes<readSize)return false;
    const auto d=reinterpret_cast<uintptr_t>(dst),s=reinterpret_cast<uintptr_t>(src);
    if((d>=s && d-s<readSize) || (s>=d && s-d<size))return false;
    auto* output=static_cast<uint8_t*>(dst);const auto* input=static_cast<const uint8_t*>(src);
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x)
        std::memcpy(output+GfxVitaLightmapTexel(x,y,w,h)*bpp,input+size_t(y)*stride+x*bpp,bpp);
    return true;
}
inline bool GfxVitaVerifyLightmap(const void* swizzled,const void* linear,uint32_t w,uint32_t h,uint32_t bpp,uint32_t stride) {
    if(!swizzled || !linear || !GfxVitaLightmapDimensions(w,h,bpp) || stride<w*bpp)return false;
    const auto* a=static_cast<const uint8_t*>(swizzled);const auto* b=static_cast<const uint8_t*>(linear);
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x)
        if(std::memcmp(a+GfxVitaLightmapTexel(x,y,w,h)*bpp,b+size_t(y)*stride+x*bpp,bpp))return false;
    return true;
}

// A test either owns the whole bounded comparison set or none of it.
// Call only on the render owner, after required loading and GPU drain.
template<typename Prepare,typename Rollback>
bool GfxVitaPrepareLightmapBatch(const uint32_t* leases,size_t count,Prepare prepare,Rollback rollback) {
    if(!leases || !count || count>8){rollback();return false;}
    for(size_t i=0;i<count;++i) {
        if(!leases[i]){rollback();return false;}
        for(size_t j=0;j<i;++j)if(leases[j]==leases[i]){rollback();return false;}
    }
    for(size_t i=0;i<count;++i)if(!prepare(leases[i])){rollback();return false;}
    return true;
}


// One bit per texel, reused for every image. No second pixel allocation.
constexpr size_t kGfxVitaLightmapWorkspaceBytes=128u*1024u;
inline bool GfxVitaReorderLightmap(void* pixels,size_t bytes,uint32_t w,uint32_t h,uint32_t bpp,
                                 bool swizzle,void* workspace,size_t workspaceBytes) {
    if(!pixels || !workspace || !GfxVitaLightmapDimensions(w,h,bpp))return false;
    const size_t count=size_t(w)*h,needed=(count+7)/8,payload=count*bpp;
    if(needed>kGfxVitaLightmapWorkspaceBytes || workspaceBytes<needed || bytes<payload)return false;
    const auto a=reinterpret_cast<uintptr_t>(pixels),b=reinterpret_cast<uintptr_t>(workspace);
    if((a>=b && a-b<needed) || (b>=a && b-a<payload))return false;
    auto* data=static_cast<uint8_t*>(pixels);auto* seen=static_cast<uint8_t*>(workspace);
    std::memset(seen,0,needed);
    for(size_t start=0;start<count;++start) {
        if(seen[start/8]&(1u<<(start%8)))continue;
        uint8_t carry[4];std::memcpy(carry,data+start*bpp,bpp);
        size_t current=start;
        do {
            const size_t next=GfxVitaLightmapTexel(uint32_t(current%w),uint32_t(current/w),w,h);
            seen[current/8]|=uint8_t(1u<<(current%8));
            if(swizzle) {
                for(uint32_t byte=0;byte<bpp;++byte){const auto value=data[next*bpp+byte];data[next*bpp+byte]=carry[byte];carry[byte]=value;}
            } else {
                std::memcpy(data+current*bpp,next==start?carry:data+next*bpp,bpp);
            }
            current=next;
        }while(current!=start);
    }
    return true;
}

#include <vita/vita_sha256.h>
inline kisak::vita::VitaSha256::Digest GfxVitaLightmapHash(const void* pixels,uint32_t w,uint32_t h,uint32_t bpp,bool swizzled) {
    const auto* bytes=static_cast<const uint8_t*>(pixels);
    if(!swizzled)return kisak::vita::VitaSha256::Hash(bytes,size_t(w)*h*bpp);
    kisak::vita::VitaSha256 hash;uint8_t chunk[1024];size_t used=0;
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
        std::memcpy(chunk+used,bytes+GfxVitaLightmapTexel(x,y,w,h)*bpp,bpp);used+=bpp;
        if(used==sizeof(chunk)){hash.Update(chunk,used);used=0;}
    }
    if(used)hash.Update(chunk,used);
    return hash.Finish();
}

#include <atomic>
// One renderer writer, source readers. The sequence protects a consistent
// pair without holding a lock during conversion. Source deadlines keep running.
class GfxVitaLightmapTransitionClock {
    std::atomic<uint32_t> sequence_{0};
    std::atomic<uint64_t> total_{0},start_{0};
public:
    void Reset(){++sequence_;total_=0;start_=0;++sequence_;}
    void Begin(uint64_t now){++sequence_;start_=now;++sequence_;}
    void End(uint64_t now){++sequence_;const auto start=start_.load();total_=total_.load()+(now>start?now-start:0);start_=0;++sequence_;}
    uint64_t PausedUs(uint64_t now)const {
        for(;;) {
            const auto before=sequence_.load();if(before&1u)continue;
            const auto total=total_.load(),start=start_.load();
            if(before==sequence_.load())return total+(start && now>start?now-start:0);
        }
    }
};
