#pragma once
#include <array>
#include <cstdint>

namespace kisak::vita {
// Render-owner only. Count completed frames, never simulation updates or
// published snapshots. Retain the interval crossing the one-second boundary
// so slow frames and stalls contribute their full elapsed duration.
class VitaPresentedFrameRate {
public:
    struct Result {
        uint32_t timesTen=0, intervals=0;
        uint64_t durationUs=0, intervalUs=0;
        bool valid=false;
    };
    void Reset() { first_=count_=0; generation_=0; }
    Result Observe(uint64_t now, uint32_t generation, bool gameplay) {
        if (!gameplay) { Reset(); return {}; }
        if (!count_ || generation!=generation_) {
            Reset(); generation_=generation;
            timestamps_[0]=now; count_=1; return {};
        }
        const uint64_t previous=At(count_-1);
        if (now==previous) return Read(0); // A duplicate is not another frame.
        if (now<previous) {
            Reset(); generation_=generation;
            timestamps_[0]=now; count_=1; return {};
        }
        if (count_==timestamps_.size()) Pop();
        timestamps_[(first_+count_)%timestamps_.size()]=now; ++count_;
        const uint64_t cutoff=now>1000000 ? now-1000000 : 0;
        while (count_>2 && At(1)<=cutoff) Pop();
        return Read(now-previous);
    }
private:
    uint64_t At(uint32_t offset) const { return timestamps_[(first_+offset)%timestamps_.size()]; }
    void Pop() { first_=(first_+1)%timestamps_.size(); --count_; }
    Result Read(uint64_t interval) const {
        if (count_<2) return {};
        const uint64_t duration=At(count_-1)-At(0);
        const uint32_t intervals=count_-1;
        return {uint32_t(uint64_t(intervals)*10000000/duration), intervals, duration, interval, true};
    }
    std::array<uint64_t,256> timestamps_{};
    uint32_t first_=0, count_=0, generation_=0;
};
}
