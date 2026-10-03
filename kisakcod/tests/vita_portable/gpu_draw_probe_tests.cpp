#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_render_pass_probe.h>
#include <cassert>
#include <array>
using namespace kisak::vita;
static VitaGpuDrawProbe probe;
int main() {
    for(uint32_t ms=0;ms<=200000;++ms) {
        const auto phase=VitaGpuDrawPhase(ms);VitaRenderPassProbe p;p.Begin(kVitaGpuDrawProbe+phase,0);
        assert(phase==(ms<20000?1u:ms<160000?2u:ms<180000?3u:0u));
        assert(p.drawProbe==(phase!=0));assert(!p.Fenced() && !p.ClipProps() && !p.SuppressProps());
        p.Begin(kVitaFastBaselineDrawProbe+phase,0);
        assert(p.fastDrawProbe==(phase!=0) && p.drawProbe==(phase!=0));
        assert(!p.earlyAlpha && !p.weaponOpaque && !p.hud && !p.Fenced());
        p.Begin(0,0);assert(!p.fastDrawProbe && !p.drawProbe);
    }
    for(unsigned phase=1;phase<=3;++phase)for(auto other:{kVitaGpuDrawProbe,kVitaFastColorComparison,kVitaEarlyAlphaComparison,kVitaHudComparison}) {
        VitaRenderPassProbe p;p.Begin(kVitaFastBaselineDrawProbe|other|phase,0);assert(!p.phase && !p.drawProbe && !p.fastDrawProbe);
    }
    for(unsigned phase=1;phase<=3;++phase) {
        const auto mode=kVitaCurrentDrawProbe|phase;
        VitaRenderPassProbe p;p.Begin(mode,0);
        assert(p.currentDrawProbe && p.drawProbe && p.worldAlpha && p.weaponSpecular && p.weaponVertex && p.weaponPipeline);
        assert(!p.Fenced() && !p.ClipProps() && !p.SuppressProps());
        assert(VitaWorldAlphaEnabled(mode) && VitaWeaponSpecularEnabled(mode) && VitaWeaponVertexEnabled(mode) && VitaWeaponPipelineEnabled(mode));
        for(auto other:{kVitaWorldAlphaComparison,kVitaGpuDrawProbe,kVitaFastBaselineDrawProbe}) {
            p.Begin(mode|other,0);assert(!p.phase && !p.currentDrawProbe);
        }
    }
    for(auto mode:{0u,kVitaCurrentDrawProbe,kVitaCurrentDrawProbe|4u}) {
        VitaRenderPassProbe p;p.Begin(mode,0);assert(!p.currentDrawProbe && !VitaWorldAlphaEnabled(mode));
    }
    VitaGpuDrawKey fullscreen;fullscreen.words.fill(77);const auto old=fullscreen;
    VitaGpuDrawFullscreenInputs(fullscreen,22,33);
    for(unsigned i=0;i<11;++i)assert(fullscreen.words[i]==old.words[i]);
    assert(fullscreen.words[11]==UINT32_MAX && fullscreen.words[12]==22 && fullscreen.words[13]==33 && fullscreen.words[22]==256);
    for(unsigned i=14;i<24;++i)if(i!=22)assert(!fullscreen.words[i]);
    VitaGpuDrawFullscreenInputs(fullscreen,44,0);assert(fullscreen.words[12]==44 && !fullscreen.words[13]);
    std::array<unsigned,501> coverage{};
    for(unsigned frame=0;frame<128;++frame) {
        probe.Begin(1,frame==0?1:frame==127?3:2);
        for(unsigned ordinal=0;ordinal<501;++ordinal) {
            VitaGpuDrawKey key;key.words[1]=ordinal;
            auto id=probe.Draw(key,"TestDraw");assert(id==ordinal+1);
            if(probe.selected) {assert(probe.sampleCount<8);++probe.sampleCount;++coverage[ordinal];}
        }
        assert(probe.count==501 && !probe.invalid);
        assert(probe.sampleCount==(frame==0 || frame==127?0:probe.first==496?5:8));
        probe.End();
    }
    for(auto n:coverage)assert(n==2); // complete rotating coverage incl. short last block
    probe.Begin(2,1);assert(probe.count==0 && probe.step==0 && !probe.invalid);
    // Force two distinct keys into the same hash slot; compare full keys.
    VitaGpuDrawKey a,b;a.words[0]=1;
    for(unsigned i=2;;++i) {b.words[0]=i;if((VitaGpuDrawProbe::Hash(a)&4095)==(VitaGpuDrawProbe::Hash(b)&4095))break;}
    assert(probe.Draw(a,"A")==1 && probe.Draw(b,"B")==2 && probe.Draw(a,"A")==1);
    probe.Begin(3,1);
    for(unsigned i=0;i<VitaGpuDrawProbe::Capacity;++i){VitaGpuDrawKey k;k.words[1]=i;assert(probe.Draw(k,"D")==i+1);}
    VitaGpuDrawKey extra;extra.words[1]=99999;assert(probe.Draw(extra,"D")==0 && probe.invalid);
    probe.End();probe.Begin(3,2);assert(probe.Draw(extra,"D")==0 && probe.invalid && !probe.selected);
    probe.End();probe.Begin(3,0);assert(probe.Draw(a,"D")==0 && !probe.selected);
    probe.Begin(3,1);assert(!probe.invalid && probe.count==0 && probe.published==0);
    // Bounded report fits the existing renderer event queue before normal
    // per-frame events: order512 + descriptors128 + samples16 + summary12.
    static_assert(VitaGpuDrawProbe::Capacity/4+32*4+VitaGpuDrawProbe::Batch*2+12<800);
}
