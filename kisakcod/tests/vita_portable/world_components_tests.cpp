#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>
#include <gfx_vita/gfx_vita_world_components.h>
#include <gfx_vita/gfx_vita_resource_audit.h>
#include <vita/vita_render_pass_probe.h>
using namespace kisak::vita;
int main(int argc,char** argv) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    assert(argc==2 || argc==3);std::ifstream file(argv[1]);assert(file);
    std::string text((std::istreambuf_iterator<char>(file)),{});
    for(size_t p=0;(p=text.find('\r',p))!=std::string::npos;)text.erase(p,1);
    auto a=text.find("kNativeLitFragmentShader = R\"VITA_SHADER(");assert(a!=std::string::npos);
    a=text.find('(',a)+1;auto b=text.find(")VITA_SHADER\"",a);assert(b!=std::string::npos);
    const auto original=text.substr(a,b-a);std::string base;assert(GfxVitaBuildWorldAlpha(original.c_str(),base));
    for(unsigned variant=1;variant<=2;++variant) {
        std::string candidate;assert(GfxVitaBuildWorldComponent(original.c_str(),variant,candidate));
        if(variant==1) {
            const auto branch=base.find("    if (usePrimaryLightmap && useSecondaryLightmap) {");
            const auto first=base.find("        vec2 topCoord =",branch),last=base.find("        float sourceSunDot =",first);
            auto expected=base;expected.replace(first,last-first,"        vec3 secondaryRgb = vec3(0.0);\n");assert(candidate==expected);
            assert(candidate.substr(0,first)==base.substr(0,first));
            assert(candidate.substr(candidate.find("        float sourceSunDot =",first))==base.substr(last));
        } else {
            auto expected=base;const std::string marker="#if !defined(VITA_NO_SPECULAR)";
            expected.replace(expected.find(marker),marker.size(),"#if 0 // diagnostic: specular and reflection contribution disabled");assert(candidate==expected);
        }
        assert(!GfxVitaBuildWorldComponent(nullptr,variant,candidate) && candidate.empty());
        assert(!GfxVitaBuildWorldComponent("",variant,candidate) && candidate.empty());
        auto broken=original;
        const std::string marker=variant==1?"    if (usePrimaryLightmap && useSecondaryLightmap) {":"#if !defined(VITA_NO_SPECULAR)";
        broken.erase(broken.find(marker),marker.size());assert(!GfxVitaBuildWorldComponent(broken.c_str(),variant,candidate));
        assert(!GfxVitaBuildWorldComponent((original+marker).c_str(),variant,candidate));
    }
    std::string out;assert(!GfxVitaBuildWorldComponent(original.c_str(),0,out));assert(!GfxVitaBuildWorldComponent(original.c_str(),3,out));
    for(unsigned variant=1;variant<=2;++variant) {
        const std::string start=variant==1?"            lit += clamp(highlight *":"                lit += clamp(environment *";
        const auto first=base.find(start);assert(first!=std::string::npos);
        const auto last=base.find("));\n",first)+4;assert(last>first);
        const auto accumulation=base.substr(first,last-first);
        auto expected=base;expected.erase(first,last-first);
        assert(GfxVitaBuildWorldSpecularSplit(original.c_str(),variant,out) && out==expected);
        // The other lobe and all shared inputs remain byte-for-byte intact.
        assert(out.find(variant==1?"lit += clamp(environment *":"lit += clamp(highlight *")!=std::string::npos);
        for(const char* marker:{"vec4 mask = texture2D(u_specular", "vec3 viewVector =", "reflection = viewVector -", "vec3 secondaryRgb =", "gl_FragColor ="})
            assert(out.find(marker)!=std::string::npos);
        auto missing=original;missing.erase(missing.find(accumulation),accumulation.size());
        assert(!GfxVitaBuildWorldSpecularSplit(missing.c_str(),variant,out) && out.empty());
        assert(!GfxVitaBuildWorldSpecularSplit((original+accumulation).c_str(),variant,out) && out.empty());
        assert(!GfxVitaBuildWorldSpecularSplit(nullptr,variant,out) && out.empty());
        assert(!GfxVitaBuildWorldSpecularSplit("",variant,out) && out.empty());
    }
    assert(!GfxVitaBuildWorldSpecularSplit(original.c_str(),0,out));assert(!GfxVitaBuildWorldSpecularSplit(original.c_str(),3,out));
    for(unsigned phase=0;phase<=6;++phase) {
        auto mode=kVitaWorldComponentsComparison|phase;VitaRenderPassProbe probe;probe.Begin(mode,1);
        bool active=phase>=1 && phase<=5;
        assert(probe.worldComponents==active && !probe.worldGloss && !probe.worldMath && !probe.worldBasis);
        assert(!probe.Fenced() && !probe.postFilm && !probe.lightmapLayout && !probe.worldLifetime && !probe.drawProbe);
        assert(probe.worldAlpha==active && probe.weaponPipeline==active && probe.weaponVertex==active && probe.weaponSpecular==active);
        assert(VitaWorldAlphaEnabled(mode)==active && VitaWeaponSpecularEnabled(mode)==active && VitaWeaponVertexEnabled(mode)==active && VitaWeaponPipelineEnabled(mode)==active);
        assert(GfxVitaWorldComponentVariant(phase)==(phase==2?1u:phase==4?2u:0u));
    }
    for(unsigned ms:{0u,19999u,20000u,39999u,40000u,59999u,60000u,79999u,80000u,99999u,100000u,0xffffffffu})
        assert(VitaWorldComponentsPhase(ms)==(ms<100000?1+ms/20000:0));
    for(bool family:{false,true})for(unsigned surface:{261u,507u,588u,0xffffffffu})for(unsigned cert:{0u,233u,kGfxVitaGravelSpecCertificate})
        assert(GfxVitaWorldComponentEligible(family,surface,cert)==(family && surface==588 && cert==kGfxVitaGravelSpecCertificate));
    GfxVitaResourceAuditGate gate;
    for(unsigned phase=1;phase<=5;++phase)for(unsigned surface:{261u,507u,588u}) {
        assert(gate.Take(1,phase,surface,true,5));assert(!gate.Take(1,phase,surface,true,5));
    }
    assert(!gate.Take(2,4,588,true));assert(!gate.Take(2,6,588,true,5));assert(!gate.Take(2,1,588,true,8));assert(!gate.Take(2,1,588,false,5));assert(gate.Take(2,1,588,true,5));
    if(argc==3) {
        std::ifstream f(argv[2],std::ios::binary);assert(f);std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),{});
        VitaTextureIdentity key{VitaSha256::Hash(bytes.data(),bytes.size()),256,256,9,0x35545844u,0,uint32_t(bytes.size())};assert(GfxVitaGravelSpecIdentity(key));
        auto bad=key;bad.digest[0]^=1;assert(!GfxVitaGravelSpecIdentity(bad));
        for(unsigned* field:{&key.width,&key.height,&key.levels,&key.format,&key.flags,&key.bytes}) {++*field;assert(!GfxVitaGravelSpecIdentity(key));--*field;}
        VitaTexturePool<2,3> pool;auto lease=pool.Insert(key,7,kGfxVitaGravelSpecCertificate),shared=pool.Acquire(key);
        assert(pool.Certificate(shared)==kGfxVitaGravelSpecCertificate && !pool.Release(lease) && !pool.Certificate(lease));
        assert(pool.Release(shared)==7 && !pool.Certificate(shared));
        auto next=pool.Insert(key,7);assert(next && !pool.Certificate(next));pool.Clear([](uint32_t){});assert(!pool.Certificate(next));
    }
}
