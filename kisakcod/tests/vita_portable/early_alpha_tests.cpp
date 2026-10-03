#ifdef NDEBUG
#undef NDEBUG
#endif
#include <gfx_vita/gfx_vita_early_alpha.h>
#include <vita/vita_render_pass_probe.h>
#include <cassert>
#include <fstream>
#include <iterator>
#include <algorithm>
int main(int argc,char** argv) {
    assert(argc==2);std::ifstream in(argv[1],std::ios::binary);assert(in);
    std::string text((std::istreambuf_iterator<char>(in)),{});
    text.erase(std::remove(text.begin(),text.end(),'\r'),text.end());
    auto start=text.find("kNativeLitFragmentShader = R\"VITA_SHADER(");assert(start!=std::string::npos);
    start=text.find('(',start)+1;auto end=text.find(")VITA_SHADER\"",start);assert(end!=std::string::npos);
    const auto original=text.substr(start,end-start);
    const std::string block=GfxVitaOpaqueAlphaBlock(),fetch="vec4 encoded = texture2D(u_normal, v_texcoord);";
    for(bool sun:{false,true}) {
        std::string shader;assert(GfxVitaBuildEarlyAlpha(original.c_str(),sun,shader));
        assert(shader.find("texture2D(u_normal")<shader.find(block));
        assert(shader.find(block)<shader.find("float normalLength"));
        // Undo only the documented movement: all equations, texture sampling,
        // precision, alpha thresholds and output must recover byte-for-byte.
        if(sun){const std::string define="#define VITA_SUN_ONLY 1\n";auto d=shader.find(define);assert(d!=std::string::npos);shader.erase(d,define.size());}
        const std::string moved=std::string(GfxVitaEarlyAlphaPrefetch())+block+"\n";
        shader.erase(shader.find(moved),moved.size());
        const std::string replacement="vec4 encoded = earlyEncoded;";
        shader.replace(shader.find(replacement),replacement.size(),fetch);
        shader.insert(original.find(block),block);assert(shader==original);
    }
    std::string out;
    assert(!GfxVitaBuildEarlyAlpha(nullptr,false,out));
    for(const auto& bad:{std::string(),original+block,original+fetch,original+"discard;"})
        assert(!GfxVitaBuildEarlyAlpha(bad.c_str(),true,out));
    for(unsigned mask=0;mask<128;++mask)for(unsigned mode=0;mode<8;++mode)
        assert(GfxVitaEarlyAlphaEligible(mask&1,mask&2,mask&4,mask&8,mask&16,mask&32,mask&64,mode)
            ==(mask==29 && mode==0));
    for(unsigned phase=1;phase<=3;++phase) {
        kisak::vita::VitaRenderPassProbe p;p.Begin(kisak::vita::kVitaEarlyAlphaComparison|phase,0);
        assert(p.earlyAlpha && p.phase==phase && !p.Fenced() && !p.hud && !p.weaponOpaque);
        p.Begin(0,0);assert(!p.earlyAlpha && !p.phase);
        for(auto other:{kisak::vita::kVitaHudComparison,kisak::vita::kVitaFastColorComparison,kisak::vita::kVitaWeaponOpaqueComparison}) {
            p.Begin(other|kisak::vita::kVitaEarlyAlphaComparison|phase,0);assert(!p.phase);
        }
    }
}
