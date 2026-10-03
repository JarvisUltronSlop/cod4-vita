#ifdef NDEBUG
#undef NDEBUG
#endif
#include <gfx_vita/gfx_vita_hud_shader.h>
#include <vita/vita_render_pass_probe.h>
#include <vita/vita_cpu_index.h>
#include <cassert>

int main() {
    // No state inherited from world, instancing, weapon, clear or quality
    // diagnostics may send a scene draw through the screen-space program.
    for (unsigned mask=0; mask<32; ++mask)
        for (unsigned mode=0; mode<8; ++mode) {
            const bool eligible=GfxVitaHudShaderEligible(mask&1,mask&2,mask&4,mask&8,mask&16,mode);
            assert(eligible==(mask==1 && mode==0));
            for (unsigned phase=0; phase<4; ++phase) {
                assert(!GfxVitaUseHudShader(eligible,false,true,phase));
                assert(GfxVitaUseHudShader(eligible,true,true,phase)==(eligible && phase==2));
                assert(GfxVitaUseHudShader(eligible,true,false,phase)==eligible);
            }
        }
    for (unsigned phase=1; phase<=3; ++phase) {
        kisak::vita::VitaRenderPassProbe p;p.Begin(kisak::vita::kVitaHudComparison|phase,0);
        assert(p.phase==phase && p.hud && !p.weaponOpaque && !p.fastColor && !p.Fenced());
        p.Begin(0,0);assert(!p.hud && !p.phase);
        for (auto flag : {kisak::vita::kVitaWeaponOpaqueComparison,kisak::vita::kVitaFastColorComparison,kisak::vita::kVitaGpuDrawProbe}) {
            p.Begin(kisak::vita::kVitaHudComparison|flag|phase,0);assert(!p.phase);
        }
    }
    // Positive and negative source material caches must not survive map
    // teardown even when the arena reuses exactly the same pointer.
    kisak::vita::VitaCpuPointerIndex<8> index;
    for (uintptr_t i=1;i<=8;++i) {
        auto* cell=index.Find(i,0);assert(cell && !cell->id);
        *cell={i,0,i&1?uint16_t(i):UINT16_MAX};
    }
    assert(!index.Find(9,0));
    for (uintptr_t i=1;i<=8;++i) assert(index.Find(i,0)->id==(i&1?i:UINT16_MAX));
    index.Reset();
    for (uintptr_t i=1;i<=8;++i) assert(index.Find(i,0) && !index.Find(i,0)->id);
}
