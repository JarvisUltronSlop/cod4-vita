#include <vita/kvap_reader.h>

#if defined(__vita__)
#include <psp2/kernel/clib.h>
#include <vita/vita_script_services.h>
#include <cstdio>
#else
#include <cstdio>
#define sceClibPrintf std::printf
#endif

#include <cstring>
#include <array>
#include <vita/vita_cpu_profile.h>

namespace kisak::vita {

namespace {

bool RangeWithin(uint64_t offset, uint64_t size, uint64_t total) {
    return offset <= total && size <= total - offset;
}

} // namespace

void KvapReportReadFailure(const KvapChunk* chunk, KvapError error, uint32_t actualCrc) {
#if defined(__vita__) && KISAK_VITA_CPU_RUNTIME
    if(!chunk)return;
    char detail[192]{};
    std::snprintf(detail,sizeof(detail),
        "error=%u type=%u id=%016llx offset=%llu bytes=%llu expected=%08x actual=%08x",
        unsigned(error),unsigned(chunk->type),static_cast<unsigned long long>(chunk->assetId),
        static_cast<unsigned long long>(chunk->offset),static_cast<unsigned long long>(chunk->rawSize),
        chunk->crc32,actualCrc);
    VitaScriptServicesTrace("vita_chunk_read_failed","package bytes",detail);
#else
    (void)chunk;(void)error;(void)actualCrc;
#endif
}

uint32_t KvapCrc32(const void* data, size_t size) {
    return ~KvapCrc32Update(0xFFFFFFFFu, data, size);
}

uint32_t KvapCrc32Update(uint32_t crc, const void* data, size_t size) {
#if defined(__vita__) && KISAK_VITA_CPU_RUNTIME
    VITA_CPU_LOADING("package-checksum");
#endif
#if KISAK_VITA_CPU_RUNTIME
    // Same reflected polynomial, seed and final complement as the source
    // bit loop. Read-only compile-time table: no initialization race or heap.
    static constexpr auto table=[] {
        std::array<uint32_t,256> result{};
        for(uint32_t i=0;i<result.size();++i) {
            uint32_t value=i;
            for(unsigned bit=0;bit<8;++bit)
                value=(value>>1)^(0xEDB88320u & static_cast<uint32_t>(-(value&1u)));
            result[i]=value;
        }
        return result;
    }();
    const auto* bytes=static_cast<const uint8_t*>(data);
    for(size_t i=0;i<size;++i)crc=(crc>>8)^table[(crc^bytes[i])&255u];
#else
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & static_cast<uint32_t>(-(crc & 1u)));
    }
#endif
    return crc;
}

KvapError KvapReader::Open(KvapReadFn read, void* context, uint64_t fileSize,
                           KvapChunk* tocStorage, uint32_t tocCapacity) {
    if (!read || !tocStorage || tocCapacity == 0 || fileSize < sizeof(KvapHeader))
        return KvapError::InvalidArgument;
    read_ = read;
    context_ = context;
    fileSize_ = fileSize;
    toc_ = tocStorage;
    chunkCount_ = 0;
    if (!read_(context_, 0, &header_, sizeof(header_)))
        return KvapError::ReadFailed;
    if (std::memcmp(header_.magic, "KVAP", 4) != 0)
        return KvapError::BadMagic;
    if (header_.major != kKvapMajor || header_.headerSize != sizeof(KvapHeader))
        return KvapError::UnsupportedVersion;
    if (header_.endian != kKvapLittleEndian)
        return KvapError::UnsupportedEndian;
    if (header_.tocCount > tocCapacity)
        return KvapError::TocTooLarge;
    const uint64_t tocBytes = static_cast<uint64_t>(header_.tocCount) * sizeof(KvapChunk);
    if (!RangeWithin(header_.tocOffset, tocBytes, fileSize_))
        return KvapError::BadHeader;
    if (!read_(context_, header_.tocOffset, tocStorage, static_cast<size_t>(tocBytes)))
        return KvapError::ReadFailed;
    for (uint32_t i = 0; i < header_.tocCount; ++i) {
        const KvapChunk& chunk = tocStorage[i];
        if (chunk.alignment == 0 || (chunk.alignment & (chunk.alignment - 1)) != 0
            || chunk.offset % chunk.alignment != 0
            || !RangeWithin(chunk.offset, chunk.storedSize, fileSize_)
            || chunk.rawSize > static_cast<uint64_t>(SIZE_MAX))
        {
            sceClibPrintf("[kisak-vita] kvap bad chunk index=%u type=%u asset=%llu offset=%llu stored=%llu raw=%llu alignment=%u file=%llu\\n",
                          static_cast<unsigned>(i), static_cast<unsigned>(chunk.type),
                          static_cast<unsigned long long>(chunk.assetId),
                          static_cast<unsigned long long>(chunk.offset),
                          static_cast<unsigned long long>(chunk.storedSize),
                          static_cast<unsigned long long>(chunk.rawSize),
                          static_cast<unsigned>(chunk.alignment),
                          static_cast<unsigned long long>(fileSize_));
            return KvapError::BadChunk;
        }
    }
    chunkCount_ = header_.tocCount;
    return KvapError::Ok;
}

const KvapChunk* KvapReader::Chunk(uint32_t index) const {
    return index < chunkCount_ ? &toc_[index] : nullptr;
}

KvapError KvapReader::ReadChunk(uint32_t index, void* destination, size_t destinationSize) const {
    const KvapChunk* chunk = Chunk(index);
    if (!chunk || (!destination && chunk->rawSize != 0))
        return KvapError::InvalidArgument;
    if (chunk->rawSize > destinationSize)
        return KvapError::DestinationTooSmall;
    const KvapError rangeError = ReadChunkRange(index, 0, destination,
                                                static_cast<size_t>(chunk->rawSize));
    if (rangeError != KvapError::Ok) {
        KvapReportReadFailure(chunk,rangeError);
        return rangeError;
    }
    const uint32_t actualCrc=KvapCrc32(destination, static_cast<size_t>(chunk->rawSize));
    if (actualCrc != chunk->crc32) {
        KvapReportReadFailure(chunk,KvapError::ChecksumMismatch,actualCrc);
        return KvapError::ChecksumMismatch;
    }
    return KvapError::Ok;
}

KvapError KvapReader::ReadChunkRange(uint32_t index, uint64_t offset,
                                     void* destination, size_t size) const {
    const KvapChunk* chunk = Chunk(index);
    if (!chunk || (!destination && size != 0))
        return KvapError::InvalidArgument;
    if (chunk->codec != static_cast<uint32_t>(KvapCodec::None))
        return KvapError::UnsupportedCodec;
    if (chunk->storedSize != chunk->rawSize || offset > chunk->rawSize
        || static_cast<uint64_t>(size) > chunk->rawSize - offset
        || chunk->offset > UINT64_MAX - offset)
        return KvapError::BadChunk;
    if (size == 0)
        return KvapError::Ok;
    if (!read_(context_, chunk->offset + offset, destination, size))
        return KvapError::ReadFailed;
    return KvapError::Ok;
}

} // namespace kisak::vita
