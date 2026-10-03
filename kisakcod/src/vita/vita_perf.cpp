#include <vita/vita_perf.h>
#include <vita/vita_cpu_profile.h>
#include <vita/vita_sha256.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace kisak::vita {
namespace {

void Accumulate(VitaPerfSummary& summary, const VitaPerfFrame& frame) {
    summary.maximumFrameUsec = std::max(summary.maximumFrameUsec, frame.frameUsec);
    summary.maximumSwapWaitUsec = std::max(summary.maximumSwapWaitUsec, frame.swapWaitUsec);
    summary.maximumDrawCalls = std::max(summary.maximumDrawCalls, frame.drawCalls);
    summary.maximumTriangles = std::max(summary.maximumTriangles, frame.triangles);
    summary.maximumStateChanges = std::max(summary.maximumStateChanges, frame.stateChanges);
    if (frame.cpuMemoryAvailable) {
        summary.cpuMemoryAvailable = summary.cpuMemoryMissingSamples == 0;
        summary.cpuResidentPeakBytes = std::max(summary.cpuResidentPeakBytes, frame.cpuResidentBytes);
    } else {
        ++summary.cpuMemoryMissingSamples;
        summary.cpuMemoryAvailable = false;
    }
    if (frame.gpuMemoryAvailable) {
        summary.gpuMemoryAvailable = summary.gpuMemoryMissingSamples == 0;
        summary.gpuResidentPeakBytes = std::max(summary.gpuResidentPeakBytes, frame.gpuResidentBytes);
    } else {
        ++summary.gpuMemoryMissingSamples;
        summary.gpuMemoryAvailable = false;
    }
    summary.cpuWorkTotalUsec += frame.cpuWorkUsec;
    summary.sourceServerTotalUsec += frame.sourceServerUsec;
    summary.sourceClientTotalUsec += frame.sourceClientUsec;
    summary.visibilityTotalUsec += frame.visibilityUsec;
    summary.drawSubmitTotalUsec += frame.drawSubmitUsec;
    summary.swapWaitTotalUsec += frame.swapWaitUsec;
    summary.audioTotalUsec += frame.audioUsec;
    summary.streamingTotalUsec += frame.streamingUsec;
    summary.arenaHighWaterBytes = std::max(summary.arenaHighWaterBytes, frame.arenaHighWaterBytes);
    summary.overThirtyFpsBudget += frame.frameUsec > 33334u;
    summary.overFifteenFpsBudget += frame.frameUsec > 66667u;
    summary.gameplayStalls += frame.frameUsec > 250000u;
}

uint32_t Percentile(std::array<uint32_t, VitaPerfRing::kCapacity>* values,
                    uint32_t count, uint32_t numerator, uint32_t denominator) {
    if (!values || count == 0 || denominator == 0)
        return 0;
    std::sort(values->begin(), values->begin() + count);
    // Nearest-rank percentile.  p95(1) therefore selects the only sample and
    // never indexes beyond the bounded array.
    const uint64_t rank = (static_cast<uint64_t>(count) * numerator
                           + denominator - 1u) / denominator;
    const uint32_t index = static_cast<uint32_t>(std::max<uint64_t>(1u, rank) - 1u);
    return (*values)[std::min(index, count - 1u)];
}

} // namespace

void VitaPerfRun::Reset(uint32_t mapIndex, uint64_t warmupUsec) {
    summary_ = {};
    summary_.mapIndex = mapIndex;
    origin_ = start_ = windowStart_ = previousEnd_ = 0;
    windowFrames_ = slowRun_ = 0;
    warmupUsec_ = warmupUsec;
    started_ = measured_ = interrupted_ = false;
    clockValid_ = true;
}

void VitaPerfRun::Interrupt() { if (!summary_.completeRun) interrupted_ = true; }

void VitaPerfRun::Record(const VitaPerfFrame& frame) {
    if (summary_.completeRun)
        return;
    if (frame.mapIndex != summary_.mapIndex || frame.frameUsec == 0
        || frame.wallEndUsec < frame.frameUsec
        || (started_ && frame.wallEndUsec <= previousEnd_)) {
        clockValid_ = false;
        return;
    }
    const uint64_t begin = frame.wallEndUsec - frame.frameUsec;
    if (started_ && begin != previousEnd_) {
        // A gap or overlapping interval means the recording is incomplete.
        // Comparing before subtraction also prevents unsigned warmup wrap.
        clockValid_ = false;
        previousEnd_ = frame.wallEndUsec;
        return;
    }
    if (!started_) {
        origin_ = begin;
        started_ = true;
    }
    previousEnd_ = frame.wallEndUsec;
    if (begin - origin_ < warmupUsec_)
        return;
    if (!measured_) {
        start_ = windowStart_ = begin;
        measured_ = true;
    }
    // Finish every elapsed half-open one-second window, including empty
    // windows crossed by a stall. Never reset the clock to the latest frame.
    while (frame.wallEndUsec >= windowStart_ + 1000000u) {
        slowRun_ = windowFrames_ < 15u ? slowRun_ + 1u : 0u;
        summary_.consecutiveSlowOneSecondWindows = std::max(
            summary_.consecutiveSlowOneSecondWindows, slowRun_);
        windowFrames_ = 0;
        windowStart_ += 1000000u;
    }
    ++windowFrames_;
    Accumulate(summary_, frame);
    if (summary_.sampleCount < kCapacity)
        times_[summary_.sampleCount++] = frame.frameUsec;
    else
        summary_.recordingOverflow = true;
    summary_.durationUsec = frame.wallEndUsec - start_;
    summary_.completeRun = summary_.durationUsec >= kTraversalUsec;
}

VitaPerfSummary VitaPerfRun::Summarize() const {
    VitaPerfSummary result = summary_;
    result.timingValid = clockValid_ && started_;
    result.interrupted = interrupted_;
    if (result.sampleCount) {
        std::copy_n(times_.begin(), result.sampleCount, sorted_.begin());
        std::sort(sorted_.begin(), sorted_.begin() + result.sampleCount);
        const auto percentile = [&](uint32_t percent) {
            return sorted_[(static_cast<uint64_t>(result.sampleCount) * percent + 99u) / 100u - 1u];
        };
        result.medianFrameUsec = percentile(50);
        result.p95FrameUsec = percentile(95);
    }
    result.mandatoryFloorMet = result.completeRun && clockValid_
        && !interrupted_ && !result.recordingOverflow
        && result.p95FrameUsec <= 66667u && result.gameplayStalls == 0
        && result.consecutiveSlowOneSecondWindows < 2u;
    return result;
}

void VitaPerfRing::Reset(uint32_t mapIndex) {
    frames_.fill(VitaPerfFrame{});
    next_ = 0;
    count_ = 0;
    mapIndex_ = mapIndex;
    totalFrames_ = 0;
}

void VitaPerfRing::Record(const VitaPerfFrame& frame) {
    frames_[next_] = frame;
    next_ = (next_ + 1u) & (kCapacity - 1u);
    count_ = std::min(count_ + 1u, kCapacity);
    ++totalFrames_;
    mapIndex_ = frame.mapIndex;
}

VitaPerfSummary VitaPerfRing::Summarize() const {
    VitaPerfSummary summary{};
    summary.sampleCount = count_;
    summary.mapIndex = mapIndex_;
    if (count_ == 0)
        return summary;

    std::array<uint32_t, kCapacity> frameTimes{};
    uint32_t oldest = (next_ + kCapacity - count_) & (kCapacity - 1u);
    uint64_t windowStart = 0;
    uint32_t windowFrames = 0;
    uint32_t slowWindowsInARow = 0;
    for (uint32_t index = 0; index < count_; ++index) {
        const VitaPerfFrame& frame = frames_[(oldest + index) & (kCapacity - 1u)];
        summary.cpuMemoryAvailable |= frame.cpuMemoryAvailable;
        summary.gpuMemoryAvailable |= frame.gpuMemoryAvailable;
        frameTimes[index] = frame.frameUsec;
        summary.maximumFrameUsec = std::max(summary.maximumFrameUsec,
                                            frame.frameUsec);
        summary.maximumSwapWaitUsec = std::max(summary.maximumSwapWaitUsec,
                                               frame.swapWaitUsec);
        summary.maximumDrawCalls = std::max(summary.maximumDrawCalls,
                                            frame.drawCalls);
        summary.maximumTriangles = std::max(summary.maximumTriangles,
                                            frame.triangles);
        summary.maximumStateChanges = std::max(summary.maximumStateChanges,
                                               frame.stateChanges);
        summary.cpuResidentPeakBytes = std::max(summary.cpuResidentPeakBytes,
                                                frame.cpuResidentBytes);
        summary.gpuResidentPeakBytes = std::max(summary.gpuResidentPeakBytes,
                                                frame.gpuResidentBytes);
        summary.arenaHighWaterBytes = std::max(summary.arenaHighWaterBytes,
                                               frame.arenaHighWaterBytes);
        if (frame.frameUsec > 33333u)
            ++summary.overThirtyFpsBudget;
        if (frame.frameUsec > 66667u)
            ++summary.overFifteenFpsBudget;
        if (frame.frameUsec > 250000u)
            ++summary.gameplayStalls;

        if (windowStart == 0)
            windowStart = frame.wallEndUsec;
        ++windowFrames;
        if (frame.wallEndUsec >= windowStart + 1000000u) {
            if (windowFrames < 15u) {
                ++slowWindowsInARow;
                summary.consecutiveSlowOneSecondWindows = std::max(
                    summary.consecutiveSlowOneSecondWindows,
                    slowWindowsInARow);
            } else {
                slowWindowsInARow = 0;
            }
            windowStart = frame.wallEndUsec;
            windowFrames = 0;
        }
    }
    std::array<uint32_t, kCapacity> medianTimes = frameTimes;
    summary.medianFrameUsec = Percentile(&medianTimes, count_, 50u, 100u);
    summary.p95FrameUsec = Percentile(&frameTimes, count_, 95u, 100u);
    // Do not certify a short startup fragment. At 15 FPS, 30 samples cover
    // two full one-second windows, which is the minimum sustained-floor test.
    // An overlay window is never a complete traversal and cannot certify it.
    summary.mandatoryFloorMet = false;
    return summary;
}

bool VitaPerfWriter::Open(const char* path, const char* buildKind,
                          const char* contentHash, const char* deviceIdentity,
                          bool stockClocks) {
    Close();
    if (!path)
        return false;
    // Vita3K's file layer can reopen an existing file for write without
    // truncating it. A shorter run then inherits the previous run's tail.
    // This writer owns a replace-on-open report; fail closed if replacement
    // is not possible rather than mixing evidence from different runs.
#if KISAK_VITA_CPU_RUNTIME
    asynchronous_=VitaCpuProfile_Read().active;
    if(!asynchronous_)return false;
#else
    if (std::remove(path) != 0 && errno != ENOENT)
        return false;
    file_ = std::fopen(path, "wb");
    if (!IsOpen())
        return false;
    std::setvbuf(file_,buffer_.data(),_IOFBF,buffer_.size());
#endif
    char header[512]{};
    const int length = std::snprintf(
        header, sizeof(header),
        "{\"format\":\"CODVIT-PERF-2\",\"build\":\"%s\","
        "\"contentHash\":\"%s\",\"device\":\"%s\","
        "\"stockClocks\":%s}\n",
        buildKind ? buildKind : "unknown",
        contentHash ? contentHash : "unknown",
        deviceIdentity ? deviceIdentity : "unknown",
        stockClocks ? "true" : "false");
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(header)
        || !Append(header, static_cast<uint32_t>(length))) {
        Close();
        return false;
    }
    // The packaged manifest binds exact SELF, source, VitaGL and pack hashes.
    // Hash once before gameplay; never hash content in a measured frame.
    char identity[65]{};
    FILE* provenance=std::fopen("app0:benchmark-identity.json","rb");
    if(provenance) {
        VitaSha256 hash; char block[4096]; size_t n=0,total=0;
        while((n=std::fread(block,1,sizeof(block),provenance))!=0 && total<2u*1024u*1024u) {
            hash.Update(block,n); total+=n;
        }
        if(!std::ferror(provenance) && std::feof(provenance)) {
            const auto digest=hash.Finish();
            for(unsigned i=0;i<32;++i) std::snprintf(identity+2*i,3,"%02x",unsigned(digest[i]));
        }
        std::fclose(provenance);
    }
    const int identityLength=std::snprintf(header,sizeof(header),
        "{\"type\":\"identity\",\"packagedManifestSha256\":%s%s%s,\"hardwareQualified\":false}\n",
        identity[0]?"\"":"",identity[0]?identity:"null",identity[0]?"\"":"");
    if(!Append(header,uint32_t(identityLength))) return false;
    return true;
}

bool VitaPerfWriter::Append(const char* text, uint32_t bytes) {
    if (!IsOpen() || !text || bytes == 0)
        return false;
    if (bytes > kMaxBytes - bytes_) {
#if KISAK_VITA_CPU_RUNTIME
        VitaCpuProfile_Invalidate("play-frame-log-full");
#endif
        Close();
        return false;
    }
    // Bounded buffering avoids a synchronous storage flush every frame.
    // Aggregate checkpoints and Close commit the buffer; crash tails can be incomplete.
#if KISAK_VITA_CPU_RUNTIME
    if(!VitaCpuProfile_AppendPerf(text,bytes)) {
#else
    if (std::fwrite(text, 1, bytes, file_) != bytes) {
#endif
        Close();
        return false;
    }
    bytes_ += bytes;
    return true;
}

bool VitaPerfWriter::WriteFrame(const VitaPerfFrame& f) {
    char line[2048];
    const int n=std::snprintf(line,sizeof(line),
        "{\"type\":\"play-frame\",\"map\":%u,\"n\":%u,\"endUs\":%llu,\"intervalUs\":%u,"
        "\"world\":[%u,%u],\"viewmodel\":[%u,%u],\"hud\":[960,544],\"post\":[960,544],"
        "\"viewportSeen\":%u,\"viewportMismatches\":%u,"
        "\"worldViewport\":[%d,%d,%d,%d],\"propViewport\":[%d,%d,%d,%d],\"viewmodelViewport\":[%d,%d,%d,%d],"
        "\"compositionUs\":%u,\"postUs\":%u,\"targetLogicalBytes\":%llu,"
        "\"grassOptionalCandidates\":%u,\"grassOmissions\":%u,\"balancedGrass\":%s,\"adaptiveLighting\":%s,"
        "\"sunOnlyDraws\":%u,\"fullLightDraws\":%u,\"clocks\":[%d,%d,%d,%d],"
        "\"mainElapsedUs\":%u,\"serverUs\":%u,\"clientUs\":%u,\"ticks\":%u,\"backlogMs\":%u,"
        "\"audioUs\":%u,\"swapUs\":%u,\"queueUs\":%u,\"cleanupWaitUs\":%u,"
        "\"draws\":%u,\"triangles\":%u,\"textureBytes\":%llu,\"meshBytes\":%llu,"
        "\"cpuMemoryAvailable\":%s,\"gpuMemoryAvailable\":%s,\"utilization\":null,\"hardwareQualified\":false}\n",
        f.mapIndex,f.frameNumber,(unsigned long long)f.wallEndUsec,f.frameUsec,
        f.worldWidth,f.worldHeight,f.viewmodelWidth,f.viewmodelHeight,
        f.viewportSeen,f.viewportMismatches,
        f.observedViewports[0][0],f.observedViewports[0][1],f.observedViewports[0][2],f.observedViewports[0][3],
        f.observedViewports[1][0],f.observedViewports[1][1],f.observedViewports[1][2],f.observedViewports[1][3],
        f.observedViewports[2][0],f.observedViewports[2][1],f.observedViewports[2][2],f.observedViewports[2][3],f.compositionUsec,f.postUsec,
        (unsigned long long)f.targetLogicalBytes,f.grassCandidates,f.grassOmissions,
        f.balancedGrass?"true":"false",f.adaptiveLighting?"true":"false",f.sunOnlyDraws,f.fullLightDraws,
        f.clocks[0],f.clocks[1],f.clocks[2],f.clocks[3],f.cpuWorkUsec,f.sourceServerUsec,f.sourceClientUsec,
        f.sourceTicks,f.backlogMs,f.audioUsec,f.swapWaitUsec,f.queueUsec,f.cleanupWaitUsec,f.drawCalls,f.triangles,
        (unsigned long long)f.textureBytes,(unsigned long long)f.meshBytes,
        f.cpuMemoryAvailable?"true":"false",f.gpuMemoryAvailable?"true":"false");
    return n>0 && size_t(n)<sizeof(line) && Append(line,uint32_t(n));
}

bool VitaPerfWriter::Write(const VitaPerfSummary& summary,
                           const char* appState) {
    if (!IsOpen())
        return false;
    char line[2048]{};
    char cpu[32] = "null", gpu[32] = "null";
    if (summary.cpuMemoryAvailable)
        std::snprintf(cpu, sizeof(cpu), "%llu", static_cast<unsigned long long>(summary.cpuResidentPeakBytes));
    if (summary.gpuMemoryAvailable)
        std::snprintf(gpu, sizeof(gpu), "%llu", static_cast<unsigned long long>(summary.gpuResidentPeakBytes));
    const int length = std::snprintf(
        line, sizeof(line),
        "{\"state\":\"%s\",\"map\":%u,\"samples\":%u,"
        "\"medianUs\":%u,\"p95Us\":%u,\"maxUs\":%u,"
        "\"swapMaxUs\":%u,\"drawCallsMax\":%u,"
        "\"trianglesMax\":%u,\"stateChangesMax\":%u,"
        "\"cpuPeak\":%s,\"gpuPeak\":%s,\"arenaPeak\":%llu,"
        "\"over30\":%u,\"over15\":%u,\"stalls\":%u,"
        "\"slowWindowRun\":%u,\"mandatoryFloorMet\":%s,"
        "\"durationUs\":%llu,\"completeRun\":%s,\"recordingOverflow\":%s,"
        "\"timingValid\":%s,\"interrupted\":%s,"
        "\"cpuMemoryMissingSamples\":%u,\"gpuMemoryMissingSamples\":%u,"
        "\"phaseTotalsUs\":{\"cpu\":%llu,\"server\":%llu,\"client\":%llu,"
        "\"visibility\":%llu,\"submission\":%llu,\"swap\":%llu,\"audio\":%llu,\"streaming\":%llu},"
        "\"hardwareQualified\":false}\n",
        appState ? appState : "unknown", summary.mapIndex,
        summary.sampleCount, summary.medianFrameUsec, summary.p95FrameUsec,
        summary.maximumFrameUsec, summary.maximumSwapWaitUsec,
        summary.maximumDrawCalls, summary.maximumTriangles,
        summary.maximumStateChanges,
        cpu, gpu,
        static_cast<unsigned long long>(summary.arenaHighWaterBytes),
        summary.overThirtyFpsBudget, summary.overFifteenFpsBudget,
        summary.gameplayStalls, summary.consecutiveSlowOneSecondWindows,
        summary.mandatoryFloorMet ? "true" : "false",
        static_cast<unsigned long long>(summary.durationUsec),
        summary.completeRun ? "true" : "false",
        summary.recordingOverflow ? "true" : "false",
        summary.timingValid ? "true" : "false", summary.interrupted ? "true" : "false",
        summary.cpuMemoryMissingSamples, summary.gpuMemoryMissingSamples,
        static_cast<unsigned long long>(summary.cpuWorkTotalUsec),
        static_cast<unsigned long long>(summary.sourceServerTotalUsec),
        static_cast<unsigned long long>(summary.sourceClientTotalUsec),
        static_cast<unsigned long long>(summary.visibilityTotalUsec),
        static_cast<unsigned long long>(summary.drawSubmitTotalUsec),
        static_cast<unsigned long long>(summary.swapWaitTotalUsec),
        static_cast<unsigned long long>(summary.audioTotalUsec),
        static_cast<unsigned long long>(summary.streamingTotalUsec));
    const bool written=length > 0 && static_cast<size_t>(length) < sizeof(line)
        && Append(line, static_cast<uint32_t>(length));
#if KISAK_VITA_CPU_RUNTIME
    return written && VitaCpuProfile_AppendPerf(nullptr,0,true);
#else
    return written && std::fflush(file_)==0;
#endif
}

void VitaPerfWriter::Close() {
#if KISAK_VITA_CPU_RUNTIME
    if(asynchronous_)VitaCpuProfile_AppendPerf(nullptr,0,true);
    asynchronous_=false;
#endif
    if (file_) {
        std::fflush(file_);
        std::fclose(file_);
    }
    file_ = nullptr;
    bytes_ = 0;
}

} // namespace kisak::vita
