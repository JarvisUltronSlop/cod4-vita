#pragma once

#include <array>
#include <cstddef>
#include <cstdlib>

namespace kisak::vita {

// Source-owner temporary allocations. Pointers remain stable until individually
// freed or cleared at the source compiler/session boundary. Failure callbacks
// must leave the source call: its callers do not accept a null hunk allocation.
template <size_t Capacity = 2048>
class VitaTempHunk {
public:
    using AllocateFn = void* (*)(size_t, size_t);
    using FreeFn = void (*)(void*);
    explicit VitaTempHunk(AllocateFn allocate = &std::calloc,
                          FreeFn release = &std::free)
        : allocate_(allocate), release_(release) {}
    VitaTempHunk(const VitaTempHunk&) = delete;
    VitaTempHunk& operator=(const VitaTempHunk&) = delete;
    ~VitaTempHunk() { Clear(); }

    template <typename Failure>
    void* Allocate(int bytes, Failure failure) {
        const char* reason = bytes <= 0 ? "invalid size"
            : count_ == Capacity ? "allocation table full" : nullptr;
        void* memory = nullptr;
        if (!reason) {
            memory = allocate_(1, static_cast<size_t>(bytes));
            if (!memory) reason = "heap exhausted";
        }
        if (reason) {
            failure(reason);
            // Fail closed if a caller forgot to install its source boundary.
            std::abort();
        }
        allocations_[count_++] = memory;
        return memory;
    }

    bool Untrack(void* memory) {
        for (size_t i = 0; i < count_; ++i) {
            if (allocations_[i] != memory) continue;
            allocations_[i] = allocations_[--count_];
            allocations_[count_] = nullptr;
            return true;
        }
        return false;
    }
    void Clear() {
        while (count_) {
            release_(allocations_[--count_]);
            allocations_[count_] = nullptr;
        }
    }
    size_t Count() const { return count_; }

private:
    std::array<void*, Capacity> allocations_{};
    size_t count_ = 0;
    AllocateFn allocate_;
    FreeFn release_;
};
} // namespace kisak::vita
