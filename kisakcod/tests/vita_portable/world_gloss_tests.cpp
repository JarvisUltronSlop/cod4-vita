#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>
#include <cmath>
#include <gfx_vita/gfx_vita_world_gloss.h>
#include <vita/vita_texture_pool.h>
#include <vita/vita_render_pass_probe.h>
using namespace kisak::vita;
int main(int argc,char** argv) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0,_WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    assert(argc==2 || argc==5);
    std::ifstream file(argv[1]);assert(file);
    std::string text((std::istreambuf_iterator<char>(file)),{});
    for(size_t p=0;(p=text.find('\r',p))!=std::string::npos;)text.erase(p,1);
    auto a=text.find("kNativeLitFragmentShader = R\"VITA_SHADER(");assert(a!=std::string::npos);
    a=text.find('(',a)+1;auto b=text.find(")VITA_SHADER\"",a);assert(b!=std::string::npos);
    const std::string source=text.substr(a,b-a);std::string original,candidate;
    assert(GfxVitaBuildMaterialFamily(source.c_str(),1,original));
    assert(GfxVitaBuildWorldGloss(source.c_str(),candidate));
    const std::string constant="233.0 / 255.0";
    auto at=candidate.find(constant);assert(at!=std::string::npos);
    candidate.replace(at,constant.size(),"mask.a");assert(candidate==original);
    for(const char* bad:{static_cast<const char*>(nullptr),"","exp(clamp(mask.a, 0.0, 1.0) * 9.3775177)"})
        assert(!GfxVitaBuildWorldGloss(bad,candidate) && candidate.empty());
    auto duplicated=source+"exp(clamp(mask.a, 0.0, 1.0) * 9.3775177)";
    assert(!GfxVitaBuildWorldGloss(duplicated.c_str(),candidate) && candidate.empty());
    // Complete 8x8 -> 4x4 -> 2x2 -> 1x1 chain: seven BC3 blocks.
    std::vector<uint8_t> image(7*16);
    for(size_t i=0;i<image.size();i+=16){image[i]=233;image[i+1]=234;}
    auto valid=[&](const std::vector<uint8_t>& data){return GfxVitaConstantGloss233(data.data(),data.size(),8,8,4,0x35545844u,0);};
    assert(valid(image));
    for(size_t block=0;block<7;++block)for(unsigned texel=0;texel<16;++texel)for(unsigned selector=1;selector<8;++selector) {
        auto changed=image;uint64_t bits=uint64_t(selector)<<(3*texel);
        for(unsigned j=0;j<6;++j)changed[block*16+2+j]=uint8_t(bits>>(8*j));
        assert(!valid(changed)); // includes final mip and padding texels
    }
    auto second=image;
    for(size_t i=0;i<second.size();i+=16){second[i]=234;second[i+1]=233;for(unsigned j=0;j<6;++j)second[i+2+j]=uint8_t(0x249249249249ull>>(8*j));}
    assert(valid(second));
    for(size_t length=0;length<image.size();++length)assert(!GfxVitaConstantGloss233(image.data(),length,8,8,4,0x35545844u,0));
    auto extra=image;extra.push_back(0);assert(!valid(extra));
    assert(!GfxVitaConstantGloss233(nullptr,image.size(),8,8,4,0x35545844u,0));
    for(unsigned levels:{0u,1u,3u,5u,32u})assert(!GfxVitaConstantGloss233(image.data(),image.size(),8,8,levels,0x35545844u,0));
    for(unsigned size:{0u,7u,8192u})assert(!GfxVitaConstantGloss233(image.data(),image.size(),size,8,4,0x35545844u,0));
    assert(!GfxVitaConstantGloss233(image.data(),image.size(),8,8,4,0x31545844u,0));
    assert(!GfxVitaConstantGloss233(image.data(),image.size(),8,8,4,0x35545844u,4));
    for(unsigned bits=0;bits<32;++bits)assert(GfxVitaWorldGlossEligible(bits&1,bits&2,bits&4,bits&8,bits&16)==(bits==25));
    // Certificate follows immutable images through shared and stale leases.
    VitaTexturePool<2,3> pool;VitaTextureIdentity key{};key.bytes=112;
    auto first=pool.Insert(key,7,233),shared=pool.Acquire(key);
    assert(first && shared && pool.Certificate(first)==233 && pool.Certificate(shared)==233);
    assert(!pool.Release(first) && !pool.Certificate(first) && pool.Certificate(shared)==233);
    assert(pool.Release(shared)==7 && !pool.Certificate(shared));
    auto replacement=pool.Insert(key,7);assert(replacement && !pool.Certificate(replacement) && !pool.Certificate(first));
    pool.Clear([](uint32_t){});assert(!pool.Certificate(replacement));
    auto last=pool.Insert(key,7,233);assert(last && pool.Certificate(last)==233);
    pool.Clear([](uint32_t){});assert(!pool.Certificate(last));
    for(unsigned phase=0;phase<=4;++phase) {
        const auto mode=kVitaWorldGlossComparison|phase;VitaRenderPassProbe p;p.Begin(mode,0);
        const bool active=phase>=1 && phase<=3;
        assert(p.worldGloss==active && !p.worldBasis && !p.worldMath && !p.postFilm && !p.lightmapLayout && !p.drawProbe);
        assert(!p.Fenced() && p.weaponPipeline==active && p.worldAlpha==active);
        assert(VitaWorldAlphaEnabled(mode)==active && VitaWeaponVertexEnabled(mode)==active && VitaWeaponSpecularEnabled(mode)==active);
    }
    // Highp scalar oracle; native compiler folding/texture sampling still needs
    // hardware appearance and compiled-program evidence, not a CPU-only claim.
    const float alpha=float(uint8_t(233))/255.f;
    const float exponent=std::exp(alpha*9.3775177f)+7.f;
    assert(exponent==std::exp((233.f/255.f)*9.3775177f)+7.f);
    for(int i=0;i<=20000;++i) {
        const float sun=-1.f+i/10000.f;
        const float before=std::exp(std::fmin(0.f,std::fmax(-80.f,(sun-.99925f)*(std::exp(alpha*9.3775177f)+7.f)*1.44269502f)));
        const float after=std::exp(std::fmin(0.f,std::fmax(-80.f,(sun-.99925f)*exponent*1.44269502f)));
        assert(before==after);
    }
    // Optional owned-pack payloads, kept outside the source archive.
    for(int i=2;i<argc;++i) {
        std::ifstream f(argv[i],std::ios::binary);assert(f);std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),{});
        assert(GfxVitaConstantGloss233(bytes.data(),bytes.size(),256,256,9,0x35545844u,0)==(i==2));
    }
}
