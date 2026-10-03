#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace kisak::vita {
enum class VitaTextureRole : uint32_t { Protected=0, World=1, Viewmodel=2 };
struct VitaTextureMipSelection {
    uint32_t width=0, height=0, levels=0;
    size_t offset=0, bytes=0;
    bool valid=false, reduced=false;
};
inline size_t VitaTextureMipBytes(uint32_t format, uint32_t w, uint32_t h) {
    if (format==21) return size_t(w)*h*4;
    if (format==50) return size_t(w)*h;
    if (format==0x31545844u) return size_t((w+3)/4)*((h+3)/4)*8;
    if (format==0x33545844u || format==0x35545844u)
        return size_t((w+3)/4)*((h+3)/4)*16;
    return 0;
}
// Select an existing authored mip, never resample/decode or change its format.
// Cubes, tiny images and images without a complete next mip remain unchanged.
inline VitaTextureMipSelection VitaSelectBenchmarkMip(uint32_t mode, VitaTextureRole role,
    uint32_t w, uint32_t h, uint32_t levels, uint32_t format, uint32_t flags, size_t bytes) {
    VitaTextureMipSelection s{w,h,levels,0,bytes};
    if (!w || !h || w>4096 || h>4096 || !levels || levels>16 || mode>3) return s;
    size_t required=0;
    uint32_t mw=w,mh=h;
    for (uint32_t i=0;i<levels;++i) {
        const size_t n=VitaTextureMipBytes(format,mw,mh);
        if (!n || required>bytes || n>bytes-required) return s;
        required+=n; mw=std::max(1u,mw/2); mh=std::max(1u,mh/2);
    }
    if ((flags&4u) && required>bytes/6) return s;
    s.valid=true;
    const bool requested=(role==VitaTextureRole::World && (mode&1))
        || (role==VitaTextureRole::Viewmodel && (mode&2));
    if (!requested || (flags&4u) || levels<2 || w<8 || h<8) return s;
    s.offset=VitaTextureMipBytes(format,w,h);
    s.bytes=bytes-s.offset; s.width=std::max(1u,w/2); s.height=std::max(1u,h/2);
    s.levels=levels-1; s.reduced=true;
    return s;
}
struct VitaBenchmarkTextureStats {
    uint32_t mode=0, failures=0;
    uint32_t attempts[3]{}, reduced[3]{};
    uint64_t sourceBytes[3]{}, selectedBytes[3]{};
};
} // namespace kisak::vita
