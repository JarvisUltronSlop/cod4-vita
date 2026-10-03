#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_vertex_pack.h>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <limits>
#include <random>
using namespace kisak::vita;
int main() {
    uint64_t comparisons=0;
    auto check=[&](float v) {
        const float finite=std::isfinite(v)?v:0.0f;
        assert(PackVertexSnorm(v)==static_cast<int8_t>(std::lround(std::clamp(finite,-1.0f,1.0f)*127.0f)));
        assert(PackVertexUnorm8(v)==static_cast<uint8_t>(std::lround(std::clamp(finite,0.0f,1.0f)*255.0f)));
        ++comparisons;
    };
    // Every exponent/sign category and significand boundaries, including
    // subnormal, finite saturation, both NaN kinds and infinities. Classification
    // must preserve the original nonfinite-to-zero behavior, not saturate NaNs.
    for (uint32_t exponent=0;exponent<256;++exponent)
        for (uint32_t mantissa : {0u,1u,0x1fffffu,0x3fffffu,0x400000u,0x400001u,0x7ffffeu,0x7fffffu})
            for (uint32_t sign : {0u,0x80000000u}) {
                const uint32_t bits=sign|(exponent<<23)|mantissa;
                float value;std::memcpy(&value,&bits,sizeof(value));check(value);
            }
    // Test every byte transition and neighboring float values, including ties
    // and the values for which scaled+0.5 would introduce double rounding.
    for(float scale:{127.0f,255.0f}) for(int i=-256;i<=256;++i) {
        const float tie=(float(i)+0.5f)/scale;
        float down=tie,up=tie;
        for(int j=0;j<16;++j) {
            check(down);check(up);
            down=std::nextafter(down,-std::numeric_limits<float>::infinity());
            up=std::nextafter(up,std::numeric_limits<float>::infinity());
        }
    }
    std::mt19937 rng(0xc0d414);
    for(int i=0;i<2000000;++i) {
        uint32_t bits=rng();float value;std::memcpy(&value,&bits,4);check(value);
        check(float(int(rng()%4000001)-2000000)/1000000.0f);
    }
    for(float v:{0.0f,-0.0f,1.0f,-1.0f,std::numeric_limits<float>::infinity(),
                 -std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})check(v);
    uint64_t magnitudes=0;
    // Exhaust every representable scaled magnitude that can round nonzero.
    // This covers all significands/exponents consumed by the integer decoder.
    for(uint32_t bits=0x3f000000u;bits<=0x437f0000u;++bits) {
        float value;std::memcpy(&value,&bits,4);
        assert(RoundPackedVertexMagnitude(bits)==uint32_t(std::lround(value)));
        ++magnitudes;
    }
    std::printf("%llu exact scaled-magnitude encodings checked\n",static_cast<unsigned long long>(magnitudes));
    std::printf("%llu vertex inputs: exact signed-normal and colour bytes\n",static_cast<unsigned long long>(comparisons));
}
