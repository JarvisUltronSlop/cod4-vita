#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <fstream>
#include <iterator>
#include <gfx_vita/gfx_vita_post_shader.h>
#include <vita/vita_render_pass_probe.h>
using namespace kisak::vita;
int main(int argc,char** argv) {
    assert(argc==2);std::ifstream file(argv[1],std::ios::binary);assert(file);
    std::string text((std::istreambuf_iterator<char>(file)),{});
    for(size_t p=0;(p=text.find('\r',p))!=std::string::npos;)text.erase(p,1);
    auto a=text.find("kNativePostFragmentShader = R\"VITA_SHADER(");assert(a!=std::string::npos);
    a=text.find('(',a)+1;const auto b=text.find(")VITA_SHADER\"",a);assert(b!=std::string::npos);
    const auto original=text.substr(a,b-a);
    for(unsigned bits=0;bits<16;++bits)
        assert(GfxVitaPostFilmEligible(bits&1,bits&2,bits&4,bits&8)==(bits==7));
    std::string candidate;assert(GfxVitaBuildPostFilm(original.c_str(),candidate));
    const char* names[]={"u_film_enabled","u_glow_enabled"};
    for(unsigned i=0;i<2;++i) {
        const std::string uniform=std::string("uniform float ")+names[i]+";";
        const std::string constant=std::string("const float ")+names[i]+(i?" = 0.0;":" = 1.0;");
        const auto at=candidate.find(constant);assert(at!=std::string::npos);candidate.replace(at,constant.size(),uniform);
        auto broken=original;const auto p=broken.find(uniform);assert(p!=std::string::npos);broken.erase(p,uniform.size());
        std::string unused="stale";assert(!GfxVitaBuildPostFilm(broken.c_str(),unused) && unused.empty());
        assert(!GfxVitaBuildPostFilm((original+"\n"+uniform).c_str(),unused) && unused.empty());
    }
    assert(candidate==original); // exact film/glow equations, pow, dither, precision, texture samples and all other uniforms
    for(const char* bad:{static_cast<const char*>(nullptr),"", "uniform float u_film_enabled;"}) {
        candidate="stale";assert(!GfxVitaBuildPostFilm(bad,candidate) && candidate.empty());
    }
    for(unsigned phase=0;phase<5;++phase) {
        const auto requested=kVitaPostFilmComparison|phase;VitaRenderPassProbe probe;probe.Begin(requested,0);
        const bool active=phase>=1 && phase<=3;
        assert(VitaPostFilmMode(requested)==active && VitaPostFilmCandidate(requested)==(phase==2));
        assert(probe.postFilm==active && probe.worldAlpha==active && probe.weaponSpecular==active && probe.weaponVertex==active && probe.weaponPipeline==active);
        assert(VitaWorldAlphaEnabled(requested)==active && VitaWeaponSpecularEnabled(requested)==active && VitaWeaponVertexEnabled(requested)==active && VitaWeaponPipelineEnabled(requested)==active);
        assert(!probe.Fenced() && !probe.drawProbe && !probe.earlyAlpha && !probe.weaponOpaque);
        for(auto other:{kVitaWorldAlphaComparison,kVitaCurrentDrawProbe,kVitaWeaponSpecularComparison}) {
            probe.Begin(requested|other,0);assert(!probe.phase && !probe.postFilm);
        }
    }
    for(unsigned mode:{0u,kVitaWorldAlphaComparison,kVitaCurrentDrawProbe})
        for(unsigned phase=1;phase<=3;++phase)assert(!VitaPostFilmCandidate(mode|phase));
}
