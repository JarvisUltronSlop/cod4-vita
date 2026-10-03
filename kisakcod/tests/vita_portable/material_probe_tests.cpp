#ifdef NDEBUG
#undef NDEBUG
#endif
#include <gfx_vita/gfx_vita_material_probe.h>
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
    std::string text((std::istreambuf_iterator<char>(file)),{});
    for(size_t p=0;(p=text.find('\r',p))!=std::string::npos;)text.erase(p,1);
    auto a=text.find("kNativeLitFragmentShader = R\"VITA_SHADER(");assert(a!=std::string::npos);
    a=text.find('(',a)+1;auto b=text.find(")VITA_SHADER\"",a);assert(b!=std::string::npos);
    const auto original=text.substr(a,b-a);
    for(bool sun:{false,true}) {
        std::string opaque;assert(GfxVitaBuildOpaqueShader(original.c_str(),sun,opaque));
        for(unsigned variant=0;variant<4;++variant) {
            std::string shader;assert(GfxVitaBuildMaterialProbeShader(original.c_str(),sun,variant,shader));
            assert(!GfxVitaShaderHasDiscard(shader));
            if(!variant)assert(shader==opaque);
            else if(variant<3) {
                const std::string define=variant==1?"#define VITA_NO_NORMAL_MAP 1\n":"#define VITA_NO_SPECULAR 1\n";
                auto at=shader.find(define);assert(at!=std::string::npos);shader.erase(at,define.size());assert(shader==opaque);
            } else {
                const auto first=opaque.find("    float normalLength = dot(v_normal, v_normal);");
                const auto fog=opaque.find("#ifndef VITA_NO_FOG");
                const auto removed=opaque.substr(first,fog-first);
                const std::string replacement="    vec3 lit = material.rgb;\n";
                assert(shader.substr(0,first)==opaque.substr(0,first));
                assert(shader.substr(first+replacement.size())==opaque.substr(fog));
                shader.replace(first,replacement.size(),removed);assert(shader==opaque);
                assert(removed.find("texture2D(u_normal")!=std::string::npos && removed.find("texture2D(u_specular")!=std::string::npos && removed.find("u_lightmap_secondary")!=std::string::npos);
            }
        }
    }
    std::string out;assert(!GfxVitaBuildMaterialProbeShader(original.c_str(),true,4,out));
    assert(!GfxVitaBuildMaterialProbeShader("",false,1,out));
    assert(!GfxVitaBuildMaterialProbeShader((original+"\n#ifndef VITA_NO_FOG\n").c_str(),false,3,out));
    assert(!GfxVitaBuildMaterialProbeShader((original+"\n#if !defined(VITA_NO_SPECULAR)\n").c_str(),false,2,out));
    for(unsigned elapsed=0;elapsed<=160000;++elapsed) {
        const auto phase=kisak::vita::VitaMaterialProbePhase(elapsed);
        assert(phase==(elapsed<140000?elapsed/20000+1:0));
        kisak::vita::VitaRenderPassProbe probe;probe.Begin(kisak::vita::kVitaMaterialProbe+phase,0);
        assert(!probe.Fenced() && !probe.ClipProps() && !probe.SuppressProps() && !probe.depthOrder);
        assert(probe.material==(phase!=0));
        assert(GfxVitaMaterialProbeVariant(phase)==(phase==2?1u:phase==4?2u:phase==6?3u:0u));
    }
}
