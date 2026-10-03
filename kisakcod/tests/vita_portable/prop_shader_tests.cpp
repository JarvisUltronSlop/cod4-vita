#ifdef NDEBUG
#undef NDEBUG
#endif
#include <gfx_vita/gfx_vita_prop_shader.h>
#include <cassert>
#include <fstream>
#include <iterator>
#include <cstdio>

int main(int argc,char** argv) {
    assert(argc==2);
    std::ifstream file(argv[1],std::ios::binary);assert(file);
    std::string text((std::istreambuf_iterator<char>(file)),{});
    // Normalize repository line endings, as shader text in the C++ translation
    // unit does. Compare against the actual shaders used by the backend.
    for(size_t at=0;(at=text.find('\r',at))!=std::string::npos;)text.erase(at,1);
    const auto literal=[&](const char* name) {
        auto at=text.find(std::string(name)+" = R\"VITA_SHADER(");assert(at!=std::string::npos);
        at=text.find('(',at)+1;auto end=text.find(")VITA_SHADER\"",at);assert(end!=std::string::npos);
        return text.substr(at,end-at);
    };
    auto vertex=literal("kNativeLitVertexShader"),fragment=literal("kNativeLitFragmentShader");
    std::string v,f;assert(GfxVitaBuildPropShaders(vertex.c_str(),fragment.c_str(),v,f));
    const auto restore=[](std::string& shader,const char* name,const char* value) {
        const auto constant=std::string("const float ")+name+" = "+value+";";
        const auto at=shader.find(constant);assert(at!=std::string::npos);
        shader.replace(at,constant.size(),std::string("uniform float ")+name+";");
    };
    restore(v,"u_static_instanced","1.0");restore(v,"u_world_space","1.0");restore(v,"u_viewmodel_space","0.0");
    restore(f,"u_baked_lighting","1.0");restore(f,"u_use_primary_lightmap","0.0");
    restore(f,"u_use_secondary_lightmap","0.0");restore(f,"u_has_color","1.0");
    const std::string define="#define VITA_SUN_ONLY 1\n";
    const auto at=f.find(define);assert(at!=std::string::npos);f.erase(at,define.size());
    assert(v==vertex && f==fragment); // All shader equations, textures and precision unchanged.
    assert(!GfxVitaBuildPropShaders("",fragment.c_str(),v,f));
    assert(!GfxVitaBuildPropShaders((vertex+"uniform float u_world_space;").c_str(),fragment.c_str(),v,f));
    unsigned eligible=0;
    for(unsigned mask=0;mask<(1u<<11);++mask)for(unsigned benchmark=0;benchmark<8;++benchmark) {
        bool b[11];for(unsigned i=0;i<11;++i)b[i]=(mask&(1u<<i))!=0;
        const bool selected=GfxVitaPropShaderEligible(b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],benchmark);
        // Only instanced/world/baked/color/adaptive may be set. Every other
        // draw contract, including any local light or diagnostic shader mode,
        // must route to its original program.
        assert(selected==(mask==((1u<<0)|(1u<<1)|(1u<<3)|(1u<<6)|(1u<<8)) && benchmark==0));
        eligible+=selected;
    }
    assert(eligible==1);
    std::puts("16384 dispatch cases; actual shader bodies exactly preserved after seven proven constant substitutions; malformed sources rejected.");
}
