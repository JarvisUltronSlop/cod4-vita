#include <vita/vita_cpu_audio.h>
#if KISAK_VITA_CPU_RUNTIME
#include <vita/vita_cpu_profile.h>
#include <vita/vita_cpu_queue.h>
#include <vita/vita_cpu_audio_index.h>
#include <dr_libs/dr_wav.h>
#include <dr_libs/dr_mp3.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/kernel/threadmgr/semaphore.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstring>

// Existing pinned OpenAL honors these weak application settings. Its output
// callback has priority over decoding, package jobs and buffered recording.
extern "C" {
unsigned int _oal_thread_priority=64;
unsigned int _oal_thread_affinity=SCE_KERNEL_CPU_MASK_USER_2;
}
namespace kisak::vita {
namespace {
constexpr uint32_t kChannels=13,kFrames=8192,kBlocks=8;
struct Pcm {uint32_t generation=0,frames=0;bool looped=false;int16_t samples[kFrames*2];};
struct Decoder {
    FILE* file=nullptr;drwav wav{};drmp3 mp3{};
    bool opened=false,isMp3=false,playing=false,loop=false;
    uint32_t generation=0,channels=0,rate=0;
    VitaCpuContext origin{};
};
struct Shared {
    VitaCpuQueue<Pcm,kBlocks> pcm;
    std::atomic<uint32_t> ended{0},failed{0},active{0};
};
enum class Kind {Open,Begin,Close,Barrier};
struct Command {Kind kind;uint32_t channel=0,generation=0,metadata=0;uint64_t frame=0;bool mp3=false,loop=false;VitaCpuContext origin{};char path[320]{};};
std::array<Decoder,kChannels> decoders{}; // only the worker accesses these
std::array<Shared,kChannels> shared{};
std::array<uint32_t,kChannels> nextGeneration{}; // only the source owner accesses these
VitaCpuQueue<Command,64> commands;
std::atomic<bool> stopping{false};
std::atomic<uint64_t> decoded{0},pendingReads{0},failures{0},underruns{0};
std::atomic<bool> replyReady{false};
std::atomic<uint32_t> highWater{0};
SceUID worker=-1,wake=-1,ack=-1;
std::array<VitaCpuAudioIndexRecord,kCpuAudioIndexMaximum> metadata{};
uint32_t metadataCount=0;
std::atomic<bool> indexReady{false};
bool indexValid=false; // immutable after indexReady release
#if KISAK_VITA_CPU_TEST
constexpr const char* kIndexPath="cpu-audio-fixtures/index.bin";
constexpr const char* kStreamPrefix="cpu-audio-fixtures/";
#else
constexpr const char* kIndexPath="app0:cpu-stream-index.bin";
constexpr const char* kStreamPrefix="ux0:data/CODVIT001/stream/";
#endif
void Failure(const char* label,uint32_t channel){failures.fetch_add(1);VitaCpuProfile_Counter(label,channel,failures.load());VitaCpuProfile_Invalidate(label);}
void Close(Decoder& d) {
    if(d.opened){if(d.isMp3)drmp3_uninit(&d.mp3);else drwav_uninit(&d.wav);}
    if(d.file)std::fclose(d.file);
    d=Decoder{};
}
size_t Read(void* p,void* destination,size_t bytes) {
    VITA_CPU_LOADING("stream-file-read");
    auto& d=*static_cast<Decoder*>(p);return std::fread(destination,1,bytes,d.file);
}
drwav_bool32 WavSeek(void* p,int offset,drwav_seek_origin origin){return std::fseek(static_cast<Decoder*>(p)->file,offset,int(origin))==0;}
drmp3_bool32 Mp3Seek(void* p,int offset,drmp3_seek_origin origin){return std::fseek(static_cast<Decoder*>(p)->file,offset,int(origin))==0;}
drwav_bool32 WavTell(void* p,drwav_int64* position){const long pos=std::ftell(static_cast<Decoder*>(p)->file);*position=pos;return pos>=0;}
drmp3_bool32 Mp3Tell(void* p,drmp3_int64* position){const long pos=std::ftell(static_cast<Decoder*>(p)->file);*position=pos;return pos>=0;}
bool Seek(Decoder& d,uint64_t frame){return d.isMp3?drmp3_seek_to_pcm_frame(&d.mp3,frame):drwav_seek_to_pcm_frame(&d.wav,frame);}
bool LoadIndex() {
    VITA_CPU_LOADING("stream-index-read");
    FILE* file=std::fopen(kIndexPath,"rb");if(!file)return false;
    VitaCpuAudioIndexHeader h{};
    bool valid=std::fread(&h,1,sizeof(h),file)==sizeof(h) && h.magic==kCpuAudioIndexMagic
        && h.version==1 && h.count>0 && h.count<=metadata.size() && h.recordBytes==sizeof(metadata[0]);
    if(valid)valid=std::fread(metadata.data(),sizeof(metadata[0]),h.count,file)==h.count
        && std::fgetc(file)==EOF && !std::ferror(file)
        && VitaCpuAudioCrc(metadata.data(),h.count*sizeof(metadata[0]))==h.crc;
    std::fclose(file);
    if(valid)for(uint32_t i=0;i<h.count;++i) {
        const auto& r=metadata[i];
        if(!std::memchr(r.path,0,sizeof(r.path)) || std::strncmp(r.path,"sound/",6)
            || std::strstr(r.path,"..") || std::strchr(r.path,':') || std::strchr(r.path,'\\')
            || !r.rate || !r.frames || !r.fileBytes || r.fileBytes>0x7fffffffu || r.channels<1 || r.channels>2
            || r.mp3>1 || (i && std::strcmp(metadata[i-1].path,r.path)>=0)){valid=false;break;}
    }
    metadataCount=valid?h.count:0;return valid;
}
bool ValidateFile(FILE* file,const VitaCpuAudioIndexRecord& record) {
    if(!file || std::fseek(file,0,SEEK_END) || std::ftell(file)!=long(record.fileBytes))return false;
    char buffer[4096];const size_t size=size_t(std::min<uint64_t>(record.fileBytes,sizeof(buffer)));
    const bool valid=std::fseek(file,0,SEEK_SET)==0 && std::fread(buffer,1,size,file)==size
        && VitaCpuAudioCrc(buffer,size)==record.prefixCrc
        && std::fseek(file,long(record.fileBytes-size),SEEK_SET)==0 && std::fread(buffer,1,size,file)==size
        && VitaCpuAudioCrc(buffer,size)==record.tailCrc && std::fseek(file,0,SEEK_SET)==0;
    return valid;
}
void Execute(const Command& c) {
    VitaCpuProfile_SetSequence(c.origin.generation,c.origin.sequence);
    if(c.kind==Kind::Barrier){replyReady.store(true,std::memory_order_release);sceKernelSignalSema(ack,1);return;}
    auto& d=decoders[c.channel];auto& s=shared[c.channel];
    if(c.kind==Kind::Open) {
        VITA_CPU_LOADING("stream-open-metadata");
        Close(d);s.active.store(0);d.generation=c.generation;d.isMp3=c.mp3;d.origin=c.origin;
        d.file=std::fopen(c.path,"rb");
        const auto& expected=metadata[c.metadata];
        d.opened=ValidateFile(d.file,expected) && (d.isMp3
            ? drmp3_init(&d.mp3,Read,Mp3Seek,Mp3Tell,nullptr,&d,nullptr)
            : drwav_init(&d.wav,Read,WavSeek,WavTell,&d,nullptr));
        if(d.opened) {
            d.channels=d.isMp3?d.mp3.channels:d.wav.channels;d.rate=d.isMp3?d.mp3.sampleRate:d.wav.sampleRate;
            if(d.channels==expected.channels && d.rate==expected.rate
                && (d.isMp3 || d.wav.totalPCMFrameCount==expected.frames)) {
                s.active.store(c.generation,std::memory_order_release);
            } else {Close(d);s.failed.store(c.generation);Failure("stream-format-failed",c.channel);}
        } else {Close(d);s.failed.store(c.generation);Failure("stream-open-identity-failed",c.channel);}
        return;
    }
    if(d.generation!=c.generation)return; // cancelled generation cannot affect its replacement
    if(c.kind==Kind::Close){Close(d);s.active.store(0,std::memory_order_release);return;}
    if(c.kind==Kind::Begin) {
        d.loop=c.loop;
        if(!d.opened || !Seek(d,c.frame)){s.failed.store(c.generation);Failure("stream-seek-failed",c.channel);}
        else d.playing=true;
    }
}
int Worker(SceSize,void*) {
    VitaCpuRegisterThread(VitaCpuRole::Background,SCE_KERNEL_CPU_MASK_USER_2);
    indexValid=LoadIndex();if(!indexValid)Failure("stream-index-invalid",0);
    indexReady.store(true,std::memory_order_release);sceKernelSignalSema(ack,1);
    static Pcm block{};
    while(!stopping.load(std::memory_order_acquire)) {
        bool work=false;Command command{};
        while(commands.Pop(&command)){Execute(command);work=true;}
        // One block per stream per round gives each active channel fair service.
        for(uint32_t i=0;i<kChannels;++i) {
            auto& d=decoders[i];auto& s=shared[i];
            if(!d.playing || s.pcm.Count()==kBlocks)continue;
            VitaCpuProfile_SetSequence(d.origin.generation,d.origin.sequence);
            VITA_CPU_SCOPE("stream-decode");
            block.generation=d.generation;block.looped=false;
            block.frames=uint32_t(d.isMp3?drmp3_read_pcm_frames_s16(&d.mp3,kFrames,block.samples):drwav_read_pcm_frames_s16(&d.wav,kFrames,block.samples));
            if(!block.frames && d.loop) {
                block.looped=true;
                if(Seek(d,0))block.frames=uint32_t(d.isMp3?drmp3_read_pcm_frames_s16(&d.mp3,kFrames,block.samples):drwav_read_pcm_frames_s16(&d.wav,kFrames,block.samples));
            }
            if(!block.frames){
                d.playing=false;
                if(d.loop || std::ferror(d.file)){s.failed.store(d.generation);Failure("stream-read-decode-failed",i);}
                else s.ended.store(d.generation,std::memory_order_release);
                continue;
            }
            if(!s.pcm.Push(block)){Failure("pcm-publication-failed",i);continue;}
            decoded.fetch_add(block.frames);work=true;
        }
        if(!work){SceUInt timeout=5000; VITA_CPU_SCOPE("audio-worker-idle-wait"); sceKernelWaitSema(wake,1,&timeout);}
        else { VITA_CPU_SCOPE("audio-worker-priority-yield"); sceKernelDelayThread(100); } // output callback retains priority
    }
    for(auto& d:decoders)Close(d);
    VitaCpuProfile_FlushThread();
    return 0;
}
bool Push(Command c) {
    c.origin=VitaCpuProfile_Context();
    if(worker<0){Failure("audio-worker-unavailable",c.channel);return false;}
    bool reported=false;
    while(!commands.Push(c)) {
        // Controls are lossless. Exceptional backpressure invalidates this
        // measurement, but never loses a close/cancel or reorders an event.
        if(!reported){Failure("audio-command-backpressure",c.channel);reported=true;}
        sceKernelSignalSema(wake,1); { VITA_CPU_SCOPE("audio-command-capacity-wait"); sceKernelDelayThread(100); }
    }
    const auto count=commands.Count();if(count>highWater.load())highWater.store(count);
    sceKernelSignalSema(wake,1);return true;
}
}
bool VitaCpuAudio_Start() {
    if(worker>=0)return indexReady.load(std::memory_order_acquire) && indexValid;
    stopping.store(false);commands.Reset();
    indexReady.store(false);
    wake=sceKernelCreateSema("cod4-decode-wake",0,0,1,nullptr);
    ack=sceKernelCreateSema("cod4-decode-ack",0,0,1,nullptr);
    worker=sceKernelCreateThread("cod4-audio-decode",Worker,0x10000100,128*1024,0,SCE_KERNEL_CPU_MASK_USER_2,nullptr);
    if(wake<0 || ack<0 || worker<0 || sceKernelStartThread(worker,0,nullptr)<0){
        if(worker>=0)sceKernelDeleteThread(worker);
        if(wake>=0)sceKernelDeleteSema(wake);
        if(ack>=0)sceKernelDeleteSema(ack);
        wake=ack=worker=-1;Failure("audio-worker-start-failed",0);return false;
    }
    while(!indexReady.load(std::memory_order_acquire)) { VITA_CPU_SCOPE("audio-index-ready-wait"); sceKernelWaitSema(ack,1,nullptr); }
    return indexValid;
}
VitaCpuStreamInfo VitaCpuAudio_Open(uint32_t channel,const char* path,bool mp3) {
    VitaCpuStreamInfo reply{};
    if(channel>=kChannels || !path || !path[0] || path[0]=='/' || path[0]=='\\' || std::strchr(path,':') || std::strstr(path,".."))return reply;
    if(!VitaCpuAudio_Start())return reply;
    char normalized[256];if(std::strlen(path)>=sizeof(normalized))return reply;
    std::strcpy(normalized,path);
    for(char* p=normalized;*p;++p){if(*p=='\\')*p='/';else if(*p>='A' && *p<='Z')*p+=('a'-'A');}
    const auto found=std::lower_bound(metadata.begin(),metadata.begin()+metadataCount,normalized,
        [](const auto& item,const char* name){return std::strcmp(item.path,name)<0;});
    if(found==metadata.begin()+metadataCount || std::strcmp(found->path,normalized) || bool(found->mp3)!=mp3) {
        Failure("stream-index-entry-missing",channel);return reply;
    }
    const uint32_t generation=++nextGeneration[channel];
    if(!generation){Failure("audio-generation-exhausted",channel);return reply;}
    Command c{Kind::Open};c.channel=channel;c.generation=generation;c.mp3=mp3;c.metadata=uint32_t(found-metadata.begin());
    if(std::snprintf(c.path,sizeof(c.path),"%s%s",kStreamPrefix,normalized)>=int(sizeof(c.path)))return reply;
    if(!Push(c))return reply;
    // Metadata was produced with the same pinned decoder at staging. Source
    // duration/pitch/random-start calculations continue immediately; the worker
    // validates the unchanged loose file before publishing any PCM.
    return {found->channels,found->rate,generation,found->frames,true};
}
bool VitaCpuAudio_Begin(uint32_t channel,uint32_t generation,uint64_t frame,bool loop){Command c{Kind::Begin};c.channel=channel;c.generation=generation;c.frame=frame;c.loop=loop;return channel<kChannels && Push(c);}
VitaCpuPcmResult VitaCpuAudio_Read(uint32_t channel,uint32_t generation,int16_t* samples,uint32_t* frames,bool* looped) {
    if(channel>=kChannels || !samples || !frames || !looped)return VitaCpuPcmResult::Error;
    auto& s=shared[channel];static Pcm block;
    // Retire stale blocks without exposing samples from a cancelled stream.
    for(unsigned n=0;n<kBlocks && s.pcm.Pop(&block);++n) {
        if(block.generation!=generation)continue;
        *frames=block.frames;*looped=block.looped;
        std::memcpy(samples,block.samples,sizeof(block.samples));sceKernelSignalSema(wake,1);return VitaCpuPcmResult::Data;
    }
    if(s.failed.load(std::memory_order_acquire)==generation)return VitaCpuPcmResult::Error;
    if(s.ended.load(std::memory_order_acquire)==generation && s.pcm.Count()==0)return VitaCpuPcmResult::End;
    pendingReads.fetch_add(1);return VitaCpuPcmResult::Pending;
}
void VitaCpuAudio_Close(uint32_t channel,uint32_t generation){Command c{Kind::Close};c.channel=channel;c.generation=generation;if(channel<kChannels && generation)Push(c);}
void VitaCpuAudio_Drain(){
    if(worker<0)return;
    for(uint32_t i=0;i<kChannels;++i)VitaCpuAudio_Close(i,nextGeneration[i]);
    Command c{Kind::Barrier};
    replyReady.store(false,std::memory_order_relaxed);
    if(Push(c))while(!replyReady.load(std::memory_order_acquire)) { VITA_CPU_SCOPE("audio-drain-ack-wait"); sceKernelWaitSema(ack,1,nullptr); }
    static Pcm discarded;for(auto& s:shared)while(s.pcm.Pop(&discarded)){}
}
void VitaCpuAudio_Stop(){
    if(worker<0)return;
    VitaCpuAudio_Drain();stopping.store(true,std::memory_order_release);sceKernelSignalSema(wake,1);
    { VITA_CPU_SCOPE("audio-worker-join-wait"); sceKernelWaitThreadEnd(worker,nullptr,nullptr); }sceKernelDeleteThread(worker);
    sceKernelDeleteSema(wake);sceKernelDeleteSema(ack);worker=wake=ack=-1;
}
VitaCpuAudioStatus VitaCpuAudio_ReadStatus(){
    VitaCpuAudioStatus result{decoded.load(),pendingReads.load(),failures.load(),0,0,highWater.load(),underruns.load()};
    for(const auto& s:shared){result.queuedBlocks+=s.pcm.Count();result.activeStreams+=s.active.load()!=0;}
    return result;
}
void VitaCpuAudio_ReportUnderrun(uint32_t channel){VitaCpuProfile_Counter("audio-underrun",channel,underruns.fetch_add(1)+1);}
}
#endif
