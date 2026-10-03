#include <vita/vita_cpu_render.h>
#if KISAK_VITA_CPU_RUNTIME
#include <vita/vita_cpu_profile.h>
#include <vita/vita_cpu_source_service.h>
#include <vita/vita_cpu_queue.h>
#include <vita/vita_viewmodel_packet.h>
#include <vita/vita_normal_response_cache.h>
#include <vita/vita_viewmodel_light_cache.h>
#include <vita/vita_viewmodel_vertices.h>
#include <vita/vita_render_pipeline.h>
#include <vita/vita_presented_frame_rate.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/kernel/threadmgr/semaphore.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <memory>
#include <utility>

namespace kisak::vita {
namespace {
constexpr size_t kSlotBytes=6*1024*1024;
constexpr size_t Align(size_t size){return (size+15)&~size_t(15);}
struct Header { void (*execute)(const void*); uint32_t bytes; };
struct Snapshot {
    void* allocation=nullptr;
    uint8_t* storage=nullptr;
    size_t used=0;
    uint64_t atUs=0,publishedAtUs=0;
    uint32_t generation=0,sequence=0;
    bool gameplay=false,diagnostic=false;
    uint32_t renderProbePhase=0;
    uint64_t preparationUs=0,serverUs=0,clientUs=0,audioUs=0;
    uint32_t visibilityUs=0,visibleSurfaces=0,submittedSurfaces=0,grassCandidates=0,grassOmissions=0,ticks=0,backlogMs=0;
};
std::array<Snapshot,3> slots{};
GfxDrawLightBounds pendingLightBounds{}; // Source-owner only; copied with each draw.
#if KISAK_VITA_CPU_TEST
int allocationFailureCountdown=-1;
#endif
VitaCpuTripleOwnership ownership;
VitaCpuQueue<VitaCpuRenderStatus,32> completions;
const GfxBackendApi* native=nullptr;
SceUID renderThread=-1,wake=-1,ack=-1;
SceUID sourceThread=-1;
std::atomic<bool> running{false},stop{false},requestReady{false};
std::atomic<uint64_t> failures{0};
struct StallRequest {uint32_t us=0;VitaCpuContext origin{};};
VitaCpuQueue<StallRequest,8> stallRequests;
void (*requestFunction)(void*)=nullptr;
void* requestArgument=nullptr;
VitaCpuContext requestContext{};
bool recording=false,recordingFailed=false,gameplay=false,diagnostic=false;
uint32_t renderProbePhase=0;
uint32_t generation=0,sequence=0;
uint64_t publications=0,replacements=0,highWater=0;
VitaCpuRenderStatus latest{};
VitaPresentedFrameRate presentedFrameRate; // Render-owner only.
VitaPresentedFrameRate completedRenderRate; // Separate test rate, never playable FPS.
uint32_t completedRatePhase=UINT32_MAX;
// Renderer-owned conversion storage, grown only by loading transactions.
std::unique_ptr<GfxDrawVertex[]> viewModelVertices;
std::unique_ptr<GfxVitaViewmodelVertex[]> packedViewModelVertices;
VitaViewModelVertexConverter viewModelVertexConverter;
uint32_t viewModelCapacity=0;
bool weaponVertexCandidate=false;
uint32_t weaponVertexUpdates=0,weaponVertexCount=0;
NormalResponseCache<> viewModelLightCache;
VitaViewModelLightCache persistentViewModelLightCache;
bool weaponPipelineCandidate=false; // renderer-owned; never read source renderProbePhase here
uint32_t weaponCacheUpdates=0,weaponCacheCandidateUpdates=0,weaponCacheHits=0,weaponCacheMisses=0,weaponCacheResets=0;
uint32_t lightGeneration=UINT32_MAX,lightSequence=UINT32_MAX,lightEntity=UINT32_MAX;
bool rejectSnapshot=false;
std::array<uint32_t,128> cubeHandles{}; // immutable-resource catalog, source owned
void Fail(const char* name) {
    failures.fetch_add(1,std::memory_order_relaxed);
    VitaCpuProfile_Counter(name,failures.load());
    VitaCpuProfile_Invalidate(name);
}
void Wake() { if(wake>=0)sceKernelSignalSema(wake,1); }
Snapshot& WriteSlot(){return slots[ownership.WriteSlot()];}
void* Reserve(size_t bytes,void (*execute)(const void*)) {
    if(!recording || !execute || bytes>kSlotBytes-Align(sizeof(Header))) {
        recordingFailed=true;
        Fail("snapshot-command-invalid");return nullptr;
    }
    auto& slot=WriteSlot();
    const size_t total=Align(sizeof(Header))+Align(bytes);
    if(slot.used>kSlotBytes-total) {recordingFailed=true;Fail("snapshot-capacity-exhausted");return nullptr;}
    auto* h=new (slot.storage+slot.used) Header{execute,uint32_t(total)};
    slot.used+=total;
    return reinterpret_cast<uint8_t*>(h)+Align(sizeof(Header));
}
bool RunStall() {
    StallRequest request;
    if(!stallRequests.Pop(&request))return false;
    const auto saved=VitaCpuProfile_Context();
    VitaCpuProfile_SetSequence(request.origin.generation,request.origin.sequence);
    {VITA_CPU_SCOPE("injected-renderer-stall");sceKernelDelayThread(request.us);}
    VitaCpuProfile_SetSequence(saved.generation,saved.sequence);
    return true;
}
void ExecuteSnapshotCommands(const Snapshot& snapshot);
bool Consume() {
    if(!ownership.Acquire())return false;
    const auto& snapshot=slots[ownership.ReadSlot()];
    VitaCpuProfile_SetSequence(snapshot.generation,snapshot.sequence);
    const uint64_t start=VitaCpuNow();
    RunStall();
    VitaCpuRenderStatus result{};
    result.generation=snapshot.generation;result.sequence=snapshot.sequence;
    result.acquiredAgeUs=VitaCpuNow()-snapshot.atUs;result.bytes=uint32_t(snapshot.used);
    result.diagnostic=snapshot.diagnostic || snapshot.renderProbePhase!=0;
    result.renderProbePhase=snapshot.renderProbePhase;
    result.preparationUs=snapshot.preparationUs;result.serverUs=snapshot.serverUs;
    result.clientUs=snapshot.clientUs;result.audioUs=snapshot.audioUs;
    result.visibilityUs=snapshot.visibilityUs;result.visibleSurfaces=snapshot.visibleSurfaces;result.submittedSurfaces=snapshot.submittedSurfaces;
    result.grassCandidates=snapshot.grassCandidates;result.grassOmissions=snapshot.grassOmissions;
    result.ticks=snapshot.ticks;result.backlogMs=snapshot.backlogMs;
    {
        VITA_CPU_SCOPE("render-snapshot");
        GfxVita_SetCpuDiagnostic(snapshot.diagnostic);
        GfxVita_SetGameplayFrame(snapshot.gameplay);
        GfxVita_BeginRenderProbe(snapshot.renderProbePhase);
        const auto pipeline=VitaRenderPipeline::Select(snapshot.gameplay,snapshot.diagnostic,snapshot.renderProbePhase);
        weaponPipelineCandidate=pipeline.prepareWeapon;
        weaponVertexCandidate=pipeline.packWeapon;
        weaponVertexUpdates=weaponVertexCount=0;
        if(!weaponPipelineCandidate)persistentViewModelLightCache.Invalidate();
        weaponCacheUpdates=weaponCacheCandidateUpdates=weaponCacheHits=weaponCacheMisses=weaponCacheResets=0;
        const auto preparationStart=VitaCpuNow();
        { VITA_CPU_SCOPE("render-frame-setup");
        native->BeginFrame();
        GfxVita_RenderPassBoundary(VitaRenderPass::Setup);
        }
        { VITA_CPU_SCOPE("render-command-execution");
        ExecuteSnapshotCommands(snapshot);
        }
        result.renderPreparationUs=VitaCpuNow()-preparationStart;
        if(pipeline.ordinary) {
            VitaCpuProfile_Counter("render-pipeline-weapon-prep",weaponCacheCandidateUpdates,weaponCacheUpdates);
            VitaCpuProfile_Counter("render-pipeline-weapon-pack",weaponVertexUpdates,weaponVertexCount);
            VitaCpuProfile_Counter("render-pipeline-weapon-bytes",uint64_t(weaponVertexCount)*sizeof(GfxVitaViewmodelVertex),viewModelVertexConverter.Ready());
        }
        if((snapshot.renderProbePhase&~0xffu)==kVitaWeaponPipelineComparison || (VitaWeaponVertexMode(snapshot.renderProbePhase) || VitaWeaponSpecularMode(snapshot.renderProbePhase) || VitaWorldAlphaMode(snapshot.renderProbePhase) || VitaCurrentDrawMode(snapshot.renderProbePhase) || VitaPostFilmMode(snapshot.renderProbePhase) || VitaWorldLifetimeMode(snapshot.renderProbePhase) || VitaLightmapLayoutMode(snapshot.renderProbePhase) || VitaWorldMathMode(snapshot.renderProbePhase) || VitaWorldBasisMode(snapshot.renderProbePhase) || VitaWorldGlossMode(snapshot.renderProbePhase) || VitaWorldComponentsMode(snapshot.renderProbePhase) || VitaGraphicsProfileMode(snapshot.renderProbePhase) || VitaCompositionMode(snapshot.renderProbePhase) || VitaPassIsolationMode(snapshot.renderProbePhase) || VitaWorldPrecisionMode(snapshot.renderProbePhase) || VitaWorldScalingMode(snapshot.renderProbePhase))) {
            VitaCpuProfile_Counter("render-probe-weapon-cache-use",weaponCacheCandidateUpdates,weaponCacheUpdates);
            VitaCpuProfile_Counter("render-probe-weapon-cache-hits",weaponCacheHits,weaponCacheMisses);
            VitaCpuProfile_Counter("render-probe-weapon-cache-resets",weaponCacheResets,sizeof(persistentViewModelLightCache));
        }
        if((VitaWeaponVertexMode(snapshot.renderProbePhase) || VitaWeaponSpecularMode(snapshot.renderProbePhase) || VitaWorldAlphaMode(snapshot.renderProbePhase) || VitaCurrentDrawMode(snapshot.renderProbePhase) || VitaPostFilmMode(snapshot.renderProbePhase) || VitaWorldLifetimeMode(snapshot.renderProbePhase) || VitaLightmapLayoutMode(snapshot.renderProbePhase) || VitaWorldMathMode(snapshot.renderProbePhase) || VitaWorldBasisMode(snapshot.renderProbePhase) || VitaWorldGlossMode(snapshot.renderProbePhase) || VitaWorldComponentsMode(snapshot.renderProbePhase) || VitaGraphicsProfileMode(snapshot.renderProbePhase) || VitaCompositionMode(snapshot.renderProbePhase) || VitaPassIsolationMode(snapshot.renderProbePhase) || VitaWorldPrecisionMode(snapshot.renderProbePhase) || VitaWorldScalingMode(snapshot.renderProbePhase))) {
            VitaCpuProfile_Counter("render-probe-weapon-vertices",weaponVertexUpdates,weaponCacheUpdates);
            VitaCpuProfile_Counter("render-probe-weapon-vertex-count",weaponVertexCount,
                weaponVertexCandidate?uint64_t(weaponVertexCount)*sizeof(GfxVitaViewmodelVertex):uint64_t(weaponVertexCount)*sizeof(GfxDrawVertex));
            VitaCpuProfile_Counter("render-probe-weapon-vertex-table",viewModelVertexConverter.Ready(),sizeof(viewModelVertexConverter));
        }
        // Source preparation already includes snapshot construction. Add only
        // time after publication: queueing and render preparation, excluding
        // final presentation. This is a latency, never CPU utilization.
        result.preparationLatencyUs=snapshot.preparationUs+VitaCpuNow()-snapshot.publishedAtUs;
        const auto presentStart=VitaCpuNow();
        native->EndFrame();
        result.presentationUs=VitaCpuNow()-presentStart;
        GfxVita_GetFrameCounters(&result.counters);
    }
    result.submitUs=VitaCpuNow()-start;
    result.presentUs=VitaCpuNow();
    result.ageUs=result.presentUs-snapshot.atUs;
    static uint64_t consumed=0;result.presented=++consumed;
    if(completedRatePhase!=snapshot.renderProbePhase) {
        completedRenderRate.Reset();completedRatePhase=snapshot.renderProbePhase;
    }
    const auto completedRate=completedRenderRate.Observe(result.presentUs,snapshot.generation,snapshot.gameplay);
    result.completedRenderRate10=completedRate.timesTen;result.completedRenderRateValid=completedRate.valid;
    const auto rate=presentedFrameRate.Observe(result.presentUs,snapshot.generation,
        snapshot.gameplay && !result.diagnostic && (result.counters.viewportSeen&1u));
    result.playableRate10=rate.timesTen;result.playableRateIntervals=rate.intervals;
    result.playableRateWindowUs=rate.durationUs;result.playableFrameIntervalUs=rate.intervalUs;
    result.playableRateValid=rate.valid;
    if(rate.valid) VitaCpuProfile_Counter("render-playable-rate10",rate.timesTen,rate.durationUs);
    VitaCpuProfile_Counter(snapshot.diagnostic?"diagnostic-snapshot":"presented-snapshot",result.ageUs,result.submitUs);
    VitaCpuProfile_Counter("snapshot-bytes",snapshot.used,result.counters.swapWaitUsec);
    VitaCpuProfile_Counter("render-preparation-us",result.renderPreparationUs,result.presentationUs);
    VitaCpuProfile_Counter("cpu-preparation-latency-us",result.preparationLatencyUs);
    if(!completions.Push(result))Fail("render-completion-overflow");
    return true;
}
int RenderMain(SceSize,void*) {
    VitaCpuRegisterThread(VitaCpuRole::Render,SCE_KERNEL_CPU_MASK_USER_1);
    for(;;) {
        bool any=Consume();
        if(!any)any=RunStall();
        if(requestReady.load(std::memory_order_acquire)) {
            // The source is waiting and cannot publish another frame. Retire
            // its latest publication before changing or deleting any resource.
            while(Consume() || RunStall()){}
            VitaCpuProfile_SetSequence(requestContext.generation,requestContext.sequence);
            requestFunction(requestArgument);
            requestReady.store(false,std::memory_order_release);
            sceKernelSignalSema(ack,1);any=true;
        }
        if(stop.load(std::memory_order_acquire))break;
        if(!any){SceUInt timeout=10000; VITA_CPU_SCOPE("render-idle-wait"); sceKernelWaitSema(wake,1,&timeout);}
    }
    VitaCpuProfile_FlushThread();
    return 0;
}
template<class F> bool Sync(F&& function) {
    return VitaCpuRender_Call([](void* data){(*static_cast<std::remove_reference_t<F>*>(data))();},&function);
}
bool Init(uint32_t width,uint32_t height,size_t transientBytes,size_t gpuBytes) {
    if(running.load())return false;
    sourceThread=sceKernelGetThreadId();
    stop.store(false);requestReady.store(false);ownership.Reset();completions.Reset();stallRequests.Reset();
    recording=recordingFailed=false;renderProbePhase=0;
    for(auto& slot:slots) {
#if KISAK_VITA_CPU_TEST
        if(allocationFailureCountdown==0){slot.allocation=nullptr;allocationFailureCountdown=-1;}
        else {
            if(allocationFailureCountdown>0)--allocationFailureCountdown;
            slot.allocation=std::malloc(kSlotBytes+15);
        }
#else
        slot.allocation=std::malloc(kSlotBytes+15);
#endif
        // ARM newlib only needs to guarantee max_align_t, which may be eight
        // bytes. Snapshot records explicitly require a sixteen-byte boundary.
        slot.storage=slot.allocation?reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(slot.allocation)+15)&~uintptr_t(15)):nullptr;
        if(!slot.storage) {
            for(auto& allocated:slots){std::free(allocated.allocation);allocated.allocation=nullptr;allocated.storage=nullptr;}
            Fail("snapshot-allocation-failed");return false;
        }
    }
    wake=sceKernelCreateSema("cod4-render-wake",0,0,1,nullptr);
    ack=sceKernelCreateSema("cod4-render-ack",0,0,1,nullptr);
    renderThread=sceKernelCreateThread("cod4-render-owner",RenderMain,0x10000100,512*1024,0,SCE_KERNEL_CPU_MASK_USER_1,nullptr);
    if(wake<0 || ack<0 || renderThread<0 || sceKernelStartThread(renderThread,0,nullptr)<0) {
        if(renderThread>=0)sceKernelDeleteThread(renderThread);
        if(wake>=0)sceKernelDeleteSema(wake);
        if(ack>=0)sceKernelDeleteSema(ack);
        renderThread=wake=ack=-1;
        for(auto& slot:slots){std::free(slot.allocation);slot.allocation=nullptr;slot.storage=nullptr;}
        Fail("render-thread-start-failed");return false;
    }
    running.store(true,std::memory_order_release);
    bool result=false;
    VitaCpuProfile_Startup("native-renderer-init-enter",0);
    Sync([&]{
        VitaCpuProfile_Startup("native-renderer-owner-enter",0);
        result=native->Init(width,height,transientBytes,gpuBytes);
    });
    VitaCpuProfile_Startup("native-renderer-init-return",result?0:-1);
    if(!result) {
        Sync([]{native->Shutdown();});
        Fail("renderer-initialization-failed");
        stop.store(true,std::memory_order_release);Wake();
        { VITA_CPU_SCOPE("render-thread-join-wait"); sceKernelWaitThreadEnd(renderThread,nullptr,nullptr); }
        sceKernelDeleteThread(renderThread);sceKernelDeleteSema(wake);sceKernelDeleteSema(ack);
        renderThread=wake=ack=-1;running.store(false);
        for(auto& slot:slots){std::free(slot.allocation);slot.allocation=nullptr;slot.storage=nullptr;}
    }
    return result;
}
void Shutdown() {
    if(!running.load())return;
    if(recording){recording=false;Fail("shutdown-during-snapshot");}
    VitaCpuRender_Drain();
    Sync([]{
        viewModelVertices.reset();packedViewModelVertices.reset();viewModelCapacity=0;weaponVertexCandidate=false;
        lightGeneration=lightSequence=lightEntity=UINT32_MAX;
        persistentViewModelLightCache.Invalidate();weaponPipelineCandidate=false;
        presentedFrameRate.Reset();
        completedRenderRate.Reset();completedRatePhase=UINT32_MAX;
        native->Shutdown();
    });
    stop.store(true,std::memory_order_release);Wake();
    { VITA_CPU_SCOPE("render-thread-join-wait"); sceKernelWaitThreadEnd(renderThread,nullptr,nullptr); }
    sceKernelDeleteThread(renderThread);sceKernelDeleteSema(wake);sceKernelDeleteSema(ack);
    renderThread=wake=ack=-1;running.store(false);
    for(auto& slot:slots){std::free(slot.allocation);slot.allocation=nullptr;slot.storage=nullptr;}
}
void BeginFrame() {
    pendingLightBounds={};
    VitaCpuRender_Read();
    if(recording){Fail("nested-snapshot");return;}
    auto& slot=WriteSlot();slot.used=0;slot.atUs=VitaCpuNow();
    slot.generation=generation;slot.sequence=sequence;
    slot.gameplay=gameplay;slot.diagnostic=diagnostic && gameplay;
    slot.renderProbePhase=gameplay?renderProbePhase:0;
    recording=true;recordingFailed=false;
}
void EndFrame() {
    if(!recording){Fail("end-without-snapshot");return;}
    recording=false;highWater=std::max(highWater,uint64_t(WriteSlot().used));
    if(recordingFailed){VitaCpuProfile_Counter("snapshot-rejected",sequence,WriteSlot().used);return;}
    WriteSlot().publishedAtUs=VitaCpuNow();
    ++publications;if(ownership.Publish())++replacements;
    VitaCpuProfile_Counter("snapshot-published",publications,replacements);Wake();
}
// All arrays below are deep copies in the current producer-owned arena.
struct Draw {
    uint32_t vertices=0,indices=0;
    GfxTextureHandle texture{},primary{},secondary{},reflection{};
    uint8_t flags=0;
    bool lit=false;
};
void ExecuteDraw(const void* pointer) {
    const auto& r=*static_cast<const Draw*>(pointer);
    const auto* v=reinterpret_cast<const GfxDrawVertex*>(static_cast<const uint8_t*>(pointer)+Align(sizeof(Draw)));
    const auto* i=reinterpret_cast<const uint16_t*>(v+r.vertices);
    if(r.lit)native->DrawIndexedLightmapped(v,r.vertices,i,r.indices,r.texture,r.primary,r.secondary,r.reflection,r.flags);
    else native->DrawIndexed(v,r.vertices,i,r.indices);
}

// Renderer-owned scratch, allocated before startup, never shared with source.
// Only adjacent plain triangle draws may merge. Every state/control command,
// lit draw, invalid range or capacity boundary flushes the batch in order.
alignas(16) std::array<GfxDrawVertex,1024> drawBatchVertices{};
std::array<uint16_t,1536> drawBatchIndices{};
void ExecuteSnapshotCommands(const Snapshot& snapshot) {
    rejectSnapshot=false;
    uint32_t vertices=0,indices=0,inputs=0,outputs=0;
    auto flush=[&]{
        if(!indices)return;
        native->DrawIndexed(drawBatchVertices.data(),vertices,drawBatchIndices.data(),indices);
        ++outputs;vertices=indices=0;
    };
    size_t offset=0;
    while(offset<snapshot.used) {
        const auto* h=reinterpret_cast<const Header*>(snapshot.storage+offset);
        if(h->bytes<Align(sizeof(Header)) || h->bytes>snapshot.used-offset) {
            flush();Fail("snapshot-command-corrupt");break;
        }
        const auto* payload=snapshot.storage+offset+Align(sizeof(Header));
        bool merged=false;
        if(h->execute==ExecuteDraw) {
            if(h->bytes<Align(sizeof(Header))+Align(sizeof(Draw))) {
                flush();Fail("snapshot-draw-corrupt");break;
            }
            const auto& d=*reinterpret_cast<const Draw*>(payload);
            const size_t payloadBytes=h->bytes-Align(sizeof(Header))-Align(sizeof(Draw));
            if(d.vertices>payloadBytes/sizeof(GfxDrawVertex)
                || d.indices>(payloadBytes-size_t(d.vertices)*sizeof(GfxDrawVertex))/sizeof(uint16_t)) {
                flush();Fail("snapshot-draw-corrupt");break;
            }
            const auto* v=reinterpret_cast<const GfxDrawVertex*>(payload+Align(sizeof(Draw)));
            const auto* i=reinterpret_cast<const uint16_t*>(v+d.vertices);
            bool valid=!d.lit && d.vertices>0 && d.vertices<=drawBatchVertices.size()
                && d.indices>=3 && d.indices<=drawBatchIndices.size() && d.indices%3==0;
            if(valid)for(uint32_t n=0;n<d.indices;++n)if(i[n]>=d.vertices){valid=false;break;}
            if(valid) {
                if(vertices+d.vertices>drawBatchVertices.size() || indices+d.indices>drawBatchIndices.size())flush();
                std::memcpy(drawBatchVertices.data()+vertices,v,size_t(d.vertices)*sizeof(*v));
                for(uint32_t n=0;n<d.indices;++n)drawBatchIndices[indices+n]=uint16_t(i[n]+vertices);
                vertices+=d.vertices;indices+=d.indices;++inputs;merged=true;
            }
        }
        if(!merged){flush();h->execute(payload);if(rejectSnapshot)break;}
        offset+=h->bytes;
    }
    flush();
    VitaCpuProfile_Counter("render-unlit-draw-batch",inputs,outputs);
}

void CopyDraw(const Draw& draw,const GfxDrawVertex* vertices,const uint16_t* indices) {
    if(draw.vertices>kSlotBytes/sizeof(*vertices) || draw.indices>kSlotBytes/sizeof(*indices)
        || (!vertices && draw.vertices) || (!indices && draw.indices)) {
        recordingFailed=true;
        Fail("invalid-draw-payload");return;
    }
    const size_t vertexBytes=size_t(draw.vertices)*sizeof(*vertices),indexBytes=size_t(draw.indices)*sizeof(*indices);
    auto* dest=static_cast<uint8_t*>(Reserve(Align(sizeof(Draw))+vertexBytes+indexBytes,ExecuteDraw));
    if(!dest)return;
    std::memcpy(dest,&draw,sizeof(draw));
    if(vertexBytes)std::memcpy(dest+Align(sizeof(Draw)),vertices,vertexBytes);
    if(indexBytes)std::memcpy(dest+Align(sizeof(Draw))+vertexBytes,indices,indexBytes);
}
void DrawIndexed(const GfxDrawVertex* v,uint32_t vc,const uint16_t* i,uint32_t ic){Draw d{};d.vertices=vc;d.indices=ic;CopyDraw(d,v,i);}
void DrawIndexedLightmapped(const GfxDrawVertex* v,uint32_t vc,const uint16_t* i,uint32_t ic,GfxTextureHandle t,GfxTextureHandle p,GfxTextureHandle s,GfxTextureHandle r,uint8_t flags){
    Draw d{};d.vertices=vc;d.indices=ic;d.texture=t;d.primary=p;d.secondary=s;d.reflection=r;d.flags=flags;d.lit=true;CopyDraw(d,v,i);
}
bool UpdateDynamicVertexBuffer(GfxBufferHandle buffer,const GfxDrawVertex* vertices,uint32_t count) {
    VITA_CPU_SCOPE("source-pose-copy");
    struct Update {GfxBufferHandle buffer;uint32_t count;};
    if(count>kSlotBytes/sizeof(*vertices) || (!vertices && count)){recordingFailed=true;Fail("invalid-pose-payload");return false;}
    auto* dest=static_cast<uint8_t*>(Reserve(Align(sizeof(Update))+size_t(count)*sizeof(*vertices),[](const void* p){
        const auto& u=*static_cast<const Update*>(p);
        const auto* v=reinterpret_cast<const GfxDrawVertex*>(static_cast<const uint8_t*>(p)+Align(sizeof(Update)));
        VITA_CPU_SCOPE("render-pose-upload");
        if(!native->UpdateDynamicVertexBuffer(u.buffer,v,u.count))Fail("pose-upload-failed");
    }));
    if(!dest)return false;
    const Update value{buffer,count};std::memcpy(dest,&value,sizeof(value));
    if(count)std::memcpy(dest+Align(sizeof(Update)),vertices,size_t(count)*sizeof(*vertices));
    return true;
}
GfxTextureHandle CreateTexture2D(const uint8_t* p,size_t bytes,uint32_t w,uint32_t h,uint32_t levels,uint32_t format,uint32_t flags) {
    GfxTextureHandle result{};Sync([&]{result=native->CreateTexture2D(p,bytes,w,h,levels,format,flags);});
    if(result.value && (flags&4)) {
        auto found=std::find(cubeHandles.begin(),cubeHandles.end(),0u);
        if(found==cubeHandles.end())Fail("cube-catalog-full");else *found=result.value;
    }
    return result;
}
void DestroyTexture(GfxTextureHandle t){Sync([&]{native->DestroyTexture(t);});for(auto& value:cubeHandles)if(value==t.value)value=0;}
GfxBufferHandle CreateStaticVertexBuffer(const GfxDrawVertex* v,uint32_t count){GfxBufferHandle result{};Sync([&]{result=native->CreateStaticVertexBuffer(v,count);});return result;}
GfxBufferHandle CreateInstancedModelVertexBuffer(const GfxDrawVertex* v,uint32_t count){GfxBufferHandle result{};Sync([&]{result=native->CreateInstancedModelVertexBuffer(v,count);});return result;}
GfxBufferHandle CreateStaticVertexBufferRemapped(const GfxDrawVertex* v,uint32_t vc,const uint32_t* i,uint32_t count){GfxBufferHandle result{};Sync([&]{result=native->CreateStaticVertexBufferRemapped(v,vc,i,count);});return result;}
GfxBufferHandle CreateStaticIndexBuffer(const uint16_t* i,uint32_t count){GfxBufferHandle result{};Sync([&]{result=native->CreateStaticIndexBuffer(i,count);});return result;}
GfxBufferHandle CreateStaticInstanceBuffer(const GfxStaticModelInstance* i,uint32_t count){GfxBufferHandle result{};Sync([&]{result=native->CreateStaticInstanceBuffer(i,count);});return result;}
GfxBufferHandle CreateDynamicVertexBuffer(uint32_t count){
    GfxBufferHandle result{};
    Sync([&]{
        if(count>kSlotBytes/sizeof(VitaPackedSkinVertex)){Fail("viewmodel-scratch-capacity-invalid");return;}
        if(count>viewModelCapacity){
            std::unique_ptr<GfxDrawVertex[]> replacement;
            std::unique_ptr<GfxVitaViewmodelVertex[]> packedReplacement;
#if KISAK_VITA_CPU_TEST
            if(allocationFailureCountdown==0)allocationFailureCountdown=-1;
            else
#endif
            {
#if KISAK_VITA_CPU_TEST
                if(allocationFailureCountdown>0)--allocationFailureCountdown;
#endif
                replacement.reset(new(std::nothrow) GfxDrawVertex[count]);
            }
            if(!replacement){Fail("viewmodel-scratch-allocation-failed");return;}
            #if KISAK_VITA_CPU_TEST
            if(allocationFailureCountdown==0)allocationFailureCountdown=-1;
            else
#endif
            packedReplacement.reset(new(std::nothrow) GfxVitaViewmodelVertex[count]);
            if(!packedReplacement){Fail("viewmodel-packed-scratch-allocation-failed");return;}
            viewModelVertices=std::move(replacement);packedViewModelVertices=std::move(packedReplacement);viewModelCapacity=count;
            VitaCpuProfile_Counter("render-viewmodel-scratch-bytes",size_t(count)*(sizeof(GfxDrawVertex)+sizeof(GfxVitaViewmodelVertex)));
        }
        viewModelVertexConverter.Prepare();
        result=native->CreateDynamicVertexBuffer(count);
    });
    return result;
}
void DestroyBuffer(GfxBufferHandle b){Sync([&]{native->DestroyBuffer(b);});}

void Clear(float r,float g,float b,float a){struct Value{float r,g,b,a;};VitaCpuRender_Copy(Value{r,g,b,a},+[](const Value& v){native->Clear(v.r,v.g,v.b,v.a);});}
void BindTexture(GfxTextureHandle t){VitaCpuRender_Copy(t,+[](const GfxTextureHandle& v){native->BindTexture(v);});}
void SetSamplerState(uint32_t unit,uint8_t state){struct Value{uint32_t unit;uint8_t state;};VitaCpuRender_Copy(Value{unit,state},+[](const Value& v){native->SetSamplerState(v.unit,v.state);});}
void SetCullMode(GfxCullMode m){VitaCpuRender_Copy(m,+[](const GfxCullMode& v){native->SetCullMode(v);});}
void SetSurfaceMode(GfxSurfaceMode m){VitaCpuRender_Copy(m,+[](const GfxSurfaceMode& v){native->SetSurfaceMode(v);});}
void SetAlphaTestFunc(GfxAlphaTestFunc f){VitaCpuRender_Copy(f,+[](const GfxAlphaTestFunc& v){native->SetAlphaTestFunc(v);});}
void SetFog(bool enabled,float start,float density,const float color[3]) {
    struct Value{bool enabled;float start,density,color[3];};
    const Value v{enabled,start,density,{color?color[0]:0,color?color[1]:0,color?color[2]:0}};
    VitaCpuRender_Copy(v,+[](const Value& r){native->SetFog(r.enabled,r.start,r.density,r.color);});
}
template<class T> struct Optional{bool present;T value;};
#define CPU_STATE(Name,Type) \
void Name(const Type* p){VitaCpuRender_Copy(Optional<Type>{p!=nullptr,p?*p:Type{}},+[](const Optional<Type>& r){native->Name(r.present?&r.value:nullptr);});}
CPU_STATE(SetFrameLighting,GfxFrameLightingState)
CPU_STATE(SetMaterialPass,GfxMaterialPassState)
CPU_STATE(SetShadowPass,GfxShadowPassState)
CPU_STATE(SetEffectBatch,GfxEffectBatchState)
CPU_STATE(ApplyViewEffects,GfxViewEffectState)
#undef CPU_STATE
void SetWorldViewProjection(const float matrix[16],float nearPlane){
    struct Value{float matrix[16],nearPlane;bool present;};Value v{};v.nearPlane=nearPlane;v.present=matrix!=nullptr;
    if(matrix)std::memcpy(v.matrix,matrix,sizeof(v.matrix));
    VitaCpuRender_Copy(v,+[](const Value& r){native->SetWorldViewProjection(r.present?r.matrix:nullptr,r.nearPlane);});
}
void SetDrawLightBounds(const GfxDrawLightBounds* bounds){pendingLightBounds=bounds?*bounds:GfxDrawLightBounds{};}
struct StaticDraw {GfxBufferHandle vb,ib,instances;uint32_t fv,vc,fi,ic,firstInstance,instanceCount;bool lit;GfxDrawLightBounds bounds{};};
bool DrawStaticIndexed(GfxBufferHandle vb,uint32_t fv,uint32_t vc,GfxBufferHandle ib,uint32_t fi,uint32_t ic){
    const auto bounds=std::exchange(pendingLightBounds,GfxDrawLightBounds{});
    return VitaCpuRender_Copy(StaticDraw{vb,ib,{},fv,vc,fi,ic,0,0,false,bounds},+[](const StaticDraw& r){if(native->SetDrawLightBounds)native->SetDrawLightBounds(&r.bounds);if(!native->DrawStaticIndexed(r.vb,r.fv,r.vc,r.ib,r.fi,r.ic))Fail("static-draw-failed");});
}
bool DrawStaticIndexedInstanced(GfxBufferHandle vb,uint32_t fv,uint32_t vc,GfxBufferHandle ib,uint32_t fi,uint32_t ic,GfxBufferHandle instances,uint32_t first,uint32_t count){
    const auto bounds=std::exchange(pendingLightBounds,GfxDrawLightBounds{});
    return VitaCpuRender_Copy(StaticDraw{vb,ib,instances,fv,vc,fi,ic,first,count,false,bounds},+[](const StaticDraw& r){if(native->SetDrawLightBounds)native->SetDrawLightBounds(&r.bounds);if(!native->DrawStaticIndexedInstanced(r.vb,r.fv,r.vc,r.ib,r.fi,r.ic,r.instances,r.firstInstance,r.instanceCount))Fail("instanced-draw-failed");});
}
bool DrawDynamicIndexed(GfxBufferHandle vb,uint32_t fv,uint32_t vc,GfxBufferHandle ib,uint32_t fi,uint32_t ic,bool lit){
    pendingLightBounds={};
    return VitaCpuRender_Copy(StaticDraw{vb,ib,{},fv,vc,fi,ic,0,0,lit},+[](const StaticDraw& r){if(!native->DrawDynamicIndexed(r.vb,r.fv,r.vc,r.ib,r.fi,r.ic,r.lit))Fail("dynamic-draw-failed");});
}
const GfxBackendApi api={Init,Shutdown,BeginFrame,Clear,CreateTexture2D,DestroyTexture,BindTexture,SetSamplerState,SetCullMode,DrawIndexed,DrawIndexedLightmapped,
    SetSurfaceMode,SetAlphaTestFunc,SetFog,SetFrameLighting,SetMaterialPass,SetShadowPass,SetEffectBatch,ApplyViewEffects,
    CreateStaticVertexBuffer,CreateStaticVertexBufferRemapped,CreateStaticIndexBuffer,CreateStaticInstanceBuffer,CreateDynamicVertexBuffer,UpdateDynamicVertexBuffer,DestroyBuffer,
    SetWorldViewProjection,DrawStaticIndexed,DrawStaticIndexedInstanced,DrawDynamicIndexed,EndFrame,CreateInstancedModelVertexBuffer,SetDrawLightBounds};
}
const GfxBackendApi* VitaCpuRender_Backend(const GfxBackendApi* direct){native=direct;return &api;}
#if KISAK_VITA_CPU_TEST
void VitaCpuRender_TestAllocationFailure(int after){allocationFailureCountdown=after;}
#endif
bool VitaCpuRender_ViewModel(GfxBufferHandle buffer,const VitaPackedSkinVertex* vertices,
                            uint32_t count,const VitaViewModelInputs& inputs) {
    VITA_CPU_SCOPE("source-packed-pose-copy");
    struct Update {GfxBufferHandle buffer;uint32_t count;VitaViewModelInputs inputs;};
    static_assert(std::is_trivially_copyable_v<Update>);
    if(!VitaCpuRender_OnSource() || !buffer.value || !count || !vertices
        || count>kSlotBytes/sizeof(*vertices)) {
        recordingFailed=true;Fail("invalid-packed-pose-payload");return false;
    }
    auto* dest=static_cast<uint8_t*>(Reserve(Align(sizeof(Update))+size_t(count)*sizeof(*vertices),[](const void* p){
        const auto& u=*static_cast<const Update*>(p);
        const auto* v=reinterpret_cast<const VitaPackedSkinVertex*>(static_cast<const uint8_t*>(p)+Align(sizeof(Update)));
        if(u.count>viewModelCapacity){rejectSnapshot=true;Fail("viewmodel-scratch-too-small");return;}
        if(!weaponVertexCandidate) {
            VITA_CPU_SCOPE("render-viewmodel-convert");
            for(uint32_t i=0;i<u.count;++i) {
                if(!ConvertSourceVertex(v[i],viewModelVertices[i],u.inputs.projection)) {
                    // Never draw an old buffer after a failed conversion.
                    rejectSnapshot=true;Fail("viewmodel-conversion-failed");return;
                }
            }
        }
        const auto context=VitaCpuProfile_Context();
        if(lightGeneration!=context.generation || lightSequence!=context.sequence || lightEntity!=u.inputs.entity) {
            viewModelLightCache.BeginSample();lightGeneration=context.generation;
            lightSequence=context.sequence;lightEntity=u.inputs.entity;
        }
        viewModelLightCache.hits=viewModelLightCache.misses=0;
        ++weaponCacheUpdates;
        if(weaponPipelineCandidate) {
            ++weaponCacheCandidateUpdates;
            weaponCacheResets+=persistentViewModelLightCache.Begin(context.generation,u.inputs.entity,u.inputs);
        }
        const PreparedModelLight prepared(u.inputs.sample,u.inputs.world);
        const auto resolve=[&](const float* normal,float* response) {
            const auto evaluate=[&](const float* n,float* out){return prepared.Evaluate(n,out);};
            return weaponPipelineCandidate?persistentViewModelLightCache.Resolve(normal,response,evaluate)
                :viewModelLightCache.Resolve(normal,response,evaluate);
        };
        if(weaponVertexCandidate) {
            VITA_CPU_SCOPE("render-viewmodel-fused");
            for(uint32_t i=0;i<u.count;++i)
                if(!viewModelVertexConverter.Convert(v[i],packedViewModelVertices[i],u.inputs.projection,resolve)) {
                    rejectSnapshot=true;Fail("viewmodel-conversion-failed");return;
                }
            ++weaponVertexUpdates;
        } else {
            VITA_CPU_SCOPE("render-viewmodel-lighting");
            for(uint32_t i=0;i<u.count;++i) {
                auto& vertex=viewModelVertices[i];float response[3]{1,1,1};
                if(resolve(vertex.normal,response))
                    for(uint32_t c=0;c<3;++c)vertex.color[c]*=response[c];
            }
        }
        weaponVertexCount+=u.count;
        const auto hits=weaponPipelineCandidate?persistentViewModelLightCache.Hits():viewModelLightCache.hits;
        const auto misses=weaponPipelineCandidate?persistentViewModelLightCache.Misses():viewModelLightCache.misses;
        weaponCacheHits+=hits;weaponCacheMisses+=misses;
        VitaCpuProfile_Counter("render-viewmodel-light-cache",hits,misses);
        {
            VITA_CPU_SCOPE("render-pose-upload");
            if(!(weaponVertexCandidate?GfxVita_UpdatePackedViewmodel(u.buffer,packedViewModelVertices.get(),u.count)
                    :native->UpdateDynamicVertexBuffer(u.buffer,viewModelVertices.get(),u.count))) {
                rejectSnapshot=true;Fail("pose-upload-failed");
            }
        }
    }));
    if(!dest)return false;
    const Update update{buffer,count,inputs};std::memcpy(dest,&update,sizeof(update));
    std::memcpy(dest+Align(sizeof(Update)),vertices,size_t(count)*sizeof(*vertices));
    VitaCpuProfile_Counter("source-packed-pose-bytes",size_t(count)*sizeof(*vertices),size_t(count)*sizeof(GfxDrawVertex));
    return true;
}
bool VitaCpuRender_OnSource(){
    const auto role=VitaCpuCurrentRole();
    const int id=sceKernelGetThreadId();
    if(running.load(std::memory_order_acquire) &&
       !((id==sourceThread && role==VitaCpuRole::Source) || (id==renderThread && role==VitaCpuRole::Render))) {
        Fail("graphics-access-from-unregistered-owner");std::abort();
    }
    return running.load(std::memory_order_acquire) && id==sourceThread;
}
bool VitaCpuRender_Call(void (*function)(void*),void* argument) {
    if(!VitaCpuRender_OnSource()){function(argument);return true;}
    if(recording){recordingFailed=true;Fail("resource-transaction-during-gameplay");return false;}
    VITA_CPU_LOADING("renderer-resource-transaction");
    VitaCpuRender_Read();
    requestFunction=function;requestArgument=argument;requestContext=VitaCpuProfile_Context();
    requestReady.store(true,std::memory_order_release);Wake();
    bool waitFailed=false;
    while(requestReady.load(std::memory_order_acquire)) {
        VitaCpuSource_Service();
        int result;SceUInt timeout=20000;
        { VITA_CPU_SCOPE("source-render-ack-wait"); result=sceKernelWaitSema(ack,1,&timeout); }
        // A service deadline is not a failed resource transaction. Continue
        // borrowing the caller storage until the renderer releases it.
        if(static_cast<uint32_t>(result)==0x80028005u)continue; // SCE_KERNEL_ERROR_WAIT_TIMEOUT
        // The renderer still borrows caller storage until its release. A
        // kernel wait failure cannot authorize returning a dangling pointer.
        if(result<0){
            if(!waitFailed)Fail("render-transaction-wait-failed");
            waitFailed=true; { VITA_CPU_SCOPE("source-render-retry-wait"); sceKernelDelayThread(100); }
        }
    }
    VitaCpuRender_Read();return !waitFailed;
}
bool VitaCpuRender_Emit(const void* value,size_t bytes,void (*execute)(const void*)) {
    if(!recording)return Sync([&]{execute(value);});
    auto* destination=Reserve(bytes,execute);if(!destination)return false;
    if(bytes)std::memcpy(destination,value,bytes);
    return true;
}
void VitaCpuRender_SetGameplay(bool active){gameplay=active;}
void VitaCpuRender_SetSequence(uint32_t gen,uint32_t seq){generation=gen;sequence=seq;}
void VitaCpuRender_SetCpuWork(uint64_t prep,uint64_t server,uint64_t client,uint64_t audio){
    if(!recording)return;
    auto& slot=WriteSlot();slot.preparationUs=prep;slot.serverUs=server;slot.clientUs=client;slot.audioUs=audio;
}
void VitaCpuRender_SetRenderProbe(uint32_t phase){renderProbePhase=VitaRenderProbeValid(phase)?phase:0;}
void VitaCpuRender_SetDiagnostic(bool active){diagnostic=active;}
void VitaCpuRender_SetPresentationMetrics(uint32_t visibilityUs,uint32_t visible,uint32_t submitted,uint32_t candidates,uint32_t omissions,uint32_t ticks,uint32_t backlog) {
    if(!recording)return;
    auto& s=WriteSlot();s.visibilityUs=visibilityUs;s.visibleSurfaces=visible;s.submittedSurfaces=submitted;
    s.grassCandidates=candidates;s.grassOmissions=omissions;s.ticks=ticks;s.backlogMs=backlog;
}
void VitaCpuRender_Stall(uint32_t us){
    if(!running.load() || !us){Fail("renderer-stall-unavailable");return;}
    const StallRequest request{std::min(us,1000000u),VitaCpuProfile_Context()};
    bool reported=false;
    while(!stallRequests.Push(request)) {
        if(!reported){Fail("renderer-control-backpressure");reported=true;}
        Wake(); { VITA_CPU_SCOPE("source-render-control-wait"); sceKernelDelayThread(100); }
    }
    Wake();
}
void VitaCpuRender_Drain(){if(running.load())Sync([]{GfxVita_WaitIdle();});}
VitaCpuRenderStatus VitaCpuRender_Read(){
    VitaCpuRenderStatus result;while(completions.Pop(&result))latest=result;
    latest.published=publications;latest.replaced=replacements;latest.snapshotHighWater=highWater;latest.failures=failures.load();
    return latest;
}
bool VitaCpuRender_IsCube(GfxTextureHandle texture){return texture.value && std::find(cubeHandles.begin(),cubeHandles.end(),texture.value)!=cubeHandles.end();}
}
#endif
