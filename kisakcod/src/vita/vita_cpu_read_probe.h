#pragma once
#include <cstdint>

namespace kisak::vita {
// Sparse, worker-owned sampling: first read of each opened file, then 1/32.
// Raw execution counters deliberately have no assumed time unit.
inline bool VitaCpuSampleRead(uint64_t ordinal){return ordinal%32==0;}
inline bool VitaCpuReadClockDelta(bool beforeOk,uint64_t before,
                                  bool afterOk,uint64_t after,uint64_t& delta) {
    delta=0;
    if(!beforeOk || !afterOk || after<before)return false;
    delta=after-before;return true;
}
}
