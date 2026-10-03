#ifdef NDEBUG
#undef NDEBUG
#endif
#include <gfx_vita/gfx_vita_color_fast.h>
#include <vita/vita_render_pass_probe.h>
#include <cassert>
#include <string>
#include <fstream>
#include <iterator>
int main(int argc,char** argv) {
    assert(argc==2);
    std::ifstream f(argv[1]);assert(f);
    std::string source((std::istreambuf_iterator<char>(f)),{});
    for(size_t i=0;(i=source.find('\r',i))!=std::string::npos;)source.erase(i,1);
    for(unsigned mask=0;mask<2048;++mask) {
        bool v[11];for(unsigned i=0;i<11;++i)v[i]=(mask&(1u<<i))!=0;
        for(unsigned mode=0;mode<3;++mode)
            assert(GfxVitaFastColorEligible(v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9],v[10],mode)==
                (v[0] && v[1] && !v[2] && v[3] && v[4] && v[5] && !v[6] && !v[7] && v[8] && !v[9] && !v[10] && mode==0));
    }
    // Preserve the original shader's threshold edge behavior and fog expression.
    const std::string candidate=kVitaFastColorFragment;
    auto a=candidate.find("    if (u_alpha_test_enabled");auto b=candidate.find("    vec3 lit",a);
    assert(source.find(candidate.substr(a,b-a))!=std::string::npos);
    a=candidate.find("    if (u_fog_enabled");b=candidate.find("    gl_FragColor",a);
    assert(source.find(candidate.substr(a,b-a))!=std::string::npos);
    assert(candidate.find("u_normal")==std::string::npos && candidate.find("u_specular")==std::string::npos);
    assert(std::string(kVitaFastClearFragment).find("discard")==std::string::npos);
    for(unsigned phase=1;phase<=3;++phase) {
        kisak::vita::VitaRenderPassProbe p;p.Begin(kisak::vita::kVitaFastColorComparison|phase,0);
        assert(p.phase==phase && p.fastColor && !p.Fenced());
    }
    kisak::vita::VitaRenderPassProbe p;p.Begin(kisak::vita::kVitaFastColorComparison|kisak::vita::kVitaGpuDrawProbe|2,0);assert(!p.phase);
}
