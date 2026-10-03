#include <vita/vita_cpu_profile.h>
#include <vita/vita_cpu_queue.h>
#include <vita/vita_cpu_recorder_start.h>
#include <vita/vita_cpu_tls_probe.h>
#include <vita/vita_cpu_recording_policy.h>
#include <vita/vita_cpu_compressed_log.h>
#include <vita/vita_cpu_source_service.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/io/stat.h>

namespace kisak::vita {
namespace {
constexpr uint32_t kThreads=8;
constexpr uint64_t kMaximumBytes=512ull*1024*1024;
struct PerfRecord {uint32_t bytes=0;VitaCpuLog log=VitaCpuLog::Perf;bool flush=false;char text[4096]{};};
VitaCpuQueue<PerfRecord,256> perfRecords;
// A separate single-producer lane: the renderer never publishes into the
// source owner's queue. Preallocated; file writes stay on the recorder.
VitaCpuQueue<PerfRecord,64> renderTextRecords;
struct Channel {
    VitaCpuQueue<VitaCpuEvent,1024> queue;
    std::atomic<int> thread{-1};
    std::atomic<uint32_t> role{uint32_t(VitaCpuRole::Unregistered)};
};
Channel channels[kThreads];
// The high-rate source channel absorbs storage stalls without multiplying the
// capacity of every worker channel. Overflow still invalidates qualification.
VitaCpuQueue<VitaCpuEvent,kCpuSourceEventCapacity> sourceEvents;
std::atomic<uint64_t> sourceQueueDropped{0}, eventQueueDropped{0}, textQueueDropped{0};
std::atomic<uint32_t> sourceQueuePeak{0}, eventQueuePeak{0}, textQueuePeak{0};
struct ExternalThread {std::atomic<int> thread{-1};std::atomic<uint32_t> role{uint32_t(VitaCpuRole::Unregistered)};};
ExternalThread externalThreads[4];
thread_local int channel=-1;
thread_local VitaCpuRole role=VitaCpuRole::Unregistered;
thread_local uint32_t generation=0, sequence=0;
thread_local VitaCpuScope* currentScope=nullptr;
thread_local VitaCpuEvent scopeTotals[64];
thread_local uint32_t scopeTotalCount=0;
thread_local uint64_t scopeFlushAt=0;
std::atomic<bool> active{false}, stopping{false}, failed{false};
std::atomic<uint64_t> recorded{0}, dropped{0}, written{0};
std::atomic<uint32_t> flushRequest{0}, flushed{0};
SceUID writerThread=-1;
FILE* perf=nullptr;
FILE* loading=nullptr;
FILE* frameRecords=nullptr;
FILE* auxiliary[3]{};
char auxiliaryBuffer[3][65536];
char frameRecordBuffer[65536];
char perfBuffer[65536], loadingBuffer[65536];
VitaCpuFlushSchedule flushSchedule;
VitaCpuCompressedLog compressedLogs[6];
uint64_t compressedBytes=0;
uint64_t writeMaxUs=0, flushMaxUs=0, flushCount=0;
void Peak(std::atomic<uint32_t>& peak,uint32_t value) {
    auto previous=peak.load(std::memory_order_relaxed);
    while(previous<value && !peak.compare_exchange_weak(previous,value,std::memory_order_relaxed)){}
}
FILE* Sink(unsigned index) {
    return index==0?perf:index==1?loading:index==2?frameRecords:auxiliary[index-3];
}
bool InitializeCompressedLogs() {
    for(auto& log:compressedLogs)if(!log.Initialize())return false;
    compressedBytes=0;
    // Only the six previous launch logs on the device, never copied captures.
    // Remove obsolete plain counterparts so copying logs does not include them.
    const char* legacy[]={"cpu-perf", "loading-stages", "vita-perf", "script-stages", "shipment-trace", "cpu-replay"};
    for(const char* name:legacy) {
        char path[128];std::snprintf(path,sizeof(path),"ux0:data/CODVIT001/logs/%s.jsonl",name);
        std::remove(path);
    }
    return true;
}
bool WriteCompressed(FILE* file,const unsigned char* data,size_t bytes) {
    const auto begin=VitaCpuNow();
    const bool ok=std::fwrite(data,1,bytes,file)==bytes;
    writeMaxUs=std::max(writeMaxUs,VitaCpuNow()-begin);
    if(ok)compressedBytes+=bytes;else failed.store(true);
    return ok;
}
void FlushSink(unsigned index) {
    auto* file=Sink(index);
    if(!file || !flushSchedule.Dirty(index))return;
    const auto begin=VitaCpuNow();
    if(!compressedLogs[index].Flush([&](const unsigned char* data,size_t bytes){return WriteCompressed(file,data,bytes);}))failed.store(true);
    if(std::fflush(file))failed.store(true);
    flushMaxUs=std::max(flushMaxUs,VitaCpuNow()-begin);++flushCount;
    flushSchedule.Clean(index);
}
void SafeName(char* output,const char* input,size_t size) {
    size_t i=0;
    for (;input && input[i] && i+1<size;++i) {
        const unsigned char c=input[i];
        output[i]=(c>=32 && c<127 && c!='"' && c!='\\')?char(c):'_';
    }
    output[i]=0;
}
void Write(FILE* file,const char* line) {
    if (!file || failed.load()) return;
    const size_t bytes=std::strlen(line);
    unsigned index=0;while(index<6 && Sink(index)!=file)++index;
    if(index==6 || written.load()+bytes>kMaximumBytes
        || !compressedLogs[index].Append(line,bytes,[&](const unsigned char* data,size_t count){return WriteCompressed(file,data,count);})) {
        failed.store(true);return;
    }
    written.fetch_add(bytes);flushSchedule.Mark(index);
}
void WriteEvent(const VitaCpuEvent& event) {
    char line[448];
    std::snprintf(line,sizeof(line),
        "{\"type\":\"%s\",\"name\":\"%s\",\"startUs\":%llu,\"elapsedUs\":%llu,\"exclusiveUs\":%llu,\"generation\":%u,\"sequence\":%u,\"role\":%u,\"value0\":%llu,\"value1\":%llu}\n",
        event.kind==uint32_t(VitaCpuEventKind::Loading)?"loading-stage":event.kind==uint32_t(VitaCpuEventKind::Scope)?"scope":"counter",event.name,
        (unsigned long long)event.startUs,(unsigned long long)event.elapsedUs,
        (unsigned long long)event.exclusiveUs,event.generation,event.sequence,event.role,
        (unsigned long long)event.value0,(unsigned long long)event.value1);
    Write(event.kind==uint32_t(VitaCpuEventKind::Loading)?loading:perf,line);
}
void SampleThreads(uint64_t now) {
    const auto sample=[&](int id,uint32_t roleId) {
        if (id<0) return;
        SceKernelThreadInfo info{};info.size=sizeof(info);
        char line[512];
        const int result=sceKernelGetThreadInfo(id,&info);
        if (result<0) {
            std::snprintf(line,sizeof(line),"{\"type\":\"thread\",\"atUs\":%llu,\"id\":%d,\"role\":%u,\"available\":false,\"error\":%d}\n",
                (unsigned long long)now,id,roleId,result);
        } else {
            uint64_t clocks=0;static_assert(sizeof(info.runClocks)==sizeof(clocks));
            std::memcpy(&clocks,&info.runClocks,sizeof(clocks));
            char name[64];SafeName(name,info.name,sizeof(name));
            std::snprintf(line,sizeof(line),"{\"type\":\"thread\",\"atUs\":%llu,\"id\":%d,\"name\":\"%s\",\"role\":%u,\"available\":true,\"affinity\":%d,\"cpu\":%d,\"priority\":%d,\"waitType\":%u,\"runClocksRaw\":%llu,\"runClocksUnit\":\"unvalidated\",\"utilization\":null,\"freeStackBytes\":%d}\n",
                (unsigned long long)now,id,name,roleId,info.currentCpuAffinityMask,
                info.lastExecutedCpuId,info.currentPriority,info.waitType,(unsigned long long)clocks,
                sceKernelGetThreadStackFreeSize(id));
        }
        Write(perf,line);
    };
    for(auto& item:channels)sample(item.thread.load(std::memory_order_acquire),item.role.load());
    for(auto& item:externalThreads)sample(item.thread.load(std::memory_order_acquire),item.role.load());
}
template<uint32_t Capacity>
bool DrainTextRecords(VitaCpuQueue<PerfRecord,Capacity>& queue,unsigned& left,bool* requestedSink) {
    bool any=false;PerfRecord record;
    for(unsigned n=0;n<32 && left;++n,--left) {
        if(!queue.Pop(&record)){failed.store(true);left=0;break;}
        const unsigned sink=record.log==VitaCpuLog::Perf?2u:2u+uint32_t(record.log);
        Write(Sink(sink),record.text);any=true;requestedSink[sink]|=record.flush;
    }
    return any;
}
int Writer(SceSize,void*) {
    VitaCpuRegisterThread(VitaCpuRole::Recorder,SCE_KERNEL_CPU_MASK_USER_2);
    uint64_t sampleAt=0;
    for (;;) {
        // Acquire the flush request before draining. A request published after
        // this boundary is acknowledged on the next pass, after its records.
        const auto request=flushRequest.load(std::memory_order_acquire);
        bool any=false;
        // Snapshot published counts after the flush request. Drain that bounded
        // cohort fairly; newer work waits for the next pass. This acknowledges
        // every preceding record without chasing producers indefinitely.
        unsigned textLeft=perfRecords.Count(),renderTextLeft=renderTextRecords.Count(),sourceLeft=sourceEvents.Count();
        unsigned eventLeft[kThreads]{};bool requestedSink[6]{};
        for(unsigned i=0;i<kThreads;++i)eventLeft[i]=channels[i].queue.Count();
        bool remaining;
        do {
            any|=DrainTextRecords(perfRecords,textLeft,requestedSink);
            any|=DrainTextRecords(renderTextRecords,renderTextLeft,requestedSink);
            VitaCpuEvent event;
            for(unsigned n=0;n<256 && sourceLeft;++n,--sourceLeft) {
                if(!sourceEvents.Pop(&event)){failed.store(true);sourceLeft=0;break;}
                WriteEvent(event);any=true;
            }
            remaining=textLeft || renderTextLeft || sourceLeft;
            for(unsigned i=0;i<kThreads;++i) {
                for(unsigned n=0;n<32 && eventLeft[i];++n,--eventLeft[i]) {
                    if(!channels[i].queue.Pop(&event)){failed.store(true);eventLeft[i]=0;break;}
                    WriteEvent(event);any=true;
                }
                remaining|=eventLeft[i]!=0;
            }
        } while(remaining);
        // Coalesce per-record flush flags. Each flagged record is included;
        // one batch cannot force hundreds of separate storage flushes.
        for(unsigned i=0;i<6;++i)if(requestedSink[i])FlushSink(i);
        const auto now=VitaCpuNow();
        if (now>=sampleAt) {
            SampleThreads(now);sampleAt=now+1000000;
            char health[1024];std::snprintf(health,sizeof(health),
                "{\"type\":\"recording-health\",\"atUs\":%llu,\"dropped\":%llu,\"bytes\":%llu,\"ioFailed\":%s,\"sourceQueueDepth\":%u,\"sourceQueuePeak\":%u,\"eventQueuePeak\":%u,\"textQueuePeak\":%u,\"sourceQueueDropped\":%llu,\"eventQueueDropped\":%llu,\"textQueueDropped\":%llu,\"writeMaxUs\":%llu,\"flushMaxUs\":%llu,\"flushCount\":%llu,\"compressedBytes\":%llu,\"compression\":\"zlib-members-32k-v1\"}\n",
                (unsigned long long)now,(unsigned long long)dropped.load(),(unsigned long long)written.load(),failed.load()?"true":"false",
                sourceEvents.Count(),sourceQueuePeak.load(),eventQueuePeak.load(),textQueuePeak.load(),
                (unsigned long long)sourceQueueDropped.load(),(unsigned long long)eventQueueDropped.load(),(unsigned long long)textQueueDropped.load(),
                (unsigned long long)writeMaxUs,(unsigned long long)flushMaxUs,(unsigned long long)flushCount,(unsigned long long)compressedBytes);
            Write(perf,health);Write(loading,health);
        }
        if (request!=flushed.load()) {
            for(unsigned i=0;i<6;++i)FlushSink(i);
            flushed.store(request,std::memory_order_release);
        } else {
            const int file=flushSchedule.Periodic(now);
            if(file>=0)FlushSink(unsigned(file));
        }
        if (stopping.load(std::memory_order_acquire) && !any) break;
        sceKernelDelayThread(any?1000:10000);
    }
    char end[160];
    std::snprintf(end,sizeof(end),"{\"type\":\"end\",\"events\":%llu,\"dropped\":%llu,\"ioFailed\":%s,\"hardwareQualified\":false}\n",
        (unsigned long long)recorded.load(),(unsigned long long)dropped.load(),failed.load()?"true":"false");
    Write(perf,end);Write(loading,end);
    for(unsigned i=0;i<6;++i)FlushSink(i);
    if(perf){std::fclose(perf);perf=nullptr;}
    if(loading){std::fclose(loading);loading=nullptr;}
    if(frameRecords){std::fclose(frameRecords);frameRecords=nullptr;}
    for(auto& file:auxiliary)if(file){std::fclose(file);file=nullptr;}
    return 0;
}
}
uint64_t VitaCpuNow() { return sceKernelGetProcessTimeWide(); }
void VitaCpuProfile_Startup(const char* stage,int result,int priority,int affinity) {
    // Called at serialized startup boundaries, before expensive work or after
    // a kernel result. No dependency on the asynchronous recorder being alive.
    FILE* file=std::fopen("ux0:data/CODVIT001/logs/cpu-startup.jsonl","ab");
    if(!file)return;
    char safe[96],line[320];SafeName(safe,stage,sizeof(safe));
    const int size=std::snprintf(line,sizeof(line),
        "{\"stage\":\"%s\",\"atUs\":%llu,\"thread\":%d,\"result\":%d,\"resultHex\":\"0x%08x\",\"priority\":%d,\"affinity\":%d}\n",
        safe,(unsigned long long)VitaCpuNow(),sceKernelGetThreadId(),result,unsigned(result),priority,affinity);
    if(size>0 && size<int(sizeof(line)))std::fwrite(line,1,size_t(size),file);
    std::fflush(file);std::fclose(file);
}
VitaCpuRole VitaCpuCurrentRole() { return role; }
void VitaCpuProfile_ExternalThread(const char* name,int id) {
    if(id<0 || !name)return;
    const bool audio=std::strstr(name,"OpenAL")!=nullptr;
    if(!audio && !std::strstr(name,"Garbage Collector"))return;
    for(auto& item:externalThreads) {
        int empty=-1;
        if(item.thread.compare_exchange_strong(empty,id)){item.role.store(uint32_t(audio?VitaCpuRole::Audio:VitaCpuRole::Background));return;}
    }
    dropped.fetch_add(1);
}
void VitaCpuProfile_DeletedThread(int id) {
    for(auto& item:externalThreads){int expected=id;item.thread.compare_exchange_strong(expected,-1);}
    for(auto& item:channels){int expected=id;item.thread.compare_exchange_strong(expected,-1);}
}
void VitaCpuRegisterThread(VitaCpuRole value,int affinity) {
    role=value;
    const int id=sceKernelGetThreadId();
    if (affinity && sceKernelGetThreadCpuAffinityMask(id)!=affinity) {
        const int result=sceKernelChangeThreadCpuAffinityMask(id,affinity);
        if (result<0) {
            dropped.fetch_add(1);
            VitaCpuProfile_Startup("thread-affinity-failed",result,0,affinity);
        }
    }
    if (channel<0) {
        for(uint32_t i=0;i<kThreads;++i) {
            int unused=-1;
            if(channels[i].thread.compare_exchange_strong(unused,id)) {channel=int(i);break;}
        }
    }
    if(channel>=0)channels[channel].role.store(uint32_t(value));
}

#if KISAK_VITA_CPU_RUNTIME
extern "C" SceUID __real_sceKernelCreateThread(const char*,SceKernelThreadEntry,int,SceSize,SceUInt,int,const SceKernelThreadOptParam*);
extern "C" int __real_sceKernelDeleteThread(SceUID);
extern "C" SceUID __wrap_sceKernelCreateThread(const char* name,SceKernelThreadEntry entry,int priority,SceSize stack,SceUInt attr,int affinity,const SceKernelThreadOptParam* options) {
    const auto id=__real_sceKernelCreateThread(name,entry,priority,stack,attr,affinity,options);
    kisak::vita::VitaCpuProfile_ExternalThread(name,id);return id;
}
extern "C" int __wrap_sceKernelDeleteThread(SceUID id) {
    const int result=__real_sceKernelDeleteThread(id);
    if(result>=0)kisak::vita::VitaCpuProfile_DeletedThread(id);
    return result;
}
#endif
bool VitaCpuProfile_Start(const char* kind) {
    if(active.load())return true;
    sceIoMkdir("ux0:data/CODVIT001",0777);
    sceIoMkdir("ux0:data/CODVIT001/logs",0777);
    if(FILE* startup=std::fopen("ux0:data/CODVIT001/logs/cpu-startup.jsonl","wb"))std::fclose(startup);
    VitaCpuProfile_Startup("recorder-initialize",0,kVitaCpuRecorderPriority,SCE_KERNEL_CPU_MASK_USER_2);
    if(!VitaCpuVerifyThreadLocals([](const char* stage,int result,int affinity){
        VitaCpuProfile_Startup(stage,result,160,affinity);
    })) {failed.store(true);return false;}
    if(!InitializeCompressedLogs()){failed.store(true);return false;}
    perf=std::fopen("ux0:data/CODVIT001/logs/cpu-perf.jsonl.z","wb");
    loading=std::fopen("ux0:data/CODVIT001/logs/loading-stages.jsonl.z","wb");
    if(!perf || !loading) {
        if(perf)std::fclose(perf);
        if(loading)std::fclose(loading);
        perf=loading=nullptr;failed.store(true);return false;
    }
    std::setvbuf(perf,perfBuffer,_IOFBF,sizeof(perfBuffer));
    std::setvbuf(loading,loadingBuffer,_IOFBF,sizeof(loadingBuffer));
#if KISAK_VITA_CPU_RUNTIME
    frameRecords=std::fopen("ux0:data/CODVIT001/logs/vita-perf.jsonl.z","wb");
    if(frameRecords)std::setvbuf(frameRecords,frameRecordBuffer,_IOFBF,sizeof(frameRecordBuffer));
    else failed.store(true);
    const char* names[]={"script-stages.jsonl.z","shipment-trace.jsonl.z","cpu-replay.jsonl.z"};
    for(unsigned i=0;i<3;++i) {
        char path[128];std::snprintf(path,sizeof(path),"ux0:data/CODVIT001/logs/%s",names[i]);
        auxiliary[i]=std::fopen(path,"wb");
        if(auxiliary[i])std::setvbuf(auxiliary[i],auxiliaryBuffer[i],_IOFBF,sizeof(auxiliaryBuffer[i]));
        else failed.store(true);
    }
#endif
    char safe[64],header[256];SafeName(safe,kind,sizeof(safe));
    std::snprintf(header,sizeof(header),"{\"format\":\"CODVIT-CPU-1\",\"kind\":\"%s\",\"timing\":\"elapsed-including-waits\",\"hardwareQualified\":false}\n",safe);
    Write(perf,header);Write(loading,header);
    stopping.store(false);active.store(true);
    VitaCpuRegisterThread(VitaCpuRole::Source);
    writerThread=VitaCpuStartRecorder(
        [](int priority){return sceKernelCreateThread("cod4-cpu-recorder",Writer,priority,65536,0,SCE_KERNEL_CPU_MASK_USER_2,nullptr);},
        [](int id){return sceKernelStartThread(id,0,nullptr);},
        [](int id){sceKernelDeleteThread(id);},
        [](const char* stage,int result){VitaCpuProfile_Startup(stage,result,kVitaCpuRecorderPriority,SCE_KERNEL_CPU_MASK_USER_2);});
    if(writerThread<0) {
        if(writerThread>=0)sceKernelDeleteThread(writerThread);
        writerThread=-1;
        active.store(false);failed.store(true);
        if(frameRecords){std::fclose(frameRecords);frameRecords=nullptr;}
        for(auto& file:auxiliary)if(file){std::fclose(file);file=nullptr;}
        std::fclose(perf);std::fclose(loading);perf=loading=nullptr;return false;
    }
    return true;
}
void VitaCpuProfile_Stop() {
    VitaCpuProfile_FlushThread();
    if(!active.exchange(false))return;
    stopping.store(true,std::memory_order_release);
    if(writerThread>=0) {
        sceKernelWaitThreadEnd(writerThread,nullptr,nullptr);
        sceKernelDeleteThread(writerThread);writerThread=-1;
    }
}
void VitaCpuProfile_Flush(bool wait) {
    if(!active.load())return;
    VitaCpuProfile_FlushThread();
    const auto request=flushRequest.fetch_add(1,std::memory_order_release)+1;
    while(wait && flushed.load(std::memory_order_acquire)<request && active.load()) {
        VitaCpuSource_Service();sceKernelDelayThread(1000);
    }
}
void Enqueue(VitaCpuEvent event) {
    if(channel<0){dropped.fetch_add(1,std::memory_order_relaxed);eventQueueDropped.fetch_add(1);return;}
    const bool source=role==VitaCpuRole::Source;
    const bool ok=source?sourceEvents.Push(event):channels[channel].queue.Push(event);
    if(!ok) {
        dropped.fetch_add(1,std::memory_order_relaxed);
        (source?sourceQueueDropped:eventQueueDropped).fetch_add(1,std::memory_order_relaxed);
    } else {
        recorded.fetch_add(1,std::memory_order_relaxed);
        if(source)Peak(sourceQueuePeak,sourceEvents.Count());
        else Peak(eventQueuePeak,channels[channel].queue.Count());
    }
}
void VitaCpuProfile_FlushThread() {
    for(uint32_t i=0;i<scopeTotalCount;++i)Enqueue(scopeTotals[i]);
    scopeTotalCount=0;scopeFlushAt=VitaCpuNow()+250000;
}
VitaCpuContext VitaCpuProfile_Context(){return {generation,sequence};}
void VitaCpuProfile_SetSequence(uint32_t gen,uint32_t seq){
    if(gen!=generation || seq!=sequence)VitaCpuProfile_FlushThread();
    generation=gen;sequence=seq;
}
void VitaCpuProfile_Record(VitaCpuEvent event) {
    if(!active.load(std::memory_order_relaxed))return;
    event.role=uint32_t(role);event.generation=generation;event.sequence=sequence;
    if(event.kind==uint32_t(VitaCpuEventKind::Scope)) {
        if(VitaCpuNow()>=scopeFlushAt)VitaCpuProfile_FlushThread();
        for(uint32_t i=0;i<scopeTotalCount;++i)if(!std::strcmp(scopeTotals[i].name,event.name)) {
            auto& total=scopeTotals[i];total.elapsedUs+=event.elapsedUs;total.exclusiveUs+=event.exclusiveUs;
            ++total.value0;total.value1=std::max(total.value1,event.elapsedUs);return;
        }
        if(scopeTotalCount==64)VitaCpuProfile_FlushThread();
        event.value0=1;event.value1=event.elapsedUs;scopeTotals[scopeTotalCount++]=event;
    } else Enqueue(event);
}
void VitaCpuProfile_Counter(const char* name,uint64_t a,uint64_t b){
    VitaCpuEvent e{};e.startUs=VitaCpuNow();e.kind=uint32_t(VitaCpuEventKind::Counter);e.value0=a;e.value1=b;
    SafeName(e.name,name,sizeof(e.name));VitaCpuProfile_Record(e);
}
VitaCpuProfileStatus VitaCpuProfile_Read(){return {recorded.load(),dropped.load(),written.load(),active.load(),failed.load()};}
bool VitaCpuProfile_AppendPerf(const char* text,uint32_t bytes,bool flush) {
    return VitaCpuProfile_AppendLog(VitaCpuLog::Perf,text,bytes,flush);
}
bool VitaCpuProfile_AppendLog(VitaCpuLog log,const char* text,uint32_t bytes,bool flush) {
    if(!active.load() || bytes>=sizeof(PerfRecord::text) || (!text && bytes)) {
        dropped.fetch_add(1);return false;
    }
    const bool render=role==VitaCpuRole::Render && log==VitaCpuLog::Script;
    if((role!=VitaCpuRole::Source && !render) || uint32_t(log)>3){dropped.fetch_add(1);return false;}
    PerfRecord record{};record.bytes=bytes;record.flush=flush;record.log=log;
    if(bytes)std::memcpy(record.text,text,bytes);
    const bool pushed=render?renderTextRecords.Push(record):perfRecords.Push(record);
    if(!pushed){dropped.fetch_add(1);textQueueDropped.fetch_add(1);return false;}
    Peak(textQueuePeak,render?renderTextRecords.Count():perfRecords.Count());
    return true;
}
void VitaCpuProfile_Trace(const char* stage,const char* subject,const char* detail) {
    char line[4096];size_t used=0;bool valid=true;
    const auto literal=[&](const char* text) {
        const size_t size=std::strlen(text);
        if(size>=sizeof(line)-used){valid=false;return;}
        std::memcpy(line+used,text,size);used+=size;
    };
    const auto quoted=[&](const char* text) {
        literal("\"");
        for(const unsigned char* p=reinterpret_cast<const unsigned char*>(text?text:"");*p;++p) {
            char encoded[8]{};
            if(*p=='"' || *p=='\\'){encoded[0]='\\';encoded[1]=char(*p);}
            else if(*p<32)std::snprintf(encoded,sizeof(encoded),"\\u%04x",unsigned(*p));
            else encoded[0]=char(*p);
            literal(encoded);
        }
        literal("\"");
    };
    char prefix[160];std::snprintf(prefix,sizeof(prefix),"{\"atUs\":%llu,\"generation\":%u,\"sequence\":%u,\"stage\":",
        (unsigned long long)VitaCpuNow(),generation,sequence);
    literal(prefix);quoted(stage);
    if(subject){literal(",\"subject\":");quoted(subject);}
    if(detail){literal(",\"detail\":");quoted(detail);}
    literal("}\n");
    if(valid)VitaCpuProfile_AppendLog(VitaCpuLog::Script,line,uint32_t(used));
    else VitaCpuProfile_Invalidate("script-record-capacity-exhausted");
}
void VitaCpuProfile_Invalidate(const char* reason){dropped.fetch_add(1);VitaCpuProfile_Counter(reason,1);}
void* VitaCpuProfile_ScopeCheckpoint(){return currentScope;}
void VitaCpuProfile_RestoreScopes(void* checkpoint){currentScope=static_cast<VitaCpuScope*>(checkpoint);}
VitaCpuScope::VitaCpuScope(const char* name,VitaCpuEventKind kind):name_(name),kind_(kind){
    if(!active.load(std::memory_order_relaxed))return;
    parent_=currentScope;currentScope=this;start_=VitaCpuNow();
}
VitaCpuScope::~VitaCpuScope(){
    if(!start_)return;
    const auto elapsed=VitaCpuNow()-start_;
    if(parent_)parent_->children_+=elapsed;
    currentScope=parent_;
    VitaCpuEvent e{};e.startUs=start_;e.elapsedUs=elapsed;e.exclusiveUs=elapsed-std::min(elapsed,children_);e.kind=uint32_t(kind_);
    SafeName(e.name,name_,sizeof(e.name));VitaCpuProfile_Record(e);
}
}
