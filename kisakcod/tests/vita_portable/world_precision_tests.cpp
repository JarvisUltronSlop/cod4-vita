#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <iostream>
#include <gfx_vita/gfx_vita_world_precision.h>
#include <vita/vita_render_pass_probe.h>
using namespace kisak::vita;
int main(int argc,char** argv) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0,_WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    assert(argc==2);std::ifstream f(argv[1]);assert(f);
    std::string text((std::istreambuf_iterator<char>(f)),{});
    for(size_t p=0;(p=text.find('\r',p))!=std::string::npos;)text.erase(p,1);
    auto a=text.find("kNativeLitFragmentShader = R\"VITA_SHADER(");assert(a!=std::string::npos);a=text.find('(',a)+1;
    auto b=text.find(")VITA_SHADER\"",a);assert(b!=std::string::npos);const auto original=text.substr(a,b-a);
    unsigned count=0;const auto* edits=GfxVitaWorldPrecisionEdits(count);assert(count==10);
    for(bool alpha:{false,true}) {
        std::string base,candidate;assert(alpha?GfxVitaBuildWorldAlpha(original.c_str(),base):GfxVitaBuildMaterialFamily(original.c_str(),1,base));
        assert(GfxVitaBuildWorldPrecision(original.c_str(),alpha,candidate));
        // Run72: half3 lit plus float fog arguments made native lerp ambiguous.
        // Select full-precision fog interpolation, then round at the existing
        // half colour store. Test both opaque and transparent world builders.
        assert(candidate.find("lit = mix(lit, u_fog_color,")==std::string::npos);
        assert(candidate.find("lit = half3(mix(vec3(lit), u_fog_color, clamp(fog, 0.0, 1.0)));")!=std::string::npos);
        // No shader equation, input, discard or texture call may be changed
        // outside this explicit precision edit set. Reversal is byte-exact.
        for(unsigned i=count;i-->0;) {
            const std::string from=edits[i].after,to=edits[i].before;unsigned found=0;
            for(size_t at=0;(at=candidate.find(from,at))!=std::string::npos;at+=to.size()){candidate.replace(at,from.size(),to);++found;}
            assert(found==edits[i].count);
        }
        assert(candidate==base);
        for(unsigned i=0;i<count;++i) {
            auto broken=original;const std::string from=edits[i].before;auto at=broken.find(from);assert(at!=std::string::npos);broken.erase(at,from.size());
            std::string out="stale";assert(!GfxVitaBuildWorldPrecision(broken.c_str(),alpha,out) && out.empty());
            assert(!GfxVitaBuildWorldPrecision((original+"\n"+from).c_str(),alpha,out) && out.empty());
        }
    }
    std::string out="stale";assert(!GfxVitaBuildWorldPrecision(nullptr,false,out) && out.empty());
    for(unsigned bits=0;bits<32;++bits)
        assert(GfxVitaWorldPrecisionLane(bits&1,bits&2,bits&4,bits&8,bits&16)==(bits==9?1u:bits==17?2u:0u));
    for(unsigned phase=0;phase<5;++phase) {
        const auto tag=kVitaWorldPrecisionComparison|phase;const bool active=phase>=1 && phase<=3;VitaRenderPassProbe p;p.Begin(tag,0);
        assert(p.worldPrecision==active && p.worldAlpha==active && p.weaponPipeline==active && p.weaponVertex==active && p.weaponSpecular==active);
        assert(p.WorldAlphaActive()==active && VitaWeaponVertexEnabled(tag)==active && VitaWeaponSpecularEnabled(tag)==active && VitaWeaponPipelineEnabled(tag)==active);
        assert(!p.Fenced() && !p.drawProbe && !p.passIsolation && !p.composition && !p.graphicsProfile && !p.worldMath && !p.worldBasis && !p.worldGloss && !p.worldComponents && !p.prefetch && !p.earlyAlpha && !p.hud && !p.fastColor && !p.ClipProps() && !p.SuppressProps());
    }
    for(auto tag:{0u,kVitaWorldMathComparison|2u,kVitaPassIsolation|2u,kVitaCompositionComparison|2u}){VitaRenderPassProbe p;p.Begin(tag,0);assert(!p.worldPrecision);}
    // Explicit half locals survive VitaGL's translator. Interface declarations,
    // material alpha, UVs, normal reconstruction and sharp specular stay float.
    assert(GfxVitaBuildWorldPrecision(original.c_str(),true,out));
    for(const char* kept:{"vec4 material = v_color;","vec4 mask = texture2D(u_specular, v_texcoord);","float specExponent = exp(","vec3 viewVector = u_camera_position - v_worldposition;","vec4 encoded = texture2D(u_normal, v_texcoord);","varying vec2 v_texcoord;","float packedShadowGrid ="})assert(out.find(kept)!=std::string::npos);
    std::cout<<"World precision builders, missing/duplicate anchors, dispatch and unchanged sensitive inputs passed\n";
}
