#pragma once
#include <gfx_vita/gfx_vita_backend.h>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace kisak::vita {
struct VitaPackedSkinVertex;
struct VitaViewModelInputs;
// Copies packed skinned vertices and all conversion/lighting inputs into the
// current snapshot. Conversion scratch is reserved by buffer creation at load.
bool VitaCpuRender_ViewModel(GfxBufferHandle buffer,const VitaPackedSkinVertex* vertices,
                            uint32_t count,const VitaViewModelInputs& inputs);
struct VitaCpuRenderStatus {
    uint64_t published=0, presented=0, replaced=0, ageUs=0, presentUs=0;
    uint64_t submitUs=0, snapshotHighWater=0, failures=0;
    uint64_t renderPreparationUs=0, presentationUs=0, acquiredAgeUs=0;
    uint64_t preparationLatencyUs=0;
    // Completed ordinary gameplay frames in the renderer's rolling window.
    uint64_t playableRateWindowUs=0, playableFrameIntervalUs=0;
    uint32_t playableRate10=0, playableRateIntervals=0;
    bool playableRateValid=false;
    uint32_t completedRenderRate10=0;
    bool completedRenderRateValid=false;
    uint64_t preparationUs=0, serverUs=0, clientUs=0, audioUs=0;
    uint32_t generation=0, sequence=0, bytes=0;
    bool diagnostic=false;
    uint32_t renderProbePhase=0;
    uint32_t visibilityUs=0,visibleSurfaces=0,submittedSurfaces=0,grassCandidates=0,grassOmissions=0,ticks=0,backlogMs=0;
    GfxVitaFrameCounters counters{};
};
const GfxBackendApi* VitaCpuRender_Backend(const GfxBackendApi* direct);
bool VitaCpuRender_OnSource();
// Resource transactions may borrow caller storage only while the caller waits.
// They are rejected during snapshot construction: gameplay never waits on GL.
bool VitaCpuRender_Call(void (*execute)(void*),void* argument);
bool VitaCpuRender_Emit(const void* value,size_t bytes,void (*execute)(const void*));
template<class T> bool VitaCpuRender_Copy(const T& value,void (*execute)(const T&)) {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(alignof(T)<=16);
    struct Record { void (*function)(const T&); T value; };
    const Record record{execute,value};
    return VitaCpuRender_Emit(&record,sizeof(record),[](const void* p) {
        const auto& r=*static_cast<const Record*>(p);r.function(r.value);
    });
}
void VitaCpuRender_SetGameplay(bool active);
void VitaCpuRender_SetSequence(uint32_t generation,uint32_t sequence);
void VitaCpuRender_SetCpuWork(uint64_t preparation,uint64_t server,uint64_t client,uint64_t audio);
void VitaCpuRender_SetPresentationMetrics(uint32_t visibilityUs,uint32_t visible,uint32_t submitted,uint32_t grassCandidates,uint32_t grassOmissions,uint32_t ticks,uint32_t backlogMs);
void VitaCpuRender_SetDiagnostic(bool active);
void VitaCpuRender_SetRenderProbe(uint32_t phase);
void VitaCpuRender_Stall(uint32_t microseconds);
void VitaCpuRender_Drain();
VitaCpuRenderStatus VitaCpuRender_Read();
bool VitaCpuRender_IsCube(GfxTextureHandle texture);
}

// Owns the actual GPU completion boundary. Only the render owner calls GL.
void GfxVita_WaitIdle();
void GfxVita_SetCpuDiagnostic(bool active);
bool GfxVita_SceneSubmissionEnabled();
