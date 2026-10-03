#pragma once
#include <cstdint>

namespace kisak::vita {
// Deterministic sampling independent of gameplay RNG. Each thread owns its
// selector; generation changes restart sampling. No allocations or asset refs.
struct BrushScanSelector {
    uint32_t generation = UINT32_MAX;
    uint32_t ordinal = 0;
    bool Select(uint32_t nextGeneration) {
        if (generation != nextGeneration) { generation = nextGeneration; ordinal = 0; }
        const bool selected = ordinal == 0;
        ordinal = (ordinal + 1) % 64;
        return selected;
    }
};

// Sink supplies Now() and Counter(). Unselected scans make no timer/recorder
// calls. Selected scans report measured narrow-phase time and observed counts;
// do not extrapolate their times to all queries without considering sample bias.
template<class Sink> class BrushScanTiming {
public:
    explicit BrushScanTiming(bool selected) : selected_(selected) {
        if (selected_) {
            const auto before = Sink::Now();
            clockPairUs_ = Sink::Now() - before;
            start_ = Sink::Now();
        }
    }
    ~BrushScanTiming() {
        if (!selected_) return;
        const auto elapsed = Sink::Now() - start_;
        Sink::Counter("brush-sample-scan-us", elapsed, narrowUs_);
        Sink::Counter("brush-sample-counts", visited_, narrowCalls_);
        Sink::Counter("brush-sample-rejected", rejected_, visited_ - rejected_ - narrowCalls_);
        if(groupSkipped_) Sink::Counter("brush-sample-group-skipped", groupSkipped_, groups_);
        // Raw empty timer-pair duration; no automatic overhead subtraction.
        Sink::Counter("brush-sample-clock-us", clockPairUs_, 2 * narrowCalls_ + 4);
    }
    BrushScanTiming(const BrushScanTiming&) = delete;
    BrushScanTiming& operator=(const BrushScanTiming&) = delete;
    void SkipGroup(uint64_t count) { if (selected_) {groupSkipped_+=count;++groups_;} }
    void Visit() { if (selected_) ++visited_; }
    void Reject() { if (selected_) ++rejected_; }
    void BeginNarrow() { if (selected_) { ++narrowCalls_; narrowStart_ = Sink::Now(); } }
    void EndNarrow() { if (selected_) narrowUs_ += Sink::Now() - narrowStart_; }
private:
    uint64_t groupSkipped_=0, groups_=0;
    bool selected_;
    uint64_t start_ = 0, narrowStart_ = 0, narrowUs_ = 0, clockPairUs_ = 0;
    uint64_t visited_ = 0, rejected_ = 0, narrowCalls_ = 0;
};
}
