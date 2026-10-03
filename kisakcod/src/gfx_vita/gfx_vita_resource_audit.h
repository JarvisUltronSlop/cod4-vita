#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Read-only GXP header interpretation, matching the retained Vita3K
// gxm/types.h layout. Static counts are not executed instructions or occupancy.
struct GfxVitaGxpAudit {
    const uint8_t* bytes=nullptr;
    uint32_t size=0,flags=0,primaryRegisters=0,secondaryRegisters=0;
    uint32_t temporaryRegisters=0,secondaryTemporaryRegisters=0;
    uint32_t primaryInstructions=0,secondaryInstructions=0;
    uint32_t primaryOffset=0,secondaryOffset=0,scratchBuffers=0,threadBuffers=0;
    uint32_t compilerVersion=0,textureFlags0=0,textureFlags1=0;
};
inline uint32_t GfxVitaAuditU32(const uint8_t* p) {
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}
inline uint32_t GfxVitaAuditU16(const uint8_t* p) {return uint32_t(p[0])|(uint32_t(p[1])<<8);}
inline bool GfxVitaParseGxp(const uint8_t* p,size_t available,GfxVitaGxpAudit& out) {
    out={};
    if(!p || available<164 || std::memcmp(p,"GXP\0",4) || p[4]!=1 || p[5]<4 || p[5]>5)return false;
    const uint32_t size=GfxVitaAuditU32(p+8);
    if(size<164 || size>available)return false;
    GfxVitaGxpAudit a{};a.bytes=p;a.size=size;a.flags=GfxVitaAuditU32(p+20);
    a.textureFlags0=GfxVitaAuditU32(p+28);a.textureFlags1=GfxVitaAuditU32(p+32);
    a.primaryRegisters=GfxVitaAuditU16(p+48);a.secondaryRegisters=GfxVitaAuditU16(p+50);
    a.temporaryRegisters=GfxVitaAuditU32(p+52);a.secondaryTemporaryRegisters=GfxVitaAuditU16(p+56);
    a.primaryInstructions=GfxVitaAuditU32(p+60);a.secondaryInstructions=GfxVitaAuditU32(p+68);
    const auto code=[&](uint32_t field,uint32_t count,uint32_t& offset) {
        if(!count){offset=0;return true;}
        const int64_t start=int64_t(field)+int32_t(GfxVitaAuditU32(p+field));
        if(start<164 || start>size || uint64_t(count)*8>uint64_t(size-start))return false;
        offset=uint32_t(start);return true;
    };
    if(!code(64,a.primaryInstructions,a.primaryOffset) || !code(72,a.secondaryInstructions,a.secondaryOffset))return false;
    a.scratchBuffers=GfxVitaAuditU32(p+80);a.threadBuffers=GfxVitaAuditU32(p+84);
    a.compilerVersion=GfxVitaAuditU32(p+108);out=a;return true;
}
// VitaGL serializes two length-prefixed shader containers after its attribute
// table. Locate one bounded GXP in each container, allowing its optional
// matrix/texture-size metadata without assuming a host struct packing layout.
inline bool GfxVitaParseProgramAudit(const uint8_t* p,size_t size,size_t prefix,GfxVitaGxpAudit (&out)[2]) {
    out[0]={};out[1]={};if(!p || prefix>size)return false;
    size_t at=prefix;GfxVitaGxpAudit parsed[2]{};
    for(unsigned stage=0;stage<2;++stage) {
        if(size-at<4)return false;
        const uint32_t length=GfxVitaAuditU32(p+at);at+=4;
        if(length>size-at || length<164)return false;
        unsigned matches=0;
        for(size_t i=0;i<=length-164;++i) {
            GfxVitaGxpAudit a{};
            if(GfxVitaParseGxp(p+at+i,length-i,a)) {parsed[stage]=a;++matches;}
        }
        if(matches!=1 || bool(parsed[stage].flags&1u)!=bool(stage))return false;
        at+=length;
    }
    if(at!=size)return false;
    out[0]=parsed[0];out[1]=parsed[1];return true;
}

struct GfxVitaResourceAuditGate {
    uint32_t generation=0,seen=0;
    bool Take(uint32_t gen,uint32_t phase,uint32_t surface,bool staticWorld,uint32_t phases=3) {
        if(!staticWorld || phases<1 || phases>7 || phase<1 || phase>phases)return false;
        const unsigned target=surface==261?0:surface==507?1:surface==588?2:3;
        if(target==3)return false;
        if(gen!=generation){generation=gen;seen=0;}
        const uint32_t bit=1u<<((phase-1)*3+target);
        if(seen&bit)return false;
        seen|=bit;return true;
    }
};
