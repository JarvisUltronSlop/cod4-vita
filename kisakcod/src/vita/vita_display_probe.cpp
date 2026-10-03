#include "vita_display_probe.h"
#include "vita_graphics_profile.h"
#if KISAK_VITA_CPU_RUNTIME
#include "vita_cpu_queue.h"
#include "vita_cpu_profile.h"
#ifndef VITA_DISPLAY_PROBE_TEST
#include <psp2/gxm.h>
#include <psp2/display.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#endif
#include <cstring>

extern "C" {
int __real_sceGxmVshInitialize(const SceGxmInitializeParams*);
int __real_sceGxmEndScene(SceGxmContext*,const SceGxmNotification*,const SceGxmNotification*);
int __real_sceGxmDisplayQueueAddEntry(SceGxmSyncObject*,SceGxmSyncObject*,const void*);
int __real_sceDisplaySetFrameBuf(const SceDisplayFrameBuf*,SceDisplaySetBufSync);
int __real_sceDisplayWaitVblankStartMulti(unsigned int);
}
namespace {
using namespace kisak::vita;
struct Request {uint32_t id=0,phase=0;VitaCpuContext context{};};
struct Completed {
    Request request{};uint64_t enter=0,exit=0,setbuf=0,vblank=0;
    uint32_t setbufCalls=0,vblankCalls=0,errors=0;
    int thread=0,affinity=0,priority=0;
};
VitaCpuQueue<Request,32> requests;
VitaCpuQueue<Completed,128> completed;
std::atomic<uint32_t> failures{0};
std::atomic<int> callbackThread{0};
SceGxmDisplayQueueCallback* originalCallback=nullptr;
SceGxmContext* sceneContext=nullptr;
uint32_t phase=0,nextId=0,initFlags=0,maxPending=0;
bool hooked=false;
Completed* activeCallback=nullptr; // Access only on the single display callback owner.
int callbackAffinity=0,callbackPriority=0;
void Callback(const void* data) {
    Completed result;
    if(!requests.Pop(&result.request))failures.fetch_add(1,std::memory_order_relaxed);
    if(!callbackThread.load(std::memory_order_relaxed)) {
        const int id=sceKernelGetThreadId();
        callbackAffinity=sceKernelGetThreadCpuAffinityMask(id);
        callbackPriority=sceKernelGetThreadCurrentPriority();
        callbackThread.store(id,std::memory_order_release);
    }
    result.thread=callbackThread.load(std::memory_order_relaxed);
    result.affinity=callbackAffinity;result.priority=callbackPriority;
    if(result.request.phase) {
        result.enter=sceKernelGetProcessTimeWide();activeCallback=&result;
    }
    originalCallback(data); // Original payload, framebuffer update and waits unchanged.
    if(result.request.phase) {
        result.exit=sceKernelGetProcessTimeWide();activeCallback=nullptr;
        if(!completed.Push(result))failures.fetch_add(1,std::memory_order_relaxed);
    }
}
void Counter(const char* name,uint64_t a,uint64_t b=0) {VitaCpuProfile_Counter(name,a,b);}
}
extern "C" int __wrap_sceGxmVshInitialize(const SceGxmInitializeParams* params) {
    if(!params || !params->displayQueueCallback)return __real_sceGxmVshInitialize(params);
    auto copy=*params;originalCallback=params->displayQueueCallback;
    initFlags=params->flags;maxPending=params->displayQueueMaxPendingCount;
    copy.displayQueueCallback=Callback;
    const int result=__real_sceGxmVshInitialize(&copy);hooked=result==0;
    return result;
}
extern "C" int __wrap_sceGxmEndScene(SceGxmContext* context,const SceGxmNotification* vertex,const SceGxmNotification* fragment) {
    const bool profile=kisak::vita::VitaGraphicsProfile_Active();
    const auto at=profile?sceKernelGetProcessTimeWide():0;
    const int result=__real_sceGxmEndScene(context,vertex,fragment);
    if(profile)kisak::vita::VitaGraphicsProfile_Scene(at,sceKernelGetProcessTimeWide(),result);
    if(result==0)sceneContext=context;
    else if(phase)failures.fetch_add(1,std::memory_order_relaxed);
    return result;
}
extern "C" int __wrap_sceGxmDisplayQueueAddEntry(SceGxmSyncObject* oldBuffer,SceGxmSyncObject* newBuffer,const void* data) {
    Request request{++nextId,phase,VitaCpuProfile_Context()};
    uint64_t finishUs=0;int finishResult=0;
    // EndScene and PadHeartbeat have already run. Wait on the exact context
    // here, without glFinish's scene_reset or an extra scene/tile load/store.
    if(phase==2) {
        const auto start=sceKernelGetProcessTimeWide();
        if(sceneContext)sceGxmFinish(sceneContext);else finishResult=-1;
        finishUs=sceKernelGetProcessTimeWide()-start;
    }
    if(hooked && !requests.Push(request))failures.fetch_add(1,std::memory_order_relaxed);
    const uint64_t start=phase?sceKernelGetProcessTimeWide():0;
    const int result=__real_sceGxmDisplayQueueAddEntry(oldBuffer,newBuffer,data);
    const uint64_t end=phase?sceKernelGetProcessTimeWide():0;
    if(result<0)failures.fetch_add(1,std::memory_order_relaxed);
    if(phase) {
        Counter("display-submit-id",request.id,phase);
        Counter("display-submit-start",request.id,start);
        Counter("display-submit-end",request.id,end);
        Counter("display-submit-finish-us",request.id,finishUs);
        Counter("display-submit-status",request.id,uint32_t(result));
        Counter("display-submit-context",request.id,sceneContext && finishResult==0);
        Counter("display-probe-config",initFlags,maxPending);
        Counter("display-probe-health",hooked,failures.load(std::memory_order_acquire));
    }
    return result;
}
extern "C" int __wrap_sceDisplaySetFrameBuf(const SceDisplayFrameBuf* buffer,SceDisplaySetBufSync sync) {
    const bool observe=callbackThread.load(std::memory_order_acquire)==sceKernelGetThreadId();
    Completed* record=observe?activeCallback:nullptr;
    const auto start=record?sceKernelGetProcessTimeWide():0;
    const int result=__real_sceDisplaySetFrameBuf(buffer,sync);
    if(record){record->setbuf+=sceKernelGetProcessTimeWide()-start;++record->setbufCalls;record->errors+=result<0;}
    return result;
}
extern "C" int __wrap_sceDisplayWaitVblankStartMulti(unsigned int count) {
    const bool observe=callbackThread.load(std::memory_order_acquire)==sceKernelGetThreadId();
    Completed* record=observe?activeCallback:nullptr;
    const auto start=record?sceKernelGetProcessTimeWide():0;
    const int result=__real_sceDisplayWaitVblankStartMulti(count);
    if(record){record->vblank+=sceKernelGetProcessTimeWide()-start;++record->vblankCalls;record->errors+=result<0;}
    return result;
}
namespace kisak::vita {
void VitaDisplayProbe_SetPhase(uint32_t value) {phase=value<=4?value:0;}
void VitaDisplayProbe_Drain() {
    Completed value;
    while(completed.Pop(&value)) {
        const auto saved=VitaCpuProfile_Context();
        VitaCpuProfile_SetSequence(value.request.context.generation,value.request.context.sequence);
        const auto id=value.request.id;
        Counter("display-cb-enter",id,value.enter);Counter("display-cb-exit",id,value.exit);
        Counter("display-cb-setbuf-us",id,value.setbuf);Counter("display-cb-vblank-us",id,value.vblank);
        Counter("display-cb-calls",value.setbufCalls,value.vblankCalls);
        Counter("display-cb-status",id,value.errors);
        Counter("display-cb-thread",uint32_t(value.thread),uint32_t(value.affinity));
        Counter("display-cb-priority",id,uint32_t(value.priority));
        VitaCpuProfile_SetSequence(saved.generation,saved.sequence);
    }
}
void VitaDisplayProbe_Finish() {
    const int status=sceGxmDisplayQueueFinish();VitaDisplayProbe_Drain();
    Counter("display-probe-drained",uint32_t(status),failures.load(std::memory_order_acquire));
}
}
#else
namespace kisak::vita {
void VitaDisplayProbe_SetPhase(uint32_t){} void VitaDisplayProbe_Drain(){} void VitaDisplayProbe_Finish(){}
}
#endif
