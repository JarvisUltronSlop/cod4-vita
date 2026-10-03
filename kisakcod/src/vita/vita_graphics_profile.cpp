#include "vita_graphics_profile.h"
#if (defined(__vita__) && KISAK_VITA_CPU_RUNTIME) || defined(VITA_GRAPHICS_PROFILE_TEST)
#include "vita_graphics_metrics.h"
#include "vita_cpu_profile.h"
#ifndef VITA_GRAPHICS_PROFILE_TEST
#include <psp2/gxm.h>
#include <psp2/razor_hud.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/processmgr.h>
#endif
#ifndef VITA_GRAPHICS_PROFILE_TEST
static_assert(sizeof(SceRazorGpuLiveEntryJob)==72 && sizeof(SceRazorGpuLiveEntryFrame)==32 && sizeof(SceRazorGpuLiveEntryParameterBuffer)==24,"Pinned Razor record ABI changed");
#endif
#include <atomic>
#include <cstdio>
#include <cstring>
namespace kisak::vita {
namespace {
alignas(64) uint8_t buffers[2][65536]{};
std::atomic<uint32_t> status{0};
uint32_t phase=0,group=0,slot=0,passIndex=0,sceneCount=0,fbo=0,width=0,height=0;
uint32_t generation=0,errors=0,batch=0;bool counterMode=true,loaded=false,owned=false,running=false,failed=false,poisoned=false;
uint64_t overhead=0;
void Counter(const char* n,uint64_t a,uint64_t b=0){VitaCpuProfile_Counter(n,a,b);}
void Failure(uint32_t operation,int code) {
    Counter("graphics-counter-error",operation,uint32_t(code));++errors;failed=true;status.store(3);
}
void Collect() {
    SceRazorGpuLiveResultInfo info{};slot^=1;
    const auto start=VitaCpuNow();const int result=sceRazorGpuLiveSetBuffer(buffers[slot],sizeof(buffers[slot]),&info);
    (void)start;
    Counter("graphics-counter-batch",++batch,group);
    Counter("graphics-counter-buffer",info.entry_count,info.overflow_count);
    if(result<0){Failure(5,result);return;}
    if(info.overflow_count){Failure(6,-1);return;}
    const uintptr_t address=reinterpret_cast<uintptr_t>(info.result_data);
    bool bounded=info.entry_count==0;
    const auto& completedBuffer=buffers[slot^1u];
    const uintptr_t base=reinterpret_cast<uintptr_t>(completedBuffer);
    if(address>=base && address-base<=sizeof(completedBuffer) && info.buffer_size<=sizeof(completedBuffer)-(address-base))bounded=true;
    if(!bounded || (info.entry_count && info.metric_group!=group) || info.end_time<info.start_time){Failure(7,-1);return;}
    Counter("graphics-counter-window",info.start_time,info.end_time);
    uint32_t jobs=0,otherProcesses=0,frames=0,parameter=0;
    uint64_t vertex=0,fragment=0,firmware=0;
    const uint32_t pid=uint32_t(sceKernelGetProcessId());
    const bool valid=VitaGraphicsMetricsParse(info.result_data,info.buffer_size,info.entry_count,[&](const VitaGraphicsMetric& m){
        char name[72];
        if(m.kind==0) {
            if(m.process!=pid){++otherProcesses;return;}
            const auto duration=m.end-m.start;
            if(m.type==0)firmware+=duration;else if(m.type==1 || m.type==3 || m.type==5 || m.type==7)vertex+=duration;else fragment+=duration;
            const auto id=jobs++;
            if(id>=64)return; // Summary includes all jobs; raw timeline truncation is explicit.
            std::snprintf(name,sizeof(name),"graphics-job-%u-id",id);Counter(name,(uint64_t(m.frame)<<32)|m.scene,(uint64_t(group)<<32)|(m.type<<16)|m.core);
            std::snprintf(name,sizeof(name),"graphics-job-%u-time",id);Counter(name,m.start,m.end);
            std::snprintf(name,sizeof(name),"graphics-job-%u-values",id);Counter(name,(m.a<<32)|m.b,m.c);
        } else if(m.kind==1) {
            if(parameter++>=16)return;
            std::snprintf(name,sizeof(name),"graphics-parameter-%u",parameter-1);Counter(name,m.a,(m.b<<32)|m.c);
        } else {
            if(frames++>=16)return;
            std::snprintf(name,sizeof(name),"graphics-hw-frame-%u",frames-1);Counter(name,m.frame,m.a);
            std::snprintf(name,sizeof(name),"graphics-hw-frame-%u-time",frames-1);Counter(name,m.start,m.end);
        }
    });
    if(!valid){Failure(8,-1);return;}
    Counter("graphics-counter-jobs",jobs,otherProcesses);
    Counter("graphics-counter-job-us",vertex,fragment);Counter("graphics-counter-firmware-us",firmware,group);
    Counter("graphics-counter-raw-truncated",jobs>64?jobs-64:0,(frames>16?frames-16:0)+(parameter>16?parameter-16:0));
}
void Stop() {
    if(!running)return;
    const auto at=VitaCpuNow();const int result=sceRazorGpuLiveStop();(void)at;
    Counter("graphics-counter-stop",group,uint32_t(result));running=false;
    // Keep buffers alive until process exit even if the driver cannot stop.
    if(result<0){poisoned=true;Failure(9,result);return;}
    Collect();
}
void Start(uint32_t selected) {
    if(failed)return;group=selected;
    if(!loaded) {
        const int existing=sceSysmoduleIsLoaded(SCE_SYSMODULE_RAZOR_HUD);
        const int result=existing==0?0:sceSysmoduleLoadModule(SCE_SYSMODULE_RAZOR_HUD);
        Counter("graphics-counter-module",uint32_t(result),uint32_t(existing));
        if(result<0){Failure(1,result);return;}loaded=true;owned=existing!=0;
    }
    int result=sceRazorGpuLiveSetMetricsGroup(group);Counter("graphics-counter-select",group,uint32_t(result));
    if(result<0){Failure(2,result);return;}
    SceRazorGpuLiveResultInfo unused{};slot=0;
    result=sceRazorGpuLiveSetBuffer(buffers[slot],sizeof(buffers[slot]),&unused);
    if(result<0){Failure(3,result);return;}
    result=sceRazorGpuLiveStart();Counter("graphics-counter-start",group,uint32_t(result));
    if(result<0){Failure(4,result);return;}running=true;status.store(2);
}
}
bool VitaGraphicsProfile_Active(){return phase!=0;}
uint32_t VitaGraphicsProfile_Status(){return status.load();}
void VitaGraphicsProfile_Finish() {
    if(!phase && !running)return;
    Stop();
    // Retain a successfully loaded optional module for reuse. No stale imported
    // call or driver buffer pointer is exposed by unloading it between tests.
    Counter("graphics-profile-finished",errors,owned);phase=0;status.store(failed?3:0);
}
void VitaGraphicsProfile_Begin(uint32_t requested,bool counters) {
    if(requested>(counters?6u:9u))requested=0;
    if(!requested){VitaGraphicsProfile_Finish();return;}
    overhead=0;passIndex=sceneCount=fbo=width=height=0;
    const auto current=VitaCpuProfile_Context().generation;
    if(current!=generation){Stop();generation=current;errors=0;failed=poisoned;}
    if(requested!=phase || counters!=counterMode) {
        const auto at=VitaCpuNow();Stop();phase=requested;counterMode=counters;
        if(counters && phase>=2 && phase<=5)Start(phase-2);else status.store(failed && counters?3:1);
        overhead+=VitaCpuNow()-at;
        Counter("graphics-profile-phase",phase,status.load());
    }
    Counter("graphics-profile-frame",phase,status.load());
}
void VitaGraphicsProfile_End() {
    if(!phase)return;
    const auto at=VitaCpuNow();if(running && !failed)Collect();if(failed && running)Stop();
    Counter("graphics-profile-health",errors,status.load());
    Counter("graphics-profile-scenes",sceneCount,phase);
    Counter("graphics-profile-observer-us",overhead+VitaCpuNow()-at,0);
}
void VitaGraphicsProfile_Pass(uint32_t next,uint32_t target,uint32_t w,uint32_t h) {
    if(!phase)return;passIndex=next;fbo=target;width=w;height=h;
    Counter("graphics-pass-target",(uint64_t(next)<<32)|target,(uint64_t(w)<<32)|h);
}
void VitaGraphicsProfile_Scene(uint64_t start,uint64_t end,int result) {
    if(!phase)return;
    const auto id=sceneCount++;char name[64];
    if(id>=64){Counter("graphics-scene-overflow",id,phase);return;}
    std::snprintf(name,sizeof(name),"graphics-scene-%u-cpu-time",id);Counter(name,start,end);
    std::snprintf(name,sizeof(name),"graphics-scene-%u-state",id);Counter(name,(uint64_t(passIndex)<<32)|fbo,(uint64_t(width)<<32)|height);
    std::snprintf(name,sizeof(name),"graphics-scene-%u-result",id);Counter(name,uint32_t(result),phase);
}
#ifndef VITA_GRAPHICS_PROFILE_TEST
void SceneBegin(const SceGxmRenderTarget* target,const SceGxmColorSurface* color,const SceGxmDepthStencilSurface* depth,uint32_t flags,uint64_t start,uint64_t end,int result) {
    if(!phase || sceneCount>=64)return;
    char name[64];const auto id=sceneCount;
    std::snprintf(name,sizeof(name),"graphics-scene-%u-begin-cpu",id);Counter(name,start,end);
    std::snprintf(name,sizeof(name),"graphics-scene-%u-target",id);Counter(name,reinterpret_cast<uintptr_t>(target),color?reinterpret_cast<uintptr_t>(sceGxmColorSurfaceGetData(color)):0);
    std::snprintf(name,sizeof(name),"graphics-scene-%u-format",id);Counter(name,color?uint32_t(sceGxmColorSurfaceGetFormat(color)):0,color?sceGxmColorSurfaceGetStrideInPixels(color):0);
    std::snprintf(name,sizeof(name),"graphics-scene-%u-load-store",id);Counter(name,depth?(uint64_t(uint32_t(sceGxmDepthStencilSurfaceGetForceLoadMode(depth)))<<32)|uint32_t(sceGxmDepthStencilSurfaceGetForceStoreMode(depth)):0,(uint64_t(flags)<<32)|uint32_t(result));
}
#endif
}
#ifndef VITA_GRAPHICS_PROFILE_TEST
extern "C" int __real_sceGxmBeginScene(SceGxmContext*,unsigned int,const SceGxmRenderTarget*,const SceGxmValidRegion*,SceGxmSyncObject*,SceGxmSyncObject*,const SceGxmColorSurface*,const SceGxmDepthStencilSurface*);
extern "C" int __wrap_sceGxmBeginScene(SceGxmContext* context,unsigned int flags,const SceGxmRenderTarget* target,const SceGxmValidRegion* region,SceGxmSyncObject* vertex,SceGxmSyncObject* fragment,const SceGxmColorSurface* color,const SceGxmDepthStencilSurface* depth) {
    const bool active=kisak::vita::VitaGraphicsProfile_Active();const auto start=active?kisak::vita::VitaCpuNow():0;
    const int result=__real_sceGxmBeginScene(context,flags,target,region,vertex,fragment,color,depth);
    if(active)kisak::vita::SceneBegin(target,color,depth,flags,start,kisak::vita::VitaCpuNow(),result);
    return result;
}
#endif
#endif
