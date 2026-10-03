#pragma once

#include <array>
#include <cstdint>
#include <cstdio>

namespace kisak::vita {

// One pointer-free sample from the playable frame loop.  Values are stored in
// microseconds so recording a frame does not perform floating-point work or
// allocate memory on Vita.
struct VitaPerfFrame {
    uint64_t wallEndUsec = 0;
    uint32_t frameNumber = 0;
    uint32_t mapIndex = UINT32_MAX;
    uint32_t frameUsec = 0;
    // Legacy name: main-thread elapsed time INCLUDING blocking waits, not CPU utilization.
    uint32_t cpuWorkUsec = 0;
    uint32_t sourceServerUsec = 0;
    uint32_t sourceClientUsec = 0;
    uint32_t visibilityUsec = 0;
    uint32_t drawSubmitUsec = 0;
    uint32_t swapWaitUsec = 0;
    uint32_t audioUsec = 0;
    uint32_t streamingUsec = 0;
    uint32_t drawCalls = 0;
    uint32_t triangles = 0;
    uint32_t stateChanges = 0;
    uint32_t visibleSurfaces = 0;
    uint32_t submittedSurfaces = 0;
    uint64_t cpuResidentBytes = 0;
    uint64_t gpuResidentBytes = 0;
    uint64_t arenaHighWaterBytes = 0;
    uint32_t worldWidth=0, worldHeight=0, viewmodelWidth=0, viewmodelHeight=0;
    uint32_t viewportSeen=0, viewportMismatches=0;
    int observedViewports[3][4]{}; // world, static props, viewmodel; zero if not observed
    uint32_t compositionUsec=0, grassCandidates=0, grassOmissions=0;
    uint32_t sunOnlyDraws=0, fullLightDraws=0, postUsec=0, queueUsec=0, cleanupWaitUsec=0;
    uint32_t sourceTicks=0, backlogMs=0;
    uint64_t targetLogicalBytes=0, textureBytes=0, meshBytes=0;
    bool adaptiveLighting=false, balancedGrass=false;
    int clocks[4]{};
    bool cpuMemoryAvailable = false;
    bool gpuMemoryAvailable = false;

};

struct VitaPerfSummary {
    uint32_t sampleCount = 0;
    uint32_t mapIndex = UINT32_MAX;
    uint32_t medianFrameUsec = 0;
    uint32_t p95FrameUsec = 0;
    uint32_t maximumFrameUsec = 0;
    uint32_t maximumSwapWaitUsec = 0;
    uint32_t maximumDrawCalls = 0;
    uint32_t maximumTriangles = 0;
    uint32_t maximumStateChanges = 0;
    uint64_t cpuResidentPeakBytes = 0;
    uint64_t gpuResidentPeakBytes = 0;
    uint64_t arenaHighWaterBytes = 0;
    uint32_t overThirtyFpsBudget = 0;
    uint32_t overFifteenFpsBudget = 0;
    uint32_t gameplayStalls = 0;
    uint32_t consecutiveSlowOneSecondWindows = 0;
    bool mandatoryFloorMet = false;
    uint64_t durationUsec = 0;
    bool completeRun = false;
    bool recordingOverflow = false;
    bool cpuMemoryAvailable = false;
    bool gpuMemoryAvailable = false;
    uint32_t cpuMemoryMissingSamples = 0;
    uint32_t gpuMemoryMissingSamples = 0;
    uint64_t cpuWorkTotalUsec = 0;
    uint64_t sourceServerTotalUsec = 0;
    uint64_t sourceClientTotalUsec = 0;
    uint64_t visibilityTotalUsec = 0;
    uint64_t drawSubmitTotalUsec = 0;
    uint64_t swapWaitTotalUsec = 0;
    uint64_t audioTotalUsec = 0;
    uint64_t streamingTotalUsec = 0;
    bool timingValid = false;
    bool interrupted = false;
};

// Exact nearest-rank percentiles for an entire traversal, independent of the
// short overlay ring. Storage is fixed; overflow invalidates qualification.
// 16,384 samples cover the 180-second traversal at either supported cap.
class VitaPerfRun {
public:
    static constexpr uint32_t kCapacity = 16384;
    static constexpr uint64_t kWarmupUsec = 10000000;
    static constexpr uint64_t kTraversalUsec = 180000000;
    void Reset(uint32_t mapIndex, uint64_t warmupUsec = kWarmupUsec);
    void Record(const VitaPerfFrame& frame);
    void Interrupt();
    VitaPerfSummary Summarize() const;
private:
    std::array<uint32_t, kCapacity> times_{};
    mutable std::array<uint32_t, kCapacity> sorted_{};
    VitaPerfSummary summary_{};
    uint64_t origin_ = 0;
    uint64_t start_ = 0;
    uint64_t windowStart_ = 0;
    uint64_t warmupUsec_ = kWarmupUsec;
    uint64_t previousEnd_ = 0;
    uint32_t windowFrames_ = 0;
    uint32_t slowRun_ = 0;
    bool started_ = false;
    bool measured_ = false;
    bool interrupted_ = false;
    bool clockValid_ = true;
};

// A power-of-two ring is deliberately used instead of a growing telemetry
// vector.  The hot Record path is one assignment and a pair of counters; the
// percentile sort occurs only when a report is requested.
class VitaPerfRing {
public:
    static constexpr uint32_t kCapacity = 256;
    static_assert(kCapacity != 0 && (kCapacity & (kCapacity - 1u)) == 0,
                  "VitaPerfRing capacity must remain a power of two");

    void Reset(uint32_t mapIndex = UINT32_MAX);
    void Record(const VitaPerfFrame& frame);
    VitaPerfSummary Summarize() const;
    uint32_t Count() const { return count_; }
    uint64_t TotalFrames() const { return totalFrames_; }

private:
    std::array<VitaPerfFrame, kCapacity> frames_{};
    uint32_t next_ = 0;
    uint32_t count_ = 0;
    uint32_t mapIndex_ = UINT32_MAX;
    uint64_t totalFrames_ = 0;
};

// Bounded JSONL writer for hardware/profile captures.  It writes one compact
// aggregate rather than streaming every frame and disables itself after any
// I/O failure so storage latency can never enter the render loop repeatedly.
class VitaPerfWriter {
public:
#if KISAK_VITA_CPU_RUNTIME
    static constexpr uint32_t kMaxBytes = 128u * 1024u * 1024u;
#else
    static constexpr uint32_t kMaxBytes = 64u * 1024u * 1024u;
#endif

    bool Open(const char* path, const char* buildKind, const char* contentHash,
              const char* deviceIdentity, bool stockClocks);
    bool Write(const VitaPerfSummary& summary, const char* appState);
    bool WriteFrame(const VitaPerfFrame& frame);
    void Close();
    bool IsOpen() const { return file_ != nullptr || asynchronous_; }
    ~VitaPerfWriter() { Close(); }

private:
    bool Append(const char* text, uint32_t bytes);

    std::array<char,65536> buffer_{};
    FILE* file_ = nullptr;
    uint32_t bytes_ = 0;
    bool asynchronous_ = false;
};

} // namespace kisak::vita
