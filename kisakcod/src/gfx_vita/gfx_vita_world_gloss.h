#pragma once
#include "gfx_vita_material_family.h"
#include <cstddef>
#include <cstdint>

// A deliberately narrow certificate: complete BC3 mip chain, with every
// stored texel selecting an exact 233 endpoint. Reject interpolated palette
// entries even when a software decoder rounds them to 233. This also checks
// padding texels and the authored tail levels that VitaGL does not upload.
inline bool GfxVitaConstantGloss233(const uint8_t* data,size_t bytes,
    uint32_t width,uint32_t height,uint32_t levels,uint32_t format,uint32_t flags) {
    if(!data || format!=0x35545844u || (flags&4u) || !width || !height
        || width>4096 || height>4096 || (width&(width-1)) || (height&(height-1)))return false;
    uint32_t complete=1;
    for(uint32_t size=width>height?width:height;size>1;size>>=1)++complete;
    if(levels!=complete)return false;
    size_t offset=0;
    for(uint32_t level=0;level<levels;++level) {
        const size_t blocks=size_t((width+3)/4)*((height+3)/4);
        if(offset>bytes || blocks>(bytes-offset)/16)return false;
        for(size_t block=0;block<blocks;++block) {
            const uint8_t* p=data+offset+block*16;
            uint64_t selectors=0;
            for(unsigned i=0;i<6;++i)selectors|=uint64_t(p[2+i])<<(8*i);
            for(unsigned texel=0;texel<16;++texel,selectors>>=3) {
                const unsigned selector=selectors&7;
                if(selector>1 || p[selector]!=233)return false;
            }
        }
        offset+=blocks*16;
        width=width>1?width/2:1;height=height>1?height/2:1;
    }
    return offset==bytes;
}

inline bool GfxVitaWorldGlossEligible(bool staticWorld,bool viewmodel,bool instanced,
    bool opaqueFamily,bool certified) {
    return staticWorld && !viewmodel && !instanced && opaqueFamily && certified;
}

inline bool GfxVitaBuildWorldGloss(const char* original,std::string& result) {
    result.clear();if(!original)return false;std::string source;
    if(!GfxVitaBuildMaterialFamily(original,1,source))return false;
    const std::string from="exp(clamp(mask.a, 0.0, 1.0) * 9.3775177)";
    const auto at=source.find(from);
    if(at==std::string::npos || source.find(from,at+from.size())!=std::string::npos)return false;
    // Let the pinned shader compiler fold the original highp expression.
    // No host-libm approximation, coefficient fusion or reduced precision.
    source.replace(at,from.size(),"exp(clamp(233.0 / 255.0, 0.0, 1.0) * 9.3775177)");
    result.swap(source);return true;
}
