#include <vita/vita_texture_store.h>
#include <vita/vita_cpu_profile.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/kernel/threadmgr/semaphore.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
namespace kisak::vita {
namespace {
constexpr uint32_t pageBytes=65536,pages=8192,slots=2048;
struct Extent {uint32_t first=0,count=0,bytes=0,generation=0;};
std::array<Extent,slots> extents{};
std::array<bool,pages> used{};
uint64_t liveBytes=0;
enum Operation {Open,Read,Write,Stop};
struct Request {Operation operation=Open;void* bytes=nullptr;uint32_t offset=0,size=0;bool ok=false;char path[256]{};};
Request request;
std::atomic<unsigned> state{0}; // 0 idle, 1 worker owns request, 2 reply published
std::atomic_flag producer=ATOMIC_FLAG_INIT;
SceUID worker=-1,wake=-1,reply=-1;
int Worker(SceSize,void*) {
    VitaCpuRegisterThread(VitaCpuRole::Background,SCE_KERNEL_CPU_MASK_USER_2);
    FILE* file=nullptr;char path[256]{};
    for(;;) {
        while(sceKernelWaitSema(wake,1,nullptr)<0)sceKernelDelayThread(1000);
        if(state.load(std::memory_order_acquire)!=1)continue;
        auto& r=request;r.ok=false;
        if(r.operation==Open) {
            std::strcpy(path,r.path);file=std::fopen(path,"w+b");r.ok=file!=nullptr;
            if(file)std::setvbuf(file,nullptr,_IONBF,0);
        } else if(r.operation==Stop) {
            r.ok=!file || std::fclose(file)==0;file=nullptr;
            if(path[0])std::remove(path);
            state.store(2,std::memory_order_release);sceKernelSignalSema(reply,1);break;
        } else if(file && std::fseek(file,long(r.offset),SEEK_SET)==0) {
            r.ok=true;
            for(uint32_t at=0;at<r.size;) {
                const uint32_t amount=(r.size-at)>pageBytes?pageBytes:r.size-at;
                auto* data=static_cast<uint8_t*>(r.bytes)+at;
                const size_t done=r.operation==Read?std::fread(data,1,amount,file):std::fwrite(data,1,amount,file);
                if(done!=amount){r.ok=false;std::clearerr(file);break;}at+=amount;
            }
        }
        state.store(2,std::memory_order_release);
        sceKernelSignalSema(reply,1);
    }
    VitaCpuProfile_FlushThread();return 0;
}
bool Submit(Operation operation,void* data,uint32_t offset,uint32_t size) {
    if(worker<0 || producer.test_and_set(std::memory_order_acquire))return false;
    request.operation=operation;request.bytes=data;request.offset=offset;request.size=size;request.ok=false;
    state.store(1,std::memory_order_release);sceKernelSignalSema(wake,1);
    // Sleep until the worker publishes the reply; no polling on the render core.
    while(sceKernelWaitSema(reply,1,nullptr)<0){}
    if(state.load(std::memory_order_acquire)!=2)request.ok=false;
    const bool ok=request.ok;state.store(0,std::memory_order_release);producer.clear(std::memory_order_release);return ok;
}
Extent* Find(uint32_t token) {
    const uint32_t slot=token&4095;if(!slot || slot>slots)return nullptr;
    auto& e=extents[slot-1];return e.count && e.generation==(token>>12)?&e:nullptr;
}
}
bool VitaTextureStore_Start(const char* path) {
    if(worker>=0)return true;
    if(!path || std::strlen(path)>=sizeof(request.path))return false;
    std::strcpy(request.path,path);state.store(0);producer.clear();used.fill(false);liveBytes=0;
    wake=sceKernelCreateSema("cod4-tex-io",0,0,1,nullptr);
    reply=sceKernelCreateSema("cod4-tex-done",0,0,1,nullptr);
    worker=sceKernelCreateThread("cod4-texture-reader",Worker,0x10000110,32*1024,0,SCE_KERNEL_CPU_MASK_USER_2,nullptr);
    if(wake<0 || reply<0 || worker<0 || sceKernelStartThread(worker,0,nullptr)<0) {
        if(worker>=0)sceKernelDeleteThread(worker);if(wake>=0)sceKernelDeleteSema(wake);
        if(reply>=0)sceKernelDeleteSema(reply);
        worker=wake=reply=-1;return false;
    }
    if(Submit(Open,nullptr,0,0))return true;
    VitaTextureStore_Stop();return false;
}
uint32_t VitaTextureStore_Write(const void* bytes,uint32_t size) {
    if(!bytes || !size || size>8*1024*1024 || worker<0)return 0;
    uint32_t slot=0;for(;slot<slots;++slot)if(!extents[slot].count && extents[slot].generation<0xfffff)break;
    if(slot==slots)return 0;
    const uint32_t count=(size+pageBytes-1)/pageBytes;uint32_t run=0,end=0;
    for(;end<pages;++end){run=used[end]?0:run+1;if(run==count)break;}
    if(end==pages)return 0;
    const uint32_t first=end+1-count;
    if(!Submit(Write,const_cast<void*>(bytes),first*pageBytes,size))return 0;
    for(uint32_t i=first;i<=end;++i)used[i]=true;
    auto& e=extents[slot];e.first=first;e.count=count;e.bytes=size;++e.generation;liveBytes+=size;
    return (e.generation<<12)|(slot+1);
}
bool VitaTextureStore_Read(uint32_t token,void* bytes,uint32_t size) {
    auto* e=Find(token);return e && bytes && size==e->bytes && Submit(Read,bytes,e->first*pageBytes,size);
}
void VitaTextureStore_Release(uint32_t token) {
    auto* e=Find(token);if(!e)return;
    for(uint32_t i=e->first;i<e->first+e->count;++i)used[i]=false;
    liveBytes-=e->bytes;e->count=e->bytes=0;
}
void VitaTextureStore_Stop() {
    if(worker>=0){Submit(Stop,nullptr,0,0);sceKernelWaitThreadEnd(worker,nullptr,nullptr);sceKernelDeleteThread(worker);}
    if(wake>=0)sceKernelDeleteSema(wake);
    if(reply>=0)sceKernelDeleteSema(reply);worker=wake=reply=-1;
    used.fill(false);liveBytes=0;for(auto& e:extents)e.count=e.bytes=0;
}
uint64_t VitaTextureStore_LiveBytes(){return liveBytes;}
}
