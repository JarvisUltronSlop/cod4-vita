#pragma once
#include <cstdint>
#include <cstddef>

namespace kisak::vita {
// RPL1 is an explicit little-endian input tape, independent of C++ padding.
// A fixed arena owns the file for the entire source session. No gameplay-time IO.
class VitaCpuReplayTape {
public:
    static constexpr uint32_t kHeaderBytes=32, kRecordBytes=16, kMaximumRecords=4096;
    static constexpr uint32_t kMaximumBytes=kHeaderBytes+kRecordBytes*kMaximumRecords;
    struct Input {
        uint32_t timeMs=0, buttons=0;
        int16_t pitchMilliDegrees=0, yawMilliDegrees=0;
        int8_t forward=0, right=0;
        uint16_t actions=0;
    };
    static uint32_t U32(const uint8_t* p) {
        return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;
    }
    static uint16_t U16(const uint8_t* p) {return uint16_t(p[0])|uint16_t(p[1])<<8;}
    static uint32_t Crc(const uint8_t* data,size_t size) {
        uint32_t crc=0xffffffffu;
        for(size_t i=0;i<size;++i) {
            crc^=data[i];
            for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^(0xedb88320u&uint32_t(-int32_t(crc&1)));
        }
        return ~crc;
    }
    bool Load(const uint8_t* data,size_t size) {
        data_=nullptr;count_=index_=time_=0;
        if(!data || size<kHeaderBytes || size>kMaximumBytes || U32(data)!=0x314c5052u
            || U32(data+4)!=1 || U32(data+12)!=kRecordBytes || U32(data+24)!=1)return false;
        const uint32_t count=U32(data+8);
        if(!count || count>kMaximumRecords || size!=kHeaderBytes+size_t(count)*kRecordBytes
            || U32(data+16)>1000000 || U32(data+28)!=Crc(data+kHeaderBytes,size-kHeaderBytes))return false;
        uint32_t previous=0;
        for(uint32_t i=0;i<count;++i) {
            const auto* p=data+kHeaderBytes+i*kRecordBytes;
            const uint32_t at=U32(p);
            if(at<=previous || at-previous>1000 || at>120000 || (U16(p+14)&~127u))return false;
            previous=at;
        }
        data_=data;count_=count;return true;
    }
    bool Next(Input& input,uint32_t& elapsedMs) {
        if(!data_ || index_==count_)return false;
        const auto* p=data_+kHeaderBytes+index_*kRecordBytes;
        input={U32(p),U32(p+4),int16_t(U16(p+8)),int16_t(U16(p+10)),int8_t(p[12]),int8_t(p[13]),U16(p+14)};
        elapsedMs=input.timeMs-time_;time_=input.timeMs;++index_;return true;
    }
    uint32_t Clock() const {return data_?U32(data_+16)+time_:0;}
    uint32_t Seed() const {return data_?U32(data_+20):0;}
    uint32_t Checksum() const {return data_?U32(data_+28):0;}
    uint32_t Index() const {return index_;}
    uint32_t Count() const {return count_;}
    bool Complete() const {return data_ && index_==count_;}
private:
    const uint8_t* data_=nullptr;
    uint32_t count_=0,index_=0,time_=0;
};
}
