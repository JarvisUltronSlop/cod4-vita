#pragma once
#include <cstddef>
#include <cstdint>
namespace kisak::vita {
constexpr uint32_t kCpuAudioIndexMagic=0x31494143; // CAI1
constexpr uint32_t kCpuAudioIndexMaximum=1024;
#pragma pack(push,1)
struct VitaCpuAudioIndexHeader {uint32_t magic,version,count,recordBytes,crc;};
struct VitaCpuAudioIndexRecord {
    char path[256]{}; // normalized path relative to stream/, includes sound/
    uint32_t channels=0,rate=0;
    uint64_t frames=0,fileBytes=0;
    uint32_t prefixCrc=0,tailCrc=0,mp3=0;
};
#pragma pack(pop)
inline uint32_t VitaCpuAudioCrc(const void* data,size_t size) {
    uint32_t crc=0xffffffffu;const auto* bytes=static_cast<const uint8_t*>(data);
    for(size_t i=0;i<size;++i){crc^=bytes[i];for(int j=0;j<8;++j)crc=(crc>>1)^(0xedb88320u&uint32_t(-int32_t(crc&1)));}
    return ~crc;
}
}
