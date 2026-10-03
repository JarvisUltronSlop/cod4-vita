#ifdef NDEBUG
#undef NDEBUG
#endif
#include <gfx_vita/gfx_vita_opaque_shader.h>
#include <cassert>
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
    assert(argc==2);std::ifstream in(argv[1],std::ios::binary);assert(in);
    std::string text((std::istreambuf_iterator<char>(in)),{});
    for(size_t p=0;(p=text.find('\r',p))!=std::string::npos;)text.erase(p,1);
    auto start=text.find("kNativeLitFragmentShader = R\"VITA_SHADER(");assert(start!=std::string::npos);
    start=text.find('(',start)+1;auto end=text.find(")VITA_SHADER\"",start);assert(end!=std::string::npos);
    const auto original=text.substr(start,end-start);const std::string block=GfxVitaOpaqueAlphaBlock();
    const auto at=original.find(block);assert(at!=std::string::npos);
    for(bool sun:{false,true}) {
        std::string shader;assert(GfxVitaBuildOpaqueShader(original.c_str(),sun,shader));
        assert(!GfxVitaShaderHasDiscard(shader));
        if(sun){const std::string define="#define VITA_SUN_ONLY 1\n";const auto d=shader.find(define);assert(d!=std::string::npos);shader.erase(d,define.size());}
        shader.insert(at,block);assert(shader==original); // exact equation/texture/precision parity
    }
    assert(!GfxVitaShaderHasDiscard("// discard\n/* discard */ float discardable;"));
    assert(GfxVitaShaderHasDiscard("/* comment */ discard ;"));
    assert(GfxVitaShaderHasDiscard("/* unterminated"));
    std::string out;
    for(auto bad:{std::string(),original+block,original+"discard;"})assert(!GfxVitaBuildOpaqueShader(bad.c_str(),true,out));
    unsigned eligible=0;
    for(unsigned mask=0;mask<256;++mask)for(unsigned mode=0;mode<8;++mode) {
        bool b[8];for(unsigned i=0;i<8;++i)b[i]=(mask&(1u<<i))!=0;
        const bool selected=GfxVitaOpaqueShaderEligible(b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],mode);
        assert(selected==(mask==((1u<<0)|(1u<<2)|(1u<<5)) && mode==0));eligible+=selected;
    }
    assert(eligible==1);
    std::puts("Opaque gate: 2048 combinations; exact shader recovery; unknown state, alpha test, blending and malformed shaders rejected.");
}
