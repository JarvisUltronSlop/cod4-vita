#pragma once
#include <psp2/kernel/threadmgr/thread.h>
#include <initializer_list>

namespace kisak::vita {
// Check the linked emutls before starting owners. Joining the probe also
// keeps communication race-free when thread_local is incorrectly shared.
inline thread_local unsigned vitaCpuTlsProbeValue=17;
struct VitaCpuTlsProbeResult {unsigned initial=0;unsigned* address=nullptr;};
inline int VitaCpuTlsProbeEntry(SceSize,void* argument) {
    auto& result=**static_cast<VitaCpuTlsProbeResult**>(argument);
    result.initial=vitaCpuTlsProbeValue;
    result.address=&vitaCpuTlsProbeValue;
    vitaCpuTlsProbeValue=91;
    return 0;
}
template<class Report> bool VitaCpuVerifyThreadLocals(Report report) {
    vitaCpuTlsProbeValue=53;
    for(int affinity:{SCE_KERNEL_CPU_MASK_USER_1,SCE_KERNEL_CPU_MASK_USER_2}) {
        VitaCpuTlsProbeResult result;auto* argument=&result;
        const int id=sceKernelCreateThread("cod4-tls-check",VitaCpuTlsProbeEntry,160,65536,0,affinity,nullptr);
        report("tls-probe-create",id,affinity);
        if(id<0)return false;
        const int started=sceKernelStartThread(id,sizeof(argument),&argument);
        report("tls-probe-start",started,affinity);
        if(started<0){sceKernelDeleteThread(id);return false;}
        // The worker borrows this stack until it has exited.
        while(sceKernelWaitThreadEnd(id,nullptr,nullptr)<0)sceKernelDelayThread(1000);
        sceKernelDeleteThread(id);
        const bool valid=result.initial==17 && result.address!=&vitaCpuTlsProbeValue && vitaCpuTlsProbeValue==53;
        report("tls-isolation",valid?0:-1,affinity);
        if(!valid)return false;
    }
    return true;
}
}
