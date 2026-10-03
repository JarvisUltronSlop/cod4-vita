#pragma once
#include <cstdlib>

#if defined(KISAK_VITA_SLICE)
#include <vita/vita_crt_random.h>
#endif

// Platform CRT boundary. Keep the original source's call order and scaling,
// and keep the Windows reference on its actual CRT for parity comparisons.
inline int Com_CrtRand() {
#if defined(KISAK_VITA_SLICE)
    return kisak::vita::SourceCrtRand();
#else
    return std::rand();
#endif
}
inline void Com_CrtSrand(unsigned int seed) {
#if defined(KISAK_VITA_SLICE)
    kisak::vita::SourceCrtSrand(seed);
#else
    std::srand(seed);
#endif
}
