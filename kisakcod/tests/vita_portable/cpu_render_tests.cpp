#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <vita/vita_cpu_render.h>
#include <vita/vita_cpu_profile.h>
#include <vita/vita_cpu_source_service.h>
#include <vita/vita_viewmodel_packet.h>
#include <vita/vita_viewmodel_vertices.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <atomic>
#include <vector>
#include <array>
#include <cstring>
#include <cstdio>
#include <limits>
#include <cstdlib>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
using namespace kisak::vita;
namespace kisak::vita {void VitaCpuRender_TestAllocationFailure(int);}
void CpuTestFailNextSourceWait();
namespace {
std::atomic<unsigned> frames{0},draws{0},destroyed{0};
std::atomic<float> lastValue{0};
bool initResult=true,captureGeometry=false;
unsigned capturedDraws=0,invalidDraws=0;
uint32_t boundTexture=0;
std::vector<GfxDrawVertex> capturedTriangles;
std::vector<uint32_t> capturedTextures;
std::vector<GfxDrawVertex> uploadedPose;
unsigned poseUploads=0,poseAfterCommands=0;
GfxDrawLightBounds observedBounds{};
std::vector<GfxDrawLightBounds> staticBounds;
std::vector<GfxVitaViewmodelVertex> uploadedPackedPose;
bool packedUploadFails=false;
GfxVitaViewmodelVertex PackExpected(const GfxDrawVertex& source) {
    GfxVitaViewmodelVertex dest{};
    std::memcpy(dest.position,source.position,sizeof(dest.position));
    std::memcpy(dest.texCoord,source.texCoord,sizeof(dest.texCoord));
    std::memcpy(dest.worldPosition,source.worldPosition,sizeof(dest.worldPosition));
    for(unsigned c=0;c<3;++c){dest.normal[c]=PackVertexSnorm(source.normal[c]);dest.tangent[c]=PackVertexSnorm(source.tangent[c]);}
    for(unsigned c=0;c<4;++c)dest.color[c]=PackVertexUnorm8(source.color[c]);
    return dest;
}
GfxBufferHandle CreatePose(uint32_t count){assert(VitaCpuCurrentRole()==VitaCpuRole::Render);return {count};}
bool UploadPose(GfxBufferHandle,const GfxDrawVertex* vertices,uint32_t count){
    assert(VitaCpuCurrentRole()==VitaCpuRole::Render);
    uploadedPose.assign(vertices,vertices+count);
    uploadedPackedPose.clear();for(unsigned i=0;i<count;++i)uploadedPackedPose.push_back(PackExpected(vertices[i]));
    ++poseUploads;return true;
}
void Bind(GfxTextureHandle h){assert(VitaCpuCurrentRole()==VitaCpuRole::Render);boundTexture=h.value;}

void CheckRender(){assert(VitaCpuCurrentRole()==VitaCpuRole::Render);}
bool Init(uint32_t,uint32_t,size_t,size_t){CheckRender();return initResult;}
void Shutdown(){CheckRender();}
void Begin(){CheckRender();}
void End(){CheckRender();++frames;}
void Draw(const GfxDrawVertex* vertices,uint32_t count,const uint16_t* indices,uint32_t indicesCount) {
    assert((reinterpret_cast<uintptr_t>(vertices)&15)==0);
    CheckRender();
    if(captureGeometry) {
        ++capturedDraws;
        for(uint32_t i=0;i<indicesCount;++i)if(indices[i]>=count){++invalidDraws;return;}
        for(uint32_t i=0;i<indicesCount-indicesCount%3;++i){capturedTriangles.push_back(vertices[indices[i]]);capturedTextures.push_back(boundTexture);}
        return;
    }
    assert(count==64 && indicesCount==3);assert(indices[0]==0 && indices[1]==1 && indices[2]==2);
    const float value=vertices[0].position[0];
    for(unsigned i=0;i<count;++i)assert(vertices[i].position[0]==value);
    lastValue.store(value);++draws;
}
void Destroy(GfxBufferHandle){CheckRender();++destroyed;}
void LightBounds(const GfxDrawLightBounds* b){CheckRender();observedBounds=b?*b:GfxDrawLightBounds{};}
bool Static(GfxBufferHandle,uint32_t,uint32_t,GfxBufferHandle,uint32_t,uint32_t){CheckRender();staticBounds.push_back(observedBounds);return true;}
bool Instanced(GfxBufferHandle v,uint32_t a,uint32_t b,GfxBufferHandle i,uint32_t c,uint32_t d,GfxBufferHandle,uint32_t,uint32_t){return Static(v,a,b,i,c,d);}
}
bool GfxVita_UpdatePackedViewmodel(GfxBufferHandle,const GfxVitaViewmodelVertex* vertices,uint32_t count) {
    CheckRender();if(packedUploadFails)return false;
    uploadedPose.clear();
    uploadedPackedPose.assign(vertices,vertices+count);++poseUploads;return true;
}
void GfxVita_SetGameplayFrame(bool){CheckRender();}
bool GfxVita_GetFrameCounters(GfxVitaFrameCounters* counters){CheckRender();counters->frameNumber=frames.load();return true;}
void GfxVita_WaitIdle(){CheckRender();}
void GfxVita_SetCpuDiagnostic(bool){CheckRender();}
static uint32_t observedProbePhase=0;
void GfxVita_BeginRenderProbe(uint32_t phase){CheckRender();observedProbePhase=phase;}
void GfxVita_RenderPassBoundary(VitaRenderPass){CheckRender();}
int main() {
#ifdef _MSC_VER
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    VitaCpuRegisterThread(VitaCpuRole::Source);
    GfxBackendApi native{};native.Init=Init;native.Shutdown=Shutdown;native.BeginFrame=Begin;native.EndFrame=End;native.DrawIndexed=Draw;native.DestroyBuffer=Destroy;native.BindTexture=Bind;
    native.CreateDynamicVertexBuffer=CreatePose;native.UpdateDynamicVertexBuffer=UploadPose;
    native.SetDrawLightBounds=LightBounds;native.DrawStaticIndexed=Static;native.DrawStaticIndexedInstanced=Instanced;
    const auto* gfx=VitaCpuRender_Backend(&native);
    for(int slot=0;slot<3;++slot){VitaCpuRender_TestAllocationFailure(slot);assert(!gfx->Init(960,544,0,0));assert(CpuTestLiveThreads()==0);}
    CpuTestFailNextThreadStart();assert(!gfx->Init(960,544,0,0));assert(CpuTestLiveThreads()==0);
    initResult=false;assert(!gfx->Init(960,544,0,0));assert(CpuTestLiveThreads()==0);initResult=true;
    assert(gfx->Init(960,544,0,0));assert(CpuTestLiveThreads()==1);
    VitaCpuRender_SetGameplay(true);
    std::array<GfxDrawVertex,64> live{};uint16_t indices[3]={0,1,2};
    uint32_t sequence=0;
    // The producer may overwrite bounds immediately; a delayed consumer sees
    // each draw's copied bounds, and unset draws cannot inherit prior bounds.
    gfx->BeginFrame();
    GfxDrawLightBounds b{{100,200,300},{110,210,310},true};
    gfx->SetDrawLightBounds(&b);assert(gfx->DrawStaticIndexed({1},0,3,{2},0,3));
    b.minimum[0]=400;gfx->SetDrawLightBounds(&b);
    assert(gfx->DrawStaticIndexedInstanced({1},0,3,{2},0,3,{3},0,2));
    b={};assert(gfx->DrawStaticIndexed({1},0,3,{2},0,3));
    gfx->EndFrame();VitaCpuRender_Drain();
    assert(staticBounds.size()==3 && staticBounds[0].valid && staticBounds[0].minimum[0]==100
        && staticBounds[1].valid && staticBounds[1].minimum[0]==400 && !staticBounds[2].valid);
    b.valid=true;gfx->SetDrawLightBounds(&b);gfx->BeginFrame();
    assert(gfx->DrawStaticIndexed({1},0,3,{2},0,3));gfx->EndFrame();VitaCpuRender_Drain();
    assert(!staticBounds.back().valid);
    for(uint32_t phase:{1u,2u,3u,257u,258u,259u,513u,514u,515u,516u,517u,1025u,1026u,1027u,2049u,2050u,2051u,4097u,4098u,4099u,4100u,4101u,4102u,4103u,8193u,8194u,8195u,16385u,16386u,16387u,32769u,32770u,32771u,65537u,65538u,65539u,131073u,131074u,131075u,262145u,262146u,262147u,524289u,524290u,524291u,1048577u,1048578u,1048579u,0u}) {
        VitaCpuRender_SetRenderProbe(phase);gfx->BeginFrame();
        VitaCpuRender_SetRenderProbe(0); // a delayed consumer must use the slot's copy
        gfx->EndFrame();VitaCpuRender_Drain();
        assert(observedProbePhase==phase);
        const auto status=VitaCpuRender_Read();assert(status.renderProbePhase==phase && status.diagnostic==(phase!=0));
    }
    assert(VitaRenderProbePhase(0)==1 && VitaRenderProbePhase(19999)==1);
    assert(VitaRenderProbePhase(20000)==2 && VitaRenderProbePhase(39999)==2);
    assert(VitaRenderProbePhase(40000)==3 && VitaRenderProbePhase(59999)==3 && VitaRenderProbePhase(60000)==0);
    VitaRenderPassProbe probe;probe.Begin(2,100);
    for(uint32_t i=0;i<7;++i){assert(probe.Observe(static_cast<VitaRenderPass>(i)));assert(!probe.Observe(static_cast<VitaRenderPass>(i)));}
    assert(probe.Fenced() && probe.Complete());probe.Begin(0,200);assert(!probe.Observe(VitaRenderPass::World) && !probe.Complete());
    probe.Begin(99,300);assert(!probe.phase && !probe.Fenced());
    for(uint32_t phase:{257u,258u,259u}){probe.Begin(phase,400);assert(probe.comparison && !probe.Fenced() && probe.phase==(phase&0xff));}
    for(uint32_t phase=513;phase<=517;++phase) {
        probe.Begin(phase,600);assert(probe.workload && !probe.comparison && !probe.Fenced());
        assert(probe.ClipProps()==(phase==514) && probe.SuppressProps()==(phase==516));
    }
    for(uint32_t phase:{1025u,1026u,1027u}){probe.Begin(phase,700);assert(probe.opaque && !probe.Fenced() && !probe.ClipProps() && !probe.SuppressProps());}
    for(uint32_t phase:{2049u,2050u,2051u}){probe.Begin(phase,800);assert(probe.depthOrder && !probe.Fenced() && !probe.ClipProps() && !probe.SuppressProps());}
    for(uint32_t phase=4097;phase<=4103;++phase){probe.Begin(phase,900);assert(probe.material && !probe.depthOrder && !probe.Fenced() && !probe.ClipProps() && !probe.SuppressProps());}
    for(uint32_t invalid:{256u,260u,512u,518u,768u,1024u,1028u,1282u,2048u,2052u,3074u,4096u,4104u,6146u,8192u,8196u,12290u,16384u,16388u,24578u,32768u,32772u,49154u,65536u,65540u,98306u,131072u,131076u,196610u,262144u,262148u,393218u,524288u,524292u,786434u,1048576u,1048580u,1572866u,UINT32_MAX}){probe.Begin(invalid,500);assert(!probe.phase && !probe.comparison && !probe.Fenced());}
    for(uint32_t stall:{100000u,500000u,1000000u}) {
        VitaCpuRender_Stall(stall);
        const auto start=VitaCpuNow();
        for(int frame=0;frame<90;++frame) {
            ++sequence;
            for(auto& vertex:live)vertex.position[0]=float(sequence);
            VitaCpuRender_SetSequence(7,sequence);gfx->BeginFrame();
            gfx->DrawIndexed(live.data(),live.size(),indices,3);
            VitaCpuRender_SetCpuWork(1000,100,200,50);gfx->EndFrame();
            for(auto& vertex:live)vertex.position[0]=-1; // live source is immediately reusable
            if(frame==0)sceKernelDelayThread(2000);
        }
        assert(VitaCpuNow()-start<stall); // source publication did not wait on rendering
        VitaCpuRender_Drain();
        const auto status=VitaCpuRender_Read();assert(status.generation==7 && status.sequence==sequence);
        assert(lastValue.load()==float(sequence));assert(status.replaced>0);
        assert(status.snapshotHighWater<=6*1024*1024);
    }
    // Reconstruct triangles through the native callback to prove batching
    // preserves copied vertex bytes, primitive order and texture barriers.
    captureGeometry=true;
    std::vector<GfxDrawVertex> expectedTriangles;std::vector<uint32_t> expectedTextures;
    unsigned originalDraws=0;
    gfx->BeginFrame();
    for(unsigned group=0;group<3;++group) {
        gfx->BindTexture({100+group});
        for(unsigned draw=0;draw<70;++draw) {
            for(unsigned i=0;i<live.size();++i) {
                live[i]={};live[i].position[0]=float(group*10000+draw*100+i);
                live[i].position[1]=float(i)*.125f;live[i].color[0]=float(draw)*.01f;
                live[i].texCoord[1]=float(group)*.25f;
            }
            const uint16_t triangle[]={0,63,2};
            gfx->DrawIndexed(live.data(),live.size(),triangle,3);++originalDraws;
            for(auto i:triangle){expectedTriangles.push_back(live[i]);expectedTextures.push_back(100+group);}
            if(draw==30) {
                const uint16_t invalid[]={0,64,2};gfx->DrawIndexed(live.data(),live.size(),invalid,3);++originalDraws;
                // Incomplete trailing triangle must remain unmerged.
                const uint16_t partial[]={0,2,3,4};gfx->DrawIndexed(live.data(),live.size(),partial,4);++originalDraws;
                for(unsigned n=0;n<3;++n){expectedTriangles.push_back(live[partial[n]]);expectedTextures.push_back(100+group);}
            }
        }
    }
    gfx->EndFrame();for(auto& v:live)v={};VitaCpuRender_Drain();
    assert(capturedTriangles.size()==expectedTriangles.size());
    assert(std::memcmp(capturedTriangles.data(),expectedTriangles.data(),expectedTriangles.size()*sizeof(GfxDrawVertex))==0);
    assert(capturedTextures==expectedTextures && invalidDraws==3);
    assert(capturedDraws<originalDraws/4);
    std::printf("Consecutive draw batching: %u source draws -> %u native calls, exact ordered triangles/state; vertex bytes=%zu\n",originalDraws,capturedDraws,sizeof(GfxDrawVertex));
    captureGeometry=false;
    // Packed pose jobs own all input bytes through supersession, stalls and
    // generation changes. Compare complete uploaded vertices with the former
    // conversion/light path, then immediately overwrite caller input storage.
    VitaCpuRender_TestAllocationFailure(0);
    assert(!gfx->CreateDynamicVertexBuffer(128).value);
    VitaCpuRender_TestAllocationFailure(1);
    assert(!gfx->CreateDynamicVertexBuffer(128).value);
    const auto poseBuffer=gfx->CreateDynamicVertexBuffer(128);assert(poseBuffer.value);
    std::array<VitaPackedSkinVertex,128> packed{};
    std::array<GfxDrawVertex,128> expectedPose{};
    for(uint32_t mode:{0u,kVitaWeaponVertexComparison|1u,kVitaWeaponVertexComparison|2u,kVitaWeaponVertexComparison|3u,kVitaWeaponSpecularComparison|1u,kVitaWeaponSpecularComparison|2u,kVitaWeaponSpecularComparison|3u,kVitaWorldAlphaComparison|1u,kVitaWorldAlphaComparison|2u,kVitaWorldAlphaComparison|3u,kVitaCurrentDrawProbe|1u,kVitaCurrentDrawProbe|2u,kVitaCurrentDrawProbe|3u,kVitaPostFilmComparison|1u,kVitaPostFilmComparison|2u,kVitaPostFilmComparison|3u,kVitaWorldLifetimeComparison|1u,kVitaWorldLifetimeComparison|2u,kVitaWorldLifetimeComparison|3u,kVitaLightmapLayoutComparison|1u,kVitaLightmapLayoutComparison|2u,kVitaLightmapLayoutComparison|3u})
    for(uint32_t stall:{100000u,500000u,1000000u}) {
        VitaCpuRender_Stall(stall);
        const auto start=VitaCpuNow();
        for(uint32_t frame=0;frame<30;++frame) {
            VitaViewModelInputs input{};
            input.entity=frame%3;
            input.projection.cameraAxis[0][0]=1;input.projection.cameraAxis[1][1]=1;input.projection.cameraAxis[2][2]=1;
            input.projection.tanHalfFovX=.67f;input.projection.tanHalfFovY=.53f;
            input.projection.viewOffset[1]=float(frame)*.25f;
            input.sample.valid=input.world.valid=true;input.sample.hasDirectionalColors=frame%2==0;
            input.sample.primaryLightWeight=.37f;
            input.sample.color[0]=float(frame)/30;input.sample.color[1]=.7f;input.sample.color[2]=.3f;
            for(unsigned i=0;i<168;++i)input.sample.directionalColors[i]=uint8_t(frame+i);
            input.world.sunDirection[2]=1;input.world.sunLightColor[0]=.9f;input.world.sunLightColor[1]=.8f;input.world.sunLightColor[2]=.4f;
            PreparedModelLight light(input.sample,input.world);
            for(unsigned i=0;i<packed.size();++i){
                auto& v=packed[i];v={};v.xyz[0]=10+float(i);v.xyz[1]=float(frame);v.xyz[2]=float(i)*.2f;
                v.normal.packed=0x3ff07f80;v.tangent.packed=0x3f7f80f0;
                v.color.packed=0xffa08060;v.texCoord.packed=0x40003800;
                assert(ConvertSourceVertex(v,expectedPose[i],input.projection));
                float response[3]{1,1,1};
                if(light.Evaluate(expectedPose[i].normal,response))
                    for(unsigned c=0;c<3;++c)expectedPose[i].color[c]*=response[c];
            }
            VitaCpuRender_SetSequence(stall,++sequence);VitaCpuRender_SetRenderProbe(mode);gfx->BeginFrame();
            VitaCpuRender_SetRenderProbe(0); // acquisition must use the published mode
            assert(VitaCpuRender_ViewModel(poseBuffer,packed.data(),packed.size(),input));
            gfx->EndFrame();std::memset(packed.data(),0xa5,sizeof(packed));input={};
        }
        assert(VitaCpuNow()-start<stall);
        VitaCpuRender_Drain();
        assert(uploadedPackedPose.size()==expectedPose.size());
        for(unsigned i=0;i<expectedPose.size();++i){const auto expected=PackExpected(expectedPose[i]);assert(std::memcmp(&uploadedPackedPose[i],&expected,sizeof(expected))==0);}
        if(mode==0)assert(uploadedPose.empty()); // ordinary now uses packed native upload
        if(mode!=0 && !VitaWeaponVertexEnabled(mode)) {
            assert(uploadedPose.size()==expectedPose.size());
            assert(std::memcmp(uploadedPose.data(),expectedPose.data(),sizeof(expectedPose))==0);
        }
    }
    // Missing scratch, nonfinite projection and oversized publication fail
    // closed; no following command can draw a stale dynamic vertex buffer.
    for(uint32_t mode:{0u,kVitaWeaponVertexComparison|2u}) for(bool capacityFailure:{false,true}) {
        VitaViewModelInputs input{};
        input.projection.tanHalfFovX=0; // 0/0 is invalid
        std::array<VitaPackedSkinVertex,129> invalid{};
        const auto before=poseUploads;
        VitaCpuRender_SetRenderProbe(mode);gfx->BeginFrame();VitaCpuRender_SetRenderProbe(0);
        assert(VitaCpuRender_ViewModel(poseBuffer,invalid.data(),capacityFailure?129:128,input));
        assert(VitaCpuRender_Copy(1,+[](const int&){++poseAfterCommands;}));
        gfx->EndFrame();VitaCpuRender_Drain();
        assert(poseUploads==before && poseAfterCommands==0);
    }
    // Upload failure rejects the remaining snapshot, followed by recovery.
    for(bool fail:{true,false}) {
        VitaViewModelInputs input{};packed={};packedUploadFails=fail;
        const auto before=poseUploads;
        VitaCpuRender_SetRenderProbe(kVitaWeaponVertexComparison|2u);gfx->BeginFrame();
        VitaCpuRender_SetRenderProbe(0);
        assert(VitaCpuRender_ViewModel(poseBuffer,packed.data(),packed.size(),input));
        assert(VitaCpuRender_Copy(1,+[](const int&){++poseAfterCommands;}));
        gfx->EndFrame();VitaCpuRender_Drain();
        assert(poseUploads==before+(fail?0:1) && poseAfterCommands==(fail?0:1));
    }
    const auto beforeInvalid=VitaCpuRender_Read().published;
    gfx->BeginFrame();
    assert(!VitaCpuRender_ViewModel(poseBuffer,packed.data(),UINT32_MAX,{}));
    gfx->EndFrame();assert(VitaCpuRender_Read().published==beforeInvalid);
    // A long resource transaction retains its borrowed argument, while only
    // the source owner services playback at bounded intervals.
    static unsigned serviced=0;
    VitaCpuSource_SetService(+[]{
        assert(VitaCpuCurrentRole()==VitaCpuRole::Source);++serviced;
        VitaCpuSource_Service(); // recursion must not enter again
    });
    const auto failuresBeforeService=VitaCpuRender_Read().failures;
    int serviceBorrow=0;
    assert(VitaCpuRender_Call(+[](void* p){
        CheckRender();VitaCpuSource_Service();
        sceKernelDelayThread(150000);*static_cast<int*>(p)=456;
    },&serviceBorrow));
    assert(serviceBorrow==456 && serviced>=4 && serviced<=9);
    assert(VitaCpuRender_Read().failures==failuresBeforeService);
    VitaCpuSource_SetService(nullptr);const auto stoppedServices=serviced;
    VitaCpuSource_Service();assert(serviced==stoppedServices);
    const auto published=VitaCpuRender_Read().published;
    std::vector<GfxDrawVertex> tooLarge((7*1024*1024)/sizeof(GfxDrawVertex));
    gfx->BeginFrame();gfx->DrawIndexed(tooLarge.data(),tooLarge.size(),indices,3);gfx->EndFrame();
    assert(VitaCpuRender_Read().published==published); // never publish a partial snapshot
    gfx->BeginFrame();gfx->DrawIndexed(live.data(),UINT32_MAX,indices,3);gfx->EndFrame();
    assert(VitaCpuRender_Read().published==published);
    gfx->BeginFrame();gfx->DestroyBuffer({42});gfx->EndFrame();assert(destroyed.load()==0);
    assert(VitaCpuRender_Read().published==published);
    gfx->DestroyBuffer({42});assert(destroyed.load()==1);
    int borrowed=0;CpuTestFailNextSourceWait();
    assert(!VitaCpuRender_Call(+[](void* value){
        CheckRender();sceKernelDelayThread(20000);*static_cast<int*>(value)=123;
    },&borrowed));
    assert(borrowed==123); // wait failure did not release borrowed storage early
    // Control commands must not replace each other, even without a new snapshot.
    const auto controlsStart=VitaCpuNow();
    VitaCpuRender_Stall(100000);VitaCpuRender_Stall(100000);VitaCpuRender_Stall(100000);
    VitaCpuRender_Drain();assert(VitaCpuNow()-controlsStart>=300000);
    gfx->Shutdown();assert(CpuTestLiveThreads()==0);
    // Joined shutdown permits clean restart without inheriting a stop flag.
    assert(gfx->Init(960,544,0,0));gfx->Shutdown();assert(CpuTestLiveThreads()==0);
}
