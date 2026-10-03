#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace kisak::vita {
// Source-owned, exact-bit memoization for one immutable lighting sample.
// No asset pointers or quantization. Collisions replace entries and only affect
// hit rate. Reset before another entity/sample, even if its position is equal.
template<size_t Capacity = 1024> class NormalResponseCache {
    static_assert(Capacity && !(Capacity & (Capacity - 1)));
    struct Entry { uint32_t bits[3]{}, stamp=0; float response[3]{}; };
    std::array<Entry, Capacity> entries_{};
    uint32_t stamp_ = 0;
public:
    uint32_t hits = 0, misses = 0;
    void BeginSample() {
        if (++stamp_ == 0) {
            for (auto& entry : entries_) entry.stamp = 0;
            stamp_ = 1;
        }
    }
    template<class Evaluate> bool Resolve(const float* normal, float* response, Evaluate evaluate) {
        uint32_t bits[3]; std::memcpy(bits, normal, sizeof(bits));
        uint32_t hash = bits[0] * 0x9e3779b1u;
        hash ^= bits[1] * 0x85ebca6bu;
        hash ^= bits[2] * 0xc2b2ae35u;
        hash ^= hash >> 16;
        auto& entry = entries_[hash & (Capacity - 1)];
        if (stamp_ && entry.stamp == stamp_ && std::memcmp(bits, entry.bits, sizeof(bits)) == 0) {
            ++hits; std::memcpy(response, entry.response, sizeof(entry.response)); return true;
        }
        ++misses;
        if (!evaluate(normal, response)) return false;
        std::memcpy(entry.bits, bits, sizeof(bits));
        std::memcpy(entry.response, response, sizeof(entry.response)); entry.stamp = stamp_;
        return true;
    }
};
}
