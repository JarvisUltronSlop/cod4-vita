#pragma once
#include <vita/kvap_reader.h>
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace kisak::vita {
enum class VitaPackLayoutError { Ok, CorruptIndex, MissingPack, MismatchedPack };
inline const char* VitaPackLayoutErrorName(VitaPackLayoutError error) {
    switch(error) {
    case VitaPackLayoutError::Ok:return "ok";
    case VitaPackLayoutError::CorruptIndex:return "app-pack-index-invalid";
    case VitaPackLayoutError::MissingPack:return "not-in-this-delivery";
    case VitaPackLayoutError::MismatchedPack:return "installed-pack-differs";
    }
    return "unknown";
}

// App-packaged layout identity. Check the installed file size and exact header/
// TOC before decoding. Payload CRC checks remain mandatory and independent.
inline VitaPackLayoutError VitaPackLayoutValidate(const uint8_t* bytes,size_t count,
        uint32_t mapIndex,uint64_t installedSize,uint32_t metadataCrc) {
    constexpr size_t header=16,record=48;
    const auto U32=[](const uint8_t* source) {uint32_t value;std::memcpy(&value,source,4);return value;};
    if(!bytes || count<header || std::memcmp(bytes,"VPKL",4)
        || U32(bytes+4)!=1 || U32(bytes+8)!=record)return VitaPackLayoutError::CorruptIndex;
    const uint32_t records=U32(bytes+12);
    if(!records || records>17 || count!=header+record*records)return VitaPackLayoutError::CorruptIndex;
    bool found=false;uint64_t expectedSize=0;uint32_t expectedCrc=0;
    for(uint32_t i=0;i<records;++i) {
        const auto* row=bytes+header+record*i;
        if(!std::memchr(row+16,0,32) || !row[16])return VitaPackLayoutError::CorruptIndex;
        for(uint32_t earlier=0;earlier<i;++earlier)
            if(U32(bytes+header+record*earlier)==U32(row))return VitaPackLayoutError::CorruptIndex;
        if(U32(row)!=mapIndex)continue;
        found=true;std::memcpy(&expectedSize,row+4,8);expectedCrc=U32(row+12);
    }
    if(!found)return VitaPackLayoutError::MissingPack;
    return expectedSize==installedSize && expectedCrc==metadataCrc
        ? VitaPackLayoutError::Ok : VitaPackLayoutError::MismatchedPack;
}

// Bounded metadata only; never decode a map at Main. CRC-covered payload
// readers still verify every chunk. FILE belongs exclusively to this caller.
inline VitaPackLayoutError VitaPackLayoutProbeFile(const uint8_t* index,size_t indexSize,
        uint32_t mapIndex,FILE* file,uint64_t* installedSize=nullptr,uint32_t* metadataCrc=nullptr) {
    const auto shape=VitaPackLayoutValidate(index,indexSize,mapIndex,0,0);
    if(shape==VitaPackLayoutError::CorruptIndex || shape==VitaPackLayoutError::MissingPack)return shape;
    if(!file || std::fseek(file,0,SEEK_END))return VitaPackLayoutError::MismatchedPack;
    const long length=std::ftell(file);
    KvapHeader header{};
    if(length<64 || std::fseek(file,0,SEEK_SET)
        || std::fread(&header,1,sizeof(header),file)!=sizeof(header)
        || std::memcmp(header.magic,"KVAP",4) || !header.tocCount || header.tocCount>256
        || header.tocOffset>uint64_t(length)
        || uint64_t(header.tocCount)*sizeof(KvapChunk)>uint64_t(length)-header.tocOffset)
        return VitaPackLayoutError::MismatchedPack;
    uint32_t crc=KvapCrc32Update(0xffffffffu,&header,sizeof(header));
    if(std::fseek(file,long(header.tocOffset),SEEK_SET))return VitaPackLayoutError::MismatchedPack;
    KvapChunk chunk{};
    for(uint32_t i=0;i<header.tocCount;++i) {
        if(std::fread(&chunk,1,sizeof(chunk),file)!=sizeof(chunk))return VitaPackLayoutError::MismatchedPack;
        crc=KvapCrc32Update(crc,&chunk,sizeof(chunk));
    }
    if(installedSize)*installedSize=uint64_t(length);
    if(metadataCrc)*metadataCrc=~crc;
    return VitaPackLayoutValidate(index,indexSize,mapIndex,uint64_t(length),~crc);
}
}
