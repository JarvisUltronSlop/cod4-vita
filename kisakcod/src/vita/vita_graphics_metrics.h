#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
namespace kisak::vita {
// Decode only the ABI declared by the pinned SDK. Other layouts are unavailable,
// never reinterpreted as this one. No unaligned access or SDK-owned pointers escape.
struct VitaGraphicsMetric {
    uint32_t kind=0,type=0,frame=0,scene=0,process=0,core=0;
    uint64_t start=0,end=0,a=0,b=0,c=0;
};
template<class T> inline T VitaMetricRead(const uint8_t* p) {T value;std::memcpy(&value,p,sizeof(value));return value;}
template<class Emit> bool VitaGraphicsMetricsParse(const void* data,uint32_t bytes,uint32_t count,Emit emit) {
    if(count>8192 || bytes>65536 || (count && !data))return false;
    const auto* p=static_cast<const uint8_t*>(data);uint32_t at=0;
    // Validate the whole batch first, so malformed tails cannot publish partial evidence.
    for(unsigned pass=0;pass<2;++pass) {
        at=0;
        for(uint32_t i=0;i<count;++i) {
            if(at>bytes || bytes-at<8)return false;
            const auto* q=p+at;const auto size=VitaMetricRead<uint16_t>(q),kind=VitaMetricRead<uint16_t>(q+2);
            if(size>bytes-at || (kind==0?size!=72:kind==1?size!=24:kind==2?size!=32:true))return false;
            VitaGraphicsMetric m;m.kind=kind;
            if(kind==0) {
                m.start=VitaMetricRead<uint64_t>(q+8);m.end=VitaMetricRead<uint64_t>(q+16);
                m.type=q[24];m.core=q[25];m.scene=VitaMetricRead<uint16_t>(q+26);m.frame=VitaMetricRead<uint32_t>(q+28);m.process=VitaMetricRead<uint32_t>(q+64);
                if(m.end<m.start || m.type>8)return false;
                m.a=VitaMetricRead<uint32_t>(q+32);m.b=VitaMetricRead<uint32_t>(q+36);m.c=VitaMetricRead<uint32_t>(q+40);
                if(m.type==1 || m.type==2) {
                    for(unsigned j=0;j<(m.type==1?1u:3u);++j) {
                        const float v=VitaMetricRead<float>(q+32+j*4);
                        if(!std::isfinite(v) || v<0 || v>100)return false;
                    }
                }
            } else if(kind==1) {
                m.start=VitaMetricRead<uint64_t>(q+8);m.a=VitaMetricRead<uint32_t>(q+16);m.b=q[20];m.c=q[21];
                if(m.b>1 || m.c>1)return false;
            } else {
                m.start=VitaMetricRead<uint64_t>(q+8);m.end=m.start+VitaMetricRead<uint32_t>(q+16);m.frame=VitaMetricRead<uint32_t>(q+20);m.a=VitaMetricRead<uint32_t>(q+24);
                if(m.end<m.start)return false;
            }
            if(pass)emit(m);at+=size;
        }
    }
    return true;
}
}
