#include <vita/kvap_reader.h>

static_assert(sizeof(kisak::vita::KvapHeader) == 64);
static_assert(sizeof(kisak::vita::KvapChunk) == 64);

extern "C" unsigned kvap_reader_probe() {
    return kisak::vita::KvapCrc32("vita", 4);
}
