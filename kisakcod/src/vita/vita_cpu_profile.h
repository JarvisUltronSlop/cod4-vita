#pragma once
#include <cstdint>

namespace kisak::vita {
enum class VitaCpuRole : uint32_t { Source, Render, Background, Audio, Recorder, Unregistered };
enum class VitaCpuEventKind : uint32_t { Scope, Loading, Counter, Lifecycle };
enum class VitaCpuLog : uint32_t { Perf, Script, Shipment, Replay };

struct VitaCpuEvent {
    uint64_t startUs=0, elapsedUs=0, exclusiveUs=0, value0=0, value1=0;
    uint32_t generation=0, sequence=0, role=0, kind=0;
    char name[96]{};
};
struct VitaCpuProfileStatus {
    uint64_t events=0, dropped=0, bytes=0;
    bool active=false, ioFailed=false;
};
struct VitaCpuContext { uint32_t generation=0, sequence=0; };

bool VitaCpuProfile_Start(const char* kind);
// Startup/failure boundaries only: a synchronous sink independent of recorder
// creation. Do not use this for per-update or ordinary gameplay telemetry.
void VitaCpuProfile_Startup(const char* stage,int result,int priority=0,int affinity=0);
void VitaCpuProfile_Stop();
void VitaCpuProfile_Flush(bool wait=false);
void VitaCpuProfile_FlushThread();
uint64_t VitaCpuNow();
void VitaCpuRegisterThread(VitaCpuRole role, int affinity=0);
VitaCpuRole VitaCpuCurrentRole();
void VitaCpuProfile_SetSequence(uint32_t generation, uint32_t sequence);
VitaCpuContext VitaCpuProfile_Context();
void VitaCpuProfile_Record(VitaCpuEvent event);
void VitaCpuProfile_Counter(const char* name, uint64_t a, uint64_t b=0);
VitaCpuProfileStatus VitaCpuProfile_Read();
bool VitaCpuProfile_AppendPerf(const char* text,uint32_t bytes,bool flush=false);
bool VitaCpuProfile_AppendLog(VitaCpuLog log,const char* text,uint32_t bytes,bool flush=false);
void VitaCpuProfile_Trace(const char* stage,const char* subject,const char* detail);
void VitaCpuProfile_Invalidate(const char* reason);
void* VitaCpuProfile_ScopeCheckpoint();
void VitaCpuProfile_RestoreScopes(void* checkpoint);

class VitaCpuScope {
public:
    explicit VitaCpuScope(const char* name, VitaCpuEventKind kind=VitaCpuEventKind::Scope);
    ~VitaCpuScope();
    VitaCpuScope(const VitaCpuScope&)=delete;
    VitaCpuScope& operator=(const VitaCpuScope&)=delete;
private:
    const char* name_;
    VitaCpuEventKind kind_;
    uint64_t start_=0, children_=0;
    VitaCpuScope* parent_=nullptr;
};
}

#define VITA_CPU_JOIN_(a,b) a##b
#define VITA_CPU_JOIN(a,b) VITA_CPU_JOIN_(a,b)
#if KISAK_VITA_CPU_PROFILE
#define VITA_CPU_SCOPE(name) kisak::vita::VitaCpuScope VITA_CPU_JOIN(cpuScope_,__LINE__)(name)
#define VITA_CPU_LOADING(name) kisak::vita::VitaCpuScope VITA_CPU_JOIN(cpuLoad_,__LINE__)(name,kisak::vita::VitaCpuEventKind::Loading)
#else
#define VITA_CPU_SCOPE(name)
#define VITA_CPU_LOADING(name)
#endif
