#ifdef NDEBUG
#undef NDEBUG
#endif
#include <gfx_vita/gfx_vita_composition.h>
#include <vita/vita_render_pass_probe.h>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#ifdef _WIN32
#include <crtdbg.h>
#endif
int main(int argc,char** argv) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0,_WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    // Baseline and fused draw initialize identical colour/far depth even with
    // stale near depth and masked state. Simulate viewport depth transformation;
    // driver coverage/precision is deliberately left to physical validation.
    for(bool fused:{false,true})for(float nearDepth:{0.0f,0.015625f,0.25f})for(float farDepth:{0.75f,1.0f}) {
        float range[2]{nearDepth,farDepth};float depth=fused?0.0f:farDepth;
        float color=-1.0f;unsigned draws=0,reads=0,sets=0;bool always=false,write=false;
        GfxVitaCompositionDepth(fused,
            [&](float* out){++reads;out[0]=range[0];out[1]=range[1];},
            [&](float a,float b){++sets;range[0]=a;range[1]=b;},
            [&](bool a){always=a;write=true;},
            [&]{++draws;color=0.37f;if(fused){assert(always && write);depth=(range[0]+range[1])*0.5f;}});
        assert(color==0.37f && depth==farDepth && draws==1);
        assert(range[0]==nearDepth && range[1]==farDepth);
        assert(reads==(fused?1u:0u) && sets==(fused?2u:0u));
        if(fused)assert(!always && write);
        // A weapon fragment nearer than far now passes in either path.
        assert((nearDepth+farDepth)*0.5f<=depth);
    }
    for(bool fused:{false,true}) {
        float before[2]{0.015625f,1},after[2]{0.015625f,1};
        GfxVitaCompositionState state;state.range[0]=fused?1:before[0];state.range[1]=1;
        state.depth=state.write=state.always=fused;state.colorMask=15;
        assert(GfxVitaCompositionCheck(fused,before,state,after)==0);
        after[0]=0;assert(GfxVitaCompositionCheck(fused,before,state,after)==32);after[0]=before[0];
        state.colorMask=7;assert(GfxVitaCompositionCheck(fused,before,state,after)==8);state.colorMask=15;
        state.range[0]=0;assert(GfxVitaCompositionCheck(fused,before,state,after)==2);state.range[0]=fused?1:before[0];
        state.write=!fused;assert(GfxVitaCompositionCheck(fused,before,state,after)==4);state.write=fused;
        state.scissor=true;assert(GfxVitaCompositionCheck(fused,before,state,after)==16);state.scissor=false;
        before[0]=NAN;assert(GfxVitaCompositionCheck(fused,before,state,after)&1);
    }
    using namespace kisak::vita;
    for(unsigned phase=1;phase<=6;++phase) {
        VitaRenderPassProbe profile;profile.Begin(kVitaGraphicsProfile|phase,0);
        assert(profile.WorldAlphaActive() && !profile.Fenced());
        if(phase<=3) {
            VitaRenderPassProbe p;p.Begin(kVitaCompositionComparison|phase,0);
            assert(p.phase==phase && p.composition && p.WorldAlphaActive() && p.weaponPipeline && !p.Fenced());
            assert(!p.worldComponents && !p.graphicsProfile && !p.worldMath && !p.worldBasis);
            assert(VitaCompositionCandidate(p.requestedPhase)==(phase==2));
            assert(VitaWeaponSpecularEnabled(p.requestedPhase) && VitaWeaponVertexEnabled(p.requestedPhase));
        }
    }
    for(auto tag:{0u,kVitaCompositionComparison,kVitaCompositionComparison+4u,0xffffffffu}) {
        assert(!VitaCompositionMode(tag));VitaRenderPassProbe p;p.Begin(tag,0);assert(!p.composition);
    }
    // Protect both actual shader selection and its validation from the run67
    // duplicated-list omission, rather than testing only mode predicates.
    assert(argc==2);std::ifstream in(argv[1]);assert(in);
    std::string s((std::istreambuf_iterator<char>(in)),{});
    auto selection=s.substr(s.find("void SelectNativeLightShader"));selection=selection.substr(0,selection.find("void ",6));
    assert(selection.find("g_renderPipeline.worldAlphaActive")!=std::string::npos);
    assert(s.find("if(actual!=(worldAlphaEligible && g_renderPipeline.worldAlphaActive))")!=std::string::npos);
    auto begin=s.find("void GfxVita_BeginNativeViewmodel()");auto end=s.find("bool GfxVita_PrepareLightmapLayouts",begin);
    auto copy=s.substr(begin,end-begin);
    for(const char* required:{"if (!fused) Clear(0,0,0,1)","SetSurfaceMode(g_nativeSurfaceMode)",
         "GfxVitaCompositionDepth(fused", "glGetFloatv(GL_DEPTH_RANGE,range)",
         "glDepthRangef(nearDepth,farDepth)","glDepthFunc(always?GL_ALWAYS:GL_LEQUAL)",
         "render-probe-composition-valid"})assert(copy.find(required)!=std::string::npos);
    assert(copy.find("glFinish")==std::string::npos);
}
