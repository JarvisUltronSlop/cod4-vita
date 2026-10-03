#pragma once

#include <cstddef>
#include <cstdint>

#include <vita/kvap_reader.h>

namespace kisak::vita {

// Fixed-capacity registry for the v0.1 package backend.  It owns only the
// validated TOC; chunk bytes stay in the package and are read on demand so
// mounting a large Shipment package does not duplicate its payload in RAM.
class KvapRegistry {
public:
    static constexpr uint32_t kMaxChunks = 256;

    KvapError Mount(KvapReadFn read, void* context, uint64_t fileSize);
    void Unmount();

    bool Mounted() const { return mounted_; }
    uint32_t ChunkCount() const { return mounted_ ? reader_.ChunkCount() : 0u; }
    const KvapHeader* Header() const { return mounted_ ? &reader_.Header() : nullptr; }
    const KvapChunk* Chunk(uint32_t index) const { return mounted_ ? reader_.Chunk(index) : nullptr; }
    const KvapChunk* Find(KvapChunkType type, uint64_t assetId) const;
    KvapError Read(const KvapChunk* chunk, void* destination, size_t destinationSize) const;
    KvapError ReadRange(const KvapChunk* chunk, uint64_t offset,
                       void* destination, size_t size) const;

private:
    KvapReader reader_;
    KvapChunk toc_[kMaxChunks]{};
    bool mounted_ = false;
};

} // namespace kisak::vita
