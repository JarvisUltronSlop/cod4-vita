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
        assert(GfxVitaWeaponSpecularEligible(v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9])==(bits==555));
    }
    std::string candidate,baseline;
    assert(GfxVitaBuildWeaponSpecular(original.c_str(),candidate));
    assert(GfxVitaBuildOpaqueShader(original.c_str(),true,baseline));
    const char* names[]={"u_has_color","u_has_normal","u_has_specular","u_has_reflection_probe","u_use_primary_lightmap","u_use_secondary_lightmap","u_baked_lighting"};
    for(unsigned i=0;i<7;++i) {
        const std::string uniform=std::string("uniform float ")+names[i]+";";
        const std::string constant=std::string("const float ")+names[i]+" = "+(i==0 || i==2 || i==6?"1.0;":"0.0;");
        const auto at=candidate.find(constant);assert(at!=std::string::npos);
        candidate.replace(at,constant.size(),uniform);
        auto broken=original;const auto p=broken.find(uniform);assert(p!=std::string::npos);
        broken.erase(p,uniform.size());std::string unused;
        assert(!GfxVitaBuildWeaponSpecular(broken.c_str(),unused));
        assert(!GfxVitaBuildWeaponSpecular((original+"\n"+uniform).c_str(),unused));
    }
    assert(candidate==baseline); // fallback normal, lighting, specular, fog, samplers and precision unchanged
    for(unsigned phase=0;phase<5;++phase) {
        const auto requested=kVitaWeaponSpecularComparison|phase;VitaRenderPassProbe probe;probe.Begin(requested,0);
        const bool active=phase>=1 && phase<=3;
        assert(VitaWeaponSpecularCandidate(requested)==(phase==2));
        assert(probe.weaponSpecular==active && probe.weaponPipeline==active && probe.weaponVertex==active);
        assert(VitaWeaponPipelineEnabled(requested)==active && VitaWeaponVertexEnabled(requested)==active);
        assert(!probe.Fenced() && !probe.earlyAlpha && !probe.weaponOpaque);
    }
    for(unsigned mode:{0u,kVitaWeaponVertexComparison,kVitaWeaponPipelineComparison,kVitaEarlyAlphaComparison})
        for(unsigned phase=1;phase<=3;++phase)assert(!VitaWeaponSpecularCandidate(mode|phase));
}
