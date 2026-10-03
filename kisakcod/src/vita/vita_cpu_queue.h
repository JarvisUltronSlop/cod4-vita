#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace kisak::vita {

// One producer and one consumer. A full queue is reported, never overwritten.
// Payload publication and retirement are paired with acquire/release ordering.
template<class T, uint32_t Capacity>
class VitaCpuQueue {
    static_assert(Capacity > 1 && (Capacity & (Capacity - 1)) == 0);
    static_assert(std::is_trivially_copyable_v<T>);
public:
    bool Push(const T& value) {
        const uint32_t write = write_.load(std::memory_order_relaxed);
        if (write - read_.load(std::memory_order_acquire) == Capacity) return false;
        storage_[write & (Capacity - 1)] = value;
        write_.store(write + 1, std::memory_order_release);
        return true;
    }
    bool Pop(T* value) {
        const uint32_t read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return false;
        *value = storage_[read & (Capacity - 1)];
        read_.store(read + 1, std::memory_order_release);
        return true;
    }
    uint32_t Count() const {
        return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire);
    }
    // Only while both endpoints are quiescent, such as after joining a worker.
    void Reset() { write_.store(0); read_.store(0); }
private:
    alignas(64) std::atomic<uint32_t> write_{0};
    alignas(64) std::atomic<uint32_t> read_{0};
    std::array<T, Capacity> storage_{};
};

// Absolute rational deadlines avoid accumulating truncation at 30/60 Hz.
class VitaCpuDeadline {
public:
    void Reset(uint64_t now, uint32_t hz) { origin_=now; hz_=hz; index_=1; }
    uint64_t Next() const { return origin_ + (index_ * 1000000ull) / hz_; }
    bool Due(uint64_t now) const { return now >= Next(); }
    uint64_t Advance() { const auto result=Next(); ++index_; return result; }
private:
    uint64_t origin_=0, index_=1;
    uint32_t hz_=1;
};

// A producer owns one slot, a consumer owns at most one, and the middle slot
// contains the latest publication. Neither endpoint touches the other's slot.
// Exchange transfers ownership of both data and the publication flag atomically.
class VitaCpuTripleOwnership {
public:
    uint32_t WriteSlot() const { return write_; }
    uint32_t ReadSlot() const { return read_; }
    bool Publish() {
        const auto previous=middle_.exchange(write_ | kReady, std::memory_order_acq_rel);
        write_=previous & kIndex;
        return (previous & kReady) != 0; // an unconsumed presentation was replaced
    }
    bool Acquire() {
        auto expected=middle_.load(std::memory_order_acquire);
        while (expected & kReady) {
            if (middle_.compare_exchange_weak(expected,read_,std::memory_order_acq_rel)) {
                read_=expected & kIndex;
                return true;
            }
        }
        return false;
    }
    void Reset() { write_=0; read_=2; middle_.store(1); }
private:
    static constexpr uint32_t kReady=4, kIndex=3;
    uint32_t write_=0, read_=2;
    alignas(64) std::atomic<uint32_t> middle_{1};
};

} // namespace kisak::vita
