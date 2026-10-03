#pragma once
#include <cstdint>
struct VitaBenchmarkMetrics {
    uint64_t snapshotsUs=0, predictionUs=0, entitiesUs=0, cameraUs=0, viewmodelUs=0;
    uint32_t predictedCommands=0;
};
extern VitaBenchmarkMetrics vitaBenchmarkMetrics;
#if KISAK_VITA_BENCHMARK
#include <psp2/kernel/processmgr.h>
struct VitaBenchmarkScope {
    uint64_t& result;
    uint64_t start=sceKernelGetProcessTimeWide();
    explicit VitaBenchmarkScope(uint64_t& destination):result(destination) {}
    ~VitaBenchmarkScope() { result+=sceKernelGetProcessTimeWide()-start; }
};
#define VITA_BENCH_SCOPE(field) VitaBenchmarkScope vitaBenchmarkScope(vitaBenchmarkMetrics.field)
#else
#define VITA_BENCH_SCOPE(field)
#endif
