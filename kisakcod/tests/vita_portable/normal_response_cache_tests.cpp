#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <vita/vita_normal_response_cache.h>
#include <vita/vita_prepared_model_light.h>
#include <vita/vita_viewmodel_packet.h>
#include "source_model_light.inc"

int main() {
    kisak::vita::NormalResponseCache<> cache;
    kisak::vita::NormalResponseCache<1> collisionCache;
    std::mt19937 rng(0xc0d413);
    std::array<std::array<float,3>,64> normals{};
    for(auto& n:normals)for(auto& v:n)v=(int(rng()%511)-255)/255.0f;
    normals[0]={0,0,0}; normals[1]={-0.0f,0,1}; normals[2]={0,0,1};
    normals[3]={std::numeric_limits<float>::quiet_NaN(),0,1};
    uint64_t comparisons=0;
    for(int sampleIndex=0;sampleIndex<100;++sampleIndex) {
        for(size_t ni=4;ni<normals.size();++ni)for(auto& v:normals[ni]) {
            if(sampleIndex%5==0){const uint32_t bits=rng();std::memcpy(&v,&bits,4);}
            else v=(int(rng()%2000001)-1000000)/1000000.0f;
        }
        ShipmentLightGridSample sample{};sample.valid=sampleIndex%13!=0;sample.hasDirectionalColors=sampleIndex%7!=0;
        sample.primaryLightWeight=(sampleIndex%12)/10.0f;
        for(auto& c:sample.directionalColors)c=uint8_t(rng());
        for(auto& c:sample.color)c=(rng()%256)/255.0f;
        ShipmentWorldLighting light{};light.valid=sampleIndex%17!=0;
        for(auto& c:light.sunDirection)c=(int(rng()%511)-255)/255.0f;
        for(auto& c:light.sunLightColor)c=(rng()%256)/255.0f;
        if(sampleIndex%19==0)light.sunDirection[0]=std::numeric_limits<float>::infinity();
        if(sampleIndex%23==0)light.sunLightColor[1]=std::numeric_limits<float>::quiet_NaN();
        const kisak::vita::PreparedModelLight prepared(sample,light);
        kisak::vita::VitaViewModelInputs copied{};
        kisak::vita::CopyViewModelLight(sample,light,copied);
        const kisak::vita::PreparedModelLight copiedLight(copied.sample,copied.world);
        cache.BeginSample();collisionCache.BeginSample();
        auto evaluate=[&](const float* n,float* out){return EvaluateSourceModelLightGridResponse(sample,light,n,out);};
        for(int i=0;i<8192;++i) {
            const auto& n=normals[i%normals.size()];float original[3]={1,1,1},cached[3]={1,1,1},colliding[3]={1,1,1};
            const bool valid=evaluate(n.data(),original);
            float fast[3]={1,1,1};
            assert(prepared.Evaluate(n.data(),fast)==valid);
            assert(std::memcmp(original,fast,sizeof(original))==0);
            float moved[3]={1,1,1};
            assert(copiedLight.Evaluate(n.data(),moved)==valid);
            assert(std::memcmp(original,moved,sizeof(moved))==0);
            assert(cache.Resolve(n.data(),cached,evaluate)==valid);
            assert(collisionCache.Resolve(n.data(),colliding,evaluate)==valid);
            assert(std::memcmp(original,cached,sizeof(original))==0);
            assert(std::memcmp(original,colliding,sizeof(original))==0);++comparisons;
        }
    }
    assert(cache.hits>250000);
    // Cover every byte value in every directional slot. Each evaluation uses
    // a fresh normal instead of relying on response-cache hits; compare all
    // output bits and failure semantics with the original source equations.
    uint64_t paletteComparisons=0;
    for(uint32_t value=0;value<256;++value) {
        ShipmentLightGridSample sample{};sample.valid=true;sample.hasDirectionalColors=true;
        sample.primaryLightWeight=.37f;
        for(auto& c:sample.directionalColors)c=uint8_t(value);
        ShipmentWorldLighting light{};light.valid=true;
        light.sunDirection[0]=.4f;light.sunDirection[1]=-.3f;light.sunDirection[2]=.8f;
        light.sunLightColor[0]=.7f;light.sunLightColor[1]=1.1f;light.sunLightColor[2]=.2f;
        for(unsigned variant=0;variant<2;++variant) {
            if(variant) for(unsigned i=0;i<168;++i)sample.directionalColors[i]=uint8_t(value+i);
            const kisak::vita::PreparedModelLight prepared(sample,light);
            for(unsigned i=0;i<1024;++i) {
                float n[3];for(auto& v:n)v=(int(rng()%2000001)-1000000)/1000000.f;
                if(i<6){n[0]=n[1]=n[2]=0;n[i/2]=i%2?1.f:-1.f;}
                float expected[3]={1,1,1},actual[3]={1,1,1};
                const bool valid=EvaluateSourceModelLightGridResponse(sample,light,n,expected);
                assert(prepared.Evaluate(n,actual)==valid);
                assert(std::memcmp(expected,actual,sizeof(actual))==0);++paletteComparisons;
            }
        }
    }
    // Probe stencil face/edge ties and adjacent representable inputs, with
    // samples changing each pass. These exercise duplicate clamped endpoints,
    // zero-weight corners and the normal-length rejection threshold.
    uint64_t boundaryComparisons=0;
    for (float scale : {0.0001f, 0.001f, 1.0f, 10000.0f, 1.0e20f}) {
        ShipmentLightGridSample sample{}; sample.valid=sample.hasDirectionalColors=true;
        sample.primaryLightWeight=.63f;
        for(auto& c:sample.directionalColors)c=uint8_t(rng());
        sample.color[0]=.23f;sample.color[1]=.41f;sample.color[2]=.97f;
        ShipmentWorldLighting light{};light.valid=true;
        light.sunDirection[0]=.4f;light.sunDirection[1]=-.3f;light.sunDirection[2]=.8f;
        light.sunLightColor[0]=.7f;light.sunLightColor[1]=1.1f;light.sunLightColor[2]=.2f;
        const kisak::vita::PreparedModelLight prepared(sample,light);
        for(int z=-6;z<=6;++z)for(int y=-6;y<=6;++y)for(int x=-6;x<=6;++x) {
            const float normal[3]={x/6.f*scale,y/6.f*scale,z/6.f*scale};
            for(int direction=-1;direction<=1;++direction) {
                float n[3];std::memcpy(n,normal,sizeof(n));
                if(direction)for(auto& v:n)v=std::nextafter(v,direction<0
                    ? -std::numeric_limits<float>::infinity():std::numeric_limits<float>::infinity());
                float expected[3]={1,1,1},actual[3]={1,1,1};
                const bool valid=EvaluateSourceModelLightGridResponse(sample,light,n,expected);
                assert(prepared.Evaluate(n,actual)==valid);
                assert(std::memcmp(expected,actual,sizeof(actual))==0);++boundaryComparisons;
            }
        }
    }
    std::printf("%llu stencil boundary comparisons against original light equations\n",
        static_cast<unsigned long long>(boundaryComparisons));
    std::printf("%llu uncached directional-palette comparisons, including all 256 byte values per slot\n",
        static_cast<unsigned long long>(paletteComparisons));
    std::printf("%llu source-light comparisons: exact response bits and failure results; %u hits, %u misses\n",
        static_cast<unsigned long long>(comparisons),cache.hits,cache.misses);
}
