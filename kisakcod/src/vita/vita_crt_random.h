#pragma once
#include <cstdint>

namespace kisak::vita {
// KisakCOD's Windows source uses the CRT's 15-bit range in effects,
// animation selection and random()/crandom(). Newlib's rand() has a
// different sequence and range. Keep this platform state separate from
// gameplay's Rand_Init/holdrand and BG seed pointers.
class VitaCrtRandom {
public:
    void Seed(uint32_t seed) { state_ = seed; }
    uint32_t State() const { return state_; }
    int Next() {
        state_ = state_ * 214013u + 2531011u;
        return static_cast<int>((state_ >> 16) & 0x7fffu);
    }
private:
    uint32_t state_ = 1;
};

// Matches the Windows CRT's per-thread state. All source simulation and
// effects currently run on the main thread; asset/audio workers cannot
// advance that stream.
inline thread_local VitaCrtRandom sourceCrtRandom;
inline int SourceCrtRand() { return sourceCrtRandom.Next(); }
inline void SourceCrtSrand(uint32_t seed) { sourceCrtRandom.Seed(seed); }
} // namespace kisak::vita
