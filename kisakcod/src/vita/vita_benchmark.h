#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <vita/vita_perf.h>
#include <vita/vita_sha256.h>
#include <vita/vita_benchmark_metrics.h>
#include <vita/vita_benchmark_geometry.h>
#include <vita/vita_audio_perf.h>
#include <vita/vita_benchmark_debris.h>
#include <vita/vita_benchmark_textures.h>

namespace kisak::vita {
struct VitaBenchmarkCase {
    const char* id;
    const char* baseline;
    const char* factor;
    uint32_t width, height;
    bool clearOnly;
    uint32_t shaderMode = 0; // 0 authored, 1 copy, 2 simple, 3 no lights, 4 no specular, 5 adaptive, 6 no normals, 7 no fog
    uint32_t geometryMode = 0;
    uint32_t audioMode = 0;
    uint32_t textureMode = 0; // bit 0 world/props, bit 1 viewmodel; one authored top mip
};
// Resolution is the only quality factor in round one. The clear control is
// explicitly separate and must never be interpreted as a playable candidate.
inline constexpr VitaBenchmarkCase kResolutionRound[] = {
    {"clear-control", "native-a", "scene-disabled", 960, 544, true},
    {"native-a", "native-a", "none", 960, 544, false},
    {"scene-720", "native-a", "scene-resolution", 720, 408, false},
    {"native-b", "native-a", "none", 960, 544, false},
    {"scene-480", "native-b", "scene-resolution", 480, 272, false},
    {"native-c", "native-b", "none", 960, 544, false},
};

inline constexpr VitaBenchmarkCase kShadingRound[] = {
    {"native-a", "native-a", "none", 960, 544, false, 0},
    {"post-copy", "native-a", "post-fragment-program", 960, 544, false, 1},
    {"native-b", "native-a", "none", 960, 544, false, 0},
    {"simple-material", "native-b", "scene-fragment-program", 960, 544, false, 2},
    {"native-c", "native-b", "none", 960, 544, false, 0},
};

inline constexpr VitaBenchmarkCase kMaterialRound[] = {
    {"native-a", "native-a", "none", 960, 544, false, 0},
    {"no-local-lights", "native-a", "local-light-loops", 960, 544, false, 3},
    {"native-b", "native-a", "none", 960, 544, false, 0},
    {"no-specular", "native-b", "specular-response", 960, 544, false, 4},
    {"native-c", "native-b", "none", 960, 544, false, 0},
};

inline constexpr VitaBenchmarkCase kNormalDispatchRound[] = {
    {"native-a", "native-a", "none", 960, 544, false, 0},
    {"adaptive-lights", "native-a", "zero-colour-light-dispatch", 960, 544, false, 5},
    {"native-b", "native-a", "none", 960, 544, false, 0},
    {"no-normal-map", "native-b", "normal-map-response", 960, 544, false, 6},
    {"native-c", "native-b", "none", 960, 544, false, 0},
};

inline constexpr VitaBenchmarkCase kGeometryRound[] = {
    {"native-a", "native-a", "none", 960, 544, false, 0, 0},
    {"no-foliage-models", "native-a", "foliage-model-rendering", 960, 544, false, 0, 1},
    {"native-b", "native-a", "none", 960, 544, false, 0, 0},
    {"no-static-models", "native-b", "static-model-rendering", 960, 544, false, 0, 2},
    {"native-c", "native-b", "none", 960, 544, false, 0, 0},
};

inline constexpr VitaBenchmarkCase kAudioRound[] = {
    {"native-a", "native-a", "none", 960, 544, false, 0, 0, 0},
    {"no-background-streams", "native-a", "looped-background-stream-work", 960, 544, false, 0, 0, 1},
    {"native-b", "native-a", "none", 960, 544, false, 0, 0, 0},
    {"ambient-pcm", "native-b", "ambient-mp3-predecode", 960, 544, false, 0, 0, 2},
    {"native-c", "native-b", "none", 960, 544, false, 0, 0, 0},
};

// Three user-requested texture combinations, each bracketed by normal textures.
inline constexpr VitaBenchmarkCase kTextureRound[] = {
    {"native-a", "native-a", "none", 960, 544, false, 0, 0, 0, 0},
    {"world-mip", "native-a", "world-top-mip", 960, 544, false, 0, 0, 0, 1},
    {"native-b", "native-a", "none", 960, 544, false, 0, 0, 0, 0},
    {"viewmodel-mip", "native-b", "viewmodel-top-mip", 960, 544, false, 0, 0, 0, 2},
    {"native-c", "native-b", "none", 960, 544, false, 0, 0, 0, 0},
    {"both-mips", "native-c", "world-and-viewmodel-top-mips", 960, 544, false, 0, 0, 0, 3},
    {"native-d", "native-c", "none", 960, 544, false, 0, 0, 0, 0},
};

inline constexpr VitaBenchmarkCase kDebrisFogRound[] = {
    {"native-a", "native-a", "none", 960, 544, false, 0, 0},
    {"no-ground-clutter", "native-a", "ground-clutter-rendering", 960, 544, false, 0, 3},
    {"native-b", "native-a", "none", 960, 544, false, 0, 0},
    {"no-fog", "native-b", "distance-fog-shader", 960, 544, false, 7, 0},
    {"native-c", "native-b", "none", 960, 544, false, 0, 0},
};

// A combined candidate is measured directly; prior individual savings are not additive.
inline constexpr VitaBenchmarkCase kBalancedRound[] = {
    {"native-a", "native-a", "none", 960,544,false},
    {"balanced-combined", "native-a", "combined-balanced-profile",720,408,false,5},
    {"native-b", "native-a", "none",960,544,false},
};
// Timings are elapsed wall time unless explicitly named otherwise. Async GC
// worker time overlaps the main thread; it is NOT an additive frame phase.
struct VitaBenchmarkFrame {
    VitaPerfFrame frame;
    VitaBenchmarkGeometry geometry;
    VitaBenchmarkDebris debris;
    uint32_t noFogDraws=0, fogEligibleDraws=0;
    bool fogEnabled=false; float fogStart=0, fogDensity=0;
    VitaAudioPerf audioDetail;
    VitaBenchmarkTextureStats textures;
    VitaBenchmarkMetrics clientPhases;
    uint32_t worldUs=0, propsFxUs=0, viewmodelDrawUs=0, priorRecorderUs=0, priorPacingUs=0;
    uint32_t sysAppMode=0, msaaEnum=0, displayBuffers=0;
    bool sunOnlyShader=false, bloomActive=false;
    uint32_t shaderMode=0, sceneShaderDraws=0, simpleMaterialDraws=0, postCopyDraws=0, authoredPostDraws=0;
    uint32_t noLocalLightDraws=0, noSpecularDraws=0, specularEligibleDraws=0, nonSunLightInputs=0;
    uint32_t noNormalMapDraws=0, normalEligibleDraws=0, adaptiveFullLightDraws=0;
    uint32_t queueUs=0, gcWaitUs=0, gcWorkerUs=0, sceneEndUs=0, swapOtherUs=0;
    uint32_t gcFrees=0, gcTargets=0, postUs=0, renderUs=0, uiUs=0, loopbackUs=0;
    uint32_t serverTicks=0, backlogMs=0, clFrameUs=0, cgameUs=0;
    uint32_t transientBytes=0, transientDrops=0, shadowPasses=0, lightCount=0;
    uint32_t sceneWidth=0, sceneHeight=0;
    uint64_t textureBytes=0, meshBytes=0, targetBytes=0;
    uint64_t audioReadUs=0, audioReadBytes=0, audioDecodeUs=0, audioFrames=0;
    uint64_t audioUploadUs=0, audioQueueUs=0;
    uint32_t audioStreams=0;
    int arm=0, bus=0, gpu=0, xbar=0, battery=-1, charging=-1;
    float origin[3]{}, angles[3]{};
    uint32_t weapon=0, sourceTime=0;
};

// Fixed storage and buffered JSONL, with no per-frame allocation or fflush.
class VitaBenchmark {
public:
    static constexpr uint32_t kSeed = 0x434f4434;
    bool Start(const char* path, const char* device, const char* contentHash, uint32_t round=1);
    bool Active() const { return active_; }
    bool NeedsLoad() const { return active_ && needsLoad_; }
    void Prepare(); // Incremental identity reads, while the menu still renders.
    void Loaded(uint64_t now);
    void TextureUpload(uint32_t nameHash, VitaTextureRole role, uint32_t width, uint32_t height,
        uint32_t levels, uint32_t format, uint32_t flags, size_t bytes,
        const VitaTextureMipSelection& selected, bool success);
    // True means the completed segment requires a new source session.
    bool Record(const VitaBenchmarkFrame& frame);
    void Abort(const char* reason);
    // Block at the same application sample that is subsequently published to
    // CL_Input. Hardware activity cannot alter the fixed benchmark workload.
    template<class InputSample>
    void FilterInput(InputSample& sample, uint32_t rawButtons=0, uint32_t touchReports=0) {
        if (!active_) return;
        const bool cancel=sample.menuBackPressed || sample.startPressed;
        const uint32_t rear=uint32_t(sample.rearFragHeld)
            | (uint32_t(sample.rearSpecialHeld)<<1)
            | (uint32_t(sample.rearSprintHeld)<<2)
            | (uint32_t(sample.rearMeleePressed)<<3);
        const std::array<int32_t,8> activity={int32_t(sample.buttons), int32_t(rawButtons),
            sample.forwardmove, sample.rightmove, sample.lookX, sample.lookY,
            int32_t(rear),int32_t(touchReports)};
        const bool edge=sample.weaponCyclePressed || sample.performanceTogglePressed
            || sample.selectPressed;
        sample={}; // Includes relative/absolute angles, held states and all edges.
        if (cancel) Abort("user-cancelled");
        else RecordBlockedInput(activity,edge);
    }
    const VitaBenchmarkCase& Case() const { return round_==9 ? kBalancedRound[index_] : round_==8 ? kDebrisFogRound[index_] : round_==7 ? kTextureRound[index_] : round_==6 ? kAudioRound[index_] : round_==5 ? kGeometryRound[index_] : round_==4 ? kNormalDispatchRound[index_] : round_==3 ? kMaterialRound[index_] : round_==2 ? kShadingRound[index_] : kResolutionRound[index_]; }
    uint32_t CaseCount() const { return round_==9 ? 3u : round_==7 ? 7u : round_>=2 ? 5u : 6u; }
    const char* RoundName() const { return round_==9 ? "balanced-combined-9" : round_==8 ? "debris-fog-8" : round_==7 ? "textures-7" : round_==6 ? "audio-6" : round_==5 ? "geometry-5" : round_==4 ? "normal-dispatch-4" : round_==3 ? "material-3" : round_==2 ? "shading-2" : "resolution-1"; }
    uint32_t Index() const { return index_; }
    const char* Message() const { return message_; }
    const char* Path() const { return path_; }
    bool Measuring() const { return measuring_; }
    ~VitaBenchmark();
private:
    bool Append(const char* line);
    void FinishSegment();
    void RecordBlockedInput(const std::array<int32_t,8>& activity, bool edge);
    FILE* file_ = nullptr;
    std::array<char, 65536> buffer_{};
    std::array<uint32_t, 8192> times_{};
    uint64_t loadedAt_=0, firstEnd_=0, lastEnd_=0, duration_=0, bytes_=0;
    uint32_t index_=0, count_=0, stalls_=0;
    uint32_t round_=1;
    std::array<int32_t,8> previousBlockedInput_{};
    uint32_t blockedInputFrames_=0, blockedInputEvents_=0, blockedInputRecords_=0;
    bool active_=false, needsLoad_=false, measuring_=false, valid_=true;
    char message_[128]{}, path_[192]{};
    FILE* identityFile_ = nullptr;
    VitaSha256 identityHash_{};
    uint32_t identityIndex_=0;
    std::array<char, 32768> identityBuffer_{};
};
} // namespace kisak::vita

// Zero during ordinary play; source random initialization is untouched then.
extern "C" unsigned VitaBenchmark_SourceSeed();
