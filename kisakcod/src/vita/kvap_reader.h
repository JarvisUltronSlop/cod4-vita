#pragma once

#include <cstddef>
#include <cstdint>

#include <vita/kvap_format.h>

namespace kisak::vita {

enum class KvapError : uint32_t {
    Ok = 0,
    InvalidArgument,
    ReadFailed,
    BadMagic,
    UnsupportedVersion,
    UnsupportedEndian,
    BadHeader,
    TocTooLarge,
    BadChunk,
    DestinationTooSmall,
    UnsupportedCodec,
    ChecksumMismatch,
};

using KvapReadFn = bool (*)(void* context, uint64_t offset, void* destination, size_t size);

class KvapReader {
public:
    KvapError Open(KvapReadFn read, void* context, uint64_t fileSize,
                   KvapChunk* tocStorage, uint32_t tocCapacity);
    KvapError ReadChunk(uint32_t index, void* destination, size_t destinationSize) const;
    // Read an uncompressed byte range without allocating the complete chunk.
    // Callers that need integrity coverage can scan ranges and compute the
    // chunk CRC incrementally; full ReadChunk remains the checked convenience
    // path for bounded records.
    KvapError ReadChunkRange(uint32_t index, uint64_t offset,
                             void* destination, size_t size) const;

    const KvapHeader& Header() const { return header_; }
    uint32_t ChunkCount() const { return chunkCount_; }
    const KvapChunk* Chunk(uint32_t index) const;

private:
    KvapReadFn read_ = nullptr;
    void* context_ = nullptr;
    uint64_t fileSize_ = 0;
    KvapHeader header_{};
    KvapChunk* toc_ = nullptr;
    uint32_t chunkCount_ = 0;
};

uint32_t KvapCrc32(const void* data, size_t size);
// Start with 0xFFFFFFFF and complement once after the last range.
uint32_t KvapCrc32Update(uint32_t state, const void* data, size_t size);
// Failed read/checksum details are buffered on Vita; no log traffic on success.
void KvapReportReadFailure(const KvapChunk* chunk, KvapError error, uint32_t actualCrc=0);

} // namespace kisak::vita
