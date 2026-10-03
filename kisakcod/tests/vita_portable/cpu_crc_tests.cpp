#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/kvap_reader.h>
#include <cassert>
#include <array>
#include <algorithm>
#include <random>
#include <cstdio>
using namespace kisak::vita;
uint32_t Original(uint32_t crc,const void* data,size_t size) {
    const auto* bytes=static_cast<const uint8_t*>(data);
    for(size_t i=0;i<size;++i) {
        crc^=bytes[i];
        for(int bit=0;bit<8;++bit)crc=(crc>>1)^(0xEDB88320u & static_cast<uint32_t>(-(crc&1u)));
    }
    return crc;
}
int main() {
    assert(KvapCrc32(nullptr,0)==0);
    assert(KvapCrc32("123456789",9)==0xcbf43926u);
    std::array<uint8_t,65536+32> bytes{};std::mt19937 rng(15);
    for(auto& b:bytes)b=uint8_t(rng());
    uint64_t compared=0;
    for(unsigned alignment=0;alignment<16;++alignment) {
        for(size_t count:{0u,1u,2u,7u,8u,15u,16u,255u,256u,4095u,4096u,65535u,65536u}) {
            const uint32_t seed=rng();const auto* start=bytes.data()+alignment;
            const auto expected=Original(seed,start,count);
            assert(KvapCrc32Update(seed,start,count)==expected);
            uint32_t incremental=seed;size_t at=0;
            while(at<count){size_t n=std::min<size_t>(1+rng()%1337,count-at);incremental=KvapCrc32Update(incremental,start+at,n);at+=n;}
            assert(incremental==expected);compared+=count;
        }
    }
    // Exhaust the byte table for varied incoming CRCs, and detect altered data.
    for(unsigned value=0;value<256;++value) {
        uint8_t b=uint8_t(value);
        for(int i=0;i<256;++i){uint32_t seed=rng();assert(KvapCrc32Update(seed,&b,1)==Original(seed,&b,1));}
    }
    const auto crc=KvapCrc32(bytes.data(),bytes.size());bytes[123]^=0x40;
    assert(KvapCrc32(bytes.data(),bytes.size())!=crc);
    std::printf("CRC parity: %llu bytes, all table entries, varied seeds/alignment and streaming boundaries\n",static_cast<unsigned long long>(compared));
}
