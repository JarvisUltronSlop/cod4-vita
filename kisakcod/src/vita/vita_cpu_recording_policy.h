#pragma once
#include <cstdint>

namespace kisak::vita {
constexpr uint32_t kCpuSourceEventCapacity = 8192;
// Writer-owned dirty-file rotation. Busy files also flush naturally when their
// existing stdio buffers fill; explicit flush requests still drain all files.
class VitaCpuFlushSchedule {
    bool dirty_[6]{};
    unsigned cursor_ = 0;
    uint64_t next_ = 0;
public:
    void Mark(unsigned file) { if (file < 6) dirty_[file] = true; }
    bool Dirty(unsigned file) const { return file < 6 && dirty_[file]; }
    void Clean(unsigned file) { if (file < 6) dirty_[file] = false; }
    int Periodic(uint64_t now) {
        if (now < next_) return -1;
        next_ = now + 1000000;
        for (unsigned n=0; n<6; ++n) {
            const unsigned file = cursor_;
            cursor_ = (cursor_ + 1) % 6;
            if (dirty_[file]) return int(file);
        }
        return -1;
    }
};
}
