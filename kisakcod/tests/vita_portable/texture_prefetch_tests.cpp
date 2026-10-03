#ifdef NDEBUG
#undef NDEBUG
#endif
#include <gfx_vita/gfx_vita_texture_prefetch.h>
#include <vita/vita_render_pass_probe.h>
#include <cassert>
#include <fstream>
#include <iterator>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
int main(int argc,char** argv) {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    assert(argc==2);std::ifstream file(argv[1],std::ios::binary);assert(file);
    std::string source((std::istreambuf_iterator<char>(file)),{});
    for(size_t p=0;(p=source.find('\r',p))!=std::string::npos;)source.erase(p,1);
    auto start=source.find("kNativeLitFragmentShader = R\"VITA_SHADER(");assert(start!=std::string::npos);
    start=source.find('(',start)+1;auto end=source.find(")VITA_SHADER\"",start);assert(end!=std::string::npos);
    const auto original=source.substr(start,end-start);
    for(unsigned family=1;family<=2;++family) {
        std::string baseline,candidate;assert(GfxVitaBuildMaterialFamily(original.c_str(),family,baseline));
        assert(GfxVitaBuildTexturePrefetch(original.c_str(),family,candidate));assert(!GfxVitaShaderHasDiscard(candidate));
        const auto first=candidate.find("    vec4 earlySpecular =");const auto material=candidate.find("    vec4 material =");
        assert(first!=std::string::npos && first<material && material<candidate.find("float normalLength"));
        const auto fetches=candidate.substr(first,material-first);
        assert(fetches.find("texture2D(u_specular, v_texcoord)")!=std::string::npos);
        if(family==1) {
            assert(fetches.find("texture2D(u_lightmap_primary, v_lightmapcoord).r")!=std::string::npos);
            assert(fetches.find("vec2(v_lightmapcoord.x, v_lightmapcoord.y * 0.5)")!=std::string::npos);
            assert(fetches.find("vec2(v_lightmapcoord.x, v_lightmapcoord.y * 0.5 + 0.5)")!=std::string::npos);
        } else assert(fetches.find("lightmap")==std::string::npos);
        candidate.erase(first,material-first);
        const auto restore=[&](const std::string& a,const std::string& b){const auto at=candidate.find(a);assert(at!=std::string::npos);candidate.replace(at,a.size(),b);};
        restore("vec4 mask = earlySpecular;","vec4 mask = texture2D(u_specular, v_texcoord);");
        if(family==1) {
            restore("float primary = earlyPrimary;","float primary = texture2D(u_lightmap_primary, v_lightmapcoord).r;");
            restore("vec4 secondaryTop = earlySecondaryTop;","vec4 secondaryTop = texture2D(u_lightmap_secondary, topCoord);");
            restore("vec4 secondaryBottom = earlySecondaryBottom;","vec4 secondaryBottom = texture2D(u_lightmap_secondary, bottomCoord);");
        }
        assert(candidate==baseline); // only texture-read placement changes
    }
    const char* anchors[]={"vec4 mask = texture2D(u_specular, v_texcoord);","float primary = texture2D(u_lightmap_primary, v_lightmapcoord).r;","\n        vec4 secondaryTop = texture2D(u_lightmap_secondary, topCoord);","\n        vec4 secondaryBottom = texture2D(u_lightmap_secondary, bottomCoord);","    vec4 material = v_color;"};
    for(auto anchor:anchors) {
        auto broken=original;const auto at=broken.find(anchor);assert(at!=std::string::npos);broken.erase(at,std::string(anchor).size());
        std::string result;assert(!GfxVitaBuildTexturePrefetch(broken.c_str(),1,result));
        assert(!GfxVitaBuildTexturePrefetch((original+"\n"+anchor).c_str(),1,result));
    }
    for(unsigned elapsed=0;elapsed<=80000;++elapsed) {
        const auto phase=kisak::vita::VitaRenderProbePhase(elapsed);kisak::vita::VitaRenderPassProbe p;
        p.Begin(kisak::vita::kVitaTexturePrefetch+phase,0);
        assert(p.prefetch==(phase!=0));assert(!p.Fenced() && !p.ClipProps() && !p.SuppressProps() && !p.family && !p.displayQueue);
    }
}
