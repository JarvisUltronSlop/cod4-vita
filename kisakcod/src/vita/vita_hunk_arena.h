#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace kisak::vita {

// Vita has no desktop reserve/commit virtual-memory contract. Allocate stable
// pages on demand while retaining source Hunk marks, alignment and zeroing.
// Each individual allocation remains contiguous. Logical marks include page
// tails, so rewinding never relocates an allocation retained by source code.
class VitaHunkArena {
public:
    static constexpr size_t kCapacity = 16u << 20;
    static constexpr size_t kPageBytes = 256u << 10;
    static constexpr size_t kMaximumAlignment = 4096;
    using Allocator = void* (*)(size_t);
    using Deallocator = void (*)(void*);

    explicit VitaHunkArena(Allocator allocate = &std::malloc,
                           Deallocator release = &std::free)
        : allocate_(allocate), release_(release) {}
    ~VitaHunkArena() { Clear(); }
    VitaHunkArena(const VitaHunkArena&) = delete;
    VitaHunkArena& operator=(const VitaHunkArena&) = delete;

    void* Allocate(size_t bytes, size_t alignment) {
        if (!alignment || alignment > kMaximumAlignment
            || (alignment & (alignment - 1)) || bytes > kCapacity)
            return nullptr;
        bytes = std::max<size_t>(bytes, 1);
        if (count_) {
            Page& page = pages_[count_ - 1];
            const size_t offset = AlignedOffset(page, position_ - page.start, alignment);
            if (offset <= page.bytes && bytes <= page.bytes - offset)
                return Use(page, offset, bytes);
        }
        const size_t pageBytes = std::max(kPageBytes,
            (bytes + alignment - 1 + 4095) & ~size_t(4095));
        if (count_ == pages_.size() || pageBytes > kCapacity - reserved_)
            return nullptr;
        void* allocation = allocate_(pageBytes);
        if (!allocation) return nullptr;
        Page& page = pages_[count_++];
        page = {static_cast<uint8_t*>(allocation), reserved_, pageBytes};
        reserved_ += pageBytes;
        peakReserved_ = std::max(peakReserved_, reserved_);
        return Use(page, AlignedOffset(page, 0, alignment), bytes);
    }

    bool Rewind(size_t mark) {
        if (mark > position_) return false;
        while (count_ && pages_[count_ - 1].start >= mark) {
            Page& page = pages_[--count_];
            reserved_ -= page.bytes;
            release_(page.data);
            page = {};
        }
        position_ = mark;
        return true;
    }

    bool Contains(const void* pointer) const {
        const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
        for (size_t i = 0; pointer && i < count_; ++i) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(pages_[i].data);
            if (address >= base && address - base < pages_[i].bytes) return true;
        }
        return false;
    }

    void Clear() {
        Rewind(0);
        highWater_ = peakReserved_ = 0;
    }
    size_t Mark() const { return position_; }
    size_t Reserved() const { return reserved_; }
    size_t HighWater() const { return highWater_; }
    size_t PeakReserved() const { return peakReserved_; }
    uint8_t* Base() const { return count_ ? pages_[0].data : nullptr; }

private:
    struct Page { uint8_t* data = nullptr; size_t start = 0; size_t bytes = 0; };
    static size_t AlignedOffset(const Page& page, size_t offset, size_t alignment) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(page.data);
        return ((base + offset + alignment - 1) & ~(uintptr_t(alignment) - 1)) - base;
    }
    void* Use(Page& page, size_t offset, size_t bytes) {
        position_ = page.start + offset + bytes;
        highWater_ = std::max(highWater_, position_);
        std::memset(page.data + offset, 0, bytes);
        return page.data + offset;
    }
    std::array<Page, kCapacity / kPageBytes> pages_{};
    size_t count_ = 0, position_ = 0, reserved_ = 0;
    size_t highWater_ = 0, peakReserved_ = 0;
    Allocator allocate_;
    Deallocator release_;
};

} // namespace kisak::vita
