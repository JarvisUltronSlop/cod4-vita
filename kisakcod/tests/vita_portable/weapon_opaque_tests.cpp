#ifdef NDEBUG
#undef NDEBUG
#endif
#include <gfx_vita/gfx_vita_weapon_opaque.h>
#include <vita/vita_render_pass_probe.h>
#include <cassert>
int main() {
    // Exhaust the state gate, including unknown material state, transparency,
    // non-viewmodel geometry and diagnostic overrides.
    constexpr unsigned eligible=(1u<<0)|(1u<<1)|(1u<<4)|(1u<<7)|(1u<<8);
    for(unsigned mask=0;mask<2048;++mask) {
        bool v[11];for(unsigned i=0;i<11;++i)v[i]=(mask&(1u<<i))!=0;
        for(unsigned mode=0;mode<8;++mode)
            assert(GfxVitaWeaponOpaqueEligible(v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9],v[10],mode)
                == (mask==eligible && mode==0));
    }
    for(unsigned phase=1;phase<=3;++phase) {
        kisak::vita::VitaRenderPassProbe p;p.Begin(kisak::vita::kVitaWeaponOpaqueComparison|phase,0);
        assert(p.phase==phase && p.weaponOpaque && !p.Fenced());
        p.Begin(0,0);assert(!p.weaponOpaque && !p.phase);
    }
    kisak::vita::VitaRenderPassProbe p;p.Begin(kisak::vita::kVitaWeaponOpaqueComparison|kisak::vita::kVitaFastColorComparison|2,0);assert(!p.phase);
}
