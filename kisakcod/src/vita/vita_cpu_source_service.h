#pragma once
#include <vita/vita_cpu_profile.h>

namespace kisak::vita {
// Source-thread-local hook: workers cannot inherit or execute OpenAL control.
// Services only existing playback while the source owner waits on resources;
// it must not advance gameplay, sound events, clocks, or renderer commands.
inline thread_local void (*cpuSourceService)() = nullptr;
inline thread_local bool cpuSourceServicing = false;
inline thread_local uint64_t cpuSourceServiceDeadline = 0;
inline void VitaCpuSource_SetService(void (*service)()) {
    cpuSourceService = service;
    cpuSourceServiceDeadline = 0;
}
inline void VitaCpuSource_Service() {
    if (!cpuSourceService || cpuSourceServicing || VitaCpuCurrentRole()!=VitaCpuRole::Source) return;
    const auto now=VitaCpuNow();
    if (now<cpuSourceServiceDeadline) return;
    cpuSourceServiceDeadline=now+20000;
    cpuSourceServicing=true;
    cpuSourceService();
    cpuSourceServicing=false;
}
}
