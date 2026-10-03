#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <gfx_vita/gfx_vita_world_scaling.h>
#include <gfx_vita/gfx_vita_resource_audit.h>
#include <gfx_vita/gfx_vita_world_lighting_policy.h>
using namespace kisak::vita;
int main(int argc,char** argv) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
#endif
    // A menu preference affects ordinary world draws only. Explicit diagnostic
    // phases always win, including their baseline and unavailable-shader paths.
    for(unsigned ready=0;ready<4;++ready)for(bool eligible:{false,true}) {
        const bool expected=ready==3 && eligible;
        assert(GfxVitaUseSimpleWorld(true,true,false,0,false,0,ready,true,eligible)==expected);
        assert(!GfxVitaUseSimpleWorld(true,false,false,0,false,0,ready,true,eligible));
        assert(!GfxVitaUseSimpleWorld(false,true,false,0,false,0,ready,true,eligible));
        assert(!GfxVitaUseSimpleWorld(true,true,true,0,false,0,ready,true,eligible));
        assert(!GfxVitaUseSimpleWorld(true,true,false,0,false,0,ready,false,eligible));
        assert(!GfxVitaUseSimpleWorld(true,true,false,kVitaPassIsolation|1,false,1,ready,true,eligible));
        for(unsigned phase=1;phase<=7;++phase)for(bool requested:{false,true})
            assert(GfxVitaUseSimpleWorld(true,requested,false,kVitaWorldScaling|phase,true,phase,ready,true,eligible)==(expected && phase==4));
    }
    assert(argc==2);std::ifstream f(argv[1]);assert(f);
    std::string text((std::istreambuf_iterator<char>(f)),{});
    for(size_t p=0;(p=text.find('\r',p))!=std::string::npos;)text.erase(p,1);
    auto a=text.find("kNativeLitFragmentShader = R\"VITA_SHADER(");assert(a!=std::string::npos);a=text.find('(',a)+1;
    const auto b=text.find(")VITA_SHADER\"",a);assert(b!=std::string::npos);const auto original=text.substr(a,b-a);
    for(bool alpha:{false,true}) {
        std::string out;assert(GfxVitaBuildCheapWorld(original.c_str(),alpha,out));
        assert(out.find("vec4 material = v_color;")!=std::string::npos);
        assert(out.find("material *= texture2D(u_color, v_texcoord);")!=std::string::npos);
        assert(out.substr(out.find("#ifndef VITA_NO_FOG"))==original.substr(original.find("#ifndef VITA_NO_FOG")));
        assert(GfxVitaShaderHasDiscard(out)==alpha);
        if(alpha)assert(out.find(GfxVitaOpaqueAlphaBlock())!=std::string::npos);
        for(const char* gone:{"texture2D(u_normal,", "texture2D(u_specular,", "textureCube(", "sourceLightmapNormalXY", "pow(", "sqrt("})assert(out.find(gone)==std::string::npos);
        assert(out.find("texture2D(u_lightmap_primary,")!=std::string::npos);
        assert(out.find("texture2D(u_lightmap_secondary,")!=std::string::npos);
        auto broken=original;broken.erase(broken.find("#ifndef VITA_NO_FOG"),19);
        assert(!GfxVitaBuildCheapWorld(broken.c_str(),alpha,out) && out.empty());
    }
    std::string out="stale";assert(!GfxVitaBuildCheapWorld(nullptr,false,out) && out.empty());
    struct Target {unsigned framebuffer,width,height;};Target active{10,720,408},spare{20,480,272};bool low=false;
    for(unsigned phase=0;phase<9;++phase) {
        const unsigned tag=kVitaWorldScaling|phase;const bool valid=phase>=1 && phase<=7;
        VitaRenderPassProbe probe;probe.Begin(tag,0);assert(probe.worldScaling==valid);
        assert(!probe.Fenced() && !probe.passIsolation && !probe.worldPrecision && !probe.worldComponents);
        assert(VitaWorldAlphaEnabled(tag)==valid && VitaWeaponVertexEnabled(tag)==valid && VitaWeaponSpecularEnabled(tag)==valid);
        assert(GfxVitaWorldScalingTarget(tag,active,spare,low));
        assert(low==(phase==6));assert(active.framebuffer==(low?20u:10u));
        GfxVitaWorldScalingState state;state.Begin(tag);
        assert(state.Draw(true,false,0,0,3,0)==(phase!=2));
        assert(state.skySuppressed==(phase==2?1u:0u));
        for(auto surface:{261u,507u,588u})assert(state.Draw(false,true,surface==588?2:1,phase==4?(surface==588?2:1):0,3,surface));
        assert(state.invalid==0);if(valid){assert(state.eligible==3 && state.targetEligible==7);assert(state.actual==(phase==4?3u:0u));}
    }
    assert(VitaWorldScalingPhase(0)==1 && VitaWorldScalingPhase(119999)==6 && VitaWorldScalingPhase(120000)==7 && VitaWorldScalingPhase(140000)==0);
    // Cancellation during low resolution restores original descriptors.
    assert(GfxVitaWorldScalingTarget(kVitaWorldScaling|6,active,spare,low));
    assert(GfxVitaWorldScalingTarget(0,active,spare,low) && !low && active.framebuffer==10);
    auto missing=spare;missing.framebuffer=0;assert(!GfxVitaWorldScalingTarget(kVitaWorldScaling|6,active,missing,low));assert(!low && active.framebuffer==10);
    GfxVitaWorldScalingState failed;failed.Begin(kVitaWorldScaling|4);assert(failed.Draw(false,true,1,0,0,261) && !failed.actual && !failed.invalid);
    failed.Draw(false,false,0,1,0,0);assert(failed.invalid==1);
    GfxVitaResourceAuditGate gate;
    for(unsigned phase=1;phase<=7;++phase)for(unsigned surface:{261u,507u,588u}){assert(gate.Take(1,phase,surface,true,7));assert(!gate.Take(1,phase,surface,true,7));}
    assert(!gate.Take(1,8,261,true,7));
}
