#include <vita/vita_perf.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace kisak::vita;
static VitaPerfRun run;

static void Require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

static void Frame(uint64_t& clock, uint32_t elapsed = 33333, bool measured = true) {
    clock += elapsed;
    VitaPerfFrame frame{};
    frame.wallEndUsec = clock;
    frame.frameUsec = elapsed;
    frame.mapIndex = 0;
    frame.cpuMemoryAvailable = frame.gpuMemoryAvailable = measured;
    frame.cpuResidentBytes = 100;
    frame.gpuResidentBytes = 200;
    run.Record(frame);
}

int main() {
    uint64_t clock = 0;
    run.Reset(0);
    for (int i = 0; i < 300; ++i) Frame(clock);
    Require(run.Summarize().sampleCount == 0, "warmup frames are excluded");
    for (int i = 0; i < 5410; ++i) Frame(clock);
    auto result = run.Summarize();
    Require(result.completeRun && result.mandatoryFloorMet, "full steady traversal qualifies timing");
    Require(result.medianFrameUsec == 33333 && result.p95FrameUsec == 33333, "exact whole-run percentiles");
    Require(result.cpuMemoryAvailable && result.cpuResidentPeakBytes == 100, "measured memory preserved");

    clock = 0; run.Reset(0, 0);
    Frame(clock, 300001);
    for (int i = 0; i < 5500; ++i) Frame(clock);
    result = run.Summarize();
    Require(result.completeRun && result.gameplayStalls == 1 && !result.mandatoryFloorMet,
            "an early stall is never forgotten after the overlay ring wraps");

    clock = 0; run.Reset(0, 0);
    for (int i = 0; i < 50; ++i) Frame(clock, 100000);
    for (int i = 0; i < 5400; ++i) Frame(clock);
    result = run.Summarize();
    Require(result.consecutiveSlowOneSecondWindows >= 2 && !result.mandatoryFloorMet,
            "sustained slow windows persist across the complete run");

    clock = 0; run.Reset(0, 0);
    for (int i = 0; i < 256; ++i) Frame(clock);
    Require(!run.Summarize().mandatoryFloorMet, "a short capture cannot qualify");
    run.Interrupt();
    for (int i = 0; i < 5500; ++i) Frame(clock);
    Require(!run.Summarize().mandatoryFloorMet, "pause/suspend interrupts qualification");

    clock = 0; run.Reset(0, 0);
    for (int i = 0; i < 20000; ++i) Frame(clock, 1000);
    Require(run.Summarize().recordingOverflow && !run.Summarize().mandatoryFloorMet,
            "sample overflow invalidates qualification");

    clock = 0; run.Reset(0, 0);
    Frame(clock, 33333, false);
    Frame(clock);
    Require(!run.Summarize().cpuMemoryAvailable && run.Summarize().cpuMemoryMissingSamples == 1,
            "a later successful query cannot hide an unavailable memory sample");
    VitaPerfWriter writer;
    Require(writer.Open("unavailable-memory.jsonl", "test", "hash", "host", false), "writer opens");
    Require(writer.Write(run.Summarize(), "InGame"), "writer records summary");
    std::ifstream input("unavailable-memory.jsonl");
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    Require(!text.empty() && text.back() == '\n'
            && text.find("\"hardwareQualified\":false}\n") != std::string::npos,
            "successful writes expose a complete record before writer shutdown");
    input.close();
    writer.Close();
    Require(text.find("\"cpuPeak\":null") != std::string::npos
            && text.find("\"gpuPeak\":null") != std::string::npos,
            "unavailable memory is null, not zero");

    Require(writer.Open("viewport-frame.jsonl", "test", "hash", "host", false), "viewport writer opens");
    VitaPerfFrame viewportFrame{};
    viewportFrame.viewportSeen = 7;
    viewportFrame.viewportMismatches = 1;
    viewportFrame.observedViewports[0][2] = 720;
    viewportFrame.observedViewports[0][3] = 408;
    viewportFrame.observedViewports[1][2] = 960; // Reproduce the faulty prop viewport in evidence.
    viewportFrame.observedViewports[1][3] = 544;
    viewportFrame.observedViewports[2][2] = 960;
    viewportFrame.observedViewports[2][3] = 544;
    viewportFrame.compositionUsec = 357;
    viewportFrame.postUsec = 261;
    Require(writer.WriteFrame(viewportFrame), "viewport frame fits the bounded writer");
    writer.Close();
    std::ifstream viewportInput("viewport-frame.jsonl");
    const std::string viewportText((std::istreambuf_iterator<char>(viewportInput)), {});
    Require(viewportText.find("\"viewportSeen\":7,\"viewportMismatches\":1") != std::string::npos
            && viewportText.find("\"worldViewport\":[0,0,720,408]") != std::string::npos
            && viewportText.find("\"propViewport\":[0,0,960,544]") != std::string::npos
            && viewportText.find("\"viewmodelViewport\":[0,0,960,544]") != std::string::npos
            && viewportText.find("\"compositionUs\":357,\"postUs\":261") != std::string::npos,
            "frame evidence preserves an actual viewport mismatch and subsequent timing fields");

    clock = 0; run.Reset(0, 0);
    Frame(clock);
    clock += 1000000;
    for (int i = 0; i < 5500; ++i) Frame(clock);
    Require(!run.Summarize().timingValid && !run.Summarize().mandatoryFloorMet,
            "missing frame intervals invalidate the complete traversal");
    clock = 1000000; run.Reset(0);
    Frame(clock);
    VitaPerfFrame overlap{};
    overlap.mapIndex = 0;
    overlap.wallEndUsec = clock + 1;
    overlap.frameUsec = 500000;
    run.Record(overlap);
    Require(!run.Summarize().timingValid && run.Summarize().sampleCount == 0,
            "overlapping intervals cannot underflow warmup and become measured samples");
    std::puts("Vita portable performance tests passed");
}
