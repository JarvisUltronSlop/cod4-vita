#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/kernel/threadmgr/semaphore.h>
#include <vita/vita_cpu_profile.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <array>
#include <memory>
#include <chrono>
#include <atomic>
#include <cassert>

namespace {
struct Thread {std::thread native;int (*entry)(SceSize,void*)=nullptr;};
struct Sema {std::mutex mutex;std::condition_variable cv;int count=0,maximum=0;};
std::array<std::unique_ptr<Thread>,128> threads;
std::array<std::unique_ptr<Sema>,128> semas;
std::mutex registry;
std::atomic<bool> failNextStart{false};
std::atomic<bool> failNextSourceWait{false};
thread_local kisak::vita::VitaCpuRole role=kisak::vita::VitaCpuRole::Unregistered;
std::atomic<uint64_t> failures{0};
thread_local int threadId=0;
}
int sceKernelGetThreadId(){return threadId;}
SceUID sceKernelCreateThread(const char*,int (*entry)(SceSize,void*),int,int,int,int,void*) {
    std::lock_guard lock(registry);
    for(int i=1;i<128;++i)if(!threads[i]){threads[i]=std::make_unique<Thread>();threads[i]->entry=entry;return i;}
    return -1;
}
int sceKernelStartThread(SceUID id,SceSize size,void* arg) {
    if(failNextStart.exchange(false))return -1;
    threads[id]->native=std::thread([=]{threadId=id;threads[id]->entry(size,arg);});return 0;
}
int sceKernelWaitThreadEnd(SceUID id,int*,void*){if(threads[id]->native.joinable())threads[id]->native.join();return 0;}
int sceKernelDeleteThread(SceUID id){std::lock_guard lock(registry);assert(!threads[id]->native.joinable());threads[id].reset();return 0;}
int sceKernelDelayThread(unsigned us){std::this_thread::sleep_for(std::chrono::microseconds(us));return 0;}
SceUID sceKernelCreateSema(const char*,int,int initial,int maximum,void*) {
    std::lock_guard lock(registry);
    for(int i=1;i<128;++i)if(!semas[i]){semas[i]=std::make_unique<Sema>();semas[i]->count=initial;semas[i]->maximum=maximum;return i;}
    return -1;
}
int sceKernelSignalSema(SceUID id,int count) {
    auto& s=*semas[id];std::lock_guard lock(s.mutex);
    if(s.count+count>s.maximum)return -1;
    s.count+=count;s.cv.notify_all();return 0;
}
int sceKernelWaitSema(SceUID id,int count,SceUInt* timeout) {
    if(role==kisak::vita::VitaCpuRole::Source && failNextSourceWait.exchange(false))return -1;
    auto& s=*semas[id];std::unique_lock lock(s.mutex);
    if(timeout){if(!s.cv.wait_for(lock,std::chrono::microseconds(*timeout),[&]{return s.count>=count;}))return static_cast<int>(0x80028005u);}
    else s.cv.wait(lock,[&]{return s.count>=count;});
    s.count-=count;return 0;
}
int sceKernelDeleteSema(SceUID id){std::lock_guard lock(registry);semas[id].reset();return 0;}
int CpuTestLiveThreads(){int count=0;for(auto& t:threads)count+=t!=nullptr;return count;}
void CpuTestFailNextThreadStart(){failNextStart.store(true);}
void CpuTestFailNextSourceWait(){failNextSourceWait.store(true);}
namespace kisak::vita {
uint64_t VitaCpuNow(){return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
void VitaCpuRegisterThread(VitaCpuRole value,int){role=value;}
VitaCpuRole VitaCpuCurrentRole(){return role;}
void VitaCpuProfile_SetSequence(uint32_t,uint32_t){}
VitaCpuContext VitaCpuProfile_Context(){return {};}
void VitaCpuProfile_FlushThread(){}
void VitaCpuProfile_Counter(const char*,uint64_t,uint64_t){}
void VitaCpuProfile_Startup(const char*,int,int,int){}
void VitaCpuProfile_Invalidate(const char*){failures.fetch_add(1);}
VitaCpuProfileStatus VitaCpuProfile_Read(){return {0,failures.load(),0,true,false};}
}
