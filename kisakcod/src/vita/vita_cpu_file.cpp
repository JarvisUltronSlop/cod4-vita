#include <vita/vita_cpu_file.h>
#include <vita/vita_cpu_read_probe.h>
#if KISAK_VITA_CPU_RUNTIME
#include <vita/vita_cpu_profile.h>
#include <vita/vita_cpu_source_service.h>
#include <vita/vita_cpu_queue.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/kernel/threadmgr/semaphore.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace kisak::vita {
namespace {
constexpr unsigned kFiles=16,kWindow=64*1024;
enum State {Free,Writing,Ready,Reading,Error};
struct Window {
    std::atomic<int> state{Free};
    uint64_t offset=0;
    uint32_t size=0;
    alignas(64) uint8_t bytes[kWindow];
};
struct File {
    FILE* file=nullptr; // worker only
    uint32_t generation=0; // worker only
    uint64_t position=0; // worker-only stdio cursor; no redundant sequential seek
    uint64_t readOrdinal=0; // worker-owned, reset on open
    bool positionKnown=false;
    Window windows[2];
};
struct Reply {std::atomic<bool> done{false};bool ok=false;uint64_t size=0;};
enum Kind {Open,Read,Close,Barrier};
struct Job {
    Kind kind;int handle=0,page=0;uint32_t generation=0,session=0,sequence=0;
    uint64_t offset=0,submittedUs=0;Reply* reply=nullptr;char path[256]{};
};
std::array<File,kFiles> files;
std::array<bool,kFiles> claimed{}; // source only
std::array<uint32_t,kFiles> generations{}; // source only
VitaCpuQueue<Job,64> jobs;
SceUID worker=-1,wake=-1,done=-1;
std::atomic<bool> stopping{false};
uint32_t session=0;
void (*progress)(void*)=nullptr;
void* progressContext=nullptr;
uint64_t progressAt=0;
void Progress() {
    VitaCpuSource_Service();
    const uint64_t now=VitaCpuNow();
    if(progress && now>=progressAt){progressAt=now+100000;progress(progressContext);}
}
void Failure(const char* name){VitaCpuProfile_Invalidate(name);}
void Submit(Job job) {
    job.session=session;
    job.sequence=VitaCpuProfile_Context().sequence;
    job.submittedUs=VitaCpuNow();
    bool full=false;
    while(!jobs.Push(job)) {
        if(!full){VitaCpuProfile_Counter("loading-queue-pressure",jobs.Count());full=true;}
        sceKernelSignalSema(wake,1);Progress(); { VITA_CPU_SCOPE("file-queue-capacity-wait"); sceKernelDelayThread(100); }
    }
    sceKernelSignalSema(wake,1);
}
void Wait(Reply& reply) {
    while(!reply.done.load(std::memory_order_acquire)) {
        Progress();SceUInt timeout=10000; { VITA_CPU_SCOPE("file-consumer-reply-wait"); sceKernelWaitSema(done,1,&timeout); }
    }
}
void Complete(Reply* reply) {
    if(reply)reply->done.store(true,std::memory_order_release);
    sceKernelSignalSema(done,1);
}
bool ReadThreadClocks(uint64_t& raw) {
    raw=0;
#if defined(KISAK_VITA_CPU_TEST)
    return false; // The portable host cannot supply Vita kernel execution counters.
#else
    SceKernelThreadInfo info{};info.size=sizeof(info);
    if(sceKernelGetThreadInfo(sceKernelGetThreadId(),&info)<0)return false;
    static_assert(sizeof(info.runClocks)==sizeof(raw));
    std::memcpy(&raw,&info.runClocks,sizeof(raw));return true;
#endif
}
int Worker(SceSize,void*) {
    VitaCpuRegisterThread(VitaCpuRole::Background,SCE_KERNEL_CPU_MASK_USER_2);
    while(!stopping.load(std::memory_order_acquire)) {
        Job job;
        if(!jobs.Pop(&job)) {SceUInt timeout=10000; { VITA_CPU_SCOPE("file-worker-idle-wait"); sceKernelWaitSema(wake,1,&timeout); } continue;}
        VitaCpuProfile_SetSequence(job.session,job.sequence);
        if(job.kind==Barrier){Complete(job.reply);continue;}
        auto& file=files[job.handle];
        if(job.kind==Open) {
            VITA_CPU_LOADING("package-file-open");
            file.file=std::fopen(job.path,"rb");file.generation=job.generation;file.readOrdinal=0;
            if(file.file && std::fseek(file.file,0,SEEK_END)==0) {
                const long size=std::ftell(file.file);
                if(size>=0){job.reply->size=uint64_t(size);job.reply->ok=true;file.position=uint64_t(size);file.positionKnown=true;}
            }
            if(!job.reply->ok && file.file){std::fclose(file.file);file.file=nullptr;}
            if(!job.reply->ok)file.positionKnown=false;
            Complete(job.reply);
        } else if(job.kind==Close) {
            if(file.generation==job.generation && file.file){std::fclose(file.file);file.file=nullptr;file.positionKnown=false;}
            Complete(job.reply);
        } else {
            VITA_CPU_LOADING("package-read");
            auto& window=file.windows[job.page];
            const auto begin=VitaCpuNow();
            VitaCpuProfile_Counter("package-job-queue-wait-us",begin-job.submittedUs);
            bool ok=file.file && file.generation==job.generation && job.offset<=0x7fffffffu;
            if(ok && (!file.positionKnown || file.position!=job.offset)) {
                VITA_CPU_LOADING("package-seek");
                ok=std::fseek(file.file,long(job.offset),SEEK_SET)==0;
            }
            const uint64_t ordinal=file.readOrdinal++;
            const bool sample=ok && VitaCpuSampleRead(ordinal);
            uint64_t beforeRaw=0,afterRaw=0;
            const bool beforeOk=sample && ReadThreadClocks(beforeRaw);
            const uint64_t readStart=sample?VitaCpuNow():0;
            {
                VITA_CPU_LOADING("package-read-window");
                window.size=ok?uint32_t(std::fread(window.bytes,1,kWindow,file.file)):0;
            }
            const uint64_t readEnd=sample?VitaCpuNow():0;
            const bool afterOk=sample && ReadThreadClocks(afterRaw);
            if(sample) {
                uint64_t rawDelta=0;
                const bool available=VitaCpuReadClockDelta(beforeOk,beforeRaw,afterOk,afterRaw,rawDelta);
                // A loading event carries the read interval, raw counter delta
                // and byte count together; scope overlap is not additive CPU time.
                VitaCpuEvent event{};event.startUs=readStart;event.elapsedUs=readEnd-readStart;
                event.exclusiveUs=event.elapsedUs;event.kind=uint32_t(VitaCpuEventKind::Loading);
                event.value0=rawDelta;event.value1=window.size;
                std::strcpy(event.name,available?"package-read-sample-raw":"package-read-sample-unavailable");
                VitaCpuProfile_Record(event);
            }
            ok=ok && !std::ferror(file.file);
            file.position=job.offset+window.size;file.positionKnown=ok;
            window.state.store(ok?Ready:Error,std::memory_order_release);
            VitaCpuProfile_Counter(ok?"package-read-bytes":"package-read-failed",window.size,VitaCpuNow()-begin);
            if(!ok)Failure("package-worker-read-failed");
            Complete(nullptr);
        }
    }
    for(auto& file:files)if(file.file){std::fclose(file.file);file.file=nullptr;}
    VitaCpuProfile_FlushThread();
    return 0;
}
void Request(int handle,int page,uint32_t generation,uint64_t offset) {
    auto& window=files[handle].windows[page];
    window.offset=offset;window.size=0;window.state.store(Writing,std::memory_order_release);
    Job job{Read};job.handle=handle;job.page=page;job.generation=generation;job.offset=offset;Submit(job);
}
}
bool VitaCpuFiles_Start() {
    if(worker>=0)return true;
    stopping.store(false);jobs.Reset();
    wake=sceKernelCreateSema("cod4-files-wake",0,0,1,nullptr);
    done=sceKernelCreateSema("cod4-files-ready",0,0,1,nullptr);
    worker=sceKernelCreateThread("cod4-package-reader",Worker,0x10000110,64*1024,0,SCE_KERNEL_CPU_MASK_USER_2,nullptr);
    if(wake<0 || done<0 || worker<0 || sceKernelStartThread(worker,0,nullptr)<0) {
        if(worker>=0)sceKernelDeleteThread(worker);
        if(wake>=0)sceKernelDeleteSema(wake);
        if(done>=0)sceKernelDeleteSema(done);
        worker=wake=done=-1;Failure("package-worker-start-failed");return false;
    }
    VitaCpuProfile_Counter("package-window-resident-bytes",kFiles*2*kWindow);
    return true;
}
bool VitaCpuFile::Open(const char* path) {
    Close();
    if(!path || std::strlen(path)>=sizeof(Job::path) || !VitaCpuFiles_Start())return false;
    for(unsigned i=0;i<kFiles;++i)if(!claimed[i]){handle_=int(i);claimed[i]=true;break;}
    if(handle_<0){Failure("package-handle-capacity-exhausted");return false;}
    generation_=++generations[handle_];
    if(!generation_){claimed[handle_]=false;handle_=-1;Failure("package-generation-exhausted");return false;}
    for(auto& window:files[handle_].windows)window.state.store(Free);
    Reply reply;Job job{::kisak::vita::Open};job.handle=handle_;job.generation=generation_;job.reply=&reply;
    std::memcpy(job.path,path,std::strlen(path)+1);Submit(job);Wait(reply);
    size_=reply.size;
    if(!reply.ok){Close();return false;}
    return true;
}
bool VitaCpuFile::Read(uint64_t offset,void* destination,size_t bytes) {
    VITA_CPU_SCOPE("package-consumer-read");
    if(handle_<0 || offset>size_ || bytes>size_-offset || (!destination && bytes))return false;
    auto* output=static_cast<uint8_t*>(destination);
    auto& file=files[handle_];
    while(bytes) {
        const uint64_t aligned=offset&~uint64_t(kWindow-1);
        if(page_<0 || file.windows[page_].offset!=aligned) {
            if(page_>=0){file.windows[page_].state.store(Free,std::memory_order_release);page_=-1;}
            for(;;) {
                int available=-1,pending=-1;
                for(int i=0;i<2;++i) {
                    auto& w=file.windows[i];const int state=w.state.load(std::memory_order_acquire);
                    if(state!=Free && w.offset==aligned){pending=i;break;}
                    if(state!=Writing)available=i;
                }
                if(pending>=0) {
                    const int state=file.windows[pending].state.load(std::memory_order_acquire);
                    if(state==Error)return false;
                    if(state==Ready){page_=pending;file.windows[page_].state.store(Reading);break;}
                } else if(available>=0)Request(handle_,available,generation_,aligned);
                Progress();SceUInt timeout=10000; { VITA_CPU_SCOPE("file-consumer-reply-wait"); sceKernelWaitSema(done,1,&timeout); }
            }
        }
        auto& window=file.windows[page_];
        const size_t within=size_t(offset-aligned);
        if(within>=window.size){Failure("package-read-short");return false;}
        // Prefetch the adjacent window while the source builds/uploads this one.
        const int next=1-page_;
        const int state=file.windows[next].state.load(std::memory_order_acquire);
        if(aligned+kWindow<size_ && state!=Writing && file.windows[next].offset!=aligned+kWindow)
            Request(handle_,next,generation_,aligned+kWindow);
        else if(aligned+kWindow<size_ && state==Free)
            Request(handle_,next,generation_,aligned+kWindow);
        const size_t count=std::min(bytes,size_t(window.size)-within);
        std::memcpy(output,window.bytes+within,count);
        output+=count;offset+=count;bytes-=count;
    }
    return true;
}
void VitaCpuFile::Close() {
    if(handle_<0)return;
    if(worker>=0) {
        Reply reply;Job job{::kisak::vita::Close};job.handle=handle_;job.generation=generation_;job.reply=&reply;
        Submit(job);Wait(reply); // cancels and drains both preceding window jobs
    }
    for(auto& window:files[handle_].windows)window.state.store(Free);
    claimed[handle_]=false;handle_=page_=-1;size_=0;
}
void VitaCpuFiles_Drain(){if(worker>=0){Reply reply;Job job{Barrier};job.reply=&reply;Submit(job);Wait(reply);}}
void VitaCpuFiles_Stop(){
    if(worker<0)return;
    VitaCpuFiles_Drain();stopping.store(true,std::memory_order_release);sceKernelSignalSema(wake,1);
    { VITA_CPU_SCOPE("file-worker-join-wait"); sceKernelWaitThreadEnd(worker,nullptr,nullptr); }sceKernelDeleteThread(worker);
    sceKernelDeleteSema(wake);sceKernelDeleteSema(done);worker=wake=done=-1;
}
void VitaCpuFiles_SetGeneration(uint32_t value){VitaCpuFiles_Drain();session=value;}
void VitaCpuFiles_SetProgress(void (*callback)(void*),void* context){progress=callback;progressContext=context;progressAt=0;}
}
#endif
