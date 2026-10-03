#include <cstddef>
#include <cstdint>

#include <vita/kvap_format.h>

using kisak::vita::KvapChunk;
using kisak::vita::KvapHeader;

static_assert(offsetof(KvapHeader, tocOffset) == 12);
static_assert(offsetof(KvapHeader, tocCount) == 20);
static_assert(offsetof(KvapHeader, packageId) == 24);
static_assert(offsetof(KvapHeader, sourceHash) == 32);
static_assert(offsetof(KvapChunk, assetId) == 8);
static_assert(offsetof(KvapChunk, offset) == 16);
static_assert(offsetof(KvapChunk, storedSize) == 24);
static_assert(offsetof(KvapChunk, rawSize) == 32);
static_assert(offsetof(KvapChunk, crc32) == 48);

extern "C" uint32_t kvap_layout_probe() {
    return static_cast<uint32_t>(sizeof(KvapHeader) + sizeof(KvapChunk));
}
