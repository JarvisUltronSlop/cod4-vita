#ifdef NDEBUG
#undef NDEBUG
#endif
#include <gfx_vita/gfx_vita_specular_shader.h>
#include <vita/vita_render_pass_probe.h>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iterator>
#include <cstdio>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
int main(int argc,char** argv) {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    assert(argc==2);std::ifstream file(argv[1],std::ios::binary);assert(file);
    std::string text((std::istreambuf_iterator<char>(file)),{});
    for(size_t p=0;(p=text.find('\r',p))!=std::string::npos;)text.erase(p,1);
    auto a=text.find("kNativeLitFragmentShader = R\"VITA_SHADER(");assert(a!=std::string::npos);
    a=text.find('(',a)+1;auto b=text.find(")VITA_SHADER\"",a);assert(b!=std::string::npos);
    const auto original=text.substr(a,b-a);
    unsigned count=0;const auto* edits=GfxVitaSpecularRewrites(count);assert(count==4);
    for(bool sun:{false,true}) {
        std::string baseline,candidate;
        assert(GfxVitaBuildOpaqueShader(original.c_str(),sun,baseline));
        assert(GfxVitaBuildSpecularShader(original.c_str(),sun,candidate));
        assert(!GfxVitaShaderHasDiscard(candidate));
        // Restore actual generated source; no hidden texture, alpha, lighting,
        // fog, precision, normalization or highlight equation changes allowed.
        for(unsigned i=count;i--;) {
            auto at=candidate.find(edits[i].after);assert(at!=std::string::npos);
            candidate.replace(at,std::string(edits[i].after).size(),edits[i].before);
        }
        assert(candidate==baseline);
        for(unsigned i=0;i<count;++i) {
            auto broken=original;auto at=broken.find(edits[i].before);assert(at!=std::string::npos);
            broken.erase(at,std::string(edits[i].before).size());
            assert(!GfxVitaBuildSpecularShader(broken.c_str(),sun,candidate));
            auto duplicate=original+"\n"+edits[i].before;
            assert(!GfxVitaBuildSpecularShader(duplicate.c_str(),sun,candidate));
        }
    }
    // Exhaust the domain at high resolution against the original pow path.
    // Reflection, normalized view and the source highlight remain identical;
    // only rounding of Fresnel can differ. Measure the actual reflection gain.
    double maxFresnel=0,maxGain=0;
    for(unsigned i=0;i<=2000000;++i) {
        const float dot=-1.0f+float(i)*0.000001f;
        const float base=1.0f-std::fmax(dot,0.0f);
        const float before=std::pow(base,5.0f),square=base*base;
        const float after=(square*square)*base;
        maxFresnel=std::fmax(maxFresnel,std::fabs(double(after)-before));
        maxGain=std::fmax(maxGain,std::fabs(double(0.04f+0.16f*after)-(0.04f+0.16f*before)));
        assert(std::fabs(double(after)-before)<2e-7);
        assert(std::fabs(double(0.04f+0.16f*after)-(0.04f+0.16f*before))<6e-8);
    }
    assert(std::pow(0.0f,5.0f)==0.0f && std::pow(1.0f,5.0f)==1.0f);
    for(unsigned elapsed=0;elapsed<=80000;++elapsed) {
        const auto phase=kisak::vita::VitaRenderProbePhase(elapsed);
        kisak::vita::VitaRenderPassProbe probe;
        probe.Begin(kisak::vita::kVitaSpecularComparison+phase,0);
        assert(probe.specular==(phase!=0));
        assert(!probe.Fenced() && !probe.ClipProps() && !probe.SuppressProps());
        assert(!probe.material && !probe.depthOrder && !probe.opaque && !probe.comparison);
    }
    std::printf("2000001 Fresnel samples: max error %.9g, max reflection gain error %.9g\n",maxFresnel,maxGain);
}
