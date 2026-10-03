#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <fstream>
#include <iterator>
#include <gfx_vita/gfx_vita_material_family.h>
#include <vita/vita_render_pass_probe.h>
using namespace kisak::vita;
int main(int argc,char** argv) {
    assert(argc==2);std::ifstream file(argv[1],std::ios::binary);assert(file);
    std::string text((std::istreambuf_iterator<char>(file)),{});
    for(size_t p=0;(p=text.find('\r',p))!=std::string::npos;)text.erase(p,1);
    auto a=text.find("kNativeLitFragmentShader = R\"VITA_SHADER(");assert(a!=std::string::npos);
    a=text.find('(',a)+1;const auto b=text.find(")VITA_SHADER\"",a);assert(b!=std::string::npos);
    const auto original=text.substr(a,b-a);
    for(unsigned bits=0;bits<1024;++bits) {
        bool v[10];for(unsigned i=0;i<10;++i)v[i]=(bits&(1u<<i))!=0;
        assert(GfxVitaWorldAlphaEligible(v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9])==(bits==1015));
    }
    std::string candidate;assert(GfxVitaBuildWorldAlpha(original.c_str(),candidate));
    const auto alpha=std::string(GfxVitaOpaqueAlphaBlock());
    assert(candidate.find(alpha)!=std::string::npos && GfxVitaShaderHasDiscard(candidate));
    const char* names[]={"u_has_color","u_has_normal","u_has_specular","u_has_reflection_probe","u_use_primary_lightmap","u_use_secondary_lightmap"};
    for(const auto* name:names) {
        const std::string uniform=std::string("uniform float ")+name+";";
        const std::string constant=std::string("const float ")+name+" = 1.0;";
        const auto at=candidate.find(constant);assert(at!=std::string::npos);candidate.replace(at,constant.size(),uniform);
        auto broken=original;const auto p=broken.find(uniform);assert(p!=std::string::npos);
        broken.erase(p,uniform.size());std::string unused="must clear";
        assert(!GfxVitaBuildWorldAlpha(broken.c_str(),unused) && unused.empty());
        assert(!GfxVitaBuildWorldAlpha((original+"\n"+uniform).c_str(),unused) && unused.empty());
    }
    const std::string define="#define VITA_SUN_ONLY 1\n";
    const auto at=candidate.find(define);assert(at!=std::string::npos);candidate.erase(at,define.size());
    assert(candidate==original); // all original equations, discard placement, texture reads and dynamic controls
    for(const char* bad:{static_cast<const char*>(nullptr),"", "#version 100\n#version 100\n"}) {
        candidate="must clear";assert(!GfxVitaBuildWorldAlpha(bad,candidate) && candidate.empty());
    }
    for(unsigned phase=0;phase<5;++phase) {
        const auto requested=kVitaWorldAlphaComparison|phase;VitaRenderPassProbe probe;probe.Begin(requested,0);
        const bool active=phase>=1 && phase<=3;
        assert(VitaWorldAlphaCandidate(requested)==(phase==2));
        assert(probe.worldAlpha==active && probe.weaponSpecular==active && probe.weaponVertex==active && probe.weaponPipeline==active);
        assert(VitaWeaponSpecularEnabled(requested)==active && VitaWeaponVertexEnabled(requested)==active && VitaWeaponPipelineEnabled(requested)==active);
        assert(!probe.Fenced() && !probe.earlyAlpha && !probe.weaponOpaque);
    }
    for(unsigned mode:{0u,kVitaWeaponSpecularComparison,kVitaWeaponVertexComparison,kVitaEarlyAlphaComparison})
        for(unsigned phase=1;phase<=3;++phase)assert(!VitaWorldAlphaCandidate(mode|phase));
}
