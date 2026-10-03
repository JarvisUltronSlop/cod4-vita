#include "gfx_vita_composition.h"
#include "gfx_vita_texture_reclaim.h"
#include "gfx_vita_texture_pressure.h"
#include "gfx_vita_world_components.h"
#include "gfx_vita_world_gloss.h"
#include "gfx_vita_world_basis.h"
#include "gfx_vita_world_math.h"
#include "gfx_vita_world_precision.h"
#include "gfx_vita_world_scaling.h"
#include "gfx_vita_world_lighting_policy.h"
#include <vita/vita_render_pipeline.h>
#include "gfx_vita_pose_buffers.h"
#include <psp2/kernel/sysmem.h>
#include "gfx_vita_lightmap_layout.h"
#include "gfx_vita_resource_audit.h"
#include <vita/vita_sha256.h>
#include "gfx_vita_world_lifetime.h"
#include "gfx_vita_hud_shader.h"
#include "gfx_vita_weapon_opaque.h"
#include "gfx_vita_early_alpha.h"
#include "gfx_vita_color_fast.h"
#include <vita/vita_cpu_render.h>
#include <vita/vita_display_probe.h>
#include <vita/vita_graphics_profile.h>
#include <vita/vita_pass_isolation.h>
#include <vita/vita_vertex_pack.h>
#include <vita/vita_dynamic_vertex_scratch.h>
#include "gfx_vita_target_transaction.h"
#include "gfx_vita_light_dispatch.h"
#include "gfx_vita_pose_light_bounds.h"
#include "gfx_vita_prop_shader.h"
#include "gfx_vita_opaque_shader.h"
#include "gfx_vita_material_probe.h"
#include "gfx_vita_specular_shader.h"
#include "gfx_vita_material_family.h"
#include "gfx_vita_texture_prefetch.h"
#include <vita/vita_gpu_diagnostic_anchor.h>
#include "gfx_vita_prop_workload.h"
#include <gfx_vita/gfx_vita_backend.h>
#include <vita/vita_cpu_profile.h>
#include <vita/vita_cpu_shader_cache.h>
#include <vita/vita_boot_status.h>
#include <gfx_vita/gfx_vita_viewmodel.h>

#include <vita/vita_script_services.h>
#include <vita/vita_texture_pool.h>
#include <vita/vita_texture_store.h>
#include <vita/vita_texture_scratch.h>
#if KISAK_VITA_CPU_RUNTIME
extern "C" int vglGetInitializationError(void);
#endif
#include <vita/vita_static_model_vertex.h>
#if KISAK_VITA_PROFILE
#include <vita/vita_render_contract.h>
#endif

#include <vitaGL.h>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/display.h>
#include <psp2/io/stat.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <malloc.h>
#include <iterator>
#include <vector>
#include <string>

#ifndef KISAK_VITA_ALLOW_SOFTWARE_FALLBACK
#define KISAK_VITA_ALLOW_SOFTWARE_FALLBACK 0
#endif

#ifndef KISAK_VITA_NATIVE_CPU_TEXTURE_UPLOAD
#define KISAK_VITA_NATIVE_CPU_TEXTURE_UPLOAD 0
#endif

// vitaGL keeps the active GXM context in this process-global so its C and
// C++ entry points share the same immediate context.  The public init call's
// return value is a resolution-fallback flag, not an initialization-success
// flag; use the context itself to distinguish a valid native backend from a
// partial Vita3K/VitaGL initialization.
extern "C" SceGxmContext* gxm_context;
extern "C" void vglSetCompressedTextureMipmapCountHint(GLuint count);
#if KISAK_VITA_PROFILE
// Pinned VitaGL display surface used only by the diagnostic readback. GPU
// transfer materializes emulator render surfaces that CPU memcpy may not see.
extern "C" void* gxm_color_surfaces_addr[];
extern "C" unsigned int gxm_back_buffer_index;
extern "C" int DISPLAY_STRIDE;
#endif

#if KISAK_VITA_CPU_RUNTIME
// Execute the normal preparation, state, packing and upload code during the
// diagnostic. Only scene draw calls are suppressed; native HUD remains visible.
namespace { void BeginGpuDraw(unsigned,const char*,unsigned,unsigned,int,unsigned,const void*); void EndGpuDraw(); }
#define glDrawArrays(mode,first,count) do { if(GfxVita_SceneSubmissionEnabled() && GfxVita_ProfileDrawSubmission()){const auto n_=(count);BeginGpuDraw(__LINE__,__func__,1,(mode),n_,1,nullptr);glDrawArrays((mode),(first),n_);EndGpuDraw();} } while(0)
#define glDrawElements(mode,count,type,indices) do { if(GfxVita_SceneSubmissionEnabled() && GfxVita_ProfileDrawSubmission()){const auto n_=(count);const void* p_=(indices);BeginGpuDraw(__LINE__,__func__,2,(mode),n_,1,p_);glDrawElements((mode),n_,(type),p_);EndGpuDraw();} } while(0)
#define glDrawElementsInstanced(mode,count,type,indices,instances) do { if(GfxVita_SceneSubmissionEnabled() && GfxVita_ProfileDrawSubmission()){const auto n_=(count);const auto i_=(instances);const void* p_=(indices);BeginGpuDraw(__LINE__,__func__,3,(mode),n_,i_,p_);glDrawElementsInstanced((mode),n_,(type),p_,i_);EndGpuDraw();} } while(0)
#endif
namespace {
bool g_cpuDiagnostic=false;
kisak::vita::VitaRenderPassProbe g_renderPassProbe;
kisak::vita::VitaRenderPipeline g_renderPipeline;
GfxVitaPropScissor g_propScissor;
bool g_propGroupOpen=false;
uint32_t g_propGroupSeen=0,g_propAttempts=0,g_propSubmitted=0,g_propClipped=0;
uint64_t g_propAttemptTriangles=0,g_propSubmittedTriangles=0,g_propGroupStart=0;

// Shared display-boundary dither. The source post chain applies this only
// when the final floating-point scene is converted to 8-bit display values;
// keep the table identical in the scalar oracle and native post shader so a
// capture cannot acquire a backend-specific 4x4 crosshatch. The values are a
// decorrelated rank ordering rather than a visible regular Bayer lattice.
constexpr uint8_t kDisplayDither8x8[64] = {
     0u, 49u, 12u, 37u,  3u, 56u, 19u, 44u,
    32u, 16u, 61u, 25u, 47u,  8u, 53u, 29u,
    11u, 42u,  4u, 58u, 22u, 35u,  1u, 63u,
    52u, 27u, 46u, 14u, 60u, 31u, 55u, 20u,
     6u, 36u, 54u, 18u, 40u,  2u, 48u, 26u,
    45u, 59u, 23u,  9u, 33u, 50u, 15u,  7u,
    21u, 39u, 62u, 30u,  5u, 43u, 28u, 57u,
    34u, 10u, 51u, 24u, 13u, 38u, 41u, 17u,
};

float DisplayDither8x8(uint32_t x, uint32_t y) {
    const uint32_t tableIndex = ((y & 7u) << 3u) | (x & 7u);
    return (static_cast<float>(kDisplayDither8x8[tableIndex]) - 31.5f)
        / (64.0f * 255.0f);
}

bool g_initialized = false;
#if KISAK_VITA_CPU_RUNTIME
bool g_nativeContextReady = false;
#endif
#if KISAK_VITA_ALLOW_SOFTWARE_FALLBACK
bool g_software = false;
#else
// Make the native-only contract visible to the compiler. A mutable flag kept
// the scalar raster/post routines and 6.7 MiB of their scratch arrays in the
// strict ELF even though no native startup could ever set it to true.
constexpr bool g_software = false;
#endif
uint32_t g_width = 0;
uint32_t g_height = 0;
uint32_t* g_framebuffer = nullptr;
uint32_t* g_displayFramebuffer = nullptr;
std::vector<float> g_depth;
size_t g_softwarePixelCount = 0;
uintptr_t g_framebufferBase = 0;
uintptr_t g_displayFramebufferBase = 0;
uint32_t g_softwareInvalidStores = 0;

struct SoftwareTexture {
    uint32_t width = 0;
    uint32_t height = 0;
    bool cubeMap = false;
    // The scalar Vita3K diagnostic stores source texels as RGBA4444.  Vita's
    // native texture path remains compressed, while the packed fallback keeps
    // the complete authored Shipment image set resident without the 4-byte
    // RGBA allocation that exhausted the emulator guest heap.  Cubemaps store
    // six base-level faces consecutively in the same vector; the source IWI
    // payload uses the same face-major ordering as KisakCOD's Image_UploadData.
    std::vector<uint16_t> pixels;
    // The authored sky cubemap and scalar world colour maps are sampled across
    // large screen regions; their smooth cloud, fog, and painted gradients
    // expose RGBA4444 quantisation immediately. They request the decoded
    // RGBA8888 path through the private high-bit flag below. Model/HUD images
    // may still use the bounded packed storage when their separate residency
    // policy does not request high precision.
    std::vector<uint32_t> highPrecisionPixels;
    // World colour maps may keep only their authored compressed base level.
    // Sampling that payload on demand avoids a second decoded thumbnail while
    // preserving the source DXT/L8/A8R8G8B8 values for every material tap.
    uint32_t sourceFormat = 0;
    std::vector<uint8_t> sourcePayload;
};

std::vector<SoftwareTexture> g_textures;
std::vector<uint32_t> g_cubeTextureHandles;
// Keep the diagnostic framebuffer in the module's pre-reserved data segment.
// Large map packages can exhaust/fragment the Vita heap before the fallback
// is initialized, even though the fixed 960x544 surface itself is small.
// Keep a render/display pair. The display worker may still be scanning the
// submitted image while the next frame is being rasterized, so publishing the
// same surface that is being cleared produces launch/menu flicker in Vita3K.
alignas(0x1000) uint32_t g_softwareStaticFramebuffer[960u * 544u]{};
alignas(0x1000) uint32_t g_softwareDisplayFramebuffer[960u * 544u]{};
// Keep the scalar scene in a floating RGB working buffer until the authored
// vision/film pass has completed. The packed framebuffer remains the display
// mirror used by Vita3K, but quantizing every material/lightmap result into
// that mirror before postfx clips high-contrast maps such as Crossfire.
// Reserve this beside the diagnostic framebuffer so the fidelity path does
// not compete with the streamed package/image heap for another large block.
alignas(0x1000) float g_softwareSceneColor[960u * 544u * 3u]{};
// The source glow path resolves a quarter-resolution scene, applies a
// separable Gaussian chain, then composites the result over the resolved
// framebuffer. Keep the scalar oracle's equivalent scratch buffers static so
// enabling authored glow does not compete with the map/VM heap.
constexpr uint32_t kSoftwareGlowMaxWidth = 960u / 4u;
constexpr uint32_t kSoftwareGlowMaxHeight = 544u / 4u;
alignas(0x1000) float g_softwareGlowBuffer[
    kSoftwareGlowMaxWidth * kSoftwareGlowMaxHeight * 3u]{};
alignas(0x1000) float g_softwareGlowScratch[
    kSoftwareGlowMaxWidth * kSoftwareGlowMaxHeight * 3u]{};
uint32_t g_softwareGlowWidth = 0;
uint32_t g_softwareGlowHeight = 0;
uint32_t g_boundTexture = 0;
// The scalar backend has one material sampler plus the two authored BSP
// lightmap samplers used by KisakCOD's lit world techniques.  Keep these as
// per-draw handles rather than baking light into shared BSP vertices: the
// desktop renderer evaluates lightmap UVs at the fragment, and many Shipment
// surfaces intentionally reuse one source vertex range with different
// lightmap coordinates.
uint32_t g_softwareLightmapPrimary = 0;
uint32_t g_softwareLightmapSecondary = 0;
uint8_t g_softwareLightmapSamplerFlags = 0;
// The scalar renderer has one logical material texture unit. Keep the
// authored low-byte sampler state here so its address/filter behavior matches
// the VitaGL path instead of relying on a texture-name heuristic.
uint8_t g_softwareSamplerState[4] = {0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu};
size_t g_textureBytes = 0;
struct NativeResourceBytes {
    uint32_t handle = 0;
    uint32_t bytes = 0;
};
// Map color, lightmap, normal/specular, probe and UI families exceed 256
// handles. A full ledger must never silently omit later allocations.
constexpr uint32_t kMaxTrackedNativeTextures = 2048u;
// Shipment and Crossfire each retain the BSP pair plus a bounded set of
// 16-bit static-model geometry chunks.  Keep telemetry/lifetime ownership for
// every buffer rather than silently dropping records after the old BSP-only
// limit.
constexpr uint32_t kMaxTrackedNativeBuffers = 128u;
std::array<NativeResourceBytes, kMaxTrackedNativeTextures> g_nativeTextures{};
kisak::vita::VitaTexturePool<> g_nativeTexturePool;
GLuint NativeTexture(uint32_t lease);
std::atomic<bool> g_texturePagingFault{false};
bool g_texturePagingDisabled=false;
kisak::vita::VitaTextureScratch g_texturePageScratch;
uint32_t g_textureReloadLease=0;
uint64_t g_textureUseSerial=0,g_pagedTextureBytes=0;
uint64_t g_texturePageBudget=kisak::vita::kVitaTexturePageGpuBytes;
GLenum g_textureUploadError=GL_NO_ERROR;
uint32_t g_textureDrawColor=0;
struct TextureDrawPin {uint32_t previous;explicit TextureDrawPin(uint32_t color):previous(g_textureDrawColor){g_textureDrawColor=color;}
    ~TextureDrawPin(){g_textureDrawColor=previous;}};
std::array<uint64_t,2048> g_textureLastUse{};
std::array<NativeResourceBytes, kMaxTrackedNativeBuffers> g_nativeBuffers{};
GfxVitaPoseBuffers g_poseBuffers;
GfxVitaPoseLightBounds<> g_poseLightBounds;
uint64_t g_poseSwapSerial=0;
unsigned g_poseRingUploads=0,g_poseRingFallbacks=0,g_poseRingRotations=0;
void DeleteNativePoseExtra(uint32_t handle);
uint64_t g_nativeTextureBytes = 0;
uint64_t g_nativeStaticBufferBytes = 0;
bool g_softwareDepthTest = true;
bool g_softwareDepthWrite = true;
GfxSurfaceMode g_softwareSurfaceMode = GfxSurfaceMode::Opaque;
GfxCullMode g_softwareCullMode = GfxCullMode::None;
GfxAlphaTestFunc g_softwareAlphaTestFunc = GfxAlphaTestFunc::GreaterEqual128;
bool g_softwareFogEnabled = false;
float g_softwareFogStart = 0.0f;
float g_softwareFogDensity = 0.0f;
float g_softwareFogColor[3] = {0.0f, 0.0f, 0.0f};
bool g_vitaGlFogEnabled = false;
GfxFrameLightingState g_frameLighting{};
GfxMaterialPassState g_materialPass{};
GfxShadowPassState g_shadowPass{};
GfxEffectBatchState g_effectBatch{};
GfxViewEffectState g_viewEffects{};
GfxSurfaceMode g_nativeSurfaceMode = GfxSurfaceMode::Opaque;
GfxAlphaTestFunc g_nativeAlphaTestFunc = GfxAlphaTestFunc::GreaterEqual128;
GfxCullMode g_nativeCullMode = GfxCullMode::None;
uint8_t g_nativeSamplerState[5] = {0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu};
bool g_nativeStaticBufferDraw = false;
// A dynamic buffered draw uses the backend-neutral GfxDrawVertex layout but,
// like immutable world draws, passes byte offsets into bound VBO/IBOs instead
// of host/client pointers.  Keep this distinct from the static flag because
// only static vertices use the packed world layout and world-space shader.
bool g_nativeDynamicBufferDraw = false;
// Dynamic first-person meshes retain camera-space homogeneous coordinates in
// their persistent VBO.  The native shader performs the perspective divide
// and clips against COD4's 0.1-unit viewmodel near plane, avoiding VitaGL's
// catastrophically expensive client-array clipping fallback.
bool g_nativeViewModelSpaceDraw = false;
bool g_nativeStaticInstancedDraw = false;
uint32_t g_nativeStaticInstanceCount = 1u;
// Consecutive immutable world/model ranges frequently share one material.
// Keep the native shader, texture set, and vertex-array enables alive across
// those draws; BindTexture(used by every real state transition and by each
// pass epilogue) closes the cache before a dynamic/client-array submission.
bool g_nativeStaticPipelineActive = false;
uint64_t g_materialPassRevision = 1u;
uint64_t g_nativeStaticMaterialRevision = 0u;
bool g_nativeStaticLayoutLane=false;
bool g_nativeWorldSpaceDraw = false;
GfxTextureHandle g_nativeSkyDrawTexture{0};
float g_nativeWorldNearPlane = 2.0f;
float g_nativeWorldViewProjection[16] = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f,
};

// UI, HUD, particles, and other small CPU-authored submissions must not use
// VitaGL client arrays. VitaGL copies every enabled client attribute into its
// circular allocator, so an interleaved vertex can be duplicated many times
// per draw. Keep one explicitly owned VBO/IBO handle per display buffer and
// stream each draw through VitaGL's bounded scratch pool instead. The slice is
// capped at 30 FPS, so two in-flight display buffers preserve a full 1 MiB
// scratch page while avoiding a third native-resolution back buffer. This
// performs one contiguous vertex copy and one index copy, while previously
// submitted storage remains alive until its GXM command list has retired.
constexpr uint32_t kNativeDisplayBufferCount = 2u;
constexpr uint32_t kNativeTransientFrameCount = kNativeDisplayBufferCount;
constexpr uint32_t kNativeTransientVertexCapacity = 8192u;
constexpr uint32_t kNativeTransientIndexCapacity = 24576u;
struct NativeTransientDrawRing {
    std::array<GLuint, kNativeTransientFrameCount> vertexBuffers{};
    std::array<GLuint, kNativeTransientFrameCount> indexBuffers{};
    uint32_t activePage = 0u;
    uint32_t vertexCursor = 0u;
    uint32_t indexCursor = 0u;
    bool ready = false;
};
NativeTransientDrawRing g_nativeTransientRing{};
uint32_t g_frameTransientUploadDrops = 0u;
uint32_t g_frameTransientUploadBytes = 0u;
uint32_t g_transientUploadHighWaterBytes = 0u;

// Immutable BSP vertices do not need the dynamic draw ABI's clip-space
// position. Keep source positions/UVs lossless and use the normalized formats
// Vita/GXM consumes natively for color, basis vectors and visibility.
struct StaticWorldVertex {
    float worldPosition[3];
    float texCoord[2];
    float lightmapCoord[2];
    int8_t normal[4];
    int8_t tangent[4];
    uint8_t color[4];
    uint16_t shadowVisibility;
    uint16_t padding;
};
static_assert(sizeof(StaticWorldVertex) == 44);
using kisak::vita::VitaStaticModelVertex;

// Skinned first-person vertices need camera-space position, source material
// UVs, the world-space basis/position used by the semantic shader, and color.
// Lightmap coordinates and shadow visibility are constant for this overlay.
// Packing the source-authored 8-bit basis/color fields removes forty unused
// bytes from every persistent ring vertex without quantizing position or UVs.
using DynamicViewModelVertex = GfxVitaViewmodelVertex;
static_assert(sizeof(DynamicViewModelVertex) == 44);
#if KISAK_VITA_CPU_RUNTIME
kisak::vita::DynamicVertexScratch<DynamicViewModelVertex> g_dynamicViewModelUpload;
#else
std::vector<DynamicViewModelVertex> g_dynamicViewModelUpload;
#endif
uint32_t g_rendererFrame = 0;
#if KISAK_VITA_PROFILE
struct NativeWorldCaptureDraw {
    GfxBufferHandle vertices{}, indices{};
    uint32_t firstVertex = 0, vertexCount = 0, firstIndex = 0, indexCount = 0;
    GfxMaterialPassState material{};
    uint64_t materialRevision = 0;
};
struct NativeCaptureRequest {
    bool checked = false, enabled = false, pending = false, completed = false;
    bool inputContracts = false;
    bool stageCaptures = false;
    bool synchronousDraws = false;
    int32_t requestedTime = 0, sourceTime = 0;
    uint32_t mapGeneration = 0, frameGeneration = 0;
    float origin[3]{}, axis[3][3]{};
    float nearPlane = 0, tanHalfFovX = 0, tanHalfFovY = 0;
    uint32_t capturedWorldBuffer = 0;
    bool replaying = false, triangleReplay = false;
    std::vector<NativeWorldCaptureDraw> worldDraws;
};
NativeCaptureRequest g_nativeCapture;
#endif
uint32_t g_frameDrawCalls = 0;
uint32_t g_frameTriangles = 0;
uint32_t g_frameStateChanges = 0;
uint32_t g_lastSwapWaitUsec = 0;
uint32_t g_postSubmitUsec = 0;
uint32_t g_swapDetail[8]{};
bool g_adaptiveLighting = true, g_rawLocalLightLoops = true;
GfxDrawLightBounds g_drawLightBounds{};
unsigned g_localLightBounded[2]{},g_localLightCulled[2]{},g_localLightMissing[2]{};
unsigned g_poseLightBounded=0,g_poseLightCulled=0,g_poseLightMissing=0;
bool g_gameplayTargets = false, g_gameplayFrame = false, g_nativeViewmodel = false;
uint32_t g_compositionUsec = 0;
uint32_t g_litFullDraws=0, g_litSunDraws=0;
bool g_benchmarkScene = false, g_nativeHud = false, g_benchmarkClear = false;
uint32_t g_benchmarkShaderMode=0, g_sceneShaderDraws=0, g_simpleMaterialDraws=0, g_postCopyDraws=0, g_authoredPostDraws=0;
bool BenchmarkSimpleMaterialBound();
void RecordBenchmarkMaterialDraw();
uint32_t g_noLocalLightDraws=0, g_noSpecularDraws=0, g_specularEligibleDraws=0;
uint32_t g_noFogDraws=0, g_fogEligibleDraws=0;
bool g_fogUniformEnabled=false; float g_fogUniformStart=0, g_fogUniformDensity=0;
float g_sceneFogStart=0, g_sceneFogDensity=0;
uint32_t g_noNormalMapDraws=0, g_normalEligibleDraws=0, g_adaptiveFullLightDraws=0;
extern "C" void vglCod4TimingRead(uint32_t*);
extern "C" void vglCod4TimingEnable(GLboolean);
extern "C" void vglCod4ConfigRead(uint32_t*);
uint32_t g_frameNativeColorPasses = 0;
uint32_t g_frameLightmappedDraws = 0;
uint32_t g_frameMaterialPasses = 0;
uint32_t g_frameNormalPasses = 0;
uint32_t g_frameSpecularPasses = 0;
uint32_t g_framePrimaryLightmapPasses = 0;
uint32_t g_frameSecondaryLightmapPasses = 0;
uint32_t g_frameShadowPasses = 0;
uint32_t g_frameEffectBatches = 0;
uint32_t g_frameWeatherDraws = 0;
uint32_t g_frameWeatherDrops = 0;
uint64_t g_frameDualLightmapFragments = 0;
float g_frameLightmapPrimarySum = 0.0f;
float g_frameLightmapPrimaryMin = 1.0f;
float g_frameLightmapPrimaryMax = 0.0f;
float g_frameLightmapSecondarySum[3] = {0.0f, 0.0f, 0.0f};
float g_frameLightmapSecondaryMin[3] = {1.0f, 1.0f, 1.0f};
float g_frameLightmapSecondaryMax[3] = {0.0f, 0.0f, 0.0f};
float g_frameStaticLightSum[3] = {0.0f, 0.0f, 0.0f};
float g_frameStaticLightMin[3] = {1.0f, 1.0f, 1.0f};
float g_frameStaticLightMax[3] = {0.0f, 0.0f, 0.0f};

template <size_t Count>
bool TrackNativeResource(std::array<NativeResourceBytes, Count>* records,
                         uint32_t handle, uint32_t bytes,
                         uint64_t* residentBytes) {
    if (!records || !residentBytes || handle == 0 || bytes == 0)
        return false;
    for (NativeResourceBytes& record : *records) {
        if (record.handle == handle) {
            *residentBytes -= std::min<uint64_t>(*residentBytes, record.bytes);
            record.bytes = bytes;
            *residentBytes += bytes;
            return true;
        }
    }
    for (NativeResourceBytes& record : *records) {
        if (record.handle == 0) {
            record.handle = handle;
            record.bytes = bytes;
            *residentBytes += bytes;
            return true;
        }
    }
    return false;
}

template <size_t Count>
void ForgetNativeResource(std::array<NativeResourceBytes, Count>* records,
                          uint32_t handle, uint64_t* residentBytes) {
    if (!records || !residentBytes || handle == 0)
        return;
    for (NativeResourceBytes& record : *records) {
        if (record.handle != handle)
            continue;
        *residentBytes -= std::min<uint64_t>(*residentBytes, record.bytes);
        record = {};
        return;
    }
}

template <size_t Count>
uint32_t NativeResourceSize(
    const std::array<NativeResourceBytes, Count>& records, uint32_t handle) {
    if (handle == 0)
        return 0;
    for (const NativeResourceBytes& record : records) {
        if (record.handle == handle)
            return record.bytes;
    }
    return 0;
}

void DestroyNativeTransientDrawRing() {
    for (GLuint& buffer : g_nativeTransientRing.vertexBuffers) {
        if (buffer != 0u) {
            ForgetNativeResource(&g_nativeBuffers, buffer,
                                 &g_nativeStaticBufferBytes);
            glDeleteBuffers(1, &buffer);
            buffer = 0u;
        }
    }
    for (GLuint& buffer : g_nativeTransientRing.indexBuffers) {
        if (buffer != 0u) {
            ForgetNativeResource(&g_nativeBuffers, buffer,
                                 &g_nativeStaticBufferBytes);
            glDeleteBuffers(1, &buffer);
            buffer = 0u;
        }
    }
    g_nativeTransientRing = {};
}

bool CreateNativeTransientDrawRing() {
    DestroyNativeTransientDrawRing();
    const uint32_t vertexBytes = kNativeTransientVertexCapacity
        * static_cast<uint32_t>(sizeof(GfxDrawVertex));
    const uint32_t indexBytes = kNativeTransientIndexCapacity
        * static_cast<uint32_t>(sizeof(uint16_t));
    while (glGetError() != GL_NO_ERROR) {}
    for (uint32_t page = 0u; page < kNativeTransientFrameCount; ++page) {
        GLuint vertexBuffer = 0u;
        GLuint indexBuffer = 0u;
        glGenBuffers(1, &vertexBuffer);
        glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
        // A minimal first allocation initializes VitaGL's stream/scratch
        // bookkeeping. UploadNativeTransientDraw replaces it with the exact
        // draw size; reserving each full page here would waste circular-pool
        // space before the first frame.
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(sizeof(GfxDrawVertex)), nullptr,
                     GL_STREAM_DRAW);
        glGenBuffers(1, &indexBuffer);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(sizeof(uint16_t)), nullptr,
                     GL_STREAM_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0u);
        glBindBuffer(GL_ARRAY_BUFFER, 0u);
        if (vertexBuffer == 0u || indexBuffer == 0u
            || glGetError() != GL_NO_ERROR
            || !TrackNativeResource(&g_nativeBuffers, vertexBuffer,
                                    vertexBytes, &g_nativeStaticBufferBytes)
            || !TrackNativeResource(&g_nativeBuffers, indexBuffer,
                                    indexBytes, &g_nativeStaticBufferBytes)) {
            if (vertexBuffer != 0u) {
                ForgetNativeResource(&g_nativeBuffers, vertexBuffer,
                                     &g_nativeStaticBufferBytes);
                glDeleteBuffers(1, &vertexBuffer);
            }
            if (indexBuffer != 0u) {
                ForgetNativeResource(&g_nativeBuffers, indexBuffer,
                                     &g_nativeStaticBufferBytes);
                glDeleteBuffers(1, &indexBuffer);
            }
            DestroyNativeTransientDrawRing();
            return false;
        }
        g_nativeTransientRing.vertexBuffers[page] = vertexBuffer;
        g_nativeTransientRing.indexBuffers[page] = indexBuffer;
    }
    g_nativeTransientRing.ready = true;
    sceClibPrintf(
        "[kisak-vita] native transient ring pages=%u vertices=%u indices=%u bytes=%u\n",
        static_cast<unsigned>(kNativeTransientFrameCount),
        static_cast<unsigned>(kNativeTransientVertexCapacity),
        static_cast<unsigned>(kNativeTransientIndexCapacity),
        static_cast<unsigned>((vertexBytes + indexBytes)
                              * kNativeTransientFrameCount));
    return true;
}

void BeginNativeTransientDrawPage() {
    if (!g_nativeTransientRing.ready)
        return;
    g_nativeTransientRing.activePage = (g_rendererFrame - 1u)
        % kNativeTransientFrameCount;
    g_nativeTransientRing.vertexCursor = 0u;
    g_nativeTransientRing.indexCursor = 0u;
}

void EndNativeTransientDrawPage() {
    // GL_STREAM_DRAW allocations are owned by VitaGL's three-frame scratch
    // pool and retire at swap. There is no mapped range to close here.
}

bool UploadNativeTransientDraw(const GfxDrawVertex* vertices,
                               uint32_t vertexCount,
                               const uint16_t* indices,
                               uint32_t indexCount,
                               const GfxDrawVertex** vertexOffset,
                               const uint16_t** indexOffset) {
    if (!g_nativeTransientRing.ready || !vertices || !indices
        || !vertexOffset || !indexOffset || vertexCount == 0u
        || indexCount < 3u
        || vertexCount > kNativeTransientVertexCapacity
            - g_nativeTransientRing.vertexCursor
        || indexCount > kNativeTransientIndexCapacity
            - g_nativeTransientRing.indexCursor) {
        ++g_frameTransientUploadDrops;
        return false;
    }
    const uint32_t page = g_nativeTransientRing.activePage;
    const size_t vertexBytes = static_cast<size_t>(vertexCount)
        * sizeof(GfxDrawVertex);
    const size_t indexBytes = static_cast<size_t>(indexCount)
        * sizeof(uint16_t);
    while (glGetError() != GL_NO_ERROR) {}
    glBindBuffer(GL_ARRAY_BUFFER,
                 g_nativeTransientRing.vertexBuffers[page]);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertexBytes),
                 vertices, GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,
                 g_nativeTransientRing.indexBuffers[page]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(indexBytes), indices,
                 GL_STREAM_DRAW);
    if (glGetError() != GL_NO_ERROR) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0u);
        glBindBuffer(GL_ARRAY_BUFFER, 0u);
        ++g_frameTransientUploadDrops;
        return false;
    }
    g_nativeTransientRing.vertexCursor += vertexCount;
    g_nativeTransientRing.indexCursor += indexCount;
    g_frameTransientUploadBytes = static_cast<uint32_t>(
        static_cast<uint64_t>(g_nativeTransientRing.vertexCursor)
            * sizeof(GfxDrawVertex)
        + static_cast<uint64_t>(g_nativeTransientRing.indexCursor)
            * sizeof(uint16_t));
    g_transientUploadHighWaterBytes = std::max(
        g_transientUploadHighWaterBytes, g_frameTransientUploadBytes);
    *vertexOffset = nullptr;
    *indexOffset = nullptr;
    return true;
}

bool NativeBufferDrawActive() {
    return g_nativeStaticBufferDraw || g_nativeDynamicBufferDraw;
}

void RecordDraw(uint32_t indexCount) {
    ++g_frameDrawCalls;
    const uint64_t triangles = static_cast<uint64_t>(indexCount / 3u)
        * (g_nativeStaticInstancedDraw ? g_nativeStaticInstanceCount : 1u);
    g_frameTriangles += static_cast<uint32_t>(std::min<uint64_t>(
        triangles, UINT32_MAX - g_frameTriangles));
}

void SubmitNativeIndexedElements(uint32_t indexCount,
                                 const uint16_t* indices) {
    if (!g_nativeHud) {
        ++g_sceneShaderDraws;
        if (BenchmarkSimpleMaterialBound()) ++g_simpleMaterialDraws;
        RecordBenchmarkMaterialDraw();
    }
    if (g_nativeStaticInstancedDraw) {
#if KISAK_VITA_CPU_RUNTIME
        if(g_renderPassProbe.workload && g_propGroupOpen) {
            const uint64_t triangles=uint64_t(indexCount/3)*g_nativeStaticInstanceCount;
            ++g_propAttempts;g_propAttemptTriangles+=triangles;
            if(g_renderPassProbe.SuppressProps())return; // GPU call only; source/CPU commands still execute.
            ++g_propSubmitted;g_propSubmittedTriangles+=triangles;
            if(g_renderPassProbe.ClipProps()) {
                GLint box[4]{};glGetIntegerv(GL_SCISSOR_BOX,box);
                if(g_propScissor.active && glIsEnabled(GL_SCISSOR_TEST) && box[0]==0 && box[1]==0 && box[2]==1 && box[3]==1)++g_propClipped;
                else kisak::vita::VitaCpuProfile_Counter("render-probe-props-clip-invalid",1);
            }
        }
#endif
        glDrawElementsInstanced(
            GL_TRIANGLES, static_cast<GLsizei>(indexCount), GL_UNSIGNED_SHORT,
            indices, static_cast<GLsizei>(g_nativeStaticInstanceCount));
    } else {
#if KISAK_VITA_PROFILE
        if (g_nativeCapture.triangleReplay && g_nativeStaticBufferDraw && g_materialPass.sourceStateValid) {
            for (uint32_t offset = 0; offset < indexCount; offset += 3)
                glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT,
                    reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(indices) + offset * 2));
            return;
        }
#endif
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indexCount),
                       GL_UNSIGNED_SHORT, indices);
    }
#if KISAK_VITA_PROFILE
    if (g_nativeCapture.pending && g_nativeCapture.synchronousDraws) { VITA_CPU_SCOPE("render-gpu-finish"); glFinish(); }
#endif
}

// These masks mirror gfx_d3d/r_state.h.  Keep the decode in the backend so
// the scalar oracle and the native path consume the exact same VITL v4 pair.
constexpr uint32_t kSourceStateAlphaTestDisable = 0x0800u;
constexpr uint32_t kSourceStateAlphaTestMask = 0x3000u;
constexpr uint32_t kSourceStateBlendRgbMask = 0x0700u;
constexpr uint32_t kSourceStateBlendAlphaMask = 0x07000000u;
constexpr uint32_t kSourceStateDepthWrite = 0x00000001u;
constexpr uint32_t kSourceStateDepthTestDisable = 0x00000002u;
constexpr uint32_t kSourceStateDepthFuncMask = 0x0000000Cu;

bool SourceMaterialStateApplies(const GfxMaterialPassState& state) {
    // Viewmodels are submitted through the explicit screen-space overlay
    // route. Their source material still travels in the descriptor for
    // diagnostics, but overlay depth/blend state belongs to the viewmodel
    // compositor rather than the world material state.
    return state.sourceStateValid != 0
        && (state.surfaceMode != GfxSurfaceMode::Overlay
            || (state.techniqueFlags0 & GFX_MATERIAL_PASS_SOURCE_UI) != 0);
}

uint32_t EffectiveSourceStateBits0(const GfxMaterialPassState& state) {
    uint32_t bits = state.sourceStateBits0;
    // R_ChangeState_0 mirrors RGB blend fields into the alpha fields when a
    // material does not provide a separate alpha blend operation. Do the
    // same expansion before either backend reads the factors/equations.
    if ((bits & kSourceStateBlendRgbMask) != 0
        && (bits & kSourceStateBlendAlphaMask) == 0)
        bits = (bits & 0xF800FFFFu) | ((bits & 0x000007FFu) << 16u);
    return bits;
}

bool SourceStateUsesAlphaTest(const GfxMaterialPassState& state) {
    return SourceMaterialStateApplies(state)
        && (state.sourceStateBits0 & kSourceStateAlphaTestDisable) == 0;
}

bool SourceStateUsesBlend(const GfxMaterialPassState& state) {
    return SourceMaterialStateApplies(state)
        && (state.sourceStateBits0 & kSourceStateBlendRgbMask) != 0;
}

bool SourceTechniqueIsUnlit(const GfxMaterialPassState& state) {
    return (state.techniqueFlags0 & GFX_MATERIAL_PASS_SOURCE_UNLIT) != 0;
}

GfxAlphaTestFunc SourceStateAlphaTestFunc(const GfxMaterialPassState& state) {
    if (!SourceMaterialStateApplies(state))
        return state.alphaTestFunc;
    switch (state.sourceStateBits0 & kSourceStateAlphaTestMask) {
    case 0x1000u:
        return GfxAlphaTestFunc::Greater0;
    case 0x2000u:
        return GfxAlphaTestFunc::Less128;
    case 0x3000u:
    default:
        return GfxAlphaTestFunc::GreaterEqual128;
    }
}

bool SourceStateUsesDepthTest(const GfxMaterialPassState& state) {
    if ((state.techniqueFlags0 & GFX_MATERIAL_PASS_SOURCE_UI) != 0) return false;
    return !SourceMaterialStateApplies(state)
        || (state.sourceStateBits1 & kSourceStateDepthTestDisable) == 0;
}

bool SourceStateWritesDepth(const GfxMaterialPassState& state) {
    if ((state.techniqueFlags0 & GFX_MATERIAL_PASS_SOURCE_UI) != 0) return false;
    if (!SourceMaterialStateApplies(state))
        return true;
    if ((state.sourceStateBits1 & kSourceStateDepthWrite) != 0)
        return true;
    // KisakCOD renders the lit world after its depth-prepass, so the source
    // lit pair normally has ZWRITE disabled.  The scalar oracle submits one
    // color pass and has no separate depth-prepass command stream; retain the
    // source depth ordering for opaque/cutout surfaces by emulating that
    // prepass at the color submission boundary.  Blended/overlay surfaces
    // keep the authored no-write behavior.
    return g_software
        && (state.surfaceMode == GfxSurfaceMode::Opaque
            || state.surfaceMode == GfxSurfaceMode::AlphaTest);
}

uint32_t SourceStateDepthFunction(const GfxMaterialPassState& state) {
    return SourceMaterialStateApplies(state)
        ? state.sourceStateBits1 & kSourceStateDepthFuncMask : 0x0Cu;
}

// A small, fixed set of source-space fragment probes keeps the scalar
// diagnostic useful when a map has a large lighting discontinuity.  These are
// deliberately recorded after depth testing, so each entry describes the
// world fragment that actually wins at that screen coordinate rather than an
// arbitrary triangle submitted earlier in the BSP stream.
constexpr uint32_t kSoftwareLightingProbeCount = 5u;
struct SoftwareLightingProbe {
    uint32_t x = 0;
    uint32_t y = 0;
    bool valid = false;
    bool dualLightmap = false;
    bool semanticPass = false;
    uint8_t lightmapFlags = 0;
    uint8_t surfaceMode = 0;
    uint32_t boundTexture = 0;
    uint32_t colorTexture = 0;
    uint32_t sourceSurfaceIndex = 0xFFFFFFFFu;
    float depth = 1.0f;
    float material[3] = {0.0f, 0.0f, 0.0f};
    float dynamic[3] = {1.0f, 1.0f, 1.0f};
    float secondary[3] = {0.0f, 0.0f, 0.0f};
    float staticLight[3] = {0.0f, 0.0f, 0.0f};
    float finalColor[3] = {0.0f, 0.0f, 0.0f};
    float primary = 0.0f;
    float sunDot = 0.0f;
    float shadow = 1.0f;
    float gridSunWeight = -1.0f;
};
SoftwareLightingProbe g_softwareLightingProbes[kSoftwareLightingProbeCount] = {
    {300u, 200u},
    {400u, 300u},
    {650u, 200u},
    {850u, 200u},
    {50u, 500u},
};

void ResetExtraFixedFunctionTextureUnits();
void EndNativeStaticPipeline();

void ResetFrameLightmapTelemetry() {
    g_frameDualLightmapFragments = 0;
    g_frameLightmapPrimarySum = 0.0f;
    g_frameLightmapPrimaryMin = 1.0f;
    g_frameLightmapPrimaryMax = 0.0f;
    for (uint32_t channel = 0; channel < 3u; ++channel) {
        g_frameLightmapSecondarySum[channel] = 0.0f;
        g_frameLightmapSecondaryMin[channel] = 1.0f;
        g_frameLightmapSecondaryMax[channel] = 0.0f;
        g_frameStaticLightSum[channel] = 0.0f;
        g_frameStaticLightMin[channel] = 1.0f;
        g_frameStaticLightMax[channel] = 0.0f;
    }
    for (SoftwareLightingProbe& probe : g_softwareLightingProbes) {
        probe.valid = false;
        probe.dualLightmap = false;
        probe.semanticPass = false;
        probe.lightmapFlags = 0;
        probe.surfaceMode = 0;
        probe.boundTexture = 0;
        probe.colorTexture = 0;
        probe.sourceSurfaceIndex = 0xFFFFFFFFu;
        probe.depth = 1.0f;
        probe.material[0] = probe.material[1] = probe.material[2] = 0.0f;
        probe.dynamic[0] = probe.dynamic[1] = probe.dynamic[2] = 1.0f;
        probe.secondary[0] = probe.secondary[1] = probe.secondary[2] = 0.0f;
        probe.staticLight[0] = probe.staticLight[1]
            = probe.staticLight[2] = 0.0f;
        probe.finalColor[0] = probe.finalColor[1]
            = probe.finalColor[2] = 0.0f;
        probe.primary = 0.0f;
        probe.sunDot = 0.0f;
        probe.shadow = 1.0f;
        probe.gridSunWeight = -1.0f;
    }
}

void RecordFrameLightmapTelemetry(float primaryIntensity,
                                  const float secondaryRgb[3],
                                  const float staticLight[3]) {
    if (!std::isfinite(primaryIntensity) || !secondaryRgb || !staticLight)
        return;
    ++g_frameDualLightmapFragments;
    g_frameLightmapPrimarySum += primaryIntensity;
    g_frameLightmapPrimaryMin = std::min(g_frameLightmapPrimaryMin,
                                         primaryIntensity);
    g_frameLightmapPrimaryMax = std::max(g_frameLightmapPrimaryMax,
                                         primaryIntensity);
    for (uint32_t channel = 0; channel < 3u; ++channel) {
        const float secondary = std::isfinite(secondaryRgb[channel])
            ? secondaryRgb[channel] : 0.0f;
        const float light = std::isfinite(staticLight[channel])
            ? staticLight[channel] : 0.0f;
        g_frameLightmapSecondarySum[channel] += secondary;
        g_frameLightmapSecondaryMin[channel] = std::min(
            g_frameLightmapSecondaryMin[channel], secondary);
        g_frameLightmapSecondaryMax[channel] = std::max(
            g_frameLightmapSecondaryMax[channel], secondary);
        g_frameStaticLightSum[channel] += light;
        g_frameStaticLightMin[channel] = std::min(
            g_frameStaticLightMin[channel], light);
        g_frameStaticLightMax[channel] = std::max(
            g_frameStaticLightMax[channel], light);
    }
}

void RecordSoftwareLightingProbe(int x, int y, float depth,
                                 const float material[3],
                                 const float dynamic[3],
                                 float primaryIntensity,
                                 const float secondaryRgb[3],
                                 const float staticLight[3],
                                 const float finalColor[3], float sunDot,
                                 float shadowVisibility,
                                 float localLightGridSunWeight,
                                 bool dualLightmap, bool semanticPass) {
    if (!material || !dynamic || !secondaryRgb || !staticLight || !finalColor
        || x < 0 || y < 0 || x >= static_cast<int>(g_width)
        || y >= static_cast<int>(g_height)) {
        return;
    }
    const size_t pixel = static_cast<size_t>(y) * g_width
        + static_cast<size_t>(x);
    if (pixel >= g_softwarePixelCount || pixel >= g_depth.size())
        return;
    // SoftwareStoreBlockColor performs the same opaque depth comparison for
    // the world path.  Probe only candidates that would be accepted there;
    // later nearer fragments replace the prior candidate naturally.
    if (g_softwareDepthTest
        && (!std::isfinite(depth) || depth > g_depth[pixel] + 0.00001f)) {
        return;
    }
    for (SoftwareLightingProbe& probe : g_softwareLightingProbes) {
        if (probe.x != static_cast<uint32_t>(x)
            || probe.y != static_cast<uint32_t>(y)) {
            continue;
        }
        probe.valid = true;
        probe.dualLightmap = dualLightmap;
        probe.semanticPass = semanticPass;
        probe.lightmapFlags = g_softwareLightmapSamplerFlags;
        probe.surfaceMode = static_cast<uint8_t>(g_softwareSurfaceMode);
        probe.boundTexture = g_boundTexture;
        probe.colorTexture = g_materialPass.colorTexture.value;
        probe.sourceSurfaceIndex = g_materialPass.sourceSurfaceIndex;
        probe.depth = depth;
        std::memcpy(probe.material, material, sizeof(probe.material));
        std::memcpy(probe.dynamic, dynamic, sizeof(probe.dynamic));
        std::memcpy(probe.secondary, secondaryRgb, sizeof(probe.secondary));
        std::memcpy(probe.staticLight, staticLight,
                    sizeof(probe.staticLight));
        std::memcpy(probe.finalColor, finalColor,
                    sizeof(probe.finalColor));
        probe.primary = primaryIntensity;
        probe.sunDot = sunDot;
        probe.shadow = shadowVisibility;
        probe.gridSunWeight = localLightGridSunWeight;
        return;
    }
}

struct NativeLitShaderState {
    GLuint program = 0;
    GLint colorSampler = -1;
    GLint hasColor = -1;
    GLint normalSampler = -1;
    GLint specularSampler = -1;
    GLint reflectionProbeSampler = -1;
    GLint lightmapPrimarySampler = -1;
    GLint lightmapSecondarySampler = -1;
    GLint hasNormal = -1;
    GLint hasSpecular = -1;
    GLint hasReflectionProbe = -1;
    GLint usePrimaryLightmap = -1;
    GLint useSecondaryLightmap = -1;
    GLint surfaceMode = -1;
    GLint alphaTestEnabled = -1;
    GLint alphaTestFunc = -1;
    GLint bakedLighting = -1;
    GLint frameValid = -1;
    GLint cameraPosition = -1;
    GLint sunDirection = -1;
    GLint sunColor = -1;
    GLint ambientColor = -1;
    GLint diffuseColor = -1;
    GLint ambientScale = -1;
    GLint diffuseFraction = -1;
    GLint sunLight = -1;
    GLint specularColorScale = -1;
    GLint lightGridColor = -1;
    GLint lightGridSunWeight = -1;
    GLint indoorFactor = -1;
    GLint fogEnabled = -1;
    GLint fogStart = -1;
    GLint fogDensity = -1;
    GLint fogColor = -1;
    GLint primaryLightCount = -1;
    GLint sunPrimaryLightIndex = -1;
    GLint primaryLightPositionRadius = -1;
    GLint primaryLightDirectionType = -1;
    GLint primaryLightColorExponent = -1;
    GLint primaryLightCone = -1;
    GLint dynamicLightCount = -1;
    GLint dynamicLightPositionRadius = -1;
    GLint dynamicLightDirectionType = -1;
    GLint dynamicLightColorExponent = -1;
    GLint dynamicLightCone = -1;
    GLint worldSpace = -1;
    GLint viewModelSpace = -1;
    GLint staticInstanced = -1;
    GLint worldViewProjection = -1;
    GLint worldNearPlane = -1;
};

NativeLitShaderState g_nativeLitShader{};
NativeLitShaderState g_nativeFullLitShader{};
NativeLitShaderState g_nativeSunOnlyShader{};
NativeLitShaderState g_nativePropShader{};
NativeLitShaderState g_nativeOpaqueFullShader{},g_nativeOpaqueSunShader{};
NativeLitShaderState g_nativePrefetch[2]{};
uint32_t g_prefetchReady=0,g_prefetchActual[2]{},g_prefetchInvalid=0;
unsigned NativeBoundPrefetch() {
    for(unsigned i=0;i<2;++i)if(g_nativePrefetch[i].program && g_nativeLitShader.program==g_nativePrefetch[i].program)return i+1;
    return 0;
}
NativeLitShaderState g_nativeMaterialFamily[2]{};
NativeLitShaderState g_nativeWeaponSpecular{},g_nativeWorldAlpha{},g_nativeWorldLifetime{};
NativeLitShaderState g_nativeWorldMath[2]{};
NativeLitShaderState g_nativeWorldPrecision[2]{};
NativeLitShaderState g_nativeCheapWorld[2]{};
unsigned g_cheapWorldReady=0;
bool g_simpleWorldRequested=false;
unsigned g_simpleWorldActual=0,g_simpleWorldEligible=0,g_simpleWorldInvalid=0;
GfxVitaWorldScalingState g_worldScaling;
NativeLitShaderState g_nativeWorldBasis[2]{};
NativeLitShaderState g_nativeWorldGloss{};
NativeLitShaderState g_nativeWorldComponents[2]{};
uint32_t g_worldComponentsReady=0,g_worldComponentsActual=0,g_worldComponentsEligible=0,g_worldComponentsInvalid=0,g_worldComponentsTargets=0;
unsigned NativeWorldComponentBound() {for(unsigned i=0;i<2;++i)if(g_nativeWorldComponents[i].program && g_nativeLitShader.program==g_nativeWorldComponents[i].program)return i+1;return 0;}
bool NativeWorldComponentEligible(bool transparentStaticWorld) {
    // Use the caller's proof before VBO binding and during submission alike.
    return GfxVitaWorldComponentEligible(transparentStaticWorld,g_materialPass.sourceSurfaceIndex,
        g_nativeTexturePool.Certificate(g_materialPass.specularTexture.value));
}
uint32_t g_worldGlossActual=0,g_worldGlossEligible=0,g_worldGlossInvalid=0,g_worldGlossTargetActual=0,g_worldGlossTargets=0;
bool NativeWorldGlossBound(){return g_nativeWorldGloss.program && g_nativeLitShader.program==g_nativeWorldGloss.program;}
bool NativeWorldGlossEligible(bool opaqueStaticWorldFamily) {
    // The caller proves static world, non-instanced, non-viewmodel and the
    // complete opaque material family. Use that proof before VBO binding too:
    // the global static-draw flags are not set until the later submission.
    return GfxVitaWorldGlossEligible(opaqueStaticWorldFamily,false,false,opaqueStaticWorldFamily,
        g_nativeTexturePool.Certificate(g_materialPass.specularTexture.value)==233);
}
bool NativeWorldLifetimeEligible(bool staticWorld,bool viewmodel);
bool NativeWorldAlphaEligible(bool staticWorld,bool viewmodel);
GfxVitaWorldBasisState g_worldBasisState;
uint32_t g_worldBasisReady=0,g_worldBasisActual=0,g_worldBasisEligible=0,g_worldBasisInvalid=0,g_worldBasisTargets=0,g_worldBasisTargetActual=0,g_worldBasisSeen=0,g_worldBasisTriangles=0,g_worldBasisFamilyTriangles=0;
unsigned NativeWorldBasisBound() {for(unsigned i=0;i<2;++i)if(g_nativeWorldBasis[i].program && g_nativeLitShader.program==g_nativeWorldBasis[i].program)return i+1;return 0;}
uint32_t g_worldPrecisionReady=0,g_worldPrecisionActual=0,g_worldPrecisionEligible=0,g_worldPrecisionInvalid=0,g_worldPrecisionTargets=0,g_worldPrecisionTargetActual=0;
unsigned NativeCheapWorldBound() {for(unsigned i=0;i<2;++i)if(g_nativeCheapWorld[i].program && g_nativeLitShader.program==g_nativeCheapWorld[i].program)return i+1;return 0;}
unsigned NativeWorldPrecisionBound() {for(unsigned i=0;i<2;++i)if(g_nativeWorldPrecision[i].program && g_nativeLitShader.program==g_nativeWorldPrecision[i].program)return i+1;return 0;}
uint32_t g_worldMathReady=0,g_worldMathActual=0,g_worldMathEligible=0,g_worldMathInvalid=0,g_worldMathTargets=0,g_worldMathTargetActual=0;
unsigned NativeWorldMathBound() {for(unsigned i=0;i<2;++i)if(g_nativeWorldMath[i].program && g_nativeLitShader.program==g_nativeWorldMath[i].program)return i+1;return 0;}
uint32_t g_worldLifetimeActual=0,g_worldLifetimeEligible=0,g_worldLifetimeInvalid=0,g_worldLifetimeTargets=0,g_worldLifetimeTargetActual=0;
unsigned NativeWorldLifetimeBound(){return g_nativeWorldLifetime.program && g_nativeLitShader.program==g_nativeWorldLifetime.program;}
uint32_t g_worldAlphaActual=0,g_worldAlphaEligible=0,g_worldAlphaInvalid=0,g_worldAlphaTargetActual=0,g_worldAlphaTargetEligible=0;
bool NativeWorldAlphaBound(){return NativeWorldComponentBound()!=0 || NativeWorldBasisBound()==2 || NativeWorldPrecisionBound()==2 || NativeCheapWorldBound()==2 || NativeWorldMathBound()==2 || (g_nativeWorldAlpha.program && g_nativeLitShader.program==g_nativeWorldAlpha.program);}
uint32_t g_weaponSpecularActual=0,g_weaponSpecularEligible=0,g_weaponSpecularInvalid=0,g_weaponSpecularNormalMaps=0,g_weaponSpecularMaps=0;
bool NativeWeaponSpecularBound() {return g_nativeWeaponSpecular.program && g_nativeLitShader.program==g_nativeWeaponSpecular.program;}
bool NativeWeaponSpecularEligible(bool weaponOpaque) {
    return GfxVitaWeaponSpecularEligible(weaponOpaque,g_adaptiveLighting,g_rawLocalLightLoops,
        g_materialPass.colorTexture.value!=0,g_materialPass.normalTexture.value!=0,g_materialPass.specularTexture.value!=0,
        g_materialPass.reflectionProbeTexture.value!=0,
        (g_materialPass.lightmapSamplerFlags&0x02u) && g_materialPass.lightmapPrimaryTexture.value,
        (g_materialPass.lightmapSamplerFlags&0x04u) && g_materialPass.lightmapSecondaryTexture.value,
        (g_materialPass.techniqueFlags0&GFX_MATERIAL_PASS_BAKED_LIGHTING)!=0);
}
uint32_t g_familyReady=0,g_familyActual[2]{},g_familyEligible[2]{},g_familyInvalid=0;
uint32_t g_familyCameraSeen=0;float g_familyCamera[3]{};
unsigned NativeBoundMaterialFamily() {
    if(NativeWorldGlossBound() || NativeWorldBasisBound()==1 || NativeWorldLifetimeBound() || NativeWorldPrecisionBound()==1 || NativeCheapWorldBound()==1 || NativeWorldMathBound()==1)return 1;
    for(unsigned i=0;i<2;++i)if(g_nativeMaterialFamily[i].program && g_nativeLitShader.program==g_nativeMaterialFamily[i].program)return i+1;
    return 0;
}
unsigned NativeMaterialFamily(bool opaque) {
    return GfxVitaMaterialFamily(opaque,g_adaptiveLighting,g_rawLocalLightLoops,
        g_materialPass.colorTexture.value!=0,g_materialPass.normalTexture.value!=0,g_materialPass.specularTexture.value!=0,
        g_materialPass.reflectionProbeTexture.value!=0,
        (g_materialPass.lightmapSamplerFlags&0x02u) && g_materialPass.lightmapPrimaryTexture.value,
        (g_materialPass.lightmapSamplerFlags&0x04u) && g_materialPass.lightmapSecondaryTexture.value,
        (g_materialPass.techniqueFlags0&GFX_MATERIAL_PASS_BAKED_LIGHTING)!=0);
}
NativeLitShaderState g_nativeFastColor{},g_nativeFastClear{},g_nativeHudShader{};
NativeLitShaderState g_nativeEarlyAlpha[2]{};
uint32_t g_earlyAlphaReady=0,g_earlyAlphaActual[2]{},g_earlyAlphaEligible[2]{},g_earlyAlphaInvalid=0;
uint32_t g_hudActual=0,g_hudEligible=0,g_hudInvalid=0,g_hudViewportSeen=0;
int g_hudViewport[4]{},g_hudFbo=0;
bool g_nativeClearDraw=false;
uint32_t g_weaponFamilyActual=0,g_weaponFamilyEligible=0,g_weaponFamilyInvalid=0;
uint32_t g_weaponOpaqueActual=0,g_weaponOpaqueEligible=0,g_weaponOpaqueInvalid=0,g_weaponNormalDraws=0,g_weaponSpecularDraws=0;
bool NativeWeaponOpaqueEligible();
uint32_t g_fastColorActual=0,g_fastColorEligible=0,g_fastClearActual=0,g_fastClearEligible=0,g_fastColorInvalid=0;
NativeLitShaderState g_nativeSpecularShader[2]{};
uint32_t g_specularReady=0,g_specularDraws[2]{},g_specularInputDraws=0,g_reflectionInputDraws=0;
bool NativeBoundSpecularShader() {
    for(const auto& shader:g_nativeSpecularShader)
        if(shader.program && shader.program==g_nativeLitShader.program)return true;
    return false;
}
NativeLitShaderState g_nativeMaterialProbe[3][2]{};
uint32_t g_materialProbeReady=0,g_materialProbeMask=0,g_materialProbeDraws[2]{};
uint32_t g_materialNormalDraws=0,g_materialSpecDraws=0,g_materialReflectionDraws=0;
uint32_t g_materialPrimaryDraws=0,g_materialSecondaryDraws=0,g_materialLocalDraws=0;
int NativeBoundMaterialProbe() {
    for(unsigned variant=0;variant<3;++variant)for(unsigned light=0;light<(variant==2?1u:2u);++light)
        if(g_nativeMaterialProbe[variant][light].program && g_nativeLitShader.program==g_nativeMaterialProbe[variant][light].program)
            return int(variant+1);
    return 0;
}
uint32_t g_opaqueDraws[2]{},g_opaqueEligible[2]{},g_opaqueInvalid=0;
bool NativeOpaqueEligible(bool staticWorld,bool viewmodel);
uint32_t g_propShaderDraws=0,g_propShaderEligibleDraws=0;
#if KISAK_VITA_BENCHMARK
#include "gfx_vita_benchmark_shaders.h"

NativeLitShaderState g_nativeSimpleShader{};
NativeLitShaderState g_nativeNoSpecularShader{};
NativeLitShaderState g_nativeSunNoSpecularShader{};
NativeLitShaderState g_nativeNoNormalShader{}, g_nativeSunNoNormalShader{};
NativeLitShaderState g_nativeNoFogShader{}, g_nativeSunNoFogShader{};
#endif
bool BenchmarkSimpleMaterialBound() {
#if KISAK_VITA_BENCHMARK
    return g_nativeSimpleShader.program && g_nativeLitShader.program == g_nativeSimpleShader.program;
#else
    return false;
#endif
}

void RecordBenchmarkMaterialDraw() {
    const int materialVariant=NativeBoundMaterialProbe();
    const unsigned prefetch=NativeBoundPrefetch();
    const unsigned family=prefetch?prefetch:NativeBoundMaterialFamily();
    const bool opaqueBound=(g_nativeOpaqueFullShader.program && g_nativeLitShader.program==g_nativeOpaqueFullShader.program) || (g_nativeSpecularShader[0].program && g_nativeLitShader.program==g_nativeSpecularShader[0].program)
        || (g_nativeOpaqueSunShader.program && g_nativeLitShader.program==g_nativeOpaqueSunShader.program) || (g_nativeSpecularShader[1].program && g_nativeLitShader.program==g_nativeSpecularShader[1].program) || NativeWeaponSpecularBound() || NativeBoundMaterialFamily() || materialVariant || NativeBoundSpecularShader() || family;
    const bool weaponOpaqueBound=(g_renderPassProbe.weaponOpaque || g_renderPipeline.weaponPipeline) && g_nativeViewModelSpaceDraw && opaqueBound;
    if(opaqueBound && !weaponOpaqueBound) {
        ++g_opaqueDraws[g_nativeStaticInstancedDraw?1:0];
        if(family) {
            ++g_familyActual[family-1];
            if(family!=NativeMaterialFamily(true) || (!g_renderPassProbe.prefetch && !g_renderPassProbe.drawProbe && !(g_renderPassProbe.fastColor || g_renderPassProbe.weaponOpaque || g_renderPassProbe.hud || g_renderPassProbe.earlyAlpha || g_renderPassProbe.fastDrawProbe || g_renderPipeline.weaponPipeline) && (!g_renderPassProbe.family || g_renderPassProbe.phase!=2)))++g_familyInvalid;
            if(prefetch){++g_prefetchActual[prefetch-1];if(!g_renderPassProbe.prefetch || g_renderPassProbe.phase!=2)++g_prefetchInvalid;}
        }
        if((g_renderPassProbe.family || g_renderPassProbe.displayQueue || g_renderPassProbe.prefetch || g_renderPassProbe.drawProbe || (g_renderPassProbe.fastColor || g_renderPassProbe.weaponOpaque || g_renderPassProbe.hud || g_renderPassProbe.earlyAlpha || g_renderPassProbe.fastDrawProbe || g_renderPipeline.weaponPipeline)) && !g_familyCameraSeen) {
            std::memcpy(g_familyCamera,g_frameLighting.cameraPosition,sizeof(g_familyCamera));g_familyCameraSeen=1;
        }
        if(g_renderPassProbe.specular) {
            if(NativeBoundSpecularShader())++g_specularDraws[g_nativeStaticInstancedDraw?1:0];
            g_specularInputDraws+=g_materialPass.specularTexture.value!=0;
            g_reflectionInputDraws+=g_materialPass.specularTexture.value && g_materialPass.reflectionProbeTexture.value;
        }
        if(g_renderPassProbe.material) {
            g_materialProbeMask|=1u<<materialVariant;
            if(materialVariant)++g_materialProbeDraws[g_nativeStaticInstancedDraw?1:0];
            g_materialNormalDraws+=g_materialPass.normalTexture.value!=0;
            g_materialSpecDraws+=g_materialPass.specularTexture.value!=0;
            g_materialReflectionDraws+=g_materialPass.specularTexture.value && g_materialPass.reflectionProbeTexture.value;
            g_materialPrimaryDraws+=(g_materialPass.lightmapSamplerFlags&0x02u) && g_materialPass.lightmapPrimaryTexture.value;
            g_materialSecondaryDraws+=(g_materialPass.lightmapSamplerFlags&0x04u) && g_materialPass.lightmapSecondaryTexture.value;
            g_materialLocalDraws+=g_rawLocalLightLoops;
        }
        if(!NativeOpaqueEligible(g_nativeStaticBufferDraw && g_nativeWorldSpaceDraw,g_nativeViewModelSpaceDraw))++g_opaqueInvalid;
    }
    if(g_nativePropShader.program && g_nativeLitShader.program==g_nativePropShader.program)++g_propShaderDraws;
    if(g_materialPass.sourceStateValid || g_nativeWorldSpaceDraw || g_nativeViewModelSpaceDraw) {
        if(g_nativeLitShader.program==g_nativeFullLitShader.program || (g_nativeOpaqueFullShader.program && g_nativeLitShader.program==g_nativeOpaqueFullShader.program) || (g_nativeSpecularShader[0].program && g_nativeLitShader.program==g_nativeSpecularShader[0].program)) ++g_litFullDraws;
        else if(g_nativeLitShader.program==g_nativeSunOnlyShader.program || g_nativeLitShader.program==g_nativePropShader.program || (g_nativeOpaqueSunShader.program && g_nativeLitShader.program==g_nativeOpaqueSunShader.program) || (g_nativeSpecularShader[1].program && g_nativeLitShader.program==g_nativeSpecularShader[1].program) || NativeWeaponSpecularBound() || NativeWorldAlphaBound() || NativeBoundMaterialFamily() || NativeBoundPrefetch()) ++g_litSunDraws;
    }
#if KISAK_VITA_BENCHMARK
    if (g_nativeLitShader.program &&
        (g_nativeLitShader.program==g_nativeNoFogShader.program
         || g_nativeLitShader.program==g_nativeSunNoFogShader.program)) ++g_noFogDraws;
    if (g_fogUniformEnabled && g_fogUniformDensity>0) {
        if (!g_fogEligibleDraws) { g_sceneFogStart=g_fogUniformStart; g_sceneFogDensity=g_fogUniformDensity; }
        ++g_fogEligibleDraws;
    }
    // Count the bound program, not just the requested diagnostic mode.
    if ((g_adaptiveLighting || g_benchmarkShaderMode==3 || g_benchmarkShaderMode==5) && g_nativeLitShader.program
        && (g_nativeLitShader.program==g_nativeSunOnlyShader.program || g_nativeLitShader.program==g_nativePropShader.program || (g_nativeOpaqueSunShader.program && g_nativeLitShader.program==g_nativeOpaqueSunShader.program) || (g_nativeSpecularShader[1].program && g_nativeLitShader.program==g_nativeSpecularShader[1].program) || NativeWeaponSpecularBound() || NativeWorldAlphaBound() || NativeBoundMaterialFamily() || NativeBoundPrefetch())) ++g_noLocalLightDraws;
    if (g_nativeLitShader.program &&
        (g_nativeLitShader.program==g_nativeNoSpecularShader.program
         || g_nativeLitShader.program==g_nativeSunNoSpecularShader.program)) ++g_noSpecularDraws;
    if ((g_frameLighting.flags & GFX_FRAME_LIGHTING_VALID)
        && g_materialPass.specularTexture.value != 0) ++g_specularEligibleDraws;
    if (g_nativeLitShader.program &&
        (g_nativeLitShader.program==g_nativeNoNormalShader.program
         || g_nativeLitShader.program==g_nativeSunNoNormalShader.program)) ++g_noNormalMapDraws;
    if (g_materialPass.normalTexture.value != 0) ++g_normalEligibleDraws;
    if ((g_adaptiveLighting || g_benchmarkShaderMode==5) && g_nativeLitShader.program
        && g_nativeLitShader.program==g_nativeFullLitShader.program) ++g_adaptiveFullLightDraws;
#endif
}

struct NativePostShaderState {
    GLuint program = 0;
    GLint sceneSampler = -1;
    GLint ditherSampler = -1;
    GLint filmEnabled = -1;
    GLint exposure = -1;
    GLint contrast = -1;
    GLint brightness = -1;
    GLint desaturation = -1;
    GLint invert = -1;
    GLint gamma = -1;
    GLint filmLightTint = -1;
    GLint filmDarkTint = -1;
    GLint glowEnabled = -1;
    GLint glowCutoff = -1;
    GLint glowCutoffRescale = -1;
    GLint glowDesaturation = -1;
    GLint glowIntensity = -1;
    GLint glowOffset = -1;
    GLint outputSize = -1;
};

struct NativePostState {
    NativePostShaderState shader{};
    GLuint sceneTexture = 0;
    GLuint ditherTexture = 0;
    GLuint sceneFramebuffer = 0;
    GLuint sceneDepth = 0;
    GLuint compositeVertexBuffer = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool ready = false;
};

NativePostState g_nativePost{};
struct NativeWorldTarget {
    GLuint texture=0, depth=0, framebuffer=0;
    uint32_t width=0, height=0;
} g_worldTarget, g_worldScaleTarget;
bool g_worldScaleActive=false;
struct WorldCopyProgram { GLuint program=0;GLint sampler=-1; } g_worldCopy;
bool CreateWorldTarget(NativeWorldTarget& target,uint32_t width,uint32_t height) {
        target.width=width; target.height=height;
        glGenTextures(1,&target.texture);
        if(!target.texture) return false;
        glBindTexture(GL_TEXTURE_2D,target.texture);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        glGenRenderbuffers(1,&target.depth);
        if(!target.depth) return false;
        glBindRenderbuffer(GL_RENDERBUFFER,target.depth);
        glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT16,width,height);
        glGenFramebuffers(1,&target.framebuffer);
        if(!target.framebuffer) return false;
        glBindFramebuffer(GL_FRAMEBUFFER,target.framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,target.texture,0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,target.depth);
        const bool valid=target.texture && target.depth && target.framebuffer
            && glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE && glGetError()==GL_NO_ERROR;
        glBindFramebuffer(GL_FRAMEBUFFER,0); glBindRenderbuffer(GL_RENDERBUFFER,0); glBindTexture(GL_TEXTURE_2D,0);
        return valid;
}
void DestroyWorldTarget() {
    kisak::vita::VitaGraphicsProfile_Finish();
    glBindFramebuffer(GL_FRAMEBUFFER,0);
    if (g_worldTarget.framebuffer) glDeleteFramebuffers(1,&g_worldTarget.framebuffer);
    if (g_worldTarget.depth) glDeleteRenderbuffers(1,&g_worldTarget.depth);
    if (g_worldTarget.texture) glDeleteTextures(1,&g_worldTarget.texture);
    if (g_worldScaleTarget.framebuffer) glDeleteFramebuffers(1,&g_worldScaleTarget.framebuffer);
    if (g_worldScaleTarget.depth) glDeleteRenderbuffers(1,&g_worldScaleTarget.depth);
    if (g_worldScaleTarget.texture) glDeleteTextures(1,&g_worldScaleTarget.texture);
    g_worldScaleTarget={};g_worldScaleActive=false;
    g_worldTarget={}; g_gameplayTargets=false; g_gameplayFrame=false;
}


#if KISAK_VITA_CPU_RUNTIME
kisak::vita::VitaPassIsolation g_passIsolation;
kisak::vita::VitaGpuDrawProbe g_gpuDrawProbe;
uint64_t g_gpuDrawStart=0;
unsigned GpuDrawPass() {
    unsigned pass=0;while(pass<6 && (g_renderPassProbe.seen&(1u<<pass)))++pass;
    return pass;
}
void BeginGpuDraw(unsigned site,const char* function,unsigned api,unsigned mode,int count,unsigned instances,const void* indices) {
    auto& p=g_gpuDrawProbe;p.selected=false;
    if(!p.phase)return;
    GLint shader=0,framebuffer=0,vertices=0,elements=0;
    glGetIntegerv(GL_CURRENT_PROGRAM,&shader);glGetIntegerv(GL_FRAMEBUFFER_BINDING,&framebuffer);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&vertices);glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&elements);
    const unsigned pass=GpuDrawPass();
    const uint32_t flags=(g_nativeStaticBufferDraw?1u:0u)|(g_nativeDynamicBufferDraw?2u:0u)
        |(g_nativeStaticInstancedDraw?4u:0u)|(g_nativeWorldSpaceDraw?8u:0u)
        |(g_nativeViewModelSpaceDraw?16u:0u)|(g_nativeHud?32u:0u)
        |(SourceStateUsesAlphaTest(g_materialPass)?64u:0u)|(SourceStateUsesBlend(g_materialPass)?128u:0u);
    kisak::vita::VitaGpuDrawKey key;
    key.words={pass,site,api,mode,uint32_t(std::max(count,0)),instances,uint32_t(shader),uint32_t(framebuffer),
        uint32_t(vertices),uint32_t(elements),elements?uint32_t(reinterpret_cast<uintptr_t>(indices)):0u,
        g_materialPass.sourceSurfaceIndex,g_materialPass.colorTexture.value,g_materialPass.normalTexture.value,
        g_materialPass.specularTexture.value,g_materialPass.reflectionProbeTexture.value,
        g_materialPass.lightmapPrimaryTexture.value,g_materialPass.lightmapSecondaryTexture.value,
        g_materialPass.sourceStateBits0,g_materialPass.sourceStateBits1,g_materialPass.techniqueFlags0,g_materialPass.techniqueFlags1,
        flags,uint32_t(g_materialPass.colorSamplerState)|(uint32_t(g_materialPass.normalSamplerState)<<8)
            |(uint32_t(g_materialPass.specularSamplerState)<<16)|(uint32_t(g_materialPass.lightmapSamplerFlags)<<24)};
    if(g_renderPassProbe.currentDrawProbe) {
        // Non-source fullscreen draws must not inherit the last material's
        // texture/surface labels. The bound program distinguishes the passes.
        if(GLuint(shader)==g_nativePost.shader.program)
            kisak::vita::VitaGpuDrawFullscreenInputs(key,g_nativePost.sceneTexture,g_nativePost.ditherTexture);
        else if(GLuint(shader)==g_worldCopy.program)
            kisak::vita::VitaGpuDrawFullscreenInputs(key,g_worldTarget.texture,0);
        else if(pass!=unsigned(kisak::vita::VitaRenderPass::World))key.words[11]=UINT32_MAX;
    }
    const auto id=p.Draw(key,function);
    if(!p.selected)return;
    if(!p.sampleCount) {
        // Drain the prefix separately; it can include earlier queued frames.
        // The second fence is an empty-scene control, NOT an exact correction
        // for tile load/store costs incurred by a populated draw's fence.
        auto start=kisak::vita::VitaCpuNow();glFinish();p.prefixWait=kisak::vita::VitaCpuNow()-start;
        start=kisak::vita::VitaCpuNow();glFinish();p.emptyWait=kisak::vita::VitaCpuNow()-start;
        p.passWait[pass]+=p.prefixWait+p.emptyWait;
    }
    p.samples[p.sampleCount]={id,p.draws-1,0,0};
    g_gpuDrawStart=kisak::vita::VitaCpuNow();
}
void EndGpuDraw() {
    auto& p=g_gpuDrawProbe;if(!p.selected)return;
    const auto submitted=kisak::vita::VitaCpuNow();
    glFinish();const auto completed=kisak::vita::VitaCpuNow();
    auto& sample=p.samples[p.sampleCount++];sample.submit=submitted-g_gpuDrawStart;sample.wait=completed-submitted;
    p.passWait[p.entries[sample.id-1].key.words[0]]+=sample.wait;
    p.selected=false;
}
void ReportGpuDraws() {
    auto& p=g_gpuDrawProbe;
    const auto counter=kisak::vita::VitaCpuProfile_Counter;
    const auto start=kisak::vita::VitaCpuNow();
    // Bound publication bursts below the existing 1024-event channel. Pack
    // four observed IDs per record; publish at most 32 new descriptors/frame.
    const uint32_t publishEnd=std::min(p.count,p.published+32u);
    for(uint32_t i=p.published;i<publishEnd;++i) {
        const auto& entry=p.entries[i];
        for(unsigned chunk=0;chunk<3;++chunk) {
            char label[96];int used=std::snprintf(label,sizeof(label),"gpu-key-%u-",chunk);
            for(unsigned field=0;field<8;++field)used+=std::snprintf(label+used,sizeof(label)-used,"%08x",entry.key.words[chunk*8+field]);
            counter(label,i+1,0);
        }
        char label[96];std::snprintf(label,sizeof(label),"gpu-site-%s",entry.site);
        counter(label,i+1,entry.key.words[1]);
    }
    p.published=publishEnd;
    for(uint32_t i=0;i<p.draws;i+=4) {
        char label[32];std::snprintf(label,sizeof(label),"gpu-order-%04u",i);
        const uint64_t a=p.order[i]|(i+1<p.draws?uint64_t(p.order[i+1])<<32:0);
        const uint64_t b=(i+2<p.draws?uint64_t(p.order[i+2]):0)|(i+3<p.draws?uint64_t(p.order[i+3])<<32:0);
        counter(label,a,b);
    }
    for(uint32_t i=0;i<p.sampleCount;++i) {
        const auto& sample=p.samples[i];const uint64_t key=(uint64_t(sample.ordinal)<<32)|sample.id;
        counter("gpu-draw-submit",key,sample.submit);counter("gpu-draw-wait",key,sample.wait);
    }
    for(unsigned pass=0;pass<7;++pass)counter("gpu-draw-pass-wait",pass,p.passWait[pass]);
    counter("gpu-draw-prefix",p.prefixWait,p.first);counter("gpu-draw-empty",p.emptyWait,8);
    counter("gpu-draw-window",p.first,p.sampleCount);counter("gpu-draw-frame",p.draws,p.count);
    const auto error=glGetError();if(error!=GL_NO_ERROR)p.invalid|=4;
    counter("gpu-draw-health",p.invalid,kisak::vita::VitaGpuDrawProbe::Capacity);
    counter("gpu-draw-report-us",kisak::vita::VitaCpuNow()-start,0);
    p.End();
}
#endif

static constexpr const char* kNativeLitVertexShader = R"VITA_SHADER(
#version 100
attribute vec3 a_position;
attribute vec4 a_color;
attribute vec2 a_texcoord;
attribute vec2 a_lightmapcoord;
attribute vec3 a_normal;
attribute vec3 a_tangent;
attribute vec3 a_worldposition;
// Attribute seven carries a compact pair packed into one normalized float:
// the high byte is sun-shadow visibility and the low byte is the optional
// authored per-surface light-grid sun weight.
attribute float a_shadowvisibility;
attribute vec4 a_instance_rotation;
attribute vec4 a_instance_origin_scale;
attribute vec4 a_instance_light_pos_x;
attribute vec4 a_instance_light_neg_x;
attribute vec4 a_instance_light_pos_y;
attribute vec4 a_instance_light_neg_y;
attribute vec4 a_instance_light_pos_z;
attribute vec4 a_instance_light_neg_z;
uniform float u_world_space;
uniform float u_viewmodel_space;
uniform float u_static_instanced;
uniform mat4 u_world_view_projection;
uniform float u_world_near_plane;
uniform vec3 u_sun_direction;
uniform vec3 u_sun_color;
varying vec4 v_color;
varying vec2 v_texcoord;
varying vec2 v_lightmapcoord;
varying vec3 v_normal;
varying vec3 v_tangent;
varying vec3 v_worldposition;
varying float v_shadowvisibility;
varying float v_viewdistance;

void main() {
    vec3 worldPosition = a_worldposition;
    vec3 worldNormal = a_normal;
    vec3 worldTangent = a_tangent;
    vec4 vertexColor = a_color;
    if (u_static_instanced > 0.5) {
        vec4 rotation = normalize(a_instance_rotation);
        vec3 twiceCrossPosition = 2.0 * cross(rotation.xyz,
                                               a_worldposition);
        vec3 rotatedPosition = a_worldposition
            + rotation.w * twiceCrossPosition
            + cross(rotation.xyz, twiceCrossPosition);
        worldPosition = a_instance_origin_scale.xyz
            + rotatedPosition * a_instance_origin_scale.w;
        vec3 twiceCrossNormal = 2.0 * cross(rotation.xyz, a_normal);
        worldNormal = normalize(a_normal
            + rotation.w * twiceCrossNormal
            + cross(rotation.xyz, twiceCrossNormal));
        vec3 twiceCrossTangent = 2.0 * cross(rotation.xyz, a_tangent);
        worldTangent = normalize(a_tangent
            + rotation.w * twiceCrossTangent
            + cross(rotation.xyz, twiceCrossTangent));
        vec3 weights = abs(worldNormal);
        float weightSum = max(weights.x + weights.y + weights.z, 0.000001);
        vec3 modelLight = (
            (worldNormal.x >= 0.0 ? a_instance_light_pos_x.rgb
                                  : a_instance_light_neg_x.rgb) * weights.x
            + (worldNormal.y >= 0.0 ? a_instance_light_pos_y.rgb
                                    : a_instance_light_neg_y.rgb) * weights.y
            + (worldNormal.z >= 0.0 ? a_instance_light_pos_z.rgb
                                    : a_instance_light_neg_z.rgb) * weights.z)
            / weightSum;
        vec3 sourceSun = dot(u_sun_direction, u_sun_direction) > 0.000001
            ? normalize(u_sun_direction) : vec3(0.0, 0.0, 1.0);
        float sourceSunDot = max(dot(sourceSun, worldNormal), 0.0);
        vec3 sourceResponse = 2.0 * modelLight
            + clamp(a_instance_light_pos_x.w, 0.0, 1.0)
                * u_sun_color * sourceSunDot;
        vertexColor.rgb *= sourceResponse;
    }
    // UI/effects continue to submit clip-space positions. Immutable BSP
    // buffers carry source world positions. Dynamic first-person buffers use
    // (cameraX * projectionX, cameraY * projectionY, cameraZ); constructing W
    // from cameraZ lets hardware clipping handle triangles that cross the
    // viewmodel near plane without rebuilding client arrays on the CPU.
    vec4 projected;
    if (u_world_space > 0.5) {
        projected = u_world_view_projection * vec4(worldPosition, 1.0);
    } else if (u_viewmodel_space > 0.5) {
        projected = vec4(a_position.xy,
                         a_position.z - 2.0 * u_world_near_plane,
                         a_position.z);
    } else {
        projected = vec4(a_position, 1.0);
    }
    gl_Position = projected;
    v_color = vertexColor;
    v_texcoord = a_texcoord;
    v_lightmapcoord = a_lightmapcoord;
    v_normal = worldNormal;
    v_tangent = worldTangent;
    v_worldposition = worldPosition;
    v_shadowvisibility = a_shadowvisibility;
    // Interpolate source view distance directly. Interpolating encoded depth
    // and decoding it with a hard-coded near plane changes authored fog when
    // cgame requests the default r_znear or a camera overrides that value.
    v_viewdistance = (u_world_space > 0.5 || u_viewmodel_space > 0.5)
        ? max(projected.w, 0.0)
        : 2.0 / max(1.0 - a_position.z, 0.0001);
}
)VITA_SHADER";

static constexpr const char* kNativeLitFragmentShader = R"VITA_SHADER(
#version 100
precision highp float;
uniform sampler2D u_color;
uniform float u_has_color;
uniform sampler2D u_normal;
uniform sampler2D u_specular;
uniform samplerCube u_reflection_probe;
uniform sampler2D u_lightmap_primary;
uniform sampler2D u_lightmap_secondary;
uniform float u_has_normal;
uniform float u_has_specular;
uniform float u_has_reflection_probe;
uniform float u_use_primary_lightmap;
uniform float u_use_secondary_lightmap;
uniform float u_surface_mode;
uniform float u_alpha_test_enabled;
uniform float u_alpha_test_func;
uniform float u_baked_lighting;
uniform float u_frame_valid;
uniform vec3 u_camera_position;
uniform vec3 u_sun_direction;
uniform vec3 u_sun_color;
uniform vec3 u_ambient_color;
uniform vec3 u_diffuse_color;
uniform float u_ambient_scale;
uniform float u_diffuse_fraction;
uniform float u_sun_light;
uniform float u_specular_color_scale;
uniform vec3 u_lightgrid_color;
uniform float u_lightgrid_sun_weight;
uniform float u_indoor_factor;
uniform float u_fog_enabled;
uniform float u_fog_start;
uniform float u_fog_density;
uniform vec3 u_fog_color;
uniform float u_primary_light_count;
uniform float u_sun_primary_light_index;
uniform vec4 u_primary_light_position_radius[16];
uniform vec4 u_primary_light_direction_type[16];
uniform vec4 u_primary_light_color_exponent[16];
uniform vec4 u_primary_light_cone[16];
uniform float u_dynamic_light_count;
uniform vec4 u_dynamic_light_position_radius[4];
uniform vec4 u_dynamic_light_direction_type[4];
uniform vec4 u_dynamic_light_color_exponent[4];
uniform vec4 u_dynamic_light_cone[4];
varying vec4 v_color;
varying vec2 v_texcoord;
varying vec2 v_lightmapcoord;
varying vec3 v_normal;
varying vec3 v_tangent;
varying vec3 v_worldposition;
varying float v_shadowvisibility;
varying float v_viewdistance;

void main() {
    // The source sky uses a direction-sampled cubemap. VitaGL's fixed
    // function path does not implement GL_TEXTURE_CUBE_MAP enable state.
    if (u_surface_mode > 3.5 && u_surface_mode < 4.5
        && u_has_reflection_probe > 0.5) {
        gl_FragColor = vec4(textureCube(u_reflection_probe, normalize(v_worldposition)).rgb, 1.0);
        return;
    }
    vec4 material = v_color;
    if (u_has_color > 0.5)
        material *= texture2D(u_color, v_texcoord);
    float normalLength = dot(v_normal, v_normal);
    vec3 normal = normalLength > 0.000001
        ? normalize(v_normal) : vec3(0.0, 0.0, 1.0);
    vec3 sourceLightmapNormal = normal;
    vec2 sourceLightmapNormalXY = vec2(0.0, 0.0);
    // Keep the decoded tangent-space normal alive for the source secondary
    // lightmap equation below.  A missing/identity normal slot is the retail
    // flat tangent normal, not a platform-specific fallback.
    vec3 tangentNormal = vec3(0.0, 0.0, 1.0);
#if !defined(VITA_NO_NORMAL_MAP)
    if (u_has_normal > 0.5) {
        // COD4's normal images are DXT5nm: X in alpha and Y in green.
        vec4 encoded = texture2D(u_normal, v_texcoord);
        tangentNormal.xy = encoded.ag * 2.0 - 1.0;
        tangentNormal.z = sqrt(max(0.0, 1.0
            - dot(tangentNormal.xy, tangentNormal.xy)));
        vec3 tangentBasis = v_tangent - normal * dot(normal, v_tangent);
        float tangentLength = dot(tangentBasis, tangentBasis);
        vec3 tangent = tangentLength > 0.000001
            ? normalize(tangentBasis) : vec3(1.0, 0.0, 0.0);
        vec3 bitangent = normalize(cross(normal, tangent));
        // The retail lm_* shaders use the source packed-normal decode and
        // multiply only its XY perturbation by color alpha.  Their bumped
        // world normal is the geometric normal plus the tangent/binormal
        // perturbation; the reconstructed tangent-space Z is not added to
        // the geometric normal in this pass.
        sourceLightmapNormalXY = vec2(
            encoded.a * 4.07999992 - 2.07999992,
            encoded.g * 4.06451607 - 2.06451607) * clamp(material.a, 0.0, 1.0);
        sourceLightmapNormal = normalize(normal
            + tangent * sourceLightmapNormalXY.x
            + bitangent * sourceLightmapNormalXY.y);
        normal = normalize(tangent * tangentNormal.x
            + bitangent * tangentNormal.y + normal * tangentNormal.z);
    }

#endif
    if (u_alpha_test_enabled > 0.5) {
        if (u_alpha_test_func < 0.5 && material.a <= 0.0)
            discard;
        if (u_alpha_test_func >= 0.5 && u_alpha_test_func < 1.5
            && material.a >= 0.5)
            discard;
        if (u_alpha_test_func >= 1.5 && material.a < 0.5)
            discard;
    }

    vec3 lit = material.rgb;
    // KisakCOD's BSP lit techniques get their static world illumination from
    // the authored primary/secondary lightmap pair.  The frame/lightgrid
    // response below is for unlightmapped materials and model/indoor light;
    // applying it to both paths double-multiplies BSP lighting and crushes
    // the authored lightmap range.
    const bool usePrimaryLightmap = u_use_primary_lightmap > 0.5;
    const bool useSecondaryLightmap = u_use_secondary_lightmap > 0.5;
    const bool sourceLightmappedPass = usePrimaryLightmap
        || useSecondaryLightmap;
    const bool bakedLighting = u_baked_lighting > 0.5;
    // XModels arrive with the complete desktop source-light response already
    // multiplied into v_color. Preserve that response here; semantic normals
    // remain available for the separate specular contribution below.
    vec3 sun = vec3(0.0, 0.0, 1.0);
    float sunDot = 0.0;
    float packedShadowGrid = clamp(v_shadowvisibility, 0.0, 1.0)
        * 65535.0 + 0.5;
    packedShadowGrid = clamp(packedShadowGrid, 0.0, 65535.0);
    float shadowByte = floor(packedShadowGrid / 256.0);
    float localByte = packedShadowGrid - shadowByte * 256.0;
    float shadowVisibility = shadowByte / 255.0;
    bool hasLocalLightGridSunWeight = localByte < 254.5;
    float lightGridSunWeight = hasLocalLightGridSunWeight
        ? clamp(localByte / 253.0, 0.0, 1.0)
        : clamp(u_lightgrid_sun_weight, 0.0, 1.0);
    if (u_frame_valid > 0.5) {
        sun = normalize(u_sun_direction);
        sunDot = max(dot(normal, -sun), 0.0);
        float ambientMax = max(max(u_ambient_color.r, u_ambient_color.g),
                               u_ambient_color.b);
        float ambientDenom = max(ambientMax, 0.000001);
        float sunFloor = max(0.0, (u_sun_light - u_ambient_scale)
            * u_diffuse_fraction);
        float indoor = hasLocalLightGridSunWeight
            ? 1.0 : clamp(u_indoor_factor, 0.2, 1.0);
        vec3 response = (u_ambient_color / ambientDenom) * u_ambient_scale
            + u_diffuse_color * sunFloor
            + u_sun_color * sunDot * shadowVisibility;
        vec3 primaryLightResponse = vec3(0.0);
        // A projected source lightgrid sample already carries the local
        // colour ratio against the camera sample. Do not multiply the camera
        // grid a second time; that would flatten large maps toward one
        // global tint and erase covered-area contrast.
        vec3 frameGrid = hasLocalLightGridSunWeight
            ? vec3(1.0)
            : clamp(u_lightgrid_color, 0.0, 2.0);
        response *= (0.45 + 0.55 * lightGridSunWeight) * indoor * frameGrid;
#if !defined(VITA_SUN_ONLY)
        for (int lightIndex = 0; lightIndex < 16; ++lightIndex) {
            if (float(lightIndex) >= u_primary_light_count)
                break;
            if (float(lightIndex) == u_sun_primary_light_index)
                continue;
            vec4 directionType = u_primary_light_direction_type[lightIndex];
            vec4 lightColorExponent =
                u_primary_light_color_exponent[lightIndex];
            float lightType = directionType.w;
            float contribution = 0.0;
            if (lightType < 1.5
                || (lightType > 3.5 && lightType < 4.5)) {
                contribution = max(dot(normal, -normalize(directionType.xyz)),
                                    0.0);
            } else {
                vec4 positionRadius =
                    u_primary_light_position_radius[lightIndex];
                vec3 toLight = positionRadius.xyz - v_worldposition;
                float distanceSquared = dot(toLight, toLight);
                float radius = max(positionRadius.w, 0.0001);
                float distance = sqrt(max(distanceSquared, 0.000001));
                float falloff = clamp(1.0 - distance / radius, 0.0, 1.0);
                vec3 lightDirection = toLight / distance;
                float cone = 1.0;
                if (lightType < 2.5 || (lightType > 4.5
                                        && lightType < 5.5)) {
                    vec4 coneParams = u_primary_light_cone[lightIndex];
                    float spotCos = dot(normalize(directionType.xyz),
                                        normalize(-toLight));
                    float coneRange = max(coneParams.y - coneParams.x,
                                          0.0001);
                    cone = clamp((spotCos - coneParams.x) / coneRange,
                                 0.0, 1.0);
                    cone *= cone;
                }
                contribution = max(dot(normal, lightDirection), 0.0)
                    * falloff * cone;
            }
            if (!(lightType < 1.5
                  || (lightType > 3.5 && lightType < 4.5))) {
                float exponent = clamp(
                    1.0 + lightColorExponent.a * 0.25, 1.0, 8.0);
                contribution = pow(max(contribution, 0.0), exponent);
            }
            float localShadowVisibility = 1.0;
            if (lightType > 1.5) {
                // The Vita scene bridge stores the source collision-backed
                // visibility in the spare cone component. It is intentionally
                // separate from the per-fragment sun shadow byte.
                localShadowVisibility = clamp(
                    u_primary_light_cone[lightIndex].w, 0.0, 1.0);
            }
            primaryLightResponse += lightColorExponent.rgb
                * contribution * localShadowVisibility;
        }
        // FX light elements are transient authored scene lights. They use
        // the same source omni/spot equations as primary lights, but live in
        // a separate bounded array so map primary-light truncation cannot
        // silently discard a grenade flash or weather light.
        for (int lightIndex = 0; lightIndex < 4; ++lightIndex) {
            if (float(lightIndex) >= u_dynamic_light_count)
                break;
            vec4 directionType = u_dynamic_light_direction_type[lightIndex];
            vec4 lightColorExponent =
                u_dynamic_light_color_exponent[lightIndex];
            float lightType = directionType.w;
            vec4 positionRadius =
                u_dynamic_light_position_radius[lightIndex];
            vec3 toLight = positionRadius.xyz - v_worldposition;
            float distanceSquared = dot(toLight, toLight);
            float radius = max(positionRadius.w, 0.0001);
            float distance = sqrt(max(distanceSquared, 0.000001));
            float falloff = clamp(1.0 - distance / radius, 0.0, 1.0);
            vec3 lightDirection = toLight / distance;
            float contribution = max(dot(normal, lightDirection), 0.0)
                * falloff;
            if (lightType < 2.5) {
                vec4 coneParams = u_dynamic_light_cone[lightIndex];
                float spotCos = dot(normalize(directionType.xyz),
                                    normalize(-toLight));
                float coneRange = max(coneParams.y - coneParams.x,
                                      0.0001);
                float cone = clamp((spotCos - coneParams.x) / coneRange,
                                   0.0, 1.0);
                contribution *= cone * cone;
            }
            float exponent = clamp(
                1.0 + lightColorExponent.a * 0.25, 1.0, 8.0);
            contribution = pow(max(contribution, 0.0), exponent);
            primaryLightResponse += lightColorExponent.rgb * contribution
                * clamp(u_dynamic_light_cone[lightIndex].w, 0.0, 1.0);
        }
#endif
        response += primaryLightResponse;
        response = max(response, vec3(0.000001));
        if (!sourceLightmappedPass && !bakedLighting) {
            // Keep the source frame/lightgrid response off the authored BSP
            // lightmap path. A fixed readability floor here would make
            // covered world surfaces inherit exterior light.  Unlightmapped
            // material cards use the same bounded source model-lighting
            // bridge as the scalar path, retaining contrast without allowing
            // a valid but low indoor response to collapse to black.
            lit *= clamp(response, vec3(0.35), vec3(1.35));
        } else {
            // Non-sun primary lights are dynamic additions to the authored
            // lightmap/model response. Apply them even when the global
            // frame/lightgrid term is intentionally excluded to avoid
            // multiplying the baked BSP illumination twice.
            lit *= clamp(vec3(1.0) + primaryLightResponse,
                         vec3(0.0), vec3(1.35));
        }
#if !defined(VITA_NO_SPECULAR)
        if (u_has_specular > 0.5) {
            // lm_r0c0d0n0s0 uses the reflected view vector and the source
            // mask's alpha exponent. Keep the native shader on the same
            // source constants as the scalar fallback instead of using a
            // generic Blinn/Phong approximation.
            vec4 mask = texture2D(u_specular, v_texcoord);
            vec3 viewVector = u_camera_position - v_worldposition;
            float viewLength = dot(viewVector, viewVector);
            vec3 reflection = vec3(0.0);
            if (viewLength > 0.000001) {
                viewVector = normalize(viewVector);
                reflection = viewVector - 2.0 * dot(normal, viewVector) * normal;
            }
            float reflectionSun = dot(reflection, -sun);
            float specExponent = exp(clamp(mask.a, 0.0, 1.0) * 9.3775177)
                + 7.0;
            float specLog = (reflectionSun - 0.99925) * specExponent
                * 1.44269502;
            // Equivalent to specLog >= 0 ? 1 : exp(max(specLog, -80)).
            // The conditional form produces flashing per-triangle highlights
            // in the native GXM/Vita3K path. Clamp the exponent before exp so
            // the same authored lobe is bounded without that shader branch.
            float highlight = exp(clamp(specLog, -80.0, 0.0));
            lit += clamp(highlight * mask.rgb * u_sun_color
                         * u_specular_color_scale * shadowVisibility,
                         vec3(0.0), vec3(1.0));
            if (u_has_reflection_probe > 0.5 && viewLength > 0.000001) {
                float fresnel = pow(1.0 - max(dot(normal, viewVector), 0.0), 5.0);
                vec3 environment = textureCube(
                    u_reflection_probe, normalize(reflection)).rgb;
                // KisakCOD's reflectionProbeSampler is an authored
                // environment input to the lit material. Keep the bounded
                // response behind the source specular mask so a probe never
                // becomes a global map tint on matte surfaces.
                lit += clamp(environment * mask.rgb
                    * (0.04 + 0.16 * fresnel) * shadowVisibility,
                    vec3(0.0), vec3(1.0));
            }
        }
#endif
    }
    if (usePrimaryLightmap && useSecondaryLightmap) {
        // lm_*_sm2/sm3 stores two directional secondary lobes in the top and
        // bottom halves of the authored A8R8G8B8 atlas. The alpha channels
        // reconstruct the tangent-space lobe direction; RGB carries the
        // source bounce colours. Keep this equation identical to the scalar
        // oracle and the recovered KisakCOD shader instead of multiplying two
        // generic lightmap floors.
        float primary = texture2D(u_lightmap_primary, v_lightmapcoord).r;
        vec2 topCoord = vec2(v_lightmapcoord.x,
                             v_lightmapcoord.y * 0.5);
        vec2 bottomCoord = vec2(v_lightmapcoord.x,
                                v_lightmapcoord.y * 0.5 + 0.5);
        vec4 secondaryTop = texture2D(u_lightmap_secondary, topCoord);
        vec4 secondaryBottom = texture2D(u_lightmap_secondary, bottomCoord);
        float secondaryX = secondaryTop.a * 4.07999992 - 2.07999992;
        float secondaryY = secondaryBottom.a * 4.06451607 - 2.06451607;
        float secondaryInverseLength = 1.0 / sqrt(max(0.000001,
            secondaryX * secondaryX + secondaryY * secondaryY + 1.0));
        float normalInverseLength = 1.0 / sqrt(max(0.000001,
            sourceLightmapNormalXY.x * sourceLightmapNormalXY.x
                + sourceLightmapNormalXY.y * sourceLightmapNormalXY.y + 1.0));
        float alignment = clamp((secondaryX * sourceLightmapNormalXY.x
            + secondaryY * sourceLightmapNormalXY.y + 1.0)
            * secondaryInverseLength * normalInverseLength, 0.0, 1.0);
        vec3 secondaryRgb = secondaryTop.rgb * normalInverseLength
            + secondaryBottom.rgb * alignment;
        float sourceSunDot = u_frame_valid > 0.5
            ? max(dot(sourceLightmapNormal, sun), 0.0) : 0.0;
        vec3 staticLight = secondaryRgb
            + primary * u_sun_color * sourceSunDot * shadowVisibility;
        lit *= clamp(staticLight, vec3(0.0), vec3(1.35));
    } else {
        // Preserve the authored single-map variants for utility materials
        // whose source state does not bind the complete dual-lightmap pair.
        if (usePrimaryLightmap) {
            float primary = texture2D(u_lightmap_primary,
                                      v_lightmapcoord).r;
            float sourceSunDot = u_frame_valid > 0.5
                ? max(dot(sourceLightmapNormal, sun), 0.0) : 0.0;
            lit *= clamp(primary * u_sun_color * sourceSunDot
                * shadowVisibility, vec3(0.0), vec3(1.35));
        }
        if (useSecondaryLightmap) {
            vec2 topCoord = vec2(v_lightmapcoord.x,
                                 v_lightmapcoord.y * 0.5);
            vec2 bottomCoord = vec2(v_lightmapcoord.x,
                                    v_lightmapcoord.y * 0.5 + 0.5);
            vec4 secondaryTop = texture2D(u_lightmap_secondary, topCoord);
            vec4 secondaryBottom = texture2D(u_lightmap_secondary, bottomCoord);
            float secondaryX = secondaryTop.a * 4.07999992 - 2.07999992;
            float secondaryY = secondaryBottom.a * 4.06451607 - 2.06451607;
            float secondaryInverseLength = 1.0 / sqrt(max(0.000001,
                secondaryX * secondaryX + secondaryY * secondaryY + 1.0));
            float normalInverseLength = 1.0 / sqrt(max(0.000001,
                sourceLightmapNormalXY.x * sourceLightmapNormalXY.x
                    + sourceLightmapNormalXY.y * sourceLightmapNormalXY.y + 1.0));
            float alignment = clamp((secondaryX * sourceLightmapNormalXY.x
                + secondaryY * sourceLightmapNormalXY.y + 1.0)
                * secondaryInverseLength * normalInverseLength, 0.0, 1.0);
            vec3 secondary = secondaryTop.rgb * normalInverseLength
                + secondaryBottom.rgb * alignment;
            lit *= clamp(secondary, vec3(0.0), vec3(1.35));
        }
    }

#ifndef VITA_NO_FOG
    if (u_fog_enabled > 0.5) {
        float distance = max(v_viewdistance, 0.0);
        float fog = 1.0 - exp(-u_fog_density
            * max(distance - u_fog_start, 0.0));
        lit = mix(lit, u_fog_color, clamp(fog, 0.0, 1.0));
    }
#endif
    gl_FragColor = vec4(lit, material.a);
}
)VITA_SHADER";

// The retail postfx_color material consumes the resolved scene after all
// world, model, viewmodel, HUD, and effect batches. Keep the native path on
// the same source-space equation as ApplySoftwareFrameEffects instead of
// applying a map tint to individual geometry. The scene target is RGBA16F so
// the film shoulder and glow threshold see the same values that existed
// before final display quantisation.
static constexpr const char* kNativePostVertexShader = R"VITA_SHADER(
#version 100
attribute vec2 a_position;
attribute vec2 a_texcoord;
varying vec2 v_texcoord;

void main() {
    gl_Position = vec4(a_position, 0.0, 1.0);
    v_texcoord = a_texcoord;
}
)VITA_SHADER";

static constexpr const char* kNativePostFragmentShader = R"VITA_SHADER(
#version 100
precision highp float;
uniform sampler2D u_scene;
uniform sampler2D u_dither;
uniform float u_film_enabled;
uniform float u_exposure;
uniform float u_contrast;
uniform float u_brightness;
uniform float u_desaturation;
uniform float u_invert;
uniform float u_gamma;
uniform vec3 u_film_light_tint;
uniform vec3 u_film_dark_tint;
uniform float u_glow_enabled;
uniform float u_glow_cutoff;
uniform float u_glow_cutoff_rescale;
uniform float u_glow_desaturation;
uniform float u_glow_intensity;
uniform vec2 u_glow_offset;
uniform vec2 u_output_size;
varying vec2 v_texcoord;

float DisplayDither8x8(vec2 position) {
    // Point-sample the identical 64-value table. Avoid a divergent branch tree
    // over every pixel of the native display. Restore integer ranks exactly.
    vec2 uv = (floor(mod(position, 8.0)) + 0.5) / 8.0;
    return floor(texture2D(u_dither, uv).r * 255.0 + 0.5);
}

vec3 GlowSample(vec2 coordinate) {
    vec3 source = max(texture2D(u_scene, clamp(coordinate, 0.0, 1.0)).rgb,
                      vec3(0.0));
    float luminance = max(dot(source, vec3(0.2126, 0.7152, 0.0722)), 0.0);
    float mask = clamp(max(luminance - u_glow_cutoff, 0.0)
                           * u_glow_cutoff_rescale, 0.0, 1.0);
    vec3 grey = vec3(luminance);
    return max(mix(source, grey, u_glow_desaturation), vec3(0.0)) * mask;
}

vec3 ResolveGlow(vec2 coordinate) {
    // This is the bounded native equivalent of the source quarter-resolution
    // separable Gaussian. The center/cardinal/diagonal taps retain the same
    // thresholded source colour and use a normalized compact kernel. The
    // scalar oracle uses the full radius chain; the native path keeps the
    // authored radius in u_glow_offset so maps still widen/narrow their halo.
    vec2 offset = u_glow_offset;
    vec3 result = GlowSample(coordinate) * 0.20;
    result += GlowSample(coordinate + vec2(offset.x, 0.0)) * 0.12;
    result += GlowSample(coordinate - vec2(offset.x, 0.0)) * 0.12;
    result += GlowSample(coordinate + vec2(0.0, offset.y)) * 0.12;
    result += GlowSample(coordinate - vec2(0.0, offset.y)) * 0.12;
    result += GlowSample(coordinate + vec2(offset.x, offset.y)) * 0.06;
    result += GlowSample(coordinate + vec2(offset.x, -offset.y)) * 0.06;
    result += GlowSample(coordinate + vec2(-offset.x, offset.y)) * 0.06;
    result += GlowSample(coordinate - vec2(offset.x, offset.y)) * 0.06;
    result += GlowSample(coordinate + vec2(offset.x * 2.0, 0.0)) * 0.02;
    result += GlowSample(coordinate - vec2(offset.x * 2.0, 0.0)) * 0.02;
    result += GlowSample(coordinate + vec2(0.0, offset.y * 2.0)) * 0.02;
    result += GlowSample(coordinate - vec2(0.0, offset.y * 2.0)) * 0.02;
    return result;
}

void main() {
    vec3 source = texture2D(u_scene, v_texcoord).rgb;
    vec3 color = source * max(u_exposure, 0.0);
    if (u_film_enabled > 0.5) {
        float desaturationUnit = max(u_desaturation, 1.0 / 4096.0);
        float desaturationScale = 1.0 / desaturationUnit - 1.0;
        float tintScale = clamp(u_contrast, 0.0, 4.0) * desaturationUnit;
        float tintBias = u_brightness + 0.5
            - clamp(u_contrast, 0.0, 4.0) * 0.5;
        if (u_invert > 0.0) {
            tintScale = -tintScale;
            tintBias += 1.0;
        }
        vec3 tintBase = u_film_dark_tint * tintScale;
        vec3 tintDelta = (u_film_light_tint - u_film_dark_tint)
            * tintScale;
        float luminance = max(dot(color, vec3(0.2126, 0.7152, 0.0722)), 0.0);
        color = (color + luminance * desaturationScale)
            * (tintBase + luminance * tintDelta) + tintBias;
        color = clamp(color, 0.0, 1.0);
    }
    if (u_glow_enabled > 0.5)
        color += ResolveGlow(v_texcoord) * max(u_glow_intensity, 0.0);
    float displayExponent = 1.0 / clamp(u_gamma, 0.1, 4.0);
    color = pow(clamp(color, 0.0, 1.0), vec3(displayExponent));
    // gl_FragCoord is bottom-origin while the scalar display buffer indexes
    // rows from the top. Mirror only the pattern coordinate so native and
    // scalar final quantisation use the same 8x8 blue-noise table.
    vec2 ditherPosition = vec2(gl_FragCoord.x,
                               u_output_size.y - gl_FragCoord.y);
    float dither = (DisplayDither8x8(ditherPosition) - 31.5)
        / (64.0 * 255.0);
    gl_FragColor = vec4(clamp(color + vec3(dither), 0.0, 1.0), 1.0);
}
)VITA_SHADER";

void ApplySoftwareFrameEffects();
bool CreateNativePostResources(uint32_t width, uint32_t height);
void DestroyNativePostResources();
void BindNativeSceneTarget();
bool ApplyNativePostEffects();
void BindTexture(GfxTextureHandle texture);
void DrawIndexed(const GfxDrawVertex* vertices, uint32_t vertexCount,
                 const uint16_t* indices, uint32_t indexCount);

bool CompileNativeShader(GLenum type, const char* source, GLuint* output) {
    VITA_CPU_LOADING(type == GL_VERTEX_SHADER ? "shader-vertex-compile-or-cache" : "shader-fragment-compile-or-cache");
    if (!source || !output)
        return false;
    *output = 0;
    const GLuint shader = glCreateShader(type);
    if (!shader)
        return false;
    const GLchar* sourceList[] = {source};
    glShaderSource(shader, 1, sourceList, nullptr);
    glCompileShader(shader);
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLchar log[512]{};
        GLsizei logLength = 0;
        glGetShaderInfoLog(shader, sizeof(log), &logLength, log);
        sceClibPrintf("[kisak-vita] native lit shader compile failed type=%u: %s\\n",
                      static_cast<unsigned>(type), log);
        glDeleteShader(shader);
        return false;
    }
    *output = shader;
    return true;
}

struct NativeLightmapLayout {
    GLuint original=0;void* pixels=nullptr;uint32_t bytes=0,width=0,height=0,bpp=0;
    SceGxmTextureFormat format{};kisak::vita::VitaSha256::Digest hash{};bool swizzled=false;
};
std::array<NativeLightmapLayout,8> g_lightmapLayouts{};
alignas(64) uint8_t g_lightmapWorkspace[kGfxVitaLightmapWorkspaceBytes]{};
GfxVitaLightmapTransitionClock g_lightmapTransitionClock;
std::atomic<bool> g_lightmapLayoutFault{false};
uint32_t g_lightmapLayoutBytes=0,g_lightmapLayoutFailures=0;
uint32_t g_lightmapLayoutActual=0,g_lightmapLayoutEligible=0,g_lightmapLayoutInvalid=0;
bool NativeUseLightmapLayout() {return g_renderPassProbe.lightmapLayout && g_renderPassProbe.phase==2;}
GLuint NativeLightmapTexture(uint32_t lease) {return NativeTexture(lease);}
// Called only when the final texture lease is destroyed, or after restoration
// before a new registration. VitaGL alone owns/frees the original pixels.
void DestroyNativeLightmapLayout(GLuint original) {
    for(auto& item:g_lightmapLayouts)if(item.original && (!original || item.original==original)) {
        g_lightmapLayoutBytes-=item.bytes;item={};
        if(original && !g_lightmapLayoutBytes)g_lightmapLayoutFault=false;
    }
}
bool NativeLightmapFailure(uint32_t stage,GLuint original,int error=0) {
    ++g_lightmapLayoutFailures;g_lightmapLayoutFault=true;
    kisak::vita::VitaCpuProfile_Counter("lightmap-layout-failure",stage,original);
    kisak::vita::VitaCpuProfile_Counter("lightmap-layout-failure-error",uint32_t(error),g_lightmapLayoutBytes);
    for(unsigned pool=0;pool<unsigned(VGL_MEM_EXTERNAL);++pool) {
        char name[64];std::snprintf(name,sizeof(name),"lightmap-layout-pool-%u",pool);
        kisak::vita::VitaCpuProfile_Counter(name,vglMemFree(vglMemType(pool)),vglMemTotal(vglMemType(pool)));
    }
    return false;
}
bool PrepareNativeLightmapLayout(GfxTextureHandle texture) {
    VITA_CPU_LOADING("lightmap-layout-register");
    const GLuint original=NativeTexture(texture.value);if(!original)return NativeLightmapFailure(1,0);
    for(const auto& item:g_lightmapLayouts)if(item.original==original)return true;
    NativeLightmapLayout* slot=nullptr;for(auto& item:g_lightmapLayouts)if(!item.original){slot=&item;break;}
    GLint active=0,bound=0;glGetIntegerv(GL_ACTIVE_TEXTURE,&active);glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&bound);glBindTexture(GL_TEXTURE_2D,original);
    const auto source=*vglGetGxmTexture(GL_TEXTURE_2D);const auto format=sceGxmTextureGetFormat(&source);
    const auto base=uint32_t(format)&0xff000000u;
    const uint32_t bpp=base==SCE_GXM_TEXTURE_BASE_FORMAT_U8?1u:base==SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8?4u:0u;
    const auto width=sceGxmTextureGetWidth(&source),height=sceGxmTextureGetHeight(&source);
    const uint64_t bytes=uint64_t(width)*height*bpp;
    void* pixels=vglGetTexDataPointer(GL_TEXTURE_2D);
    SceKernelMemBlockInfo memory{};memory.size=sizeof(memory);
    const int memoryResult=pixels?sceKernelGetMemBlockInfoByAddr(pixels,&memory):-1;
    const bool coherent=memoryResult==0 && (uint32_t(memory.type)==SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW
        || uint32_t(memory.type)==SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW);
    const bool valid=coherent && slot && pixels && pixels==sceGxmTextureGetData(&source)
        && GfxVitaLightmapDimensions(width,height,bpp)
        && (uint64_t(width)*height+7)/8<=sizeof(g_lightmapWorkspace)
        && bytes<=8u*1024*1024-g_lightmapLayoutBytes
        && sceGxmTextureGetType(&source)==SCE_GXM_TEXTURE_LINEAR
        && NativeResourceSize(g_nativeTextures,original)>=bytes && vglMallocUsableSize(pixels)>=bytes;
    if(valid) {
        *slot={original,pixels,uint32_t(bytes),width,height,bpp,format,GfxVitaLightmapHash(pixels,width,height,bpp,false),false};
        g_lightmapLayoutBytes+=uint32_t(bytes);
        kisak::vita::VitaCpuProfile_Counter("lightmap-layout-registered",original,bytes);
        kisak::vita::VitaCpuProfile_Counter("lightmap-layout-memory-types",uint32_t(memory.type),uint32_t(memory.type));
    }
    glBindTexture(GL_TEXTURE_2D,bound);glActiveTexture(active);EndNativeStaticPipeline();
    return valid || NativeLightmapFailure(2,original);
}
bool SetNativeLightmapLayout(bool swizzled) {
    bool needed=false;for(const auto& item:g_lightmapLayouts)needed|=item.original && item.swizzled!=swizzled;
    if(!needed)return !g_lightmapLayoutFault.load();
    const uint64_t start=sceKernelGetProcessTimeWide();g_lightmapTransitionClock.Begin(start);
    // Sole mutation boundary: finish prior draws before touching any texel.
    // This runs before BeginFrame/probe timers; the source owner keeps ticking.
    glFinish();EndNativeStaticPipeline();
    GLint active=0,bound=0;glGetIntegerv(GL_ACTIVE_TEXTURE,&active);glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&bound);bool ok=true;uint32_t converted=0;
    for(auto& item:g_lightmapLayouts)if(item.original && item.swizzled!=swizzled) {
        glBindTexture(GL_TEXTURE_2D,item.original);auto* current=vglGetGxmTexture(GL_TEXTURE_2D);
        if(vglGetTexDataPointer(GL_TEXTURE_2D)!=item.pixels || sceGxmTextureGetData(current)!=item.pixels
            || sceGxmTextureGetFormat(current)!=item.format || sceGxmTextureGetWidth(current)!=item.width
            || sceGxmTextureGetHeight(current)!=item.height
            || sceGxmTextureGetType(current)!=(item.swizzled?SCE_GXM_TEXTURE_SWIZZLED:SCE_GXM_TEXTURE_LINEAR)) {
            ok=NativeLightmapFailure(3,item.original);break;
        }
        if(GfxVitaLightmapHash(item.pixels,item.width,item.height,item.bpp,item.swizzled)!=item.hash) {
            ok=NativeLightmapFailure(4,item.original);break;
        }
        SceGxmTexture next{};
        int error=swizzled?sceGxmTextureInitSwizzled(&next,item.pixels,item.format,item.width,item.height,1)
            :sceGxmTextureInitLinear(&next,item.pixels,item.format,item.width,item.height,1);
        error|=sceGxmTextureSetMinFilter(&next,SCE_GXM_TEXTURE_FILTER_LINEAR);
        error|=sceGxmTextureSetMagFilter(&next,SCE_GXM_TEXTURE_FILTER_LINEAR);
        error|=sceGxmTextureSetMipFilter(&next,SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
        error|=sceGxmTextureSetUAddrMode(&next,SCE_GXM_TEXTURE_ADDR_CLAMP);
        error|=sceGxmTextureSetVAddrMode(&next,SCE_GXM_TEXTURE_ADDR_CLAMP);
        error|=sceGxmTextureSetLodBias(&next,sceGxmTextureGetLodBias(current));
        error|=sceGxmTextureSetGammaMode(&next,sceGxmTextureGetGammaMode(current));
        if(error){ok=NativeLightmapFailure(5,item.original,error);break;}
        if(!GfxVitaReorderLightmap(item.pixels,item.bytes,item.width,item.height,item.bpp,swizzled,g_lightmapWorkspace,sizeof(g_lightmapWorkspace))) {
            ok=NativeLightmapFailure(6,item.original);break;
        }
        // Publish the descriptor only after the entire permutation completes.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        *current=next;item.swizzled=swizzled;
        const bool verified=GfxVitaLightmapHash(item.pixels,item.width,item.height,item.bpp,swizzled)==item.hash;
        kisak::vita::VitaCpuProfile_Counter("lightmap-layout-content-verified",item.original,verified?item.bytes:0);
        kisak::vita::VitaCpuProfile_Counter("lightmap-layout-content-state",item.original,swizzled);
        if(!verified){ok=NativeLightmapFailure(7,item.original);break;}
        ++converted;
    }
    glBindTexture(GL_TEXTURE_2D,bound);glActiveTexture(active);EndNativeStaticPipeline();
    const uint64_t end=sceKernelGetProcessTimeWide();g_lightmapTransitionClock.End(end);
    kisak::vita::VitaCpuProfile_Counter("lightmap-layout-transition-us",end-start,swizzled);
    kisak::vita::VitaCpuProfile_Counter("lightmap-layout-transition-valid",ok,converted);
    return ok;
}
void RecordNativeLightmapLayout(GfxTextureHandle primary,GfxTextureHandle secondary) {
    if(!g_renderPassProbe.lightmapLayout || !g_nativeWorldSpaceDraw || g_nativeViewModelSpaceDraw)return;
    GLint active=0;glGetIntegerv(GL_ACTIVE_TEXTURE,&active);
    const GfxTextureHandle handles[]={primary,secondary};
    for(unsigned i=0;i<2;++i)if(handles[i].value) {
        ++g_lightmapLayoutEligible;glActiveTexture(GL_TEXTURE3+i);
        GLint bound=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&bound);
        const GLuint original=NativeTexture(handles[i].value),expected=NativeLightmapTexture(handles[i].value);
        const SceGxmTexture* actual=vglGetGxmTexture(GL_TEXTURE_2D);
        const bool changed=bound && sceGxmTextureGetType(actual)==SCE_GXM_TEXTURE_SWIZZLED;
        g_lightmapLayoutActual+=changed;
        const auto type=NativeUseLightmapLayout()?SCE_GXM_TEXTURE_SWIZZLED:SCE_GXM_TEXTURE_LINEAR;
        if(!bound || GLuint(bound)!=expected || changed!=NativeUseLightmapLayout()
            || sceGxmTextureGetType(actual)!=type || sceGxmTextureGetMipmapCount(actual)!=1
            || sceGxmTextureGetMinFilter(actual)!=SCE_GXM_TEXTURE_FILTER_LINEAR
            || sceGxmTextureGetMagFilter(actual)!=SCE_GXM_TEXTURE_FILTER_LINEAR
            || sceGxmTextureGetMipFilter(actual)!=SCE_GXM_TEXTURE_MIP_FILTER_DISABLED
            || sceGxmTextureGetUAddrMode(actual)!=SCE_GXM_TEXTURE_ADDR_CLAMP
            || sceGxmTextureGetVAddrMode(actual)!=SCE_GXM_TEXTURE_ADDR_CLAMP)++g_lightmapLayoutInvalid;
    }
    glActiveTexture(active);
}

// Bounded metadata only; no disk I/O, allocation or GPU completion fence.
// Counters use renderer-owned event queues, never the source-only text sink.
alignas(16) uint8_t g_resourceAuditBinary[128*1024]{};
GfxVitaResourceAuditGate g_resourceAuditGate{};
void NativeAuditCounter(const char* key,const char* field,uint64_t a,uint64_t b=0) {
    char name[96];std::snprintf(name,sizeof(name),"gpu-audit-%s-%s",key,field);
    kisak::vita::VitaCpuProfile_Counter(name,a,b);
}
void NativeAuditHash(const char* key,const char* field,const kisak::vita::VitaSha256::Digest& hash) {
    for(unsigned i=0;i<2;++i) {
        uint64_t a=0,b=0;
        for(unsigned j=0;j<8;++j){a|=uint64_t(hash[i*16+j])<<(j*8);b|=uint64_t(hash[i*16+8+j])<<(j*8);}
        char name[24];std::snprintf(name,sizeof(name),"%s%u",field,i);
        NativeAuditCounter(key,name,a,b);
    }
}
void NativeAuditProgram(GLuint program,unsigned id) {
    GLint size=0;glGetProgramiv(program,GL_PROGRAM_BINARY_LENGTH,&size);
    char key[24];std::snprintf(key,sizeof(key),"p%u",id);
    // Pinned VitaGL ignores bufSize when serializing: check exact queried
    // length against preallocated capacity before calling it on its owner.
    if(size<=0 || size>int(sizeof(g_resourceAuditBinary))) {NativeAuditCounter(key,"unavailable",1,size>0?size:0);return;}
    GLsizei written=0;GLenum format=0;
    glGetProgramBinary(program,size,&written,&format,g_resourceAuditBinary);
    GfxVitaGxpAudit stages[2]{};
    const size_t prefix=sizeof(GLuint)+16*sizeof(SceGxmVertexAttribute);
    if(written!=size || !GfxVitaParseProgramAudit(g_resourceAuditBinary,written,prefix,stages)) {
        NativeAuditCounter(key,"unavailable",2,written>0?written:0);return;
    }
    NativeAuditCounter(key,"program",program,written);
    for(unsigned i=0;i<2;++i) {
        const auto& a=stages[i];std::snprintf(key,sizeof(key),"p%u-%c",id,i?'f':'v');
        NativeAuditCounter(key,"size-flags",a.size,a.flags);
        NativeAuditCounter(key,"registers",uint64_t(a.primaryRegisters)|(uint64_t(a.secondaryRegisters)<<32),
            uint64_t(a.temporaryRegisters)|(uint64_t(a.secondaryTemporaryRegisters)<<32));
        NativeAuditCounter(key,"instructions",a.primaryInstructions,a.secondaryInstructions);
        NativeAuditCounter(key,"buffers",a.scratchBuffers,a.threadBuffers);
        NativeAuditCounter(key,"texture-flags",a.textureFlags0,a.textureFlags1);
        NativeAuditCounter(key,"compiler",a.compilerVersion,1);
        NativeAuditHash(key,"binary-hash",kisak::vita::VitaSha256::Hash(a.bytes,a.size));
        kisak::vita::VitaSha256 code;
        if(a.primaryInstructions)code.Update(a.bytes+a.primaryOffset,size_t(a.primaryInstructions)*8);
        if(a.secondaryInstructions)code.Update(a.bytes+a.secondaryOffset,size_t(a.secondaryInstructions)*8);
        NativeAuditHash(key,"code-hash",code.Finish());
    }
}
void NativeAuditWorldTextures(GfxTextureHandle color,GfxTextureHandle primary,GfxTextureHandle secondary) {
    if(!g_renderPassProbe.worldScaling && !g_renderPassProbe.worldPrecision && !g_renderPassProbe.worldLifetime && !g_renderPassProbe.lightmapLayout && !g_renderPassProbe.worldMath && !g_renderPassProbe.worldBasis && !g_renderPassProbe.worldGloss && !g_renderPassProbe.worldComponents)return;
    const auto context=kisak::vita::VitaCpuProfile_Context();
    const unsigned phase=g_renderPassProbe.phase,surface=g_materialPass.sourceSurfaceIndex;
    if(!g_resourceAuditGate.Take(context.generation,phase,surface,
        g_nativeStaticBufferDraw && g_nativeWorldSpaceDraw && !g_nativeViewModelSpaceDraw && !g_nativeStaticInstancedDraw,g_renderPassProbe.worldScaling?7u:g_renderPassProbe.worldComponents?5u:3u))return;
    const GfxTextureHandle handles[]={color,g_materialPass.normalTexture,g_materialPass.specularTexture,
        primary,secondary,g_materialPass.reflectionProbeTexture};
    GLint active=0,program=0;glGetIntegerv(GL_ACTIVE_TEXTURE,&active);glGetIntegerv(GL_CURRENT_PROGRAM,&program);
    const unsigned component=g_renderPassProbe.worldComponents && NativeWorldComponentEligible(NativeWorldAlphaEligible(true,false))?GfxVitaWorldComponentVariant(phase):0;
    const GLuint expected=g_renderPassProbe.worldScaling && phase==4 && g_cheapWorldReady==3?g_nativeCheapWorld[surface==588?1:0].program:g_renderPassProbe.worldPrecision && phase==2?g_nativeWorldPrecision[surface==588?1:0].program:component?g_nativeWorldComponents[component-1].program:g_renderPassProbe.worldGloss && phase==2 && NativeWorldGlossEligible(NativeWorldLifetimeEligible(true,false))?g_nativeWorldGloss.program:g_renderPassProbe.worldBasis && phase==2 && g_worldBasisState.active?g_nativeWorldBasis[surface==588?1:0].program:g_renderPassProbe.worldMath && phase==2?g_nativeWorldMath[surface==588?1:0].program:surface==588?g_nativeWorldAlpha.program:
        (g_renderPassProbe.worldLifetime && phase==2)?g_nativeWorldLifetime.program:g_nativeMaterialFamily[0].program;
    char key[32];std::snprintf(key,sizeof(key),"s%u-p%u",surface,phase);
    NativeAuditCounter(key,"program",program,expected);
    if(g_renderPassProbe.worldBasis)NativeAuditCounter(key,"basis",g_worldBasisState.active,1);
    if(g_renderPassProbe.worldGloss)NativeAuditCounter(key,"gloss",g_nativeTexturePool.Certificate(g_materialPass.specularTexture.value),233);
    if(g_renderPassProbe.worldComponents)NativeAuditCounter(key,"component",component,NativeWorldComponentEligible(NativeWorldAlphaEligible(true,false)));
    for(unsigned unit=0;unit<6;++unit) {
        glActiveTexture(GL_TEXTURE0+unit);
        const GLenum target=unit==5?GL_TEXTURE_CUBE_MAP:GL_TEXTURE_2D;
        GLint bound=0;glGetIntegerv(unit==5?GL_TEXTURE_BINDING_CUBE_MAP:GL_TEXTURE_BINDING_2D,&bound);
        std::snprintf(key,sizeof(key),"s%u-p%u-u%u",surface,phase,unit);
        const GLint locations[]={g_nativeLitShader.colorSampler,g_nativeLitShader.normalSampler,g_nativeLitShader.specularSampler,
            g_nativeLitShader.lightmapPrimarySampler,g_nativeLitShader.lightmapSecondarySampler,g_nativeLitShader.reflectionProbeSampler};
        NativeAuditCounter(key,"binding",bound,locations[unit]<0?0:g_nativeTexturePool.Resolve(handles[unit].value));
        if(!bound){NativeAuditCounter(key,"unavailable",1);continue;}
        const SceGxmTexture* texture=vglGetGxmTexture(target);
        if(!texture || !sceGxmTextureGetData(texture)){NativeAuditCounter(key,"unavailable",2);continue;}
        // Query the actual hardware descriptor after normal draw binding.
        NativeAuditCounter(key,"dimensions",sceGxmTextureGetWidth(texture),sceGxmTextureGetHeight(texture));
        NativeAuditCounter(key,"format-type",sceGxmTextureGetFormat(texture),sceGxmTextureGetType(texture));
        NativeAuditCounter(key,"filter",sceGxmTextureGetMinFilter(texture),sceGxmTextureGetMagFilter(texture));
        NativeAuditCounter(key,"mips",sceGxmTextureGetMipmapCount(texture),sceGxmTextureGetMipFilter(texture));
        NativeAuditCounter(key,"address",sceGxmTextureGetUAddrMode(texture),sceGxmTextureGetVAddrMode(texture));
        NativeAuditCounter(key,"lod-gamma",sceGxmTextureGetLodBias(texture),sceGxmTextureGetGammaMode(texture));
    }
    glActiveTexture(active);
}

bool CreateNativeLitShaderProgram(NativeLitShaderState& shader, const char* fragmentSource,
                                  const char* vertexSource=kNativeLitVertexShader) {
    VITA_CPU_LOADING("scene-program-create");
    if (shader.program != 0)
        return true;
    GLuint vertexShader = 0;
    GLuint fragmentShader = 0;
    if (!CompileNativeShader(GL_VERTEX_SHADER, vertexSource,
                             &vertexShader)
        || !CompileNativeShader(GL_FRAGMENT_SHADER, fragmentSource,
                                &fragmentShader)) {
        if (vertexShader)
            glDeleteShader(vertexShader);
        return false;
    }
    const GLuint program = glCreateProgram();
    if (!program) {
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return false;
    }
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glBindAttribLocation(program, 0, "a_position");
    glBindAttribLocation(program, 1, "a_color");
    glBindAttribLocation(program, 2, "a_texcoord");
    glBindAttribLocation(program, 3, "a_lightmapcoord");
    glBindAttribLocation(program, 4, "a_normal");
    glBindAttribLocation(program, 5, "a_tangent");
    glBindAttribLocation(program, 6, "a_worldposition");
    glBindAttribLocation(program, 7, "a_shadowvisibility");
    glBindAttribLocation(program, 8, "a_instance_rotation");
    glBindAttribLocation(program, 9, "a_instance_origin_scale");
    glBindAttribLocation(program, 10, "a_instance_light_pos_x");
    glBindAttribLocation(program, 11, "a_instance_light_neg_x");
    glBindAttribLocation(program, 12, "a_instance_light_pos_y");
    glBindAttribLocation(program, 13, "a_instance_light_neg_y");
    glBindAttribLocation(program, 14, "a_instance_light_pos_z");
    glBindAttribLocation(program, 15, "a_instance_light_neg_z");
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLchar log[512]{};
        GLsizei logLength = 0;
        glGetProgramInfoLog(program, sizeof(log), &logLength, log);
        sceClibPrintf("[kisak-vita] native lit shader link failed: %s\\n", log);
        glDeleteProgram(program);
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return false;
    }
    if(&shader==&g_nativeMaterialFamily[0])NativeAuditProgram(program,1);
    else if(&shader==&g_nativeWorldLifetime)NativeAuditProgram(program,2);
    else if(&shader==&g_nativeWorldAlpha)NativeAuditProgram(program,3);
    else if(&shader==&g_nativeWorldMath[0])NativeAuditProgram(program,4);
    else if(&shader==&g_nativeWorldMath[1])NativeAuditProgram(program,5);
    else if(&shader==&g_nativeWorldBasis[0])NativeAuditProgram(program,6);
    else if(&shader==&g_nativeWorldBasis[1])NativeAuditProgram(program,7);
    else if(&shader==&g_nativeWorldGloss)NativeAuditProgram(program,8);
    else if(&shader==&g_nativeWorldComponents[0])NativeAuditProgram(program,11);
    else if(&shader==&g_nativeWorldComponents[1])NativeAuditProgram(program,12);
    else if(&shader==&g_nativeWorldPrecision[0])NativeAuditProgram(program,13);
    else if(&shader==&g_nativeWorldPrecision[1])NativeAuditProgram(program,14);
    else if(&shader==&g_nativeCheapWorld[0])NativeAuditProgram(program,15);
    else if(&shader==&g_nativeCheapWorld[1])NativeAuditProgram(program,16);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    shader.program = program;
    shader.colorSampler = glGetUniformLocation(program, "u_color");
    shader.hasColor = glGetUniformLocation(program, "u_has_color");
    shader.normalSampler = glGetUniformLocation(program, "u_normal");
    shader.specularSampler = glGetUniformLocation(program, "u_specular");
    shader.reflectionProbeSampler = glGetUniformLocation(
        program, "u_reflection_probe");
    shader.lightmapPrimarySampler = glGetUniformLocation(
        program, "u_lightmap_primary");
    shader.lightmapSecondarySampler = glGetUniformLocation(
        program, "u_lightmap_secondary");
    shader.hasNormal = glGetUniformLocation(program, "u_has_normal");
    shader.hasSpecular = glGetUniformLocation(program, "u_has_specular");
    shader.hasReflectionProbe = glGetUniformLocation(
        program, "u_has_reflection_probe");
    shader.usePrimaryLightmap = glGetUniformLocation(
        program, "u_use_primary_lightmap");
    shader.useSecondaryLightmap = glGetUniformLocation(
        program, "u_use_secondary_lightmap");
    shader.surfaceMode = glGetUniformLocation(program, "u_surface_mode");
    shader.alphaTestEnabled = glGetUniformLocation(
        program, "u_alpha_test_enabled");
    shader.alphaTestFunc = glGetUniformLocation(
        program, "u_alpha_test_func");
    shader.bakedLighting = glGetUniformLocation(
        program, "u_baked_lighting");
    shader.frameValid = glGetUniformLocation(program, "u_frame_valid");
    shader.cameraPosition = glGetUniformLocation(
        program, "u_camera_position");
    shader.sunDirection = glGetUniformLocation(
        program, "u_sun_direction");
    shader.sunColor = glGetUniformLocation(program, "u_sun_color");
    shader.ambientColor = glGetUniformLocation(
        program, "u_ambient_color");
    shader.diffuseColor = glGetUniformLocation(
        program, "u_diffuse_color");
    shader.ambientScale = glGetUniformLocation(
        program, "u_ambient_scale");
    shader.diffuseFraction = glGetUniformLocation(
        program, "u_diffuse_fraction");
    shader.sunLight = glGetUniformLocation(program, "u_sun_light");
    shader.specularColorScale = glGetUniformLocation(
        program, "u_specular_color_scale");
    shader.lightGridColor = glGetUniformLocation(
        program, "u_lightgrid_color");
    shader.lightGridSunWeight = glGetUniformLocation(
        program, "u_lightgrid_sun_weight");
    shader.indoorFactor = glGetUniformLocation(
        program, "u_indoor_factor");
    shader.fogEnabled = glGetUniformLocation(program, "u_fog_enabled");
    shader.fogStart = glGetUniformLocation(program, "u_fog_start");
    shader.fogDensity = glGetUniformLocation(
        program, "u_fog_density");
    shader.fogColor = glGetUniformLocation(program, "u_fog_color");
    shader.primaryLightCount = glGetUniformLocation(
        program, "u_primary_light_count");
    shader.sunPrimaryLightIndex = glGetUniformLocation(
        program, "u_sun_primary_light_index");
    shader.primaryLightPositionRadius = glGetUniformLocation(
        program, "u_primary_light_position_radius");
    shader.primaryLightDirectionType = glGetUniformLocation(
        program, "u_primary_light_direction_type");
    shader.primaryLightColorExponent = glGetUniformLocation(
        program, "u_primary_light_color_exponent");
    shader.primaryLightCone = glGetUniformLocation(
        program, "u_primary_light_cone");
    shader.dynamicLightCount = glGetUniformLocation(
        program, "u_dynamic_light_count");
    shader.dynamicLightPositionRadius = glGetUniformLocation(
        program, "u_dynamic_light_position_radius");
    shader.dynamicLightDirectionType = glGetUniformLocation(
        program, "u_dynamic_light_direction_type");
    shader.dynamicLightColorExponent = glGetUniformLocation(
        program, "u_dynamic_light_color_exponent");
    shader.dynamicLightCone = glGetUniformLocation(
        program, "u_dynamic_light_cone");
    shader.worldSpace = glGetUniformLocation(
        program, "u_world_space");
    shader.viewModelSpace = glGetUniformLocation(
        program, "u_viewmodel_space");
    shader.staticInstanced = glGetUniformLocation(
        program, "u_static_instanced");
    shader.worldViewProjection = glGetUniformLocation(
        program, "u_world_view_projection");
    shader.worldNearPlane = glGetUniformLocation(
        program, "u_world_near_plane");

    glUseProgram(program);
    if (shader.colorSampler >= 0)
        glUniform1i(shader.colorSampler, 0);
    if (shader.normalSampler >= 0)
        glUniform1i(shader.normalSampler, 1);
    if (shader.specularSampler >= 0)
        glUniform1i(shader.specularSampler, 2);
    if (shader.reflectionProbeSampler >= 0)
        glUniform1i(shader.reflectionProbeSampler, 5);
    if (shader.lightmapPrimarySampler >= 0)
        glUniform1i(shader.lightmapPrimarySampler, 3);
    if (shader.lightmapSecondarySampler >= 0)
        glUniform1i(shader.lightmapSecondarySampler, 4);
    glUseProgram(0);
    sceClibPrintf("[kisak-vita] native source-lit shader ready program=%u\\n",
                  static_cast<unsigned>(program));
    return true;
}

bool CreateNativeLitShader() {
    if (!CreateNativeLitShaderProgram(g_nativeFullLitShader, kNativeLitFragmentShader))
        return false;
    std::string sunOnlySource(kNativeLitFragmentShader);
    const auto versionEnd = sunOnlySource.find('\n', sunOnlySource.find("#version"));
    sunOnlySource.insert(versionEnd + 1, "#define VITA_SUN_ONLY 1\n");
    if (!CreateNativeLitShaderProgram(g_nativeSunOnlyShader, sunOnlySource.c_str())) {
        glDeleteProgram(g_nativeFullLitShader.program);
        g_nativeFullLitShader = {};
        return false;
    }
#if KISAK_VITA_CPU_RUNTIME
    // Built once at startup and kept across maps. If compilation fails, keep
    // the proven generic program; never compile or allocate shaders in a draw.
    std::string propVertex,propFragment;
    const bool propReady=GfxVitaBuildPropShaders(kNativeLitVertexShader,kNativeLitFragmentShader,propVertex,propFragment)
        && CreateNativeLitShaderProgram(g_nativePropShader,propFragment.c_str(),propVertex.c_str());
    kisak::vita::VitaCpuProfile_Counter(propReady?"prop-shader-ready":"prop-shader-unavailable",g_nativePropShader.program);
    std::string opaqueFragment;
    const bool opaqueFull=GfxVitaBuildOpaqueShader(kNativeLitFragmentShader,false,opaqueFragment)
        && CreateNativeLitShaderProgram(g_nativeOpaqueFullShader,opaqueFragment.c_str());
    const bool opaqueSun=GfxVitaBuildOpaqueShader(kNativeLitFragmentShader,true,opaqueFragment)
        && CreateNativeLitShaderProgram(g_nativeOpaqueSunShader,opaqueFragment.c_str());
    kisak::vita::VitaCpuProfile_Counter(opaqueFull && opaqueSun?"opaque-shader-ready":"opaque-shader-unavailable",
        g_nativeOpaqueFullShader.program,g_nativeOpaqueSunShader.program);
    g_earlyAlphaReady=0;
    for(unsigned light=0;light<2;++light) {
        if(GfxVitaBuildEarlyAlpha(kNativeLitFragmentShader,light!=0,opaqueFragment)
            && CreateNativeLitShaderProgram(g_nativeEarlyAlpha[light],opaqueFragment.c_str()))g_earlyAlphaReady|=1u<<light;
    }
    kisak::vita::VitaCpuProfile_Counter("early-alpha-ready",g_earlyAlphaReady,3);
    CreateNativeLitShaderProgram(g_nativeHudShader,kVitaHudFragment,kVitaHudVertex);
    kisak::vita::VitaCpuProfile_Counter("hud-shader-ready",g_nativeHudShader.program,0);
    CreateNativeLitShaderProgram(g_nativeFastColor,kVitaFastColorFragment);
    CreateNativeLitShaderProgram(g_nativeFastClear,kVitaFastClearFragment);
    kisak::vita::VitaCpuProfile_Counter("fast-color-ready",g_nativeFastColor.program,g_nativeFastClear.program);
    g_worldPrecisionReady=0;
    for(unsigned lane=0;lane<2;++lane)
        if(GfxVitaBuildWorldPrecision(kNativeLitFragmentShader,lane!=0,opaqueFragment)
            && CreateNativeLitShaderProgram(g_nativeWorldPrecision[lane],opaqueFragment.c_str()))g_worldPrecisionReady|=1u<<lane;
    kisak::vita::VitaCpuProfile_Counter("world-precision-ready",g_worldPrecisionReady,3);
    kisak::vita::VitaCpuShaderCache_SetWorldPrecisionReady(g_worldPrecisionReady==3);
    g_cheapWorldReady=0;
    for(unsigned lane=0;lane<2;++lane) {
        if(GfxVitaBuildCheapWorld(kNativeLitFragmentShader,lane!=0,opaqueFragment)
            && CreateNativeLitShaderProgram(g_nativeCheapWorld[lane],opaqueFragment.c_str()))g_cheapWorldReady|=1u<<lane;
    }
    kisak::vita::VitaCpuProfile_Counter("world-scaling-shader-ready",g_cheapWorldReady,3);
    kisak::vita::VitaCpuShaderCache_SetWorldScalingReady(g_cheapWorldReady==3);
    g_worldComponentsReady=0;
    for(unsigned lane=0;lane<2;++lane)
        if(GfxVitaBuildWorldSpecularSplit(kNativeLitFragmentShader,lane+1,opaqueFragment)
            && CreateNativeLitShaderProgram(g_nativeWorldComponents[lane],opaqueFragment.c_str()))g_worldComponentsReady|=1u<<lane;
    kisak::vita::VitaCpuProfile_Counter("world-specular-split-ready",g_worldComponentsReady,3);
    const bool glossReady=GfxVitaBuildWorldGloss(kNativeLitFragmentShader,opaqueFragment)
        && CreateNativeLitShaderProgram(g_nativeWorldGloss,opaqueFragment.c_str());
    kisak::vita::VitaCpuProfile_Counter("world-gloss-ready",glossReady,233);
    g_worldBasisReady=0;
    std::string basisVertex;
    for(unsigned lane=0;lane<2;++lane)
        if(GfxVitaBuildWorldBasis(kNativeLitVertexShader,kNativeLitFragmentShader,lane!=0,basisVertex,opaqueFragment)
            && CreateNativeLitShaderProgram(g_nativeWorldBasis[lane],opaqueFragment.c_str(),basisVertex.c_str()))g_worldBasisReady|=1u<<lane;
    kisak::vita::VitaCpuProfile_Counter("world-basis-ready",g_worldBasisReady,1);
    // Run60 math variant is retained for history, not compiled in this delivery.
    // Run55 lifetime variant was measured and rejected; retain its historical builder only.
    kisak::vita::VitaCpuProfile_Counter("world-lifetime-ready",g_nativeWorldLifetime.program!=0,1);
    if(GfxVitaBuildWorldAlpha(kNativeLitFragmentShader,opaqueFragment))
        CreateNativeLitShaderProgram(g_nativeWorldAlpha,opaqueFragment.c_str());
    kisak::vita::VitaCpuProfile_Counter("world-alpha-ready",g_nativeWorldAlpha.program!=0,1);
    if(GfxVitaBuildWeaponSpecular(kNativeLitFragmentShader,opaqueFragment))
        CreateNativeLitShaderProgram(g_nativeWeaponSpecular,opaqueFragment.c_str());
    kisak::vita::VitaCpuProfile_Counter("weapon-specular-ready",g_nativeWeaponSpecular.program!=0,1);
    g_familyReady=0;
    for(unsigned family=1;family<=2;++family) {
        if(GfxVitaBuildMaterialFamily(kNativeLitFragmentShader,family,opaqueFragment)
            && CreateNativeLitShaderProgram(g_nativeMaterialFamily[family-1],opaqueFragment.c_str()))g_familyReady|=1u<<(family-1);
    }
    kisak::vita::VitaCpuProfile_Counter(g_familyReady==3?"material-family-ready":"material-family-unavailable",g_familyReady,3);
    g_prefetchReady=0;
    for(unsigned family=1;family<=2;++family) {
        if(GfxVitaBuildTexturePrefetch(kNativeLitFragmentShader,family,opaqueFragment)
            && CreateNativeLitShaderProgram(g_nativePrefetch[family-1],opaqueFragment.c_str()))g_prefetchReady|=1u<<(family-1);
    }
    kisak::vita::VitaCpuProfile_Counter(g_prefetchReady==3?"texture-prefetch-ready":"texture-prefetch-unavailable",g_prefetchReady,3);
    g_specularReady=0;
    for(unsigned light=0;light<2;++light) {
        if(GfxVitaBuildSpecularShader(kNativeLitFragmentShader,light!=0,opaqueFragment)
            && CreateNativeLitShaderProgram(g_nativeSpecularShader[light],opaqueFragment.c_str()))g_specularReady|=1u<<light;
    }
    kisak::vita::VitaCpuProfile_Counter(g_specularReady==3?"specular-shader-ready":"specular-shader-unavailable",g_specularReady,3);
    g_materialProbeReady=0;
    for(unsigned variant=0;variant<3;++variant)for(unsigned light=0;light<(variant==2?1u:2u);++light) {
        const bool ready=GfxVitaBuildMaterialProbeShader(kNativeLitFragmentShader,light!=0,variant+1,opaqueFragment)
            && CreateNativeLitShaderProgram(g_nativeMaterialProbe[variant][light],opaqueFragment.c_str());
        if(ready)g_materialProbeReady|=1u<<(variant*2+light);
    }
    kisak::vita::VitaCpuProfile_Counter(g_materialProbeReady==31?"material-probe-ready":"material-probe-unavailable",g_materialProbeReady,31);
#endif
    g_nativeLitShader = g_nativeFullLitShader;
    return true;
}

bool NativeOpaqueEligible(bool staticWorld,bool viewmodel) {
    const bool primary=(g_materialPass.lightmapSamplerFlags&0x02u) && g_materialPass.lightmapPrimaryTexture.value;
    const bool secondary=(g_materialPass.lightmapSamplerFlags&0x04u) && g_materialPass.lightmapSecondaryTexture.value;
    const bool litInputs=!SourceTechniqueIsUnlit(g_materialPass) && g_materialPass.colorTexture.value
        && (primary || secondary || g_materialPass.normalTexture.value || g_materialPass.specularTexture.value || g_materialPass.reflectionProbeTexture.value)
        && (primary || secondary || (g_materialPass.techniqueFlags0&GFX_MATERIAL_PASS_BAKED_LIGHTING));
    return GfxVitaOpaqueShaderEligible(staticWorld,viewmodel,SourceMaterialStateApplies(g_materialPass),
        SourceStateUsesAlphaTest(g_materialPass),SourceStateUsesBlend(g_materialPass),litInputs,
        g_nativeHud,g_materialPass.surfaceMode==GfxSurfaceMode::Sky,g_benchmarkShaderMode);
}

bool NativeEarlyAlphaEligible(bool staticWorld,bool viewmodel) {
    const bool lightmapped=((g_materialPass.lightmapSamplerFlags&0x02u) && g_materialPass.lightmapPrimaryTexture.value)
        || ((g_materialPass.lightmapSamplerFlags&0x04u) && g_materialPass.lightmapSecondaryTexture.value);
    const bool litNormalInputs=!SourceTechniqueIsUnlit(g_materialPass)
        && g_materialPass.colorTexture.value && g_materialPass.normalTexture.value
        && (lightmapped || (g_materialPass.techniqueFlags0&GFX_MATERIAL_PASS_BAKED_LIGHTING));
    return GfxVitaEarlyAlphaEligible(staticWorld,viewmodel,SourceMaterialStateApplies(g_materialPass),
        SourceStateUsesAlphaTest(g_materialPass),litNormalInputs,g_nativeHud,
        g_materialPass.surfaceMode==GfxSurfaceMode::Sky,g_benchmarkShaderMode);
}

bool NativeWorldLifetimeEligible(bool staticWorld,bool viewmodel) {
    return GfxVitaWorldLifetimeEligible(staticWorld,viewmodel,g_nativeStaticInstancedDraw,
        NativeOpaqueEligible(staticWorld,viewmodel),NativeMaterialFamily(true));
}

bool NativeWorldAlphaEligible(bool staticWorld,bool viewmodel) {
    const bool sourceWorld=staticWorld && !viewmodel && !g_nativeStaticInstancedDraw
        && SourceMaterialStateApplies(g_materialPass) && !SourceTechniqueIsUnlit(g_materialPass)
        && !g_nativeHud && g_materialPass.surfaceMode!=GfxSurfaceMode::Sky && g_benchmarkShaderMode==0;
    return GfxVitaWorldAlphaEligible(sourceWorld,SourceStateUsesAlphaTest(g_materialPass) || SourceStateUsesBlend(g_materialPass),
        g_adaptiveLighting,g_rawLocalLightLoops,g_materialPass.colorTexture.value!=0,g_materialPass.normalTexture.value!=0,
        g_materialPass.specularTexture.value!=0,g_materialPass.reflectionProbeTexture.value!=0,
        (g_materialPass.lightmapSamplerFlags&0x02u) && g_materialPass.lightmapPrimaryTexture.value,
        (g_materialPass.lightmapSamplerFlags&0x04u) && g_materialPass.lightmapSecondaryTexture.value);
}

bool NativeHudEligible() {
    return GfxVitaHudShaderEligible(g_nativeHud,g_nativeWorldSpaceDraw,g_nativeViewModelSpaceDraw,
        g_nativeStaticInstancedDraw,g_nativeClearDraw,g_benchmarkShaderMode);
}

bool NativeWorldScalingPass() {
#if KISAK_VITA_CPU_RUNTIME
    if(!g_renderPassProbe.requestedPhase)return g_gameplayFrame && !g_nativeViewmodel;
    return GpuDrawPass()==unsigned(kisak::vita::VitaRenderPass::World);
#else
    return false;
#endif
}
void SelectNativeLightShader(bool propEligible=false,bool opaqueEligible=false,bool fastColor=false,bool fastClear=false,bool weaponEligible=false,bool earlyAlphaEligible=false,bool worldAlphaEligible=false,bool worldLifetimeEligible=false) {
    // The original loop skips the sun index. With no other primary lights and
    // no FX lights its exact result is zero. Compile that empty workload away
    // instead of making the Vita fragment compiler carry twenty light bodies.
    const bool sunOnly = g_adaptiveLighting ? !g_rawLocalLightLoops
        : (g_frameLighting.primaryLightCount <= 1u && g_frameLighting.dynamicLightCount == 0u);
    const unsigned family=(g_renderPassProbe.family || g_renderPassProbe.prefetch || g_renderPassProbe.drawProbe || (g_renderPassProbe.fastColor || g_renderPassProbe.weaponOpaque || g_renderPassProbe.hud || g_renderPassProbe.earlyAlpha || g_renderPassProbe.fastDrawProbe || g_renderPipeline.weaponPipeline))?NativeMaterialFamily(opaqueEligible):0u;
    const unsigned materialVariant=g_renderPassProbe.material?GfxVitaMaterialProbeVariant(g_renderPassProbe.phase):0u;
    const unsigned component=g_renderPassProbe.worldComponents && NativeWorldComponentEligible(worldAlphaEligible)?GfxVitaWorldComponentVariant(g_renderPassProbe.phase):0;
    const auto& shader =
        GfxVitaUseSimpleWorld(g_gameplayFrame,g_simpleWorldRequested,g_cpuDiagnostic,
            g_renderPassProbe.requestedPhase,g_renderPassProbe.worldScaling,g_renderPassProbe.phase,
            g_cheapWorldReady,NativeWorldScalingPass(),worldLifetimeEligible || worldAlphaEligible)?g_nativeCheapWorld[worldAlphaEligible?1:0]:
        GfxVitaUseHudShader(NativeHudEligible(),g_nativeHudShader.program!=0,
            g_renderPassProbe.hud,g_renderPassProbe.phase) ? g_nativeHudShader :
        (component && g_nativeWorldComponents[component-1].program)?g_nativeWorldComponents[component-1]:
        (g_renderPassProbe.worldGloss && g_renderPassProbe.phase==2 && NativeWorldGlossEligible(worldLifetimeEligible)
            && g_nativeWorldGloss.program) ? g_nativeWorldGloss :
        (g_renderPassProbe.worldBasis && g_renderPassProbe.phase==2 && g_worldBasisState.active && (worldLifetimeEligible || worldAlphaEligible)
            && g_nativeWorldBasis[worldAlphaEligible?1:0].program) ? g_nativeWorldBasis[worldAlphaEligible?1:0] :
        (g_renderPassProbe.worldPrecision && g_worldPrecisionReady==3 && g_renderPassProbe.phase==2 && (worldLifetimeEligible || worldAlphaEligible)
            && g_nativeWorldPrecision[worldAlphaEligible?1:0].program) ? g_nativeWorldPrecision[worldAlphaEligible?1:0] :
        (g_renderPassProbe.worldMath && g_renderPassProbe.phase==2 && (worldLifetimeEligible || worldAlphaEligible)
            && g_nativeWorldMath[worldAlphaEligible?1:0].program) ? g_nativeWorldMath[worldAlphaEligible?1:0] :
        (g_renderPassProbe.worldLifetime && g_renderPassProbe.phase==2 && worldLifetimeEligible && g_nativeWorldLifetime.program) ? g_nativeWorldLifetime :
        (g_renderPipeline.worldAlpha && g_renderPipeline.worldAlphaActive && worldAlphaEligible && g_nativeWorldAlpha.program) ? g_nativeWorldAlpha :
        (g_renderPipeline.weaponSpecular && (g_renderPassProbe.phase==2 || g_renderPipeline.worldAlpha)
            && NativeWeaponSpecularEligible(weaponEligible) && g_nativeWeaponSpecular.program) ? g_nativeWeaponSpecular :
        (g_renderPipeline.weaponPipeline && (g_renderPassProbe.phase==2 || g_renderPipeline.weaponVertex) && weaponEligible
            && NativeMaterialFamily(weaponEligible)==2 && g_familyReady==3) ? g_nativeMaterialFamily[1] :
        (g_renderPassProbe.earlyAlpha && g_renderPassProbe.phase==2 && earlyAlphaEligible && g_earlyAlphaReady==3)
            ? g_nativeEarlyAlpha[sunOnly?1u:0u] :
        (g_renderPassProbe.weaponOpaque && g_renderPassProbe.phase==2 && weaponEligible
            && g_nativeOpaqueFullShader.program && g_nativeOpaqueSunShader.program)
            ? (sunOnly ? g_nativeOpaqueSunShader : g_nativeOpaqueFullShader) :
        (((g_renderPassProbe.fastColor && g_renderPassProbe.phase==2) || g_renderPassProbe.weaponOpaque || g_renderPassProbe.hud || g_renderPassProbe.earlyAlpha || g_renderPassProbe.fastDrawProbe || g_renderPipeline.weaponPipeline) && g_nativeFastColor.program && g_nativeFastClear.program && fastClear) ? g_nativeFastClear :
        (((g_renderPassProbe.fastColor && g_renderPassProbe.phase==2) || g_renderPassProbe.weaponOpaque || g_renderPassProbe.hud || g_renderPassProbe.earlyAlpha || g_renderPassProbe.fastDrawProbe || g_renderPipeline.weaponPipeline) && g_nativeFastColor.program && g_nativeFastClear.program && fastColor) ? g_nativeFastColor :
#if KISAK_VITA_BENCHMARK
        g_benchmarkShaderMode==2 && !g_nativeHud ? g_nativeSimpleShader :
        g_benchmarkShaderMode==3 && !g_nativeHud ? g_nativeSunOnlyShader :
        g_benchmarkShaderMode==5 && !g_nativeHud ?
            (g_rawLocalLightLoops ? g_nativeFullLitShader : g_nativeSunOnlyShader) :
        g_benchmarkShaderMode==7 && !g_nativeHud ?
            (sunOnly ? g_nativeSunNoFogShader : g_nativeNoFogShader) :
        g_benchmarkShaderMode==6 && !g_nativeHud ?
            (sunOnly ? g_nativeSunNoNormalShader : g_nativeNoNormalShader) :
        g_benchmarkShaderMode==4 && !g_nativeHud ?
            (sunOnly ? g_nativeSunNoSpecularShader : g_nativeNoSpecularShader) :
#endif
        (family && (g_renderPassProbe.phase==2 || g_renderPassProbe.prefetch || g_renderPassProbe.drawProbe || (g_renderPassProbe.fastColor || g_renderPassProbe.weaponOpaque || g_renderPassProbe.hud || g_renderPassProbe.earlyAlpha || g_renderPassProbe.fastDrawProbe || g_renderPipeline.weaponPipeline)) && g_familyReady==3
            && g_nativeOpaqueSunShader.program && g_nativeOpaqueFullShader.program
            ? (g_renderPassProbe.prefetch && g_renderPassProbe.phase==2 && g_prefetchReady==3 ? g_nativePrefetch[family-1] : g_nativeMaterialFamily[family-1]) :
        (opaqueEligible && g_renderPassProbe.specular && g_renderPassProbe.phase==2
            && g_specularReady==3 && g_nativeOpaqueSunShader.program && g_nativeOpaqueFullShader.program
            ? g_nativeSpecularShader[sunOnly?1u:0u] :
        (opaqueEligible && materialVariant && g_materialProbeReady==31
            && g_nativeOpaqueSunShader.program && g_nativeOpaqueFullShader.program
            ? g_nativeMaterialProbe[materialVariant-1][materialVariant==3?0u:(sunOnly?1u:0u)] :
        (opaqueEligible && ((g_renderPassProbe.opaque && g_renderPassProbe.phase==2) || g_renderPassProbe.depthOrder || g_renderPassProbe.material || g_renderPassProbe.specular || g_renderPassProbe.baseline || g_renderPassProbe.family || g_renderPassProbe.displayQueue || g_renderPassProbe.prefetch || g_renderPassProbe.drawProbe || (g_renderPassProbe.fastColor || g_renderPassProbe.weaponOpaque || g_renderPassProbe.hud || g_renderPassProbe.earlyAlpha || g_renderPassProbe.fastDrawProbe || g_renderPipeline.weaponPipeline))
            && g_nativeOpaqueSunShader.program && g_nativeOpaqueFullShader.program
            ? (sunOnly ? g_nativeOpaqueSunShader : g_nativeOpaqueFullShader) :
        (propEligible && g_nativePropShader.program
            && g_renderPassProbe.comparison && g_renderPassProbe.phase==2 ? g_nativePropShader
            : (sunOnly ? g_nativeSunOnlyShader : g_nativeFullLitShader))))));
    if (shader.program && shader.program != g_nativeLitShader.program) {
        EndNativeStaticPipeline();
        g_nativeLitShader = shader;
        g_nativeStaticMaterialRevision = 0;
    }
}

void DestroyNativeLitShader() {
    for(auto& shader:g_nativeEarlyAlpha){if(shader.program)glDeleteProgram(shader.program);shader={};}
    g_earlyAlphaReady=0;
    if(g_nativeHudShader.program)glDeleteProgram(g_nativeHudShader.program);
    g_nativeHudShader={};
    if(g_nativeFastColor.program)glDeleteProgram(g_nativeFastColor.program);
    if(g_nativeFastClear.program)glDeleteProgram(g_nativeFastClear.program);
    g_nativeFastColor={};g_nativeFastClear={};
#if KISAK_VITA_BENCHMARK
    if (g_nativeSimpleShader.program) glDeleteProgram(g_nativeSimpleShader.program);
    g_nativeSimpleShader = {};
    if (g_nativeNoSpecularShader.program) glDeleteProgram(g_nativeNoSpecularShader.program);
    if (g_nativeSunNoSpecularShader.program) glDeleteProgram(g_nativeSunNoSpecularShader.program);
    g_nativeNoSpecularShader = {};
    g_nativeSunNoSpecularShader = {};
    if (g_nativeNoNormalShader.program) glDeleteProgram(g_nativeNoNormalShader.program);
    if (g_nativeSunNoNormalShader.program) glDeleteProgram(g_nativeSunNoNormalShader.program);
    g_nativeNoNormalShader = {}; g_nativeSunNoNormalShader = {};
    if (g_nativeNoFogShader.program) glDeleteProgram(g_nativeNoFogShader.program);
    if (g_nativeSunNoFogShader.program) glDeleteProgram(g_nativeSunNoFogShader.program);
    g_nativeNoFogShader={}; g_nativeSunNoFogShader={};
#endif
    if (g_nativeFullLitShader.program) glDeleteProgram(g_nativeFullLitShader.program);
    if (g_nativeSunOnlyShader.program) glDeleteProgram(g_nativeSunOnlyShader.program);
    if(g_nativePropShader.program)glDeleteProgram(g_nativePropShader.program);
    g_nativePropShader={};
    if(g_nativeOpaqueFullShader.program)glDeleteProgram(g_nativeOpaqueFullShader.program);
    if(g_nativeOpaqueSunShader.program)glDeleteProgram(g_nativeOpaqueSunShader.program);
    g_nativeOpaqueFullShader={};g_nativeOpaqueSunShader={};
    for(auto& shader:g_nativeSpecularShader){if(shader.program)glDeleteProgram(shader.program);shader={};}
    for(auto& shader:g_nativePrefetch){if(shader.program)glDeleteProgram(shader.program);shader={};}
    for(auto& shader:g_nativeWorldComponents){if(shader.program)glDeleteProgram(shader.program);shader={};}
    g_worldComponentsReady=0;
    if(g_nativeWorldGloss.program)glDeleteProgram(g_nativeWorldGloss.program);g_nativeWorldGloss={};
    for(auto& shader:g_nativeWorldBasis){if(shader.program)glDeleteProgram(shader.program);shader={};}
    g_worldBasisReady=0;g_worldBasisState.Reset();
    for(auto& shader:g_nativeWorldPrecision){if(shader.program)glDeleteProgram(shader.program);shader={};}
    for(auto& shader:g_nativeCheapWorld){if(shader.program)glDeleteProgram(shader.program);shader={};}
    g_cheapWorldReady=0;kisak::vita::VitaCpuShaderCache_SetWorldScalingReady(false);
    g_worldPrecisionReady=0;
    for(auto& shader:g_nativeWorldMath){if(shader.program)glDeleteProgram(shader.program);shader={};}
    g_worldMathReady=0;
    if(g_nativeWorldLifetime.program)glDeleteProgram(g_nativeWorldLifetime.program);
    g_nativeWorldLifetime={};
    if(g_nativeWorldAlpha.program)glDeleteProgram(g_nativeWorldAlpha.program);
    g_nativeWorldAlpha={};
    if(g_nativeWeaponSpecular.program)glDeleteProgram(g_nativeWeaponSpecular.program);
    g_nativeWeaponSpecular={};
    for(auto& shader:g_nativeMaterialFamily){if(shader.program)glDeleteProgram(shader.program);shader={};}
    g_familyReady=0;
    g_specularReady=0;
    for(auto& variants:g_nativeMaterialProbe)for(auto& shader:variants){if(shader.program)glDeleteProgram(shader.program);shader={};}
    g_materialProbeReady=0;
    g_nativeLitShader = {};
    g_nativeFullLitShader = {};
    g_nativeSunOnlyShader = {};
}

void SetNativeUniform1f(GLint location, float value) {
    if (location >= 0)
        glUniform1f(location, std::isfinite(value) ? value : 0.0f);
}

void SetNativeUniform3fv(GLint location, const float value[3],
                         const float fallback[3]) {
    if (location < 0)
        return;
    const float safe[3] = {
        value && std::isfinite(value[0]) ? value[0] : fallback[0],
        value && std::isfinite(value[1]) ? value[1] : fallback[1],
        value && std::isfinite(value[2]) ? value[2] : fallback[2],
    };
    glUniform3fv(location, 1, safe);
}

void SetNativeUniform4fv(GLint location, const float* value, GLsizei count) {
    if (location >= 0 && value && count > 0)
        glUniform4fv(location, count, value);
}

void DestroyNativePostResources() {
    if (g_nativePost.ditherTexture != 0) {
        const GLuint texture = g_nativePost.ditherTexture;
        glDeleteTextures(1, &texture);
    }
    if (g_nativePost.compositeVertexBuffer != 0) {
        const GLuint buffer = g_nativePost.compositeVertexBuffer;
        glDeleteBuffers(1, &buffer);
    }
    if (g_nativePost.shader.program != 0) {
        const GLuint program = g_nativePost.shader.program;
        glDeleteProgram(program);
    }
    if (g_nativePost.sceneFramebuffer != 0) {
        const GLuint framebuffer = g_nativePost.sceneFramebuffer;
        glDeleteFramebuffers(1, &framebuffer);
    }
    if (g_nativePost.sceneDepth != 0) {
        const GLuint depth = g_nativePost.sceneDepth;
        glDeleteRenderbuffers(1, &depth);
    }
    if (g_nativePost.sceneTexture != 0) {
        const GLuint texture = g_nativePost.sceneTexture;
        glDeleteTextures(1, &texture);
    }
    g_nativePost = {};
}

bool CreateNativePostResources(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0)
        return false;
    if (g_nativePost.ready)
        return true;

    VITA_CPU_LOADING("post-program-create");

    g_nativePost.width = width;
    g_nativePost.height = height;

    GLuint vertexShader = 0;
    GLuint fragmentShader = 0;
    if (!CompileNativeShader(GL_VERTEX_SHADER, kNativePostVertexShader,
                             &vertexShader)
        || !CompileNativeShader(GL_FRAGMENT_SHADER,
#if KISAK_VITA_BENCHMARK
                                g_benchmarkShaderMode==1 ? kBenchmarkPostCopyFragment :
#endif
                                kNativePostFragmentShader,
                                &fragmentShader)) {
        if (vertexShader)
            glDeleteShader(vertexShader);
        if (fragmentShader)
            glDeleteShader(fragmentShader);
        DestroyNativePostResources();
        sceClibPrintf("[kisak-vita] native post shader unavailable\n");
        return false;
    }
    const GLuint program = glCreateProgram();
    if (!program) {
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        DestroyNativePostResources();
        return false;
    }
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glBindAttribLocation(program, 0, "a_position");
    glBindAttribLocation(program, 1, "a_texcoord");
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    if (!linked) {
        GLchar log[512]{};
        GLsizei logLength = 0;
        glGetProgramInfoLog(program, sizeof(log), &logLength, log);
        sceClibPrintf("[kisak-vita] native post shader link failed: %s\n",
                      log);
        glDeleteProgram(program);
        DestroyNativePostResources();
        return false;
    }

    g_nativePost.shader.program = program;
    g_nativePost.shader.sceneSampler = glGetUniformLocation(
        program, "u_scene");
    g_nativePost.shader.ditherSampler = glGetUniformLocation(program, "u_dither");
    g_nativePost.shader.filmEnabled = glGetUniformLocation(
        program, "u_film_enabled");
    g_nativePost.shader.exposure = glGetUniformLocation(
        program, "u_exposure");
    g_nativePost.shader.contrast = glGetUniformLocation(
        program, "u_contrast");
    g_nativePost.shader.brightness = glGetUniformLocation(
        program, "u_brightness");
    g_nativePost.shader.desaturation = glGetUniformLocation(
        program, "u_desaturation");
    g_nativePost.shader.invert = glGetUniformLocation(
        program, "u_invert");
    g_nativePost.shader.gamma = glGetUniformLocation(program, "u_gamma");
    g_nativePost.shader.filmLightTint = glGetUniformLocation(
        program, "u_film_light_tint");
    g_nativePost.shader.filmDarkTint = glGetUniformLocation(
        program, "u_film_dark_tint");
    g_nativePost.shader.glowEnabled = glGetUniformLocation(
        program, "u_glow_enabled");
    g_nativePost.shader.glowCutoff = glGetUniformLocation(
        program, "u_glow_cutoff");
    g_nativePost.shader.glowCutoffRescale = glGetUniformLocation(
        program, "u_glow_cutoff_rescale");
    g_nativePost.shader.glowDesaturation = glGetUniformLocation(
        program, "u_glow_desaturation");
    g_nativePost.shader.glowIntensity = glGetUniformLocation(
        program, "u_glow_intensity");
    g_nativePost.shader.glowOffset = glGetUniformLocation(
        program, "u_glow_offset");
    g_nativePost.shader.outputSize = glGetUniformLocation(
        program, "u_output_size");

    // Keep the post quad in persistent GPU storage.  A client-array pointer
    // is ambiguous whenever a preceding world/UI path leaves an array buffer
    // bound; on VitaGL that can turn the stack address into a VBO offset and
    // fault during the first composite.  The immutable 64-byte buffer also
    // removes a per-frame client-array upload from the production path.
    static constexpr float kCompositeQuad[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    glGenBuffers(1, &g_nativePost.compositeVertexBuffer);
    if (g_nativePost.compositeVertexBuffer == 0) {
        DestroyNativePostResources();
        return false;
    }
    glBindBuffer(GL_ARRAY_BUFFER, g_nativePost.compositeVertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kCompositeQuad), kCompositeQuad,
                 GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // RGBA8 avoids backend-dependent luminance swizzles; only red is sampled.
    uint8_t ditherPixels[64 * 4];
    for (uint32_t i = 0; i < 64; ++i) {
        ditherPixels[4 * i] = ditherPixels[4 * i + 1]
            = ditherPixels[4 * i + 2] = kDisplayDither8x8[i];
        ditherPixels[4 * i + 3] = 255;
    }
    glGenTextures(1, &g_nativePost.ditherTexture);
    if (g_nativePost.ditherTexture == 0) {
        DestroyNativePostResources();
        return false;
    }
    glBindTexture(GL_TEXTURE_2D, g_nativePost.ditherTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, ditherPixels);
    if (glGetError() != GL_NO_ERROR) {
        DestroyNativePostResources();
        return false;
    }
    glGenTextures(1, &g_nativePost.sceneTexture);
    if (g_nativePost.sceneTexture == 0) {
        DestroyNativePostResources();
        return false;
    }
    glBindTexture(GL_TEXTURE_2D, g_nativePost.sceneTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // The vertical slice uses the native 960x544 RGBA8 contract with no
    // MSAA. Film/color grading stays in the single resolve pass; spending a
    // second full-frame RGBA16F allocation here displaced map textures on a
    // 64 MiB GPU budget without producing a measurable console-oracle gain.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    glGenRenderbuffers(1, &g_nativePost.sceneDepth);
    if (g_nativePost.sceneDepth == 0) {
        glBindTexture(GL_TEXTURE_2D, 0);
        DestroyNativePostResources();
        return false;
    }
    glBindRenderbuffer(GL_RENDERBUFFER, g_nativePost.sceneDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16,
                          static_cast<GLsizei>(width),
                          static_cast<GLsizei>(height));

    glGenFramebuffers(1, &g_nativePost.sceneFramebuffer);
    if (g_nativePost.sceneFramebuffer == 0) {
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        DestroyNativePostResources();
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_nativePost.sceneFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g_nativePost.sceneTexture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, g_nativePost.sceneDepth);
    const GLenum framebufferStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    if (framebufferStatus != GL_FRAMEBUFFER_COMPLETE) {
        sceClibPrintf(
            "[kisak-vita] native post scene target incomplete status=0x%08x\n",
            static_cast<unsigned>(framebufferStatus));
        DestroyNativePostResources();
        return false;
    }
    g_nativePost.ready = true;
    sceClibPrintf(
        "[kisak-vita] native post scene target ready size=%ux%u format=rgba8\n",
        static_cast<unsigned>(width), static_cast<unsigned>(height));
    return true;
}

GfxVitaViewportSize ActivePassViewport() {
    return GfxVitaPassViewport(g_nativePost.ready, g_gameplayFrame,
        g_nativeViewmodel, g_nativeHud, g_benchmarkClear,
        {g_worldTarget.width,g_worldTarget.height},
        {g_nativePost.width,g_nativePost.height}, {g_width,g_height});
}

// Query cached GL state once at the first real draw of each geometry category.
// This reports actual viewport/FBO state, not only allocated texture sizes.
uint32_t g_viewportSeen=0, g_viewportMismatches=0;
int g_observedViewports[3][4]{};
void ObserveGeometryViewport(unsigned phase) {
    if (!g_gameplayFrame || phase>=3 || (g_viewportSeen & (1u<<phase))) return;
    g_viewportSeen |= 1u<<phase;
    GLint actual[4]{}, framebuffer=0;
    glGetIntegerv(GL_VIEWPORT,actual);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&framebuffer);
    for(unsigned i=0;i<4;++i) g_observedViewports[phase][i]=actual[i];
    const bool world=phase!=2 && g_worldTarget.framebuffer;
    const GLuint expectedFramebuffer=world ? g_worldTarget.framebuffer : g_nativePost.sceneFramebuffer;
    const uint32_t width=world ? g_worldTarget.width : g_nativePost.width;
    const uint32_t height=world ? g_worldTarget.height : g_nativePost.height;
    if(actual[0]!=0 || actual[1]!=0 || actual[2]!=int(width) || actual[3]!=int(height)
        || GLuint(framebuffer)!=expectedFramebuffer) ++g_viewportMismatches;
}

void BindNativeSceneTarget() {
    if (!g_nativePost.ready)
        return;
    const bool world=g_gameplayFrame && !g_nativeViewmodel && g_worldTarget.framebuffer;
    glBindFramebuffer(GL_FRAMEBUFFER, world ? g_worldTarget.framebuffer : g_nativePost.sceneFramebuffer);
    const auto viewport=ActivePassViewport();
    glViewport(0,0,viewport.width,viewport.height);
}

bool ApplyNativePostEffects() {
    if (!g_nativePost.ready || g_nativePost.shader.program == 0)
        return false;

    // Tear down any retained world/instance program before installing the
    // post program.  Doing this after glUseProgram(post) would immediately
    // replace the composite shader with program zero when a static pipeline
    // happened to remain active.
    EndNativeStaticPipeline();

    const GfxViewEffectState& effects = g_viewEffects;
    const bool filmEnabled = (effects.flags & GFX_FRAME_LIGHTING_HAS_FILM) != 0;
    const bool glowEnabled = (effects.flags & GFX_FRAME_LIGHTING_HAS_GLOW) != 0
        && effects.glowEnabled > 0.5f
        && std::isfinite(effects.glowRadius0) && effects.glowRadius0 > 0.0f
        && std::isfinite(effects.glowBloomIntensity0)
        && effects.glowBloomIntensity0 > 0.0f;
    const float exposure = std::max(
        0.0f, std::isfinite(effects.exposure) ? effects.exposure : 1.0f);
    const float contrast = std::clamp(
        std::isfinite(effects.contrast) ? effects.contrast : 1.0f,
        0.0f, 4.0f);
    const float brightness = std::isfinite(effects.brightness)
        ? effects.brightness : 0.0f;
    const float desaturation = std::clamp(
        std::isfinite(effects.desaturation) ? effects.desaturation : 0.0f,
        0.0f, 1.0f);
    const float invert = std::clamp(
        std::isfinite(effects.invert) ? effects.invert : 0.0f, 0.0f, 1.0f);
    const float gamma = std::clamp(
        std::isfinite(effects.gamma) ? effects.gamma : 0.8f, 0.1f, 4.0f);
    const float cutoff = std::clamp(
        std::isfinite(effects.glowBloomCutoff)
            ? effects.glowBloomCutoff : 0.0f,
        0.0f, 1.0f);
    const float cutoffRescale = 1.0f / std::max(
        1.0f - cutoff, 1.0f / 4096.0f);
    const float glowDesaturation = std::clamp(
        std::isfinite(effects.glowBloomDesaturation)
            ? effects.glowBloomDesaturation : 0.0f,
        0.0f, 1.0f);
    const float glowIntensity = std::clamp(
        std::isfinite(effects.glowBloomIntensity0)
            ? effects.glowBloomIntensity0 : 0.0f,
        0.0f, 8.0f);
    const uint32_t quarterWidth = std::max(1u, g_width / 4u);
    const uint32_t quarterHeight = std::max(1u, g_height / 4u);
    const int glowRadius = std::clamp(static_cast<int>(std::ceil(
        std::max(0.0f, effects.glowRadius0) * 0.25f)), 1, 12);
    const float glowOffset[2] = {
        static_cast<float>(glowRadius) / static_cast<float>(quarterWidth),
        static_cast<float>(glowRadius) / static_cast<float>(quarterHeight),
    };
    static constexpr float kWhite[3] = {1.0f, 1.0f, 1.0f};

    // Present the resolved target into the real vitaGL back buffer. The
    // fullscreen pass is deliberately after HUD/viewmodel/effect submission,
    // matching the scalar pass order and preventing postfx from making the
    // menu or weapon overlay disappear behind world depth.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, static_cast<GLsizei>(g_width),
               static_cast<GLsizei>(g_height));
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_FOG);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_FALSE);
    bool copyPost=false;
#if KISAK_VITA_CPU_RUNTIME
    copyPost=g_passIsolation.CopyPost() && g_worldCopy.program!=0;
#endif
    if(copyPost) {
        glUseProgram(g_worldCopy.program);glActiveTexture(GL_TEXTURE0);glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D,g_nativePost.sceneTexture);glUniform1i(g_worldCopy.sampler,0);
    } else {
    glUseProgram(g_nativePost.shader.program);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_nativePost.sceneTexture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_nativePost.ditherTexture);
    if (g_nativePost.shader.ditherSampler >= 0)
        glUniform1i(g_nativePost.shader.ditherSampler, 1);
    glActiveTexture(GL_TEXTURE0);
    if (g_nativePost.shader.sceneSampler >= 0)
        glUniform1i(g_nativePost.shader.sceneSampler, 0);
    SetNativeUniform1f(g_nativePost.shader.filmEnabled,
                       filmEnabled ? 1.0f : 0.0f);
    SetNativeUniform1f(g_nativePost.shader.exposure, exposure);
    SetNativeUniform1f(g_nativePost.shader.contrast, contrast);
    SetNativeUniform1f(g_nativePost.shader.brightness, brightness);
    SetNativeUniform1f(g_nativePost.shader.desaturation, desaturation);
    SetNativeUniform1f(g_nativePost.shader.invert, invert);
    SetNativeUniform1f(g_nativePost.shader.gamma, gamma);
    SetNativeUniform3fv(g_nativePost.shader.filmLightTint,
                        effects.filmLightTint, kWhite);
    SetNativeUniform3fv(g_nativePost.shader.filmDarkTint,
                        effects.filmDarkTint, kWhite);
    SetNativeUniform1f(g_nativePost.shader.glowEnabled,
                       glowEnabled ? 1.0f : 0.0f);
    SetNativeUniform1f(g_nativePost.shader.glowCutoff, cutoff);
    SetNativeUniform1f(g_nativePost.shader.glowCutoffRescale, cutoffRescale);
    SetNativeUniform1f(g_nativePost.shader.glowDesaturation, glowDesaturation);
    SetNativeUniform1f(g_nativePost.shader.glowIntensity,
                       glowEnabled ? glowIntensity : 0.0f);
    if (g_nativePost.shader.glowOffset >= 0)
        glUniform2fv(g_nativePost.shader.glowOffset, 1, glowOffset);
    if (g_nativePost.shader.outputSize >= 0) {
        const float outputSize[2] = {
            static_cast<float>(g_width),
            static_cast<float>(g_height),
        };
        glUniform2fv(g_nativePost.shader.outputSize, 1, outputSize);
    }

    }
#if KISAK_VITA_CPU_RUNTIME
    if(g_renderPassProbe.passIsolation) {
        GLint bound=0;glGetIntegerv(GL_CURRENT_PROGRAM,&bound);
        const GLuint expected=copyPost?g_worldCopy.program:g_nativePost.shader.program;
        kisak::vita::VitaCpuProfile_Counter("render-isolation-post",copyPost,
            expected!=0 && GLuint(bound)==expected && copyPost==g_passIsolation.CopyPost()?0:1);
    }
#endif
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ARRAY_BUFFER, g_nativePost.compositeVertexBuffer);
    glEnableVertexAttribArray(0u);
    glEnableVertexAttribArray(1u);
    glVertexAttribPointer(0u, 2, GL_FLOAT, GL_FALSE,
                          static_cast<GLsizei>(sizeof(float) * 4u),
                          reinterpret_cast<const void*>(0u));
    glVertexAttribPointer(1u, 2, GL_FLOAT, GL_FALSE,
                          static_cast<GLsizei>(sizeof(float) * 4u),
                          reinterpret_cast<const void*>(sizeof(float) * 2u));
    // The post composite is one submission producing two triangles.
    ++g_frameDrawCalls;
    g_frameTriangles += 2u;
    if (g_benchmarkShaderMode==1 || copyPost) ++g_postCopyDraws; else ++g_authoredPostDraws;
#if KISAK_VITA_CPU_RUNTIME
    if(g_renderPassProbe.currentDrawProbe) {
        kisak::vita::VitaCpuProfile_Counter("gpu-post-state",(filmEnabled?1u:0u)|(glowEnabled?2u:0u),glowRadius);
        kisak::vita::VitaCpuProfile_Counter("gpu-post-gamma-exposure",kisak::vita::VitaGpuFloatBits(gamma),kisak::vita::VitaGpuFloatBits(exposure));
        kisak::vita::VitaCpuProfile_Counter("gpu-post-size",g_width,g_height);
    }
#endif
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(1u);
    glDisableVertexAttribArray(0u);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
    ResetExtraFixedFunctionTextureUnits();
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glClientActiveTexture(GL_TEXTURE0);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    static uint32_t postLogCount = 0;
    if (postLogCount++ < 4u) {
        sceClibPrintf(
            "[kisak-vita] native post pass film=%u glow=%u radius=%d gamma=%.4f dither=source-8x8-blue-noise\n",
            static_cast<unsigned>(filmEnabled),
            static_cast<unsigned>(glowEnabled), glowRadius, gamma);
    }
    return true;
}

void UploadNativeLitUniforms() {
    if (g_nativeLitShader.program == 0)
        return;
    static constexpr float kWhite[3] = {1.0f, 1.0f, 1.0f};
    static constexpr float kBlack[3] = {0.0f, 0.0f, 0.0f};
    SetNativeUniform1f(g_nativeLitShader.worldSpace,
                       g_nativeWorldSpaceDraw ? 1.0f : 0.0f);
    SetNativeUniform1f(g_nativeLitShader.viewModelSpace,
                       g_nativeViewModelSpaceDraw ? 1.0f : 0.0f);
    SetNativeUniform1f(g_nativeLitShader.staticInstanced,
                       g_nativeStaticInstancedDraw ? 1.0f : 0.0f);
    SetNativeUniform1f(g_nativeLitShader.worldNearPlane,
                       g_nativeViewModelSpaceDraw ? 0.1f
                                                  : g_nativeWorldNearPlane);
    if (g_nativeLitShader.worldViewProjection >= 0) {
        glUniformMatrix4fv(g_nativeLitShader.worldViewProjection, 1, GL_FALSE,
                           g_nativeWorldViewProjection);
    }
    const GfxFrameLightingState& lighting = g_frameLighting;
    SetNativeUniform1f(g_nativeLitShader.frameValid,
                       (lighting.flags & GFX_FRAME_LIGHTING_VALID) != 0
                           ? 1.0f : 0.0f);
    SetNativeUniform3fv(g_nativeLitShader.sunDirection,
                        lighting.sunDirection, kBlack);
    SetNativeUniform3fv(g_nativeLitShader.cameraPosition,
                        lighting.cameraPosition, kBlack);
    SetNativeUniform3fv(g_nativeLitShader.sunColor, lighting.sunColor, kBlack);
    SetNativeUniform3fv(g_nativeLitShader.ambientColor,
                        lighting.ambientColor, kBlack);
    SetNativeUniform3fv(g_nativeLitShader.diffuseColor,
                        lighting.diffuseColor, kBlack);
    SetNativeUniform1f(g_nativeLitShader.ambientScale, lighting.ambientScale);
    SetNativeUniform1f(g_nativeLitShader.diffuseFraction,
                       lighting.diffuseFraction);
    SetNativeUniform1f(g_nativeLitShader.sunLight, lighting.sunLight);
    SetNativeUniform1f(g_nativeLitShader.specularColorScale,
                       lighting.specularColorScale);
    SetNativeUniform3fv(g_nativeLitShader.lightGridColor,
                        lighting.lightGridColor, kWhite);
    SetNativeUniform1f(g_nativeLitShader.lightGridSunWeight,
                       lighting.lightGridSunWeight);
    SetNativeUniform1f(g_nativeLitShader.indoorFactor, lighting.indoorFactor);
    g_fogUniformEnabled=g_vitaGlFogEnabled;
    g_fogUniformStart=lighting.fogStart; g_fogUniformDensity=lighting.fogDensity;
    SetNativeUniform1f(g_nativeLitShader.fogEnabled,
                       g_vitaGlFogEnabled ? 1.0f : 0.0f);
    SetNativeUniform1f(g_nativeLitShader.fogStart, lighting.fogStart);
    SetNativeUniform1f(g_nativeLitShader.fogDensity, lighting.fogDensity);
    SetNativeUniform3fv(g_nativeLitShader.fogColor, lighting.fogColor, kBlack);
    const uint32_t lightCount = std::min(lighting.primaryLightCount,
                                         GFX_MAX_PRIMARY_LIGHTS);
    SetNativeUniform1f(g_nativeLitShader.primaryLightCount,
                       static_cast<float>(lightCount));
    SetNativeUniform1f(g_nativeLitShader.sunPrimaryLightIndex,
                       static_cast<float>(lighting.sunPrimaryLightIndex));
    float positionRadius[GFX_MAX_PRIMARY_LIGHTS * 4u]{};
    float directionType[GFX_MAX_PRIMARY_LIGHTS * 4u]{};
    float colorExponent[GFX_MAX_PRIMARY_LIGHTS * 4u]{};
    float cone[GFX_MAX_PRIMARY_LIGHTS * 4u]{};
    for (uint32_t index = 0; index < lightCount; ++index) {
        const GfxPrimaryLightDescriptor& light = lighting.primaryLights[index];
        positionRadius[index * 4u + 0u] = light.origin[0];
        positionRadius[index * 4u + 1u] = light.origin[1];
        positionRadius[index * 4u + 2u] = light.origin[2];
        positionRadius[index * 4u + 3u] = std::max(light.radius, 0.0f);
        directionType[index * 4u + 0u] = light.direction[0];
        directionType[index * 4u + 1u] = light.direction[1];
        directionType[index * 4u + 2u] = light.direction[2];
        directionType[index * 4u + 3u] = static_cast<float>(light.type);
        colorExponent[index * 4u + 0u] = light.color[0];
        colorExponent[index * 4u + 1u] = light.color[1];
        colorExponent[index * 4u + 2u] = light.color[2];
        colorExponent[index * 4u + 3u] = static_cast<float>(light.exponent);
        cone[index * 4u + 0u] = light.cosHalfFovOuter;
        cone[index * 4u + 1u] = light.cosHalfFovInner;
        cone[index * 4u + 2u] = light.cosHalfFovExpanded;
        cone[index * 4u + 3u] = light.canUseShadowMap
            ? std::clamp(light.shadowVisibility, 0.0f, 1.0f) : 1.0f;
    }
    SetNativeUniform4fv(g_nativeLitShader.primaryLightPositionRadius,
                        positionRadius, static_cast<GLsizei>(lightCount));
    SetNativeUniform4fv(g_nativeLitShader.primaryLightDirectionType,
                        directionType, static_cast<GLsizei>(lightCount));
    SetNativeUniform4fv(g_nativeLitShader.primaryLightColorExponent,
                        colorExponent, static_cast<GLsizei>(lightCount));
    SetNativeUniform4fv(g_nativeLitShader.primaryLightCone, cone,
                        static_cast<GLsizei>(lightCount));

    const uint32_t dynamicLightCount = std::min(
        lighting.dynamicLightCount, GFX_MAX_DYNAMIC_LIGHTS);
    SetNativeUniform1f(g_nativeLitShader.dynamicLightCount,
                       static_cast<float>(dynamicLightCount));
    float dynamicPositionRadius[GFX_MAX_DYNAMIC_LIGHTS * 4u]{};
    float dynamicDirectionType[GFX_MAX_DYNAMIC_LIGHTS * 4u]{};
    float dynamicColorExponent[GFX_MAX_DYNAMIC_LIGHTS * 4u]{};
    float dynamicCone[GFX_MAX_DYNAMIC_LIGHTS * 4u]{};
    for (uint32_t index = 0; index < dynamicLightCount; ++index) {
        const GfxPrimaryLightDescriptor& light = lighting.dynamicLights[index];
        dynamicPositionRadius[index * 4u + 0u] = light.origin[0];
        dynamicPositionRadius[index * 4u + 1u] = light.origin[1];
        dynamicPositionRadius[index * 4u + 2u] = light.origin[2];
        dynamicPositionRadius[index * 4u + 3u] = std::max(light.radius, 0.0f);
        dynamicDirectionType[index * 4u + 0u] = light.direction[0];
        dynamicDirectionType[index * 4u + 1u] = light.direction[1];
        dynamicDirectionType[index * 4u + 2u] = light.direction[2];
        dynamicDirectionType[index * 4u + 3u] = static_cast<float>(light.type);
        dynamicColorExponent[index * 4u + 0u] = light.color[0];
        dynamicColorExponent[index * 4u + 1u] = light.color[1];
        dynamicColorExponent[index * 4u + 2u] = light.color[2];
        dynamicColorExponent[index * 4u + 3u] = static_cast<float>(light.exponent);
        dynamicCone[index * 4u + 0u] = light.cosHalfFovOuter;
        dynamicCone[index * 4u + 1u] = light.cosHalfFovInner;
        dynamicCone[index * 4u + 2u] = light.cosHalfFovExpanded;
        dynamicCone[index * 4u + 3u] = std::clamp(
            light.shadowVisibility, 0.0f, 1.0f);
    }
    SetNativeUniform4fv(g_nativeLitShader.dynamicLightPositionRadius,
                        dynamicPositionRadius,
                        static_cast<GLsizei>(dynamicLightCount));
    SetNativeUniform4fv(g_nativeLitShader.dynamicLightDirectionType,
                        dynamicDirectionType,
                        static_cast<GLsizei>(dynamicLightCount));
    SetNativeUniform4fv(g_nativeLitShader.dynamicLightColorExponent,
                        dynamicColorExponent,
                        static_cast<GLsizei>(dynamicLightCount));
    SetNativeUniform4fv(g_nativeLitShader.dynamicLightCone, dynamicCone,
                        static_cast<GLsizei>(dynamicLightCount));
}

float SafeUnitDot(const float left[3], const float right[3]) {
    if (!left || !right)
        return 0.0f;
    const float lengthSquared = left[0] * left[0] + left[1] * left[1]
        + left[2] * left[2];
    if (!std::isfinite(lengthSquared) || lengthSquared <= 0.000001f)
        return 0.0f;
    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    return (left[0] * right[0] + left[1] * right[1] + left[2] * right[2])
        * inverseLength;
}

struct DecodedSourceSunVisibility {
    float shadowVisibility = 1.0f;
    float localLightGridSunWeight = -1.0f;
};

DecodedSourceSunVisibility DecodeSourceSunVisibility(float packed) {
    if (!std::isfinite(packed))
        return {};
    const float encoded = std::clamp(packed * 65535.0f + 0.5f,
                                     0.0f, 65535.0f);
    const float shadowByte = std::floor(encoded / 256.0f);
    const float localByte = encoded - shadowByte * 256.0f;
    DecodedSourceSunVisibility result{};
    result.shadowVisibility = std::clamp(shadowByte / 255.0f, 0.0f, 1.0f);
    if (localByte < 254.5f)
        result.localLightGridSunWeight = std::clamp(localByte / 253.0f,
                                                    0.0f, 1.0f);
    return result;
}

void EvaluatePrimaryLightResponse(const float normal[3],
                                  const float worldPosition[3],
                                  float response[3]) {
    if (!response)
        return;
    response[0] = response[1] = response[2] = 0.0f;
    if (!(g_frameLighting.flags & GFX_FRAME_LIGHTING_VALID) || !normal)
        return;

    // ComPrimaryLight uses the same type values as the desktop renderer:
    // directional/spot/omni are 1/2/3 and their shadow-map variants are
    // 4/5/6. The source primary-light response is additive to the baked
    // lightmap, while the global sun/light-grid response is not.
    for (uint32_t lightIndex = 0;
         lightIndex < g_frameLighting.primaryLightCount; ++lightIndex) {
        if (lightIndex == g_frameLighting.sunPrimaryLightIndex)
            continue;
        const GfxPrimaryLightDescriptor& light =
            g_frameLighting.primaryLights[lightIndex];
        const bool directional = light.type == 1u || light.type == 4u;
        float contribution = 0.0f;
        if (directional) {
            const float directionLengthSquared = light.direction[0]
                * light.direction[0] + light.direction[1]
                * light.direction[1] + light.direction[2]
                * light.direction[2];
            if (std::isfinite(directionLengthSquared)
                && directionLengthSquared > 0.000001f) {
                const float inverseLength = 1.0f
                    / std::sqrt(directionLengthSquared);
                contribution = std::clamp(-(normal[0] * light.direction[0]
                    + normal[1] * light.direction[1]
                    + normal[2] * light.direction[2]) * inverseLength,
                    0.0f, 1.0f);
            }
        } else if (worldPosition && light.radius > 0.0001f) {
            const float toLight[3] = {
                light.origin[0] - worldPosition[0],
                light.origin[1] - worldPosition[1],
                light.origin[2] - worldPosition[2],
            };
            const float distanceSquared = toLight[0] * toLight[0]
                + toLight[1] * toLight[1] + toLight[2] * toLight[2];
            if (std::isfinite(distanceSquared)
                && distanceSquared > 0.000001f) {
                const float distance = std::sqrt(distanceSquared);
                const float inverseDistance = 1.0f / distance;
                const float falloff = std::clamp(
                    1.0f - distance / light.radius, 0.0f, 1.0f);
                const float lightDirection[3] = {
                    toLight[0] * inverseDistance,
                    toLight[1] * inverseDistance,
                    toLight[2] * inverseDistance,
                };
                contribution = std::max(
                    normal[0] * lightDirection[0]
                        + normal[1] * lightDirection[1]
                        + normal[2] * lightDirection[2],
                    0.0f) * falloff;
                const bool spot = light.type == 2u || light.type == 5u;
                if (spot) {
                    const float directionLengthSquared = light.direction[0]
                        * light.direction[0] + light.direction[1]
                        * light.direction[1] + light.direction[2]
                        * light.direction[2];
                    if (std::isfinite(directionLengthSquared)
                        && directionLengthSquared > 0.000001f) {
                        const float inverseDirection = 1.0f
                            / std::sqrt(directionLengthSquared);
                        const float pointDirection[3] = {
                            -lightDirection[0], -lightDirection[1],
                            -lightDirection[2],
                        };
                        const float spotCos = std::clamp(
                            (light.direction[0] * pointDirection[0]
                                + light.direction[1] * pointDirection[1]
                                + light.direction[2] * pointDirection[2])
                                * inverseDirection,
                            -1.0f, 1.0f);
                        const float outer = light.cosHalfFovOuter;
                        const float inner = light.cosHalfFovInner;
                        const float cone = inner > outer
                            ? std::clamp((spotCos - outer)
                                / std::max(inner - outer, 0.0001f),
                                0.0f, 1.0f) : 1.0f;
                        contribution *= cone * cone;
                    }
                }
            }
        }
        if (contribution <= 0.0f)
            continue;
        const float exponent = std::clamp(
            1.0f + static_cast<float>(light.exponent) * 0.25f,
            1.0f, 8.0f);
        const float shaped = directional
            ? contribution : std::pow(contribution, exponent);
        for (uint32_t channel = 0; channel < 3u; ++channel)
            response[channel] += light.color[channel] * shaped;
    }
}

void EvaluateDynamicLightResponse(const float normal[3],
                                  const float worldPosition[3],
                                  float response[3]) {
    if (!response)
        return;
    response[0] = response[1] = response[2] = 0.0f;
    if (!(g_frameLighting.flags & GFX_FRAME_LIGHTING_VALID) || !normal
        || !worldPosition)
        return;

    for (uint32_t lightIndex = 0;
         lightIndex < std::min(g_frameLighting.dynamicLightCount,
                               GFX_MAX_DYNAMIC_LIGHTS); ++lightIndex) {
        const GfxPrimaryLightDescriptor& light =
            g_frameLighting.dynamicLights[lightIndex];
        const bool spot = light.type == 2u || light.type == 5u;
        const bool omni = light.type == 3u || light.type == 6u;
        if ((!spot && !omni) || light.radius <= 0.0001f)
            continue;
        const float toLight[3] = {
            light.origin[0] - worldPosition[0],
            light.origin[1] - worldPosition[1],
            light.origin[2] - worldPosition[2],
        };
        const float distanceSquared = toLight[0] * toLight[0]
            + toLight[1] * toLight[1] + toLight[2] * toLight[2];
        if (!std::isfinite(distanceSquared)
            || distanceSquared <= 0.000001f)
            continue;
        const float distance = std::sqrt(distanceSquared);
        const float inverseDistance = 1.0f / distance;
        const float falloff = std::clamp(
            1.0f - distance / light.radius, 0.0f, 1.0f);
        const float lightDirection[3] = {
            toLight[0] * inverseDistance,
            toLight[1] * inverseDistance,
            toLight[2] * inverseDistance,
        };
        float contribution = std::max(
            normal[0] * lightDirection[0]
                + normal[1] * lightDirection[1]
                + normal[2] * lightDirection[2],
            0.0f) * falloff;
        if (spot) {
            const float directionLengthSquared = light.direction[0]
                * light.direction[0] + light.direction[1]
                * light.direction[1] + light.direction[2]
                * light.direction[2];
            if (!std::isfinite(directionLengthSquared)
                || directionLengthSquared <= 0.000001f)
                continue;
            const float inverseDirection = 1.0f
                / std::sqrt(directionLengthSquared);
            const float pointDirection[3] = {
                -lightDirection[0], -lightDirection[1], -lightDirection[2],
            };
            const float spotCos = std::clamp(
                (light.direction[0] * pointDirection[0]
                    + light.direction[1] * pointDirection[1]
                    + light.direction[2] * pointDirection[2])
                    * inverseDirection,
                -1.0f, 1.0f);
            const float outer = light.cosHalfFovOuter;
            const float inner = light.cosHalfFovInner;
            const float cone = inner > outer
                ? std::clamp((spotCos - outer)
                    / std::max(inner - outer, 0.0001f), 0.0f, 1.0f)
                : 1.0f;
            contribution *= cone * cone;
        }
        const float exponent = std::clamp(
            1.0f + static_cast<float>(light.exponent) * 0.25f,
            1.0f, 8.0f);
        contribution = std::pow(std::max(contribution, 0.0f), exponent);
        const float visibility = std::clamp(
            std::isfinite(light.shadowVisibility)
                ? light.shadowVisibility : 1.0f,
            0.0f, 1.0f);
        for (uint32_t channel = 0; channel < 3u; ++channel)
            response[channel] += light.color[channel]
                * contribution * visibility;
    }
}

void EvaluateFrameLighting(const float normal[3], const float worldPosition[3],
                           float shadowVisibility,
                           float localLightGridSunWeight,
                           float factors[3]) {
    if (!factors)
        return;
    factors[0] = factors[1] = factors[2] = 1.0f;
    if (!(g_frameLighting.flags & GFX_FRAME_LIGHTING_VALID) || !normal)
        return;
    const float normalLengthSquared = normal[0] * normal[0]
        + normal[1] * normal[1] + normal[2] * normal[2];
    if (!std::isfinite(normalLengthSquared) || normalLengthSquared <= 0.000001f)
        return;
    const float normalScale = 1.0f / std::sqrt(normalLengthSquared);
    const float n[3] = {normal[0] * normalScale, normal[1] * normalScale,
                        normal[2] * normalScale};
    float sun[3] = {g_frameLighting.sunDirection[0],
                    g_frameLighting.sunDirection[1],
                    g_frameLighting.sunDirection[2]};
    const float sunLengthSquared = sun[0] * sun[0] + sun[1] * sun[1]
        + sun[2] * sun[2];
    if (std::isfinite(sunLengthSquared) && sunLengthSquared > 0.000001f) {
        const float sunScale = 1.0f / std::sqrt(sunLengthSquared);
        sun[0] *= sunScale;
        sun[1] *= sunScale;
        sun[2] *= sunScale;
    }
    const float sunDot = std::clamp(-(n[0] * sun[0] + n[1] * sun[1]
                                      + n[2] * sun[2]), 0.0f, 1.0f);
    float primaryLightResponse[3] = {0.0f, 0.0f, 0.0f};
    EvaluatePrimaryLightResponse(n, worldPosition, primaryLightResponse);
    float dynamicLightResponse[3] = {0.0f, 0.0f, 0.0f};
    EvaluateDynamicLightResponse(n, worldPosition, dynamicLightResponse);
    for (uint32_t channel = 0; channel < 3u; ++channel)
        primaryLightResponse[channel] += dynamicLightResponse[channel];
    const float sourceSunVisibility = std::clamp(
        std::isfinite(shadowVisibility) ? shadowVisibility : 1.0f,
        0.0f, 1.0f);
    const float ambientMax = std::max({g_frameLighting.ambientColor[0],
                                       g_frameLighting.ambientColor[1],
                                       g_frameLighting.ambientColor[2]});
    const float ambientScale = std::isfinite(g_frameLighting.ambientScale)
        ? g_frameLighting.ambientScale : 0.0f;
    const float ambientDenom = ambientMax > 0.000001f ? ambientMax : 1.0f;
    const float sunFloor = std::max(0.0f,
        (g_frameLighting.sunLight - g_frameLighting.ambientScale)
            * g_frameLighting.diffuseFraction);
    const float frameIndoor = std::clamp(
        std::isfinite(g_frameLighting.indoorFactor)
            ? g_frameLighting.indoorFactor : 1.0f,
        0.20f, 1.0f);
    const bool hasShadowDescriptor =
        (g_shadowPass.flags & GFX_SHADOW_PASS_ENABLED) != 0;
    const bool hasLocalLightGridSunWeight =
        std::isfinite(localLightGridSunWeight)
        && localLightGridSunWeight >= 0.0f;
    const float indoor = hasLocalLightGridSunWeight
        ? 1.0f
        : (hasShadowDescriptor
            ? std::min(frameIndoor, g_shadowPass.indoorFactor) : frameIndoor);
    for (uint32_t channel = 0; channel < 3; ++channel) {
        const float frameGrid = std::clamp(std::isfinite(
            g_frameLighting.lightGridColor[channel])
                ? g_frameLighting.lightGridColor[channel] : 1.0f,
            0.0f, 2.0f);
        // ProjectPoint has already applied the source local/camera lightgrid
        // ratio when this is a valid local sample. Keep the camera grid for
        // legacy/no-source submissions, but avoid applying it twice here.
        const float grid = hasLocalLightGridSunWeight
            ? 1.0f
            : (hasShadowDescriptor
                ? std::min(frameGrid, g_shadowPass.lightGridColor[channel])
                : frameGrid);
        float response = (g_frameLighting.ambientColor[channel] / ambientDenom)
            * ambientScale + g_frameLighting.diffuseColor[channel] * sunFloor
            + g_frameLighting.sunColor[channel] * sunDot
                * sourceSunVisibility;
        // Indoor lightgrid samples reduce the directional term first, while
        // retaining authored bounce light and material colour.
        const float frameSunWeight = std::clamp(std::isfinite(
            g_frameLighting.lightGridSunWeight)
                ? g_frameLighting.lightGridSunWeight : 1.0f,
            0.0f, 1.0f);
        const float sunWeight = hasLocalLightGridSunWeight
            ? std::clamp(localLightGridSunWeight, 0.0f, 1.0f)
            : (hasShadowDescriptor
                ? std::min(frameSunWeight, g_shadowPass.lightGridSunWeight)
                : frameSunWeight);
        response = response * (0.45f + 0.55f * sunWeight) * indoor * grid;
        response += primaryLightResponse[channel];
        if (!std::isfinite(response) || response <= 0.0f)
            response = 1.0f;
        // Match SourceModelLightingFactors for unlightmapped cards: keep the
        // authored frame/lightgrid contrast, but bound a valid low indoor
        // response so the material is not reduced to an unusable black
        // silhouette. Lightmapped BSP fragments bypass this term entirely.
        factors[channel] = std::clamp(response, 0.35f, 1.35f);
    }
}
bool IsCubeTextureHandle(uint32_t handle) {
    return std::find(g_cubeTextureHandles.begin(), g_cubeTextureHandles.end(),
                     handle) != g_cubeTextureHandles.end();
}

void RemoveCubeTextureHandle(uint32_t handle) {
    g_cubeTextureHandles.erase(
        std::remove(g_cubeTextureHandles.begin(), g_cubeTextureHandles.end(), handle),
        g_cubeTextureHandles.end());
}
// Vita3K's display fallback is a diagnostic renderer, not the physical Vita
// submission path. Bound its scalar rasterizer per frame so a large BSP
// triangle or a dense compatibility menu cannot monopolize the emulator.
// The scalar fallback is intentionally bounded while Vita3K lacks the GXM
// shader compiler, but its priority is now scene readability rather than an
// arbitrary FPS floor. The authored world stream and 65K projected-triangle
// budget remain in place; a one-pixel sample grid and 64-million-sample pass
// cap let Shipment's near/far occlusion, thin rails, and material edges read
// as a coherent 3D scene even when the emulator runs below 10 FPS.
constexpr size_t kSoftwareRasterPixelBudget = 64u * 1024u * 1024u;
constexpr int kSoftwareRasterStep = 1;
size_t g_softwareRasterPixels = 0;
FILE* g_softwareLog = nullptr;
bool g_softwareFileLogEnabled = true;
bool g_softwareCaptureChecked = false;
uint32_t g_softwareFrameCount = 0;
uint32_t g_softwareCaptureProbeCount = 0;
// Vita3K's display queue is still a separate compatibility surface from the
// scalar framebuffer.  Keep one diagnostic switch so a capture-only run can
// qualify the rasterizer without handing Vita3K a guest display pointer.
constexpr bool kSoftwareSubmitDisplay = true;

void SoftwareLog(const char* message) {
    if (!g_softwareLog || !message)
        return;
    std::fputs(message, g_softwareLog);
    std::fputc('\n', g_softwareLog);
    std::fflush(g_softwareLog);
}

bool SoftwarePointersValid() {
    // The scalar diagnostic uses separate statically reserved raster and
    // display framebuffers. The old validator required two distinct buffers
    // but initialization still pointed both roles at one allocation,
    // consequently rejecting every clear/draw/present operation.
    // Lifetime is still guarded by the null reset in SoftwareShutdown; the
    // depth/pixel invariant prevents writes after a partial initialization.
    return g_framebuffer != nullptr && g_displayFramebuffer != nullptr
        && g_softwarePixelCount != 0
        && g_depth.size() == g_softwarePixelCount;
}

void SoftwareLogState(const char* phase) {
    if (!g_softwareLog || !phase)
        return;
    char detail[192]{};
    std::snprintf(detail, sizeof(detail),
                  "%s fb=%p display=%p pixels=%u depth=%u invalid=%u",
                  phase, static_cast<void*>(g_framebuffer),
                  static_cast<void*>(g_displayFramebuffer),
                  static_cast<unsigned>(g_softwarePixelCount),
                  static_cast<unsigned>(g_depth.size()),
                  static_cast<unsigned>(g_softwareInvalidStores));
    SoftwareLog(detail);
}

uint32_t PackColor(float r, float g, float b, float a) {
    const auto ToByte = [](float value) -> uint32_t {
        if (!std::isfinite(value))
            value = 0.0f;
        return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    return ToByte(r) | (ToByte(g) << 8) | (ToByte(b) << 16) | (ToByte(a) << 24);
}

void UnpackSoftwareColor(uint32_t source, float output[4]) {
    if (!output)
        return;
    output[0] = static_cast<float>(source & 0xFFu) / 255.0f;
    output[1] = static_cast<float>((source >> 8u) & 0xFFu) / 255.0f;
    output[2] = static_cast<float>((source >> 16u) & 0xFFu) / 255.0f;
    output[3] = static_cast<float>((source >> 24u) & 0xFFu) / 255.0f;
}

float SoftwareFogAmount(float depth) {
    // The retail sky material is sampled at infinity and is not part of the
    // distance-fogged world color pass.  Applying the exponential fog to its
    // encoded depth (1.0) turns the authored cloud cubemap into a flat grey
    // sheet in the Vita3K scalar path.  Keep fog on world/prop surfaces, but
    // leave the dedicated sky pass at its source color.
    if (!g_softwareFogEnabled
        || g_softwareSurfaceMode == GfxSurfaceMode::Overlay
        || g_softwareSurfaceMode == GfxSurfaceMode::Sky
        || !std::isfinite(depth) || g_softwareFogDensity <= 0.0f)
        return 0.0f;
    // Projected Vita vertices retain the same reciprocal-depth encoding used
    // by the GL path: z = 1 - near / cameraZ. Recover a bounded camera
    // distance so the scalar renderer evaluates the retail exponential fog
    // equation without introducing a second, platform-specific projection.
    constexpr float kNearPlane = 2.0f;
    constexpr float kFarPlane = 8192.0f;
    const float encoded = std::clamp(depth, 0.0f, 0.99975586f);
    const float cameraDistance = std::clamp(
        kNearPlane / std::max(0.00024414f, 1.0f - encoded),
        kNearPlane, kFarPlane);
    const float distanceFromStart = std::max(0.0f, cameraDistance - g_softwareFogStart);
    return std::clamp(
        1.0f - std::exp(-g_softwareFogDensity * distanceFromStart),
        0.0f, 1.0f);
}

void ApplySoftwareFog(float color[4], float depth) {
    if (!color)
        return;
    const float fogAmount = SoftwareFogAmount(depth);
    if (fogAmount <= 0.0f)
        return;
    for (uint32_t channel = 0; channel < 3u; ++channel)
        color[channel] = color[channel] * (1.0f - fogAmount)
            + std::clamp(g_softwareFogColor[channel], 0.0f, 1.0f)
                * fogAmount;
}

uint32_t Expand565(uint16_t value) {
    const uint32_t r = ((value >> 11) & 0x1Fu) * 255u / 31u;
    const uint32_t g = ((value >> 5) & 0x3Fu) * 255u / 63u;
    const uint32_t b = (value & 0x1Fu) * 255u / 31u;
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

uint32_t LerpColor(uint32_t a, uint32_t b, uint32_t aWeight, uint32_t bWeight,
                   uint32_t divisor) {
    const uint32_t r = (((a & 0xFFu) * aWeight) + ((b & 0xFFu) * bWeight)) / divisor;
    const uint32_t g = ((((a >> 8) & 0xFFu) * aWeight)
                        + (((b >> 8) & 0xFFu) * bWeight)) / divisor;
    const uint32_t bChannel = ((((a >> 16) & 0xFFu) * aWeight)
                               + (((b >> 16) & 0xFFu) * bWeight)) / divisor;
    return r | (g << 8) | (bChannel << 16) | 0xFF000000u;
}

uint16_t PackRgba4444(uint32_t value) {
    const uint16_t red = static_cast<uint16_t>((value & 0xFFu) >> 4u);
    const uint16_t green = static_cast<uint16_t>(((value >> 8u) & 0xFFu) >> 4u);
    const uint16_t blue = static_cast<uint16_t>(((value >> 16u) & 0xFFu) >> 4u);
    const uint16_t alpha = static_cast<uint16_t>(((value >> 24u) & 0xFFu) >> 4u);
    return static_cast<uint16_t>(red | (green << 4u) | (blue << 8u)
                                 | (alpha << 12u));
}

// VITI format 21 is D3DFMT_A8R8G8B8.  Its authored payload is laid out as
// BGRA bytes on the little-endian targets used by the source renderer, and
// the native upload below deliberately presents it to GL as GL_BGRA.  Keep
// the scalar sampler in the same logical RGBA order used by the DXT decoder
// and by PackRgba4444; otherwise the secondary lightmap's red/blue lobes are
// exchanged only on the Vita3K oracle.
uint32_t CanonicalizeA8R8G8B8(uint32_t value) {
    return ((value >> 16u) & 0xFFu)
        | (value & 0x0000FF00u)
        | ((value & 0x000000FFu) << 16u)
        | (value & 0xFF000000u);
}

void FitTextureDimensions(uint32_t width, uint32_t height,
                          uint32_t maxDimension, uint32_t* outputWidth,
                          uint32_t* outputHeight) {
    if (!outputWidth || !outputHeight || width == 0 || height == 0
        || maxDimension == 0) {
        return;
    }
    const uint32_t sourceMax = std::max(width, height);
    if (sourceMax <= maxDimension) {
        *outputWidth = width;
        *outputHeight = height;
        return;
    }
    // Keep normalized material/lightmap UVs tied to the same authored image
    // proportions. Squashing a 1024x2048 atlas into 256x256 changes which
    // lighting band a surface samples even though the UVs themselves look
    // valid; the scalar copy must be a bounded aspect-preserving image.
    const uint64_t roundedWidth = static_cast<uint64_t>(width)
        * maxDimension + sourceMax / 2u;
    const uint64_t roundedHeight = static_cast<uint64_t>(height)
        * maxDimension + sourceMax / 2u;
    *outputWidth = std::clamp(
        static_cast<uint32_t>(roundedWidth / sourceMax), 1u, maxDimension);
    *outputHeight = std::clamp(
        static_cast<uint32_t>(roundedHeight / sourceMax), 1u, maxDimension);
}

uint32_t ExpandRgba4444(uint16_t value) {
    const uint32_t r = (value & 0xFu) * 17u;
    const uint32_t g = ((value >> 4u) & 0xFu) * 17u;
    const uint32_t b = ((value >> 8u) & 0xFu) * 17u;
    const uint32_t a = ((value >> 12u) & 0xFu) * 17u;
    return r | (g << 8u) | (b << 16u) | (a << 24u);
}

template <typename StoreTexel>
void DecodeDxtBlockImpl(const uint8_t* source, uint32_t format,
                        uint32_t destinationWidth, uint32_t blockX,
                        uint32_t blockY, StoreTexel&& store) {
    if (!source || destinationWidth == 0)
        return;
    const bool dxt1 = format == 0x31545844u;
    // BC2/BC3 (DXT3/DXT5) store the eight-byte alpha block first and the
    // DXT1-compatible colour block second.  The old fallback treated every
    // format as DXT1-at-offset-zero, so authored font/HUD atlases were
    // decoded from alpha/index bytes and appeared as coloured static.  Keep
    // DXT1's layout unchanged and point the shared colour decoder at the
    // second half for DXT3/DXT5.
    const uint8_t* colorBlock = dxt1 ? source : source + 8;
    const uint16_t color0 = static_cast<uint16_t>(colorBlock[0]
        | (colorBlock[1] << 8));
    const uint16_t color1 = static_cast<uint16_t>(colorBlock[2]
        | (colorBlock[3] << 8));
    uint32_t colors[4] = {Expand565(color0), Expand565(color1), 0, 0};
    // DXT3/DXT5 retain a four-colour RGB palette regardless of the endpoint
    // ordering; only DXT1 switches to the three-colour + transparent mode.
    if (!dxt1 || color0 > color1) {
        colors[2] = LerpColor(colors[0], colors[1], 2, 1, 3);
        colors[3] = LerpColor(colors[0], colors[1], 1, 2, 3);
    } else {
        colors[2] = LerpColor(colors[0], colors[1], 1, 1, 2);
        colors[3] = 0;
    }
    const uint32_t indices = static_cast<uint32_t>(colorBlock[4])
        | (static_cast<uint32_t>(colorBlock[5]) << 8)
        | (static_cast<uint32_t>(colorBlock[6]) << 16)
        | (static_cast<uint32_t>(colorBlock[7]) << 24);
    const uint8_t* alpha = source;
    uint8_t alphaValues[16];
    if (format == 0x33545844u) { // DXT3
        for (uint32_t pixel = 0; pixel < 16; ++pixel)
            alphaValues[pixel] = static_cast<uint8_t>(((alpha[pixel / 2]
                >> ((pixel & 1u) * 4u)) & 0xFu) * 17u);
    } else if (format == 0x35545844u) { // DXT5
        const uint8_t a0 = alpha[0];
        const uint8_t a1 = alpha[1];
        uint8_t palette[8] = {a0, a1, 0, 0, 0, 0, 0, 0};
        if (a0 > a1) {
            for (uint32_t index = 0; index < 6; ++index)
                palette[index + 2] = static_cast<uint8_t>(((6u - index) * a0
                    + (1u + index) * a1) / 7u);
        } else {
            for (uint32_t index = 0; index < 4; ++index)
                palette[index + 2] = static_cast<uint8_t>(((4u - index) * a0
                    + (1u + index) * a1) / 5u);
            palette[6] = 0;
            palette[7] = 255;
        }
        uint64_t alphaIndices = static_cast<uint64_t>(alpha[2])
            | (static_cast<uint64_t>(alpha[3]) << 8)
            | (static_cast<uint64_t>(alpha[4]) << 16)
            | (static_cast<uint64_t>(alpha[5]) << 24)
            | (static_cast<uint64_t>(alpha[6]) << 32)
            | (static_cast<uint64_t>(alpha[7]) << 40);
        for (uint32_t pixel = 0; pixel < 16; ++pixel)
            alphaValues[pixel] = palette[(alphaIndices >> (pixel * 3u)) & 7u];
    } else {
        for (uint8_t& value : alphaValues)
            value = 255;
    }
    for (uint32_t pixel = 0; pixel < 16; ++pixel) {
        const uint32_t x = blockX * 4u + pixel % 4u;
        const uint32_t y = blockY * 4u + pixel / 4u;
        const uint32_t colorIndex = (indices >> (pixel * 2u)) & 3u;
        uint32_t value = colors[colorIndex];
        // DXT1 uses color index 3 as a transparent texel when color0 <=
        // color1. Preserve that alpha in the software fallback; forcing it
        // opaque turns fence/grate/trim cutouts into conspicuous black bands.
        const uint8_t decodedAlpha = dxt1 && color0 <= color1 && colorIndex == 3u
            ? 0u : alphaValues[pixel];
        value = (value & 0x00FFFFFFu) | (static_cast<uint32_t>(decodedAlpha) << 24);
        if (x < destinationWidth)
            store(static_cast<size_t>(y) * destinationWidth + x, value);
    }
}

void DecodeDxtBlock(const uint8_t* source, uint32_t format, uint16_t* destination,
                    uint32_t destinationWidth, uint32_t blockX, uint32_t blockY) {
    if (!destination)
        return;
    DecodeDxtBlockImpl(source, format, destinationWidth, blockX, blockY,
                      [destination](size_t index, uint32_t value) {
                          destination[index] = PackRgba4444(value);
                      });
}

void DecodeDxtBlockRgba8888(const uint8_t* source, uint32_t format,
                            uint32_t* destination, uint32_t destinationWidth,
                            uint32_t blockX, uint32_t blockY) {
    if (!destination)
        return;
    DecodeDxtBlockImpl(source, format, destinationWidth, blockX, blockY,
                      [destination](size_t index, uint32_t value) {
                          destination[index] = value;
                      });
}

#if KISAK_VITA_NATIVE_CPU_TEXTURE_UPLOAD
// VitaGL's native compressed upload uses a GXM transfer path.  Keep this
// bounded diagnostic decoder at the same source-DXT seam so a Vita3K capture
// can tell whether invalid guest writes originate in that transfer path.  It
// is intentionally limited to 16 MiB 2D levels: larger authored images remain
// on the normal compressed route, and no extra decoded image is retained after
// glTexImage2D returns.
bool DecodeNativeDxtLevelRgba8888(const uint8_t* payload, size_t payloadBytes,
                                  uint32_t width, uint32_t height,
                                  uint32_t format,
                                  std::vector<uint32_t>* output) {
    if (!payload || !output || width == 0 || height == 0
        || (format != 0x31545844u && format != 0x33545844u
            && format != 0x35545844u))
        return false;
    const uint32_t blocksWide = (width + 3u) / 4u;
    const uint32_t blocksHigh = (height + 3u) / 4u;
    const uint32_t blockBytes = format == 0x31545844u ? 8u : 16u;
    const uint64_t required = static_cast<uint64_t>(blocksWide)
        * blocksHigh * blockBytes;
    const uint64_t texelCount = static_cast<uint64_t>(width) * height;
    constexpr uint64_t kMaxDecodedBytes = 16u * 1024u * 1024u;
    if (required > payloadBytes || texelCount == 0
        || texelCount * sizeof(uint32_t) > kMaxDecodedBytes
        || texelCount > std::numeric_limits<size_t>::max())
        return false;
    output->assign(static_cast<size_t>(texelCount), 0u);
    size_t offset = 0;
    for (uint32_t blockY = 0; blockY < blocksHigh; ++blockY) {
        for (uint32_t blockX = 0; blockX < blocksWide; ++blockX) {
            DecodeDxtBlockRgba8888(payload + offset, format, output->data(),
                                   width, blockX, blockY);
            offset += blockBytes;
        }
    }
    return true;
}

uint32_t g_nativeCpuTextureUploadTraceCount = 0;
#endif

uint32_t SampleSourceTexel(const uint8_t* payload, size_t payloadBytes,
                           uint32_t width, uint32_t height, uint32_t format,
                           uint32_t x, uint32_t y) {
    if (!payload || width == 0 || height == 0 || x >= width || y >= height)
        return 0xFFFFFFFFu;
    if (format == 50u) {
        const size_t offset = static_cast<size_t>(y) * width + x;
        return offset < payloadBytes
            ? (static_cast<uint32_t>(payload[offset])
               | (static_cast<uint32_t>(payload[offset]) << 8u)
               | (static_cast<uint32_t>(payload[offset]) << 16u)
               | 0xFF000000u)
            : 0xFFFFFFFFu;
    }
    if (format == 21u) {
        const size_t offset = (static_cast<size_t>(y) * width + x)
            * sizeof(uint32_t);
        if (offset > payloadBytes || payloadBytes - offset < sizeof(uint32_t))
            return 0xFFFFFFFFu;
        uint32_t value = 0;
        std::memcpy(&value, payload + offset, sizeof(value));
        return CanonicalizeA8R8G8B8(value);
    }
    if (format != 0x31545844u && format != 0x33545844u
        && format != 0x35545844u)
        return 0xFFFFFFFFu;
    const uint32_t blocksWide = (width + 3u) / 4u;
    const uint32_t blockBytes = format == 0x31545844u ? 8u : 16u;
    const size_t offset = (static_cast<size_t>(y / 4u) * blocksWide
                           + x / 4u) * blockBytes;
    if (offset > payloadBytes || payloadBytes - offset < blockBytes)
        return 0xFFFFFFFFu;
    uint32_t block[16]{};
    DecodeDxtBlockRgba8888(payload + offset, format, block, 4u, 0u, 0u);
    return block[(y & 3u) * 4u + (x & 3u)];
}

// Decode a bounded source thumbnail for the scalar resampler.  Expanding a
// 4096px authored image just to immediately reduce it to a 192/256px
// diagnostic copy creates a transient allocation large enough to trip
// Vita3K's guest heap.  Sampling the source blocks into a capped 1024px grid
// keeps the source material and UV relationship intact without retaining the
// full-resolution intermediate.
bool DecodeSoftwareTextureThumbnail(const uint8_t* payload, size_t payloadBytes,
                                    uint32_t width, uint32_t height,
                                    uint32_t format, SoftwareTexture* output,
                                    uint32_t maxDimension = 1024u,
                                    bool highPrecision = false) {
    if (!payload || !output || width == 0 || height == 0
        || width > 4096 || height > 4096 || maxDimension == 0)
        return false;
    uint32_t thumbnailWidth = 0;
    uint32_t thumbnailHeight = 0;
    FitTextureDimensions(width, height, maxDimension, &thumbnailWidth,
                         &thumbnailHeight);
    if (thumbnailWidth == 0 || thumbnailHeight == 0)
        return false;
    output->width = thumbnailWidth;
    output->height = thumbnailHeight;
    output->pixels.clear();
    output->highPrecisionPixels.clear();
    const size_t thumbnailTexels = static_cast<size_t>(thumbnailWidth)
        * thumbnailHeight;
    if (highPrecision)
        output->highPrecisionPixels.assign(thumbnailTexels, 0xFFFFFFFFu);
    else
        output->pixels.assign(thumbnailTexels, PackRgba4444(0xFFFFFFFFu));
    for (uint32_t y = 0; y < thumbnailHeight; ++y) {
        const uint32_t sourceY = std::min(
            height - 1u,
            static_cast<uint32_t>((static_cast<uint64_t>(y) * height)
                                  / thumbnailHeight));
        for (uint32_t x = 0; x < thumbnailWidth; ++x) {
            const uint32_t sourceX = std::min(
                width - 1u,
                static_cast<uint32_t>((static_cast<uint64_t>(x) * width)
                                      / thumbnailWidth));
            const uint32_t value = SampleSourceTexel(
                payload, payloadBytes, width, height, format, sourceX, sourceY);
            const size_t index = static_cast<size_t>(y) * thumbnailWidth + x;
            if (highPrecision)
                output->highPrecisionPixels[index] = value;
            else
                output->pixels[index] = PackRgba4444(value);
        }
    }
    return true;
}

[[maybe_unused]] bool DecodeSoftwareTexture(const uint8_t* payload, size_t payloadBytes,
                           uint32_t width, uint32_t height, uint32_t format,
                           SoftwareTexture* output,
                           bool highPrecision = false) {
    if (!payload || !output || width == 0 || height == 0
        || width > 4096 || height > 4096)
        return false;
    output->width = width;
    output->height = height;
    output->pixels.clear();
    output->highPrecisionPixels.clear();
    const size_t texelCount = static_cast<size_t>(width) * height;
    if (highPrecision)
        output->highPrecisionPixels.assign(texelCount, 0xFFFFFFFFu);
    else
        output->pixels.assign(texelCount, PackRgba4444(0xFFFFFFFFu));
    if (format == 50u) { // D3DFMT_L8
        const size_t bytes = static_cast<size_t>(width) * height;
        if (payloadBytes < bytes)
            return false;
        for (size_t index = 0; index < bytes; ++index) {
            const uint32_t value = payload[index];
            const uint32_t expanded = value | (value << 8) | (value << 16)
                | 0xFF000000u;
            if (highPrecision)
                output->highPrecisionPixels[index] = expanded;
            else
                output->pixels[index] = PackRgba4444(expanded);
        }
        return true;
    }
    if (format == 21u) { // D3DFMT_A8R8G8B8
        const size_t bytes = static_cast<size_t>(width) * height * sizeof(uint32_t);
        if (payloadBytes < bytes)
            return false;
        for (size_t index = 0; index < static_cast<size_t>(width) * height; ++index) {
            uint32_t value = 0;
            std::memcpy(&value, payload + index * sizeof(uint32_t), sizeof(value));
            value = CanonicalizeA8R8G8B8(value);
            if (highPrecision)
                output->highPrecisionPixels[index] = value;
            else
                output->pixels[index] = PackRgba4444(value);
        }
        return true;
    }
    if (format != 0x31545844u && format != 0x33545844u
        && format != 0x35545844u)
        return false;
    const uint32_t blocksWide = (width + 3u) / 4u;
    const uint32_t blocksHigh = (height + 3u) / 4u;
    const uint32_t blockBytes = format == 0x31545844u ? 8u : 16u;
    const uint64_t required = static_cast<uint64_t>(blocksWide) * blocksHigh * blockBytes;
    if (required > payloadBytes)
        return false;
    size_t offset = 0;
    for (uint32_t blockY = 0; blockY < blocksHigh; ++blockY) {
        for (uint32_t blockX = 0; blockX < blocksWide; ++blockX) {
            if (highPrecision) {
                DecodeDxtBlockRgba8888(payload + offset, format,
                                       output->highPrecisionPixels.data(), width,
                                       blockX, blockY);
            } else {
                DecodeDxtBlock(payload + offset, format, output->pixels.data(),
                               width, blockX, blockY);
            }
            offset += blockBytes;
        }
    }
    return true;
}

uint32_t SoftwareTextureTexel(const SoftwareTexture& texture, size_t index) {
    if (index < texture.highPrecisionPixels.size())
        return texture.highPrecisionPixels[index];
    if (index < texture.pixels.size())
        return ExpandRgba4444(texture.pixels[index]);
    if (!texture.sourcePayload.empty() && texture.sourceFormat != 0u) {
        const size_t texelCount = static_cast<size_t>(texture.width)
            * texture.height;
        if (index >= texelCount || texture.width == 0u)
            return 0xFFFFFFFFu;
        const uint32_t x = static_cast<uint32_t>(index
            % static_cast<size_t>(texture.width));
        const uint32_t y = static_cast<uint32_t>(index
            / static_cast<size_t>(texture.width));
        return SampleSourceTexel(texture.sourcePayload.data(),
                                 texture.sourcePayload.size(), texture.width,
                                 texture.height, texture.sourceFormat, x, y);
    }
    return 0xFFFFFFFFu;
}

void SoftwareClear(float r, float g, float b, float a) {
    if (!SoftwarePointersValid() || g_depth.size() != g_softwarePixelCount)
        return;
    const uint32_t color = PackColor(r, g, b, a);
    std::fill(g_framebuffer, g_framebuffer + static_cast<size_t>(g_width) * g_height, color);
    for (size_t pixel = 0; pixel < g_softwarePixelCount; ++pixel) {
        g_softwareSceneColor[pixel * 3u + 0u] = r;
        g_softwareSceneColor[pixel * 3u + 1u] = g;
        g_softwareSceneColor[pixel * 3u + 2u] = b;
    }
    std::fill(g_depth.begin(), g_depth.end(), 1.0f);
}

bool SoftwareInit(uint32_t width, uint32_t height) {
    g_softwareLog = g_softwareFileLogEnabled
        ? std::fopen("ux0:data/CODVIT001/logs/gfx-fallback.log", "wb")
        : nullptr;
    // Vita3K can return a low sentinel from the diagnostic fopen shim when the
    // guest heap is exhausted by a very large BSP. Treat that as an absent
    // optional log sink; calling fputs on the sentinel turns a recoverable
    // renderer startup into an access violation at address 0x1.
    if (reinterpret_cast<uintptr_t>(g_softwareLog) < 0x10000u)
        g_softwareLog = nullptr;
    SoftwareLog("init");
#if defined(KISAK_VITA_CAPTURE_SOFTWARE_FRAME)
    SoftwareLog("capture-enabled-build-20260829");
#endif
    g_width = width;
    g_height = height;
    const size_t pixelCount = static_cast<size_t>(width) * height;
    if (pixelCount != std::size(g_softwareStaticFramebuffer)) {
        SoftwareLog("framebuffer dimensions unsupported");
        return false;
    }
    g_framebuffer = g_softwareStaticFramebuffer;
    g_displayFramebuffer = g_softwareDisplayFramebuffer;
    SoftwareLog("framebuffer static double-buffer");
    g_softwarePixelCount = pixelCount;
    g_framebufferBase = reinterpret_cast<uintptr_t>(g_framebuffer);
    g_displayFramebufferBase = reinterpret_cast<uintptr_t>(g_displayFramebuffer);
    g_softwareInvalidStores = 0;
    SoftwareLog("framebuffer allocated");
    g_depth.assign(pixelCount, 1.0f);
    g_textures.clear();
    // Texture handles are integers into this table; reserving the complete
    // package-sized table avoids reallocating the metadata vector while the
    // scalar path is expanding the authored world images.
    g_textures.reserve(192u);
    g_textures.emplace_back(); // handle zero is the untextured state.
    g_textureBytes = 0;
    std::fill(std::begin(g_softwareSamplerState),
              std::end(g_softwareSamplerState), 0x0Bu);
    g_softwareCaptureChecked = false;
    g_softwareFrameCount = 0;
    g_softwareCaptureProbeCount = 0;
    g_softwareDepthTest = true;
    g_softwareDepthWrite = true;
    g_softwareSurfaceMode = GfxSurfaceMode::Opaque;
    g_softwareCullMode = GfxCullMode::None;
    g_boundTexture = 0;
    g_softwareLightmapPrimary = 0;
    g_softwareLightmapSecondary = 0;
    g_softwareLightmapSamplerFlags = 0;
    g_softwareFogEnabled = false;
    g_softwareFogStart = 0.0f;
    g_softwareFogDensity = 0.0f;
    std::fill(std::begin(g_softwareFogColor), std::end(g_softwareFogColor), 0.0f);
    g_frameLighting = {};
    g_materialPass = {};
    g_shadowPass = {};
    g_effectBatch = {};
    g_viewEffects = {};
    g_rendererFrame = 0;
    g_frameDrawCalls = 0;
    g_frameLightmappedDraws = 0;
    g_frameMaterialPasses = 0;
    g_frameNormalPasses = 0;
    g_frameSpecularPasses = 0;
    g_framePrimaryLightmapPasses = 0;
    g_frameSecondaryLightmapPasses = 0;
    g_frameShadowPasses = 0;
    g_frameEffectBatches = 0;
    g_frameWeatherDraws = 0;
    g_frameWeatherDrops = 0;
    SoftwareClear(0.02f, 0.025f, 0.04f, 1.0f);
    std::memcpy(g_displayFramebuffer, g_framebuffer,
                pixelCount * sizeof(uint32_t));
    int result = 0;
    if (kSoftwareSubmitDisplay) {
        SceDisplayFrameBuf frame{};
        frame.size = sizeof(frame);
        frame.base = g_displayFramebuffer;
        frame.pitch = width;
        frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
        frame.width = width;
        frame.height = height;
        result = sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
        SoftwareLog(result >= 0 ? "display set ok" : "display set failed");
    } else {
        SoftwareLog("display submit disabled");
    }
    sceClibPrintf("[kisak-vita] software framebuffer fallback result=%x size=%ux%u\n",
                  result, width, height);
    return result >= 0;
}

void SoftwareShutdown() {
    SoftwareLog("shutdown");
    // The fallback buffers are statically reserved and therefore remain valid
    // until the guest process is torn down. Vita3K reports
    // SCE_DISPLAY_SETBUF_IMMEDIATE as unsupported; submitting a null frame at
    // this boundary can make its display worker dereference address 0xC while
    // the app is closing. Leave the last valid NEXTFRAME queued and let the
    // emulator discard it with the process, without touching the unsupported
    // detach import.
    SoftwareLog("display left queued for process teardown");
    g_textures.clear();
    g_textureBytes = 0;
    g_depth.clear();
    g_framebuffer = nullptr;
    g_displayFramebuffer = nullptr;
    g_softwarePixelCount = 0;
    g_framebufferBase = 0;
    g_displayFramebufferBase = 0;
    g_softwareInvalidStores = 0;
    g_softwareFogEnabled = false;
    g_softwareFogStart = 0.0f;
    g_softwareFogDensity = 0.0f;
    std::fill(std::begin(g_softwareFogColor), std::end(g_softwareFogColor), 0.0f);
    g_frameLighting = {};
    g_materialPass = {};
    g_shadowPass = {};
    g_effectBatch = {};
    g_viewEffects = {};
    g_rendererFrame = 0;
    g_frameDrawCalls = 0;
    g_frameLightmappedDraws = 0;
    g_frameMaterialPasses = 0;
    g_frameNormalPasses = 0;
    g_frameSpecularPasses = 0;
    g_framePrimaryLightmapPasses = 0;
    g_frameSecondaryLightmapPasses = 0;
    g_frameShadowPasses = 0;
    g_frameEffectBatches = 0;
    g_frameWeatherDraws = 0;
    g_frameWeatherDrops = 0;
    g_width = g_height = 0;
    g_boundTexture = 0;
    g_softwareLightmapPrimary = 0;
    g_softwareLightmapSecondary = 0;
    g_softwareLightmapSamplerFlags = 0;
    g_softwareCullMode = GfxCullMode::None;
    std::fill(std::begin(g_softwareSamplerState),
              std::end(g_softwareSamplerState), 0x0Bu);
    if (g_softwareLog) {
        std::fclose(g_softwareLog);
        g_softwareLog = nullptr;
    }
}

void MaybeDumpSoftwareFrame() {
    if (g_softwareCaptureProbeCount < 8u) {
        char detail[96]{};
        std::snprintf(detail, sizeof(detail),
                      "capture probe checked=%u frame=%u",
                      static_cast<unsigned>(g_softwareCaptureChecked),
                      static_cast<unsigned>(g_softwareFrameCount));
        SoftwareLog(detail);
        ++g_softwareCaptureProbeCount;
    }
    if (g_softwareCaptureChecked) {
        SoftwareLog("capture already checked");
        return;
    }
    if (!SoftwarePointersValid()) {
        SoftwareLog("capture framebuffer invalid");
        return;
    }
    // The host-side runner creates capture.req immediately before launching
    // Vita3K, but the guest filesystem can expose that file a frame later
    // while scripts/assets are still mounting.  Keep polling until the
    // request is visible; marking the check before stat made an otherwise
    // healthy diagnostic run silently keep the previous PPM forever.
    // Vita3K has exposed both canonical spellings over its installed-title
    // mount (with and without the slash after the device colon). Accept both
    // so a guarded host capture request cannot be lost to a path-normalizing
    // difference between emulator builds.
    const char* requestPath = "ux0:data/CODVIT001/capture.req";
    const char* capturePath = "ux0:data/CODVIT001/logs/software-frame.ppm";
    // Vita3K versions in the field have disagreed on whether sceIoGetstat
    // sees an installed-title file before the app0/ux0 mount settles.  Use
    // the same stdio path that writes the capture so the request test and
    // output test share the emulator's actual filesystem translation.
    FILE* request = std::fopen(requestPath, "rb");
    if (request) {
        std::fclose(request);
    } else {
        requestPath = "ux0:/data/CODVIT001/capture.req";
        capturePath = "ux0:/data/CODVIT001/logs/software-frame.ppm";
        request = std::fopen(requestPath, "rb");
        if (request) {
            std::fclose(request);
        } else {
#if defined(KISAK_VITA_CAPTURE_SOFTWARE_FRAME)
            // Some Vita3K revisions hide ux0 request files from the guest
            // while still accepting the same writable logs path. Keep the
            // fallback diagnostic-only and wait for a few complete frames so
            // the autostart route has reached the rendered Shipment view.
            if (g_softwareFrameCount < 8u) {
                SoftwareLog("capture request missing; warming up");
                return;
            }
            capturePath = "ux0:data/CODVIT001/logs/software-frame.ppm";
            requestPath = nullptr;
#else
            SoftwareLog("capture request missing");
            return;
#endif
        }
    }
    FILE* capture = std::fopen(capturePath, "wb");
    if (!capture) {
        SoftwareLog("capture output open failed");
        return;
    }
    std::fprintf(capture, "P6\n%u %u\n255\n", g_width, g_height);
    const uint32_t* displayed = g_displayFramebuffer;
    for (uint32_t y = 0; y < g_height; ++y) {
        for (uint32_t x = 0; x < g_width; ++x) {
            const uint32_t color = displayed[static_cast<size_t>(y) * g_width + x];
            const unsigned char rgb[3] = {
                static_cast<unsigned char>(color & 0xFFu),
                static_cast<unsigned char>((color >> 8) & 0xFFu),
                static_cast<unsigned char>((color >> 16) & 0xFFu),
            };
            std::fwrite(rgb, sizeof(rgb), 1, capture);
        }
    }
    std::fclose(capture);
    if (requestPath)
        std::remove(requestPath);
    g_softwareCaptureChecked = true;
    SoftwareLog("software frame capture written");
}

void SoftwareBeginFrame() {
    // Vita3K's display emulation can block indefinitely when no GXM shader
    // compiler/display queue is present. The software fallback is a diagnostic
    // path; frame pacing is enforced by the bootstrap budget instead of a
    // vblank wait. Physical Vita uses the vitaGL/GXM path below.
    g_softwareRasterPixels = 0;
    g_softwareDepthTest = true;
    g_softwareDepthWrite = true;
    g_softwareSurfaceMode = GfxSurfaceMode::Opaque;
    g_softwareCullMode = GfxCullMode::None;
    g_softwareAlphaTestFunc = GfxAlphaTestFunc::GreaterEqual128;
    g_boundTexture = 0;
    g_softwareLightmapPrimary = 0;
    g_softwareLightmapSecondary = 0;
    g_softwareLightmapSamplerFlags = 0;
    // Frame descriptors are installed by the scene after BeginFrame. Clear
    // only transient material bindings here; the map lighting/post state is
    // intentionally retained until the next map is selected.
    g_materialPass = {};
    static uint32_t beginCount = 0;
    if (beginCount++ < 8u)
        SoftwareLogState("begin");
}

void SoftwareSetSurfaceMode(GfxSurfaceMode mode) {
    // A surface-mode transition is also a material-pass boundary.  The
    // scalar path stores the last descriptor for lighting/blend decisions;
    // leaving it live while the caller moves from BSP to weather, viewmodel,
    // or HUD makes those overlays inherit the previous world's raw VITL
    // state.  The next world/model draw installs its descriptor again after
    // this transition.
    g_materialPass = {};
    // Overlay draws are intentionally depth-independent, but keep depth
    // writes disabled as well so the overlay cannot poison later diagnostics.
    g_softwareSurfaceMode = mode;
    g_softwareDepthTest = mode != GfxSurfaceMode::Overlay;
    g_softwareDepthWrite = mode == GfxSurfaceMode::Opaque
        || mode == GfxSurfaceMode::AlphaTest;
}

void SoftwareSetAlphaTestFunc(GfxAlphaTestFunc function) {
    g_softwareAlphaTestFunc = function;
}

bool SoftwareAlphaTestPass(float alpha) {
    if (!std::isfinite(alpha))
        return false;
    switch (g_softwareAlphaTestFunc) {
    case GfxAlphaTestFunc::Greater0:
        return alpha > 0.0f;
    case GfxAlphaTestFunc::Less128:
        return alpha < 0.5f;
    case GfxAlphaTestFunc::GreaterEqual128:
    default:
        return alpha >= 0.5f;
    }
}

float SoftwareSourceBlendFactor(uint32_t encoded, uint32_t channel,
                                const float source[4],
                                const float destination[3],
                                float sourceAlpha, float destinationAlpha) {
    const float sourceColor = source ? std::clamp(source[channel], 0.0f, 1.0f)
                                     : 0.0f;
    const float destinationColor = destination
        ? std::clamp(destination[channel], 0.0f, 1.0f) : 0.0f;
    switch (encoded & 0x0Fu) {
    case 0u:  // Disabled/Zero in the source state map.
    case 1u:
        return 0.0f;
    case 2u:
        return 1.0f;
    case 3u:
        return sourceColor;
    case 4u:
        return 1.0f - sourceColor;
    case 5u:
        return sourceAlpha;
    case 6u:
        return 1.0f - sourceAlpha;
    case 7u:
        return destinationAlpha;
    case 8u:
        return 1.0f - destinationAlpha;
    case 9u:
        return destinationColor;
    case 10u:
        return 1.0f - destinationColor;
    case 11u:
        return std::min(sourceAlpha, 1.0f - destinationAlpha);
    // BlendFactor/InvBlendFactor are not used by the recovered MP lit
    // material maps. The source blend constant is not part of VITL, so keep
    // a deterministic neutral mapping if a future package exposes it.
    case 12u:
        return 1.0f;
    case 13u:
        return 0.0f;
    default:
        return 0.0f;
    }
}

float SoftwareSourceBlendOperation(uint32_t encoded, float source,
                                   float destination) {
    switch (encoded & 0x07u) {
    case 1u:
        return source + destination;
    case 2u:
        return source - destination;
    case 3u:
        return destination - source;
    case 4u:
        return std::min(source, destination);
    case 5u:
        return std::max(source, destination);
    case 0u:
    default:
        return source;
    }
}

bool SoftwareSourceDepthPass(float depth, float currentDepth,
                             const GfxMaterialPassState& state) {
    if (!std::isfinite(depth) || !std::isfinite(currentDepth))
        return false;
    if (!SourceMaterialStateApplies(state))
        return depth <= currentDepth + 0.00001f;
    if ((state.sourceStateBits1 & kSourceStateDepthTestDisable) != 0) {
        return true;
    }
    switch (SourceStateDepthFunction(state)) {
    case 0x00000000u: // ALWAYS
        return true;
    case 0x00000004u: // LESS
        return depth < currentDepth - 0.00001f;
    case 0x00000008u: // EQUAL
        return std::fabs(depth - currentDepth) <= 0.00001f;
    case 0x0000000Cu: // LESS_EQUAL
    default:
        return depth <= currentDepth + 0.00001f;
    }
}

void SoftwareSetCullMode(GfxCullMode mode) {
    g_softwareCullMode = mode;
}

void SoftwareSetFog(bool enabled, float startDistance, float density,
                    const float color[3]) {
    g_softwareFogEnabled = enabled && std::isfinite(density) && density > 0.0f;
    g_softwareFogStart = std::isfinite(startDistance)
        ? std::max(0.0f, startDistance) : 0.0f;
    g_softwareFogDensity = g_softwareFogEnabled ? density : 0.0f;
    if (color) {
        for (uint32_t channel = 0; channel < 3; ++channel)
            g_softwareFogColor[channel] = std::isfinite(color[channel])
                ? std::clamp(color[channel], 0.0f, 1.0f) : 0.0f;
    } else {
        std::fill(std::begin(g_softwareFogColor), std::end(g_softwareFogColor), 0.0f);
    }
}

void SoftwareEndFrame() {
    sceClibPrintf("[kisak-vita] software-end-entry\n");
    if (!SoftwarePointersValid()) {
        SoftwareLogState("end-invalid");
        return;
    }
    static uint32_t softwareFrameCount = 0;
    if (softwareFrameCount < 8u) {
        char detail[96]{};
        std::snprintf(detail, sizeof(detail), "frame=%u raster_samples=%llu",
                      static_cast<unsigned>(softwareFrameCount),
                      static_cast<unsigned long long>(g_softwareRasterPixels));
        SoftwareLog(detail);
    }
    ++softwareFrameCount;
    ++g_softwareFrameCount;
    // Vision/film response and final blue-noise dithering happen after all
    // world, effects, viewmodel, and HUD passes, matching the source's
    // full-frame post chain rather than tinting individual materials.
#if defined(KISAK_VITA_DEBUG_NO_FRAME_EFFECTS)
    static bool noEffectsLog = false;
    if (!noEffectsLog) {
        sceClibPrintf(
            "[kisak-vita] scalar diagnostic: frame effects disabled\n");
        noEffectsLog = true;
    }
#else
    ApplySoftwareFrameEffects();
#endif
    std::swap(g_framebuffer, g_displayFramebuffer);
    if (!SoftwarePointersValid()) {
        SoftwareLogState("after-swap-invalid");
        return;
    }
    SoftwareLog("before_present");
    if (kSoftwareSubmitDisplay) {
        SceDisplayFrameBuf frame{};
        frame.size = sizeof(frame);
        frame.base = g_displayFramebuffer;
        frame.pitch = g_width;
        frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
        frame.width = g_width;
        frame.height = g_height;
        // Publish the swapped buffer immediately so the last composited HUD/viewmodel pass
        // is preserved in the capture/display surface.
        // Keep the fallback on Vita's normal next-frame queue. Vita3K
        // reports SCE_DISPLAY_SETBUF_IMMEDIATE as unsupported; the
        // accelerated Vita backend continues to use vitaGL's swap contract.
        sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
    }
    SoftwareLog("after_present");
    SoftwareLog("before_capture");
#if defined(KISAK_VITA_CAPTURE_SOFTWARE_FRAME)
    // Keep the capture-only scalar diagnostic on the completed display
    // surface.  This is intentionally in the frame epilogue (rather than
    // behind the request-file poll) so emulator revisions that hide ux0
    // request files still produce an evidence frame from the exact buffer
    // that was submitted above.
    static bool directCaptureDone = false;
    if (!directCaptureDone) {
        const char* capturePath = "ux0:data/CODVIT001/logs/software-frame.ppm";
        FILE* capture = std::fopen(capturePath, "wb");
        if (!capture) {
            capturePath = "ux0:/data/CODVIT001/logs/software-frame.ppm";
            capture = std::fopen(capturePath, "wb");
        }
        if (!capture) {
            SoftwareLog("direct capture output open failed");
        } else {
            std::fprintf(capture, "P6\n%u %u\n255\n", g_width, g_height);
            const uint32_t* displayed = g_displayFramebuffer;
            for (uint32_t y = 0; y < g_height; ++y) {
                for (uint32_t x = 0; x < g_width; ++x) {
                    const uint32_t color = displayed[static_cast<size_t>(y) * g_width + x];
                    const unsigned char rgb[3] = {
                        static_cast<unsigned char>(color & 0xFFu),
                        static_cast<unsigned char>((color >> 8) & 0xFFu),
                        static_cast<unsigned char>((color >> 16) & 0xFFu),
                    };
                    std::fwrite(rgb, sizeof(rgb), 1, capture);
                }
            }
            std::fclose(capture);
            directCaptureDone = true;
            g_softwareCaptureChecked = true;
            SoftwareLog("direct capture written");
        }
    }
#else
    MaybeDumpSoftwareFrame();
#endif
    SoftwareLog("after_capture");
}

GfxTextureHandle SoftwareCreateTexture(const uint8_t* payload, size_t payloadBytes,
                                       uint32_t width, uint32_t height,
                                       uint32_t levelCount, uint32_t format,
                                       uint32_t flags) {
    // The display fallback is a bounded emulator diagnostic path.  Decode
    // only the base level into host-readable RGBA pixels; the real Vita
    // path continues to upload the original compressed mip chain through
    // vitaGL.  Keeping a hard aggregate cap prevents a malformed package or
    // the complete world texture set from exhausting the emulator guest.
    // The scalar path is intentionally fidelity-first while it is used as a
    // Vita3K diagnostic.  Keep the authored menu background and VITF atlas
    // resident after the bounded Shipment world/model closure; the old 48 MiB
    // cap evicted those assets before the frontend was drawn and silently
    // reverted menus to the debug bitmap font.  This does not change the
    // accelerated Vita path, which uploads the original compressed chains.
    constexpr size_t kTextureBudgetBytes = 128u * 1024u * 1024u;
    const bool cubeMap = (flags & 0x4u) != 0; // IMG_FLAG_CUBEMAP
    const bool resampleWorld = !cubeMap
        && (flags & GFX_VITA_TEXTURE_FLAG_SCALAR_WORLD_RESAMPLE) != 0;
    const bool largeMap = (flags & GFX_VITA_TEXTURE_FLAG_SCALAR_LARGE_MAP) != 0;
    const bool compactModel = resampleWorld
        && (flags & GFX_VITA_TEXTURE_FLAG_SCALAR_MODEL_COMPACT) != 0;
    // A cubemap does not use the 2D world-resample branch, but it still needs
    // the huge-map scalar dimensions.  Gating this only on resampleWorld
    // made a large map's six-face sky allocate the normal 512ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â²-per-face
    // diagnostic copy and bypass the bounded residency estimate upstream.
    const bool hugeMap = (flags & GFX_VITA_TEXTURE_FLAG_SCALAR_HUGE_MAP) != 0;
    const bool scalarLightmap =
        (flags & GFX_VITA_TEXTURE_FLAG_SCALAR_LIGHTMAP) != 0;
    const bool scalarViewmodel = resampleWorld
        && (flags & GFX_VITA_TEXTURE_FLAG_SCALAR_VIEWMODEL) != 0;
    const bool highPrecision = cubeMap
        || (flags & GFX_VITA_TEXTURE_FLAG_SCALAR_HIGH_PRECISION) != 0;
    const bool sourceDecode = !cubeMap
        && (flags & GFX_VITA_TEXTURE_FLAG_SCALAR_SOURCE_DECODE) != 0;
    const auto SourceBaseLevelBytes = [](uint32_t imageFormat,
                                         uint32_t imageWidth,
                                         uint32_t imageHeight) -> size_t {
        const uint32_t blocksWide = (imageWidth + 3u) / 4u;
        const uint32_t blocksHigh = (imageHeight + 3u) / 4u;
        if (imageFormat == 0x31545844u)
            return static_cast<size_t>(blocksWide) * blocksHigh * 8u;
        if (imageFormat == 0x33545844u || imageFormat == 0x35545844u)
            return static_cast<size_t>(blocksWide) * blocksHigh * 16u;
        if (imageFormat == 50u)
            return static_cast<size_t>(imageWidth) * imageHeight;
        if (imageFormat == 21u)
            return static_cast<size_t>(imageWidth) * imageHeight * sizeof(uint32_t);
        return 0;
    };
    if (sourceDecode) {
        // Retain only the authored compressed base level. SampleSourceTexel
        // decodes the same DXT/L8/A8R8G8B8 value as the thumbnail path, but
        // the compressed representation avoids a second RGBA allocation for
        // every Crossfire material and keeps precision independent of the
        // scalar thumbnail cap.
        const size_t sourceBytes = SourceBaseLevelBytes(format, width, height);
        if (!payload || sourceBytes == 0 || sourceBytes > payloadBytes
            || sourceBytes > kTextureBudgetBytes
            || g_textureBytes > kTextureBudgetBytes - sourceBytes)
            return GfxTextureHandle{0};
        SoftwareTexture source;
        source.width = width;
        source.height = height;
        source.sourceFormat = format;
        source.sourcePayload.assign(payload, payload + sourceBytes);
        g_textures.push_back(std::move(source));
        g_textureBytes += sourceBytes;
        const uint32_t handle = static_cast<uint32_t>(g_textures.size() - 1u);
        char message[112]{};
        std::snprintf(message, sizeof(message),
                      "source texture %ux%u format=%08x bytes=%u handle=%u",
                      width, height, format, static_cast<unsigned>(sourceBytes),
                      handle);
        SoftwareLog(message);
        return GfxTextureHandle{handle};
    }
    // Keep all six authored faces in the scalar oracle.  The retail payload
    // remains untouched and the accelerated Vita path uploads the original
    // 1024^2 chain; the scalar cap is only a bounded diagnostic copy.  A
    // 256^2 sky was visibly stepping across the authored cloud gradient in
    // Vita3K captures, so retain 512^2 faces (6 MiB for Shipment's 1024^2
    // source) while keeping the ordinary world/material budget unchanged.
    // Cubemaps and source-authored world colour maps are decoded at RGBA8888
    // precision below; only explicitly packed model/HUD images use the
    // compact 4-bit diagnostic path. This keeps texture precision from
    // changing at a world-material boundary and makes scalar captures useful
    // for comparing lighting equations rather than quantisation noise.
    const uint32_t scalarWorldLimit = scalarViewmodel ? 512u
        : (hugeMap ? 192u
                   : (largeMap ? (compactModel ? 224u : 256u) : 512u));
    uint32_t scalarWidth = width;
    uint32_t scalarHeight = height;
    if (cubeMap) {
        scalarWidth = std::min(width, hugeMap ? 128u : 512u);
        scalarHeight = std::min(height, hugeMap ? 256u : 512u);
    } else if (resampleWorld) {
        FitTextureDimensions(width, height, scalarWorldLimit, &scalarWidth,
                             &scalarHeight);
    }
    const size_t faceTexels = static_cast<size_t>(scalarWidth) * scalarHeight;
    const size_t decodedBytes = faceTexels
        * (highPrecision ? sizeof(uint32_t) : sizeof(uint16_t))
        * (cubeMap ? 6u : 1u);
    if (decodedBytes == 0 || decodedBytes > kTextureBudgetBytes
        || g_textureBytes > kTextureBudgetBytes - decodedBytes)
        return GfxTextureHandle{0};
    SoftwareTexture decoded;
    decoded.width = scalarWidth;
    decoded.height = scalarHeight;
    decoded.cubeMap = cubeMap;
    if (!cubeMap) {
        if (!resampleWorld) {
            if (!DecodeSoftwareTexture(payload, payloadBytes, width, height, format,
                                       &decoded, highPrecision)
                || (highPrecision ? decoded.highPrecisionPixels.size()
                                  : decoded.pixels.size()) != faceTexels)
                return GfxTextureHandle{0};
        } else {
            // Decode the source image once, then resample at texel centres into
            // the bounded diagnostic texture.  This preserves the authored
            // material/UV relationship while avoiding a full-resolution copy
            // for every 1024/2048px BSP image, leaving enough guest memory for
            // the complete world projection and static-model pass.
            // Preserve the requested precision in the temporary source grid
            // as well as the bounded destination. A high-precision
            // destination that is first decoded through RGBA4444 is already
            // irreversibly banded before the resampler can filter it, which
            // is especially visible in sky gradients and dark props.
            SoftwareTexture source;
             const uint32_t sourceThumbnailLimit = scalarLightmap
                 ? (hugeMap ? 256u : (largeMap ? 512u : 1024u))
                 : (scalarViewmodel ? 1024u
                                    : (compactModel && hugeMap ? 256u : 1024u));
             if (!DecodeSoftwareTextureThumbnail(payload, payloadBytes,
                                                 width, height, format,
                                                 &source, sourceThumbnailLimit,
                                                 highPrecision))
                return GfxTextureHandle{0};
            if (highPrecision)
                decoded.highPrecisionPixels.resize(faceTexels, 0xFFFFFFFFu);
            else
                decoded.pixels.resize(faceTexels, PackRgba4444(0xFFFFFFFFu));
            const auto SourceTexel = [&](uint32_t x, uint32_t y) {
                return SoftwareTextureTexel(source,
                    static_cast<size_t>(y) * source.width + x);
            };
            for (uint32_t y = 0; y < scalarHeight; ++y) {
                const float sourceY =
                    (static_cast<float>(y) + 0.5f)
                        * static_cast<float>(source.height)
                        / static_cast<float>(scalarHeight) - 0.5f;
                const int baseY = static_cast<int>(std::floor(sourceY));
                const uint32_t y0 = static_cast<uint32_t>(std::clamp(
                    baseY, 0, static_cast<int>(source.height) - 1));
                const uint32_t y1 = static_cast<uint32_t>(std::clamp(
                    baseY + 1, 0, static_cast<int>(source.height) - 1));
                const float ty = sourceY - std::floor(sourceY);
                for (uint32_t x = 0; x < scalarWidth; ++x) {
                    const float sourceX =
                        (static_cast<float>(x) + 0.5f)
                            * static_cast<float>(source.width)
                            / static_cast<float>(scalarWidth) - 0.5f;
                    const int baseX = static_cast<int>(std::floor(sourceX));
                    const uint32_t x0 = static_cast<uint32_t>(std::clamp(
                        baseX, 0, static_cast<int>(source.width) - 1));
                    const uint32_t x1 = static_cast<uint32_t>(std::clamp(
                        baseX + 1, 0, static_cast<int>(source.width) - 1));
                    const float tx = sourceX - std::floor(sourceX);
                    const uint32_t samples[4] = {
                        SourceTexel(x0, y0), SourceTexel(x1, y0),
                        SourceTexel(x0, y1), SourceTexel(x1, y1),
                    };
                    uint32_t filtered = 0;
                    for (uint32_t channel = 0; channel < 4; ++channel) {
                        const uint32_t shift = channel * 8u;
                        const auto Channel = [shift](uint32_t value) {
                            return static_cast<float>((value >> shift) & 0xFFu);
                        };
                        const float top = Channel(samples[0])
                            + (Channel(samples[1]) - Channel(samples[0])) * tx;
                        const float bottom = Channel(samples[2])
                            + (Channel(samples[3]) - Channel(samples[2])) * tx;
                        filtered |= (static_cast<uint32_t>(std::lround(
                            top + (bottom - top) * ty)) & 0xFFu) << shift;
                    }
                    const size_t index = static_cast<size_t>(y) * scalarWidth + x;
                    if (highPrecision)
                        decoded.highPrecisionPixels[index] = filtered;
                    else
                        decoded.pixels[index] = PackRgba4444(filtered);
                }
            }
        }
    } else {
        // A cubemap image is serialized face-major (six faces, each with its
        // conventional base-first mip chain). Decode only the base level for
        // the scalar oracle, retaining every authored face rather than
        // silently sampling the first `+X` face as a 2D image.
        const auto MipBytes = [](uint32_t imageFormat, uint32_t mipWidth,
                                 uint32_t mipHeight) -> size_t {
            const uint32_t blocksWide = (mipWidth + 3u) / 4u;
            const uint32_t blocksHigh = (mipHeight + 3u) / 4u;
            if (imageFormat == 0x31545844u)
                return static_cast<size_t>(blocksWide) * blocksHigh * 8u;
            if (imageFormat == 0x33545844u || imageFormat == 0x35545844u)
                return static_cast<size_t>(blocksWide) * blocksHigh * 16u;
            if (imageFormat == 21u) // D3DFMT_A8R8G8B8
                return static_cast<size_t>(mipWidth) * mipHeight * 4u;
            return 0;
        };
        size_t faceBytes = 0;
        uint32_t mipWidth = width;
        uint32_t mipHeight = height;
        for (uint32_t level = 0; level < levelCount; ++level) {
            const size_t bytes = MipBytes(format, mipWidth, mipHeight);
            if (bytes == 0
                || faceBytes > std::numeric_limits<size_t>::max() - bytes)
                return GfxTextureHandle{0};
            faceBytes += bytes;
            mipWidth = std::max(1u, mipWidth / 2u);
            mipHeight = std::max(1u, mipHeight / 2u);
        }
        if (faceBytes == 0 || faceBytes > payloadBytes / 6u)
            return GfxTextureHandle{0};
        decoded.highPrecisionPixels.resize(faceTexels * 6u);
        for (uint32_t face = 0; face < 6u; ++face) {
            SoftwareTexture decodedFace;
            // The fallback only needs a filtered diagnostic copy. Avoid
            // materializing a full RGBA face before reducing it; on the
            // largest maps that transient collides with the authored world
            // vertex stream in Vita3K's guest heap.
            const uint32_t thumbnailLimit = hugeMap ? 256u : 1024u;
            if (!DecodeSoftwareTextureThumbnail(
                    payload + face * faceBytes, faceBytes, width, height,
                    format, &decodedFace, thumbnailLimit, true)
                || decodedFace.highPrecisionPixels.size()
                    != static_cast<size_t>(decodedFace.width) * decodedFace.height)
                return GfxTextureHandle{0};
            uint32_t* destination = decoded.highPrecisionPixels.data()
                + static_cast<std::ptrdiff_t>(face * faceTexels);
            // Resample the authored face at texel centers.  The hardware path
            // still receives every source texel and mip level; this filtering
            // only prevents a 1024-to-512 diagnostic preview from adding a
            // second, visible stair-step to the sky's low-frequency gradient.
            for (uint32_t y = 0; y < scalarHeight; ++y) {
                const float sourceY =
                    (static_cast<float>(y) + 0.5f)
                        * static_cast<float>(decodedFace.height)
                            / static_cast<float>(scalarHeight)
                    - 0.5f;
                const int baseY = static_cast<int>(std::floor(sourceY));
                const uint32_t y0 = static_cast<uint32_t>(std::clamp(
                    baseY, 0, static_cast<int>(decodedFace.height) - 1));
                const uint32_t y1 = static_cast<uint32_t>(std::clamp(
                    baseY + 1, 0, static_cast<int>(decodedFace.height) - 1));
                const float ty = sourceY - std::floor(sourceY);
                for (uint32_t x = 0; x < scalarWidth; ++x) {
                    const float sourceX =
                        (static_cast<float>(x) + 0.5f)
                            * static_cast<float>(decodedFace.width)
                                / static_cast<float>(scalarWidth)
                        - 0.5f;
                    const int baseX = static_cast<int>(std::floor(sourceX));
                    const uint32_t x0 = static_cast<uint32_t>(std::clamp(
                        baseX, 0, static_cast<int>(decodedFace.width) - 1));
                    const uint32_t x1 = static_cast<uint32_t>(std::clamp(
                        baseX + 1, 0, static_cast<int>(decodedFace.width) - 1));
                    const float tx = sourceX - std::floor(sourceX);
                    const auto Sample = [&](uint32_t sampleX, uint32_t sampleY) {
                        return SoftwareTextureTexel(decodedFace,
                            static_cast<size_t>(sampleY) * decodedFace.width + sampleX);
                    };
                    const uint32_t samples[4] = {
                        Sample(x0, y0), Sample(x1, y0),
                        Sample(x0, y1), Sample(x1, y1),
                    };
                    uint32_t filtered = 0;
                    for (uint32_t channel = 0; channel < 4; ++channel) {
                        const uint32_t shift = channel * 8u;
                        const auto Channel = [shift](uint32_t value) {
                            return static_cast<float>((value >> shift) & 0xFFu);
                        };
                        const float top = Channel(samples[0])
                            + (Channel(samples[1]) - Channel(samples[0])) * tx;
                        const float bottom = Channel(samples[2])
                            + (Channel(samples[3]) - Channel(samples[2])) * tx;
                        const uint32_t value = static_cast<uint32_t>(std::lround(
                            top + (bottom - top) * ty));
                        filtered |= (value & 0xFFu) << shift;
                    }
                    destination[static_cast<size_t>(y) * scalarWidth + x] = filtered;
                }
            }
        }
    }
    g_textures.push_back(std::move(decoded));
    g_textureBytes += decodedBytes;
    const uint32_t handle = static_cast<uint32_t>(g_textures.size() - 1u);
    char message[96]{};
    std::snprintf(message, sizeof(message), "texture %ux%u format=%08x bytes=%u handle=%u",
                  width, height, format, static_cast<unsigned>(decodedBytes), handle);
    SoftwareLog(message);
    return GfxTextureHandle{handle};
}

void SoftwareDestroyTexture(GfxTextureHandle texture) {
    if (texture.value > 0 && texture.value < g_textures.size()) {
        g_textureBytes -= std::min(g_textureBytes,
                                   g_textures[texture.value].pixels.size()
                                       * sizeof(uint16_t)
                                       + g_textures[texture.value]
                                             .highPrecisionPixels.size()
                                           * sizeof(uint32_t)
                                       + g_textures[texture.value]
                                             .sourcePayload.size());
        g_textures[texture.value] = SoftwareTexture{};
    }
    if (g_boundTexture == texture.value)
        g_boundTexture = 0;
    if (g_softwareLightmapPrimary == texture.value)
        g_softwareLightmapPrimary = 0;
    if (g_softwareLightmapSecondary == texture.value)
        g_softwareLightmapSecondary = 0;
}

void SoftwareBindTexture(GfxTextureHandle texture) {
    g_boundTexture = texture.value < g_textures.size() ? texture.value : 0;
}

void SoftwareSetSamplerState(uint32_t textureUnit, uint8_t samplerState) {
    if (textureUnit >= std::size(g_softwareSamplerState))
        return;
    // A zero filter/mipmap nibble is invalid in KisakCOD. Treat it as the
    // normal linear+mipmap-nearest material default for defensive callers.
    if ((samplerState & 0x1Fu) == 0)
        samplerState = 0x0Bu;
    g_softwareSamplerState[textureUnit] = samplerState;
}

uint32_t SampleSoftwareTextureCube(const SoftwareTexture& texture,
                                   float x, float y, float z) {
    if (!texture.cubeMap || texture.width == 0 || texture.height == 0
        || (texture.pixels.size() < static_cast<size_t>(texture.width)
                * texture.height * 6u
            && texture.highPrecisionPixels.size()
                < static_cast<size_t>(texture.width) * texture.height * 6u))
        return 0xFFFFFFFFu;
    const float ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
    const float major = std::max({ax, ay, az});
    if (!std::isfinite(major) || major <= 0.000001f)
        return 0xFFFFFFFFu;
    uint32_t face = 0;
    float s = 0.0f;
    float t = 0.0f;
    // Match the D3DCUBEMAP_FACE_* addressing used by KisakCOD's
    // Image_UploadData.  The v coordinate is converted to the top-left
    // raster convention after selecting the source face.
    if (ax >= ay && ax >= az) {
        if (x >= 0.0f) {
            face = 0; // +X
            s = -z / ax;
            t = -y / ax;
        } else {
            face = 1; // -X
            s = z / ax;
            t = -y / ax;
        }
    } else if (ay >= ax && ay >= az) {
        if (y >= 0.0f) {
            face = 2; // +Y
            s = x / ay;
            t = z / ay;
        } else {
            face = 3; // -Y
            s = x / ay;
            t = -z / ay;
        }
    } else if (z >= 0.0f) {
        face = 4; // +Z
        s = x / az;
        t = -y / az;
    } else {
        face = 5; // -Z
        s = -x / az;
        t = -y / az;
    }
    // D3D cubemap sampling is seam-aware: a bilinear tap that falls just
    // outside one face is taken from the adjacent authored face. Clamping
    // every tap to the current face (the old scalar path) exposes a bright
    // one-texel column at the +X/+Z boundary, which is especially visible in
    // the flat Shipment sky. Keep the source face orientation above, but map
    // each tap's direction back through the same six-face addressing before
    // fetching its texel.
    const float u = std::clamp(0.5f * (s + 1.0f), 0.0f, 1.0f);
    const float v = std::clamp(0.5f * (t + 1.0f), 0.0f, 1.0f);
    const float texelX = u * static_cast<float>(texture.width) - 0.5f;
    const float texelY = v * static_cast<float>(texture.height) - 0.5f;
    const int baseX = static_cast<int>(std::floor(texelX));
    const int baseY = static_cast<int>(std::floor(texelY));
    const float tx = texelX - std::floor(texelX);
    const float ty = texelY - std::floor(texelY);
    const auto DirectionForFace = [](uint32_t directionFace, float faceS,
                                     float faceT, float output[3]) {
        switch (directionFace) {
        case 0: // +X
            output[0] = 1.0f;
            output[1] = -faceT;
            output[2] = -faceS;
            break;
        case 1: // -X
            output[0] = -1.0f;
            output[1] = -faceT;
            output[2] = faceS;
            break;
        case 2: // +Y
            output[0] = faceS;
            output[1] = 1.0f;
            output[2] = faceT;
            break;
        case 3: // -Y
            output[0] = faceS;
            output[1] = -1.0f;
            output[2] = -faceT;
            break;
        case 4: // +Z
            output[0] = faceS;
            output[1] = -faceT;
            output[2] = 1.0f;
            break;
        default: // -Z
            output[0] = -faceS;
            output[1] = -faceT;
            output[2] = -1.0f;
            break;
        }
    };
    const auto FaceCoordinates = [](float directionX, float directionY,
                                    float directionZ, uint32_t* outputFace,
                                    float* outputS, float* outputT) {
        if (!outputFace || !outputS || !outputT)
            return false;
        const float directionAbsX = std::fabs(directionX);
        const float directionAbsY = std::fabs(directionY);
        const float directionAbsZ = std::fabs(directionZ);
        const float directionMajor = std::max({directionAbsX, directionAbsY,
                                               directionAbsZ});
        if (!std::isfinite(directionMajor) || directionMajor <= 0.000001f)
            return false;
        uint32_t directionFace = 0;
        float directionS = 0.0f;
        float directionT = 0.0f;
        if (directionAbsX >= directionAbsY && directionAbsX >= directionAbsZ) {
            if (directionX >= 0.0f) {
                directionFace = 0;
                directionS = -directionZ / directionAbsX;
                directionT = -directionY / directionAbsX;
            } else {
                directionFace = 1;
                directionS = directionZ / directionAbsX;
                directionT = -directionY / directionAbsX;
            }
        } else if (directionAbsY >= directionAbsX
                   && directionAbsY >= directionAbsZ) {
            if (directionY >= 0.0f) {
                directionFace = 2;
                directionS = directionX / directionAbsY;
                directionT = directionZ / directionAbsY;
            } else {
                directionFace = 3;
                directionS = directionX / directionAbsY;
                directionT = -directionZ / directionAbsY;
            }
        } else if (directionZ >= 0.0f) {
            directionFace = 4;
            directionS = directionX / directionAbsZ;
            directionT = -directionY / directionAbsZ;
        } else {
            directionFace = 5;
            directionS = -directionX / directionAbsZ;
            directionT = -directionY / directionAbsZ;
        }
        *outputFace = directionFace;
        *outputS = std::clamp(directionS, -1.0f, 1.0f);
        *outputT = std::clamp(directionT, -1.0f, 1.0f);
        return true;
    };
    const auto SampleSeamAwareTap = [&](int sampleX, int sampleY) {
        const float tapS = 2.0f
            * (static_cast<float>(sampleX) + 0.5f)
            / static_cast<float>(texture.width) - 1.0f;
        const float tapT = 2.0f
            * (static_cast<float>(sampleY) + 0.5f)
            / static_cast<float>(texture.height) - 1.0f;
        float tapDirection[3]{};
        DirectionForFace(face, tapS, tapT, tapDirection);
        uint32_t tapFace = face;
        float tapFaceS = 0.0f;
        float tapFaceT = 0.0f;
        if (!FaceCoordinates(tapDirection[0], tapDirection[1], tapDirection[2],
                             &tapFace, &tapFaceS, &tapFaceT))
            return 0xFFFFFFFFu;
        const uint32_t tapX = static_cast<uint32_t>(std::clamp(
            static_cast<int>(std::floor(
                0.5f * (tapFaceS + 1.0f)
                * static_cast<float>(texture.width))),
            0, static_cast<int>(texture.width) - 1));
        const uint32_t tapY = static_cast<uint32_t>(std::clamp(
            static_cast<int>(std::floor(
                0.5f * (tapFaceT + 1.0f)
                * static_cast<float>(texture.height))),
            0, static_cast<int>(texture.height) - 1));
        const size_t tapOffset = static_cast<size_t>(tapFace)
            * texture.width * texture.height
            + static_cast<size_t>(tapY) * texture.width + tapX;
        return SoftwareTextureTexel(texture, tapOffset);
    };
    const uint32_t samples[4] = {
        SampleSeamAwareTap(baseX, baseY),
        SampleSeamAwareTap(baseX + 1, baseY),
        SampleSeamAwareTap(baseX, baseY + 1),
        SampleSeamAwareTap(baseX + 1, baseY + 1),
    };
    const auto Channel = [&](uint32_t sample, uint32_t shift) {
        return static_cast<float>((sample >> shift) & 0xFFu);
    };
    const auto Bilinear = [&](uint32_t shift) {
        const float top = Channel(samples[0], shift)
            + (Channel(samples[1], shift) - Channel(samples[0], shift)) * tx;
        const float bottom = Channel(samples[2], shift)
            + (Channel(samples[3], shift) - Channel(samples[2], shift)) * tx;
        return static_cast<uint32_t>(std::lround(top + (bottom - top) * ty));
    };
    return Bilinear(0u) | (Bilinear(8u) << 8u)
        | (Bilinear(16u) << 16u) | (Bilinear(24u) << 24u);
}

uint32_t SampleSoftwareTextureHandle(uint32_t textureHandle, float u, float v,
                                     uint32_t textureUnit) {
    if (textureHandle == 0 || textureHandle >= g_textures.size())
        return 0xFFFFFFFFu;
    const SoftwareTexture& texture = g_textures[textureHandle];
    if ((texture.pixels.empty() && texture.highPrecisionPixels.empty()
            && texture.sourcePayload.empty())
        || texture.width == 0 || texture.height == 0)
        return 0xFFFFFFFFu;
    // Match the authored MaterialTextureDef sampler state. The source low
    // byte uses bits 0x20/0x40 for U/V clamp and bits 0..2 for the filter
    // family (1=nearest, 2=linear, 3/4=anisotropic). Mip selection is not
    // observable in the scalar base-level oracle, but address/filter behavior
    // is, especially on alpha cards and the lightmap-facing world seams.
    // Font atlases remain clamped as their source material requires, even
    // when an older package omitted the sampler byte.
    // This preserves the existing edge-clamped filtering behavior for those
    // atlases while extending it to any source material that requests clamp.
    const uint8_t samplerState = g_softwareSamplerState[
        std::min<uint32_t>(textureUnit,
                           static_cast<uint32_t>(std::size(g_softwareSamplerState) - 1u))];
    const bool clampAtlas = texture.width == 512u && texture.height == 1024u;
    const bool clampU = (samplerState & 0x20u) != 0
        || clampAtlas;
    const bool clampV = (samplerState & 0x40u) != 0
        || clampAtlas;
    const bool nearest = (samplerState & 0x07u) == 0x01u;
    uint32_t x0 = 0;
    uint32_t y0 = 0;
    uint32_t x1 = 0;
    uint32_t y1 = 0;
    float tx = 0.0f;
    float ty = 0.0f;
    if (!std::isfinite(u))
        u = 0.0f;
    if (!std::isfinite(v))
        v = 0.0f;
    if (clampU)
        u = std::clamp(u, 0.0f, 1.0f);
    else
        u -= std::floor(u);
    if (clampV)
        v = std::clamp(v, 0.0f, 1.0f);
    else
        v -= std::floor(v);
    if (nearest) {
        const auto NearestCoordinate = [](float coordinate, uint32_t extent) {
            const float scaled = coordinate * static_cast<float>(extent);
            return static_cast<uint32_t>(std::clamp(
                static_cast<int>(std::floor(scaled)), 0,
                static_cast<int>(extent) - 1));
        };
        x0 = x1 = NearestCoordinate(u, texture.width);
        y0 = y1 = NearestCoordinate(v, texture.height);
    } else {
        const float texelX = u * static_cast<float>(texture.width) - 0.5f;
        const float texelY = v * static_cast<float>(texture.height) - 0.5f;
        const int baseX = static_cast<int>(std::floor(texelX));
        const int baseY = static_cast<int>(std::floor(texelY));
        x0 = static_cast<uint32_t>(std::clamp(baseX, 0,
                                              static_cast<int>(texture.width) - 1));
        y0 = static_cast<uint32_t>(std::clamp(baseY, 0,
                                              static_cast<int>(texture.height) - 1));
        x1 = static_cast<uint32_t>(std::clamp(baseX + 1, 0,
                                              static_cast<int>(texture.width) - 1));
        y1 = static_cast<uint32_t>(std::clamp(baseY + 1, 0,
                                              static_cast<int>(texture.height) - 1));
        tx = texelX - std::floor(texelX);
        ty = texelY - std::floor(texelY);
    }
    /*
     * The branch above intentionally uses the same center-correct bilinear
     * sample for clamped and repeating textures. For repeating source
     * textures the coordinates have already been wrapped; for clamped source
     * textures x0/x1 and y0/y1 are edge-clamped.
     */
    if (!nearest && !clampU && !clampV) {
        // The source repeat path historically used texel corners rather than
        // centers. Preserve that behavior for world materials to avoid a
        // package-wide texture phase shift while still honoring clamp bits.
        const float texelX = u * static_cast<float>(texture.width);
        const float texelY = v * static_cast<float>(texture.height);
        x0 = static_cast<uint32_t>(texelX)
            % std::max(1u, texture.width);
        y0 = static_cast<uint32_t>(texelY)
            % std::max(1u, texture.height);
        x1 = (x0 + 1u) % std::max(1u, texture.width);
        y1 = (y0 + 1u) % std::max(1u, texture.height);
        tx = texelX - std::floor(texelX);
        ty = texelY - std::floor(texelY);
    }
    const uint32_t samples[4] = {
        SoftwareTextureTexel(texture,
                             static_cast<size_t>(y0) * texture.width + x0),
        SoftwareTextureTexel(texture,
                             static_cast<size_t>(y0) * texture.width + x1),
        SoftwareTextureTexel(texture,
                             static_cast<size_t>(y1) * texture.width + x0),
        SoftwareTextureTexel(texture,
                             static_cast<size_t>(y1) * texture.width + x1),
    };
    const auto Channel = [&](uint32_t sample, uint32_t shift) {
        return static_cast<float>((sample >> shift) & 0xFFu);
    };
    const auto Bilinear = [&](uint32_t shift) {
        const float top = Channel(samples[0], shift)
            + (Channel(samples[1], shift) - Channel(samples[0], shift)) * tx;
        const float bottom = Channel(samples[2], shift)
            + (Channel(samples[3], shift) - Channel(samples[2], shift)) * tx;
        return static_cast<uint32_t>(std::lround(top + (bottom - top) * ty));
    };
    return Bilinear(0u) | (Bilinear(8u) << 8u)
        | (Bilinear(16u) << 16u) | (Bilinear(24u) << 24u);
}

uint32_t SampleSoftwareTexture(float u, float v) {
    return SampleSoftwareTextureHandle(g_boundTexture, u, v, 0u);
}

bool PrepareSoftwareGlow() {
    g_softwareGlowWidth = 0;
    g_softwareGlowHeight = 0;
    if (!SoftwarePointersValid()
        || (g_viewEffects.flags & GFX_FRAME_LIGHTING_HAS_GLOW) == 0
        || g_viewEffects.glowEnabled <= 0.5f
        || !std::isfinite(g_viewEffects.glowRadius0)
        || !std::isfinite(g_viewEffects.glowBloomIntensity0)
        || g_viewEffects.glowRadius0 <= 0.0f
        || g_viewEffects.glowBloomIntensity0 <= 0.0f)
        return false;

    const uint32_t glowWidth = std::clamp(g_width / 4u, 1u,
                                          kSoftwareGlowMaxWidth);
    const uint32_t glowHeight = std::clamp(g_height / 4u, 1u,
                                           kSoftwareGlowMaxHeight);
    g_softwareGlowWidth = glowWidth;
    g_softwareGlowHeight = glowHeight;
    const float cutoff = std::clamp(
        std::isfinite(g_viewEffects.glowBloomCutoff)
            ? g_viewEffects.glowBloomCutoff : 0.0f,
        0.0f, 1.0f);
    // R_SetGlowInfo publishes 1/(1-cutoff) as the second GLOW_SETUP
    // component. Keep the same bounded rescale rather than treating cutoff
    // as a soft artistic multiplier.
    const float cutoffRescale = 1.0f / std::max(1.0f - cutoff, 1.0f / 4096.0f);
    const float desaturation = std::clamp(
        std::isfinite(g_viewEffects.glowBloomDesaturation)
            ? g_viewEffects.glowBloomDesaturation : 0.0f,
        0.0f, 1.0f);
    const auto GlowIndex = [&](uint32_t x, uint32_t y, uint32_t channel) {
        return (static_cast<size_t>(y) * glowWidth + x) * 3u + channel;
    };
    const auto SceneIndex = [&](uint32_t x, uint32_t y, uint32_t channel) {
        return (static_cast<size_t>(y) * g_width + x) * 3u + channel;
    };
    // glow_consistent_setup first downsamples the resolved scene and applies a
    // cutoff/rescale mask. Average texels in the same 4x footprint so the
    // scalar path does not select a single aliased source pixel.
    for (uint32_t y = 0; y < glowHeight; ++y) {
        const uint32_t sourceY0 = y * g_height / glowHeight;
        const uint32_t sourceY1 = std::max(
            sourceY0 + 1u, (y + 1u) * g_height / glowHeight);
        for (uint32_t x = 0; x < glowWidth; ++x) {
            const uint32_t sourceX0 = x * g_width / glowWidth;
            const uint32_t sourceX1 = std::max(
                sourceX0 + 1u, (x + 1u) * g_width / glowWidth);
            float average[3] = {0.0f, 0.0f, 0.0f};
            uint32_t samples = 0;
            for (uint32_t sourceY = sourceY0;
                 sourceY < std::min(sourceY1, g_height); ++sourceY) {
                for (uint32_t sourceX = sourceX0;
                     sourceX < std::min(sourceX1, g_width); ++sourceX) {
                    for (uint32_t channel = 0; channel < 3u; ++channel)
                        average[channel] += g_softwareSceneColor[
                            SceneIndex(sourceX, sourceY, channel)];
                    ++samples;
                }
            }
            const float inverseSamples = 1.0f / std::max(1u, samples);
            for (float& component : average)
                component *= inverseSamples;
            const float luminance = std::max(
                0.0f, average[0] * 0.2126f + average[1] * 0.7152f
                    + average[2] * 0.0722f);
            const float bloomMask = std::clamp(
                std::max(0.0f, luminance - cutoff) * cutoffRescale,
                0.0f, 1.0f);
            for (uint32_t channel = 0; channel < 3u; ++channel) {
                const float grey = luminance;
                const float source = average[channel] * (1.0f - desaturation)
                    + grey * desaturation;
                g_softwareGlowBuffer[GlowIndex(x, y, channel)] =
                    std::max(0.0f, source) * bloomMask;
            }
        }
    }

    // RB_ApplyGlowFilter maps the source radius to the quarter-resolution
    // target. A normalized Gaussian is a compact scalar equivalent of the
    // source symmetric filter chain and stays bounded for authored radii such
    // as Pipeline (2.1), Cargo Ship (6), and Bog (25.2691).
    const int radius = std::clamp(static_cast<int>(std::ceil(
        std::max(0.0f, g_viewEffects.glowRadius0) * 0.25f)), 1, 12);
    const float sigma = std::max(0.75f, static_cast<float>(radius) * 0.5f);
    float weights[25]{};
    float weightSum = 0.0f;
    for (int offset = -radius; offset <= radius; ++offset) {
        const float distance = static_cast<float>(offset);
        const float weight = std::exp(-(distance * distance)
                                      / (2.0f * sigma * sigma));
        weights[offset + 12] = weight;
        weightSum += weight;
    }
    if (weightSum <= 0.0f || !std::isfinite(weightSum))
        return false;
    for (int offset = -radius; offset <= radius; ++offset)
        weights[offset + 12] /= weightSum;
    for (uint32_t y = 0; y < glowHeight; ++y) {
        for (uint32_t x = 0; x < glowWidth; ++x) {
            for (uint32_t channel = 0; channel < 3u; ++channel) {
                float value = 0.0f;
                for (int offset = -radius; offset <= radius; ++offset) {
                    const uint32_t sampleX = static_cast<uint32_t>(std::clamp(
                        static_cast<int>(x) + offset, 0,
                        static_cast<int>(glowWidth) - 1));
                    value += g_softwareGlowBuffer[GlowIndex(sampleX, y, channel)]
                        * weights[offset + 12];
                }
                g_softwareGlowScratch[GlowIndex(x, y, channel)] = value;
            }
        }
    }
    for (uint32_t y = 0; y < glowHeight; ++y) {
        for (uint32_t x = 0; x < glowWidth; ++x) {
            for (uint32_t channel = 0; channel < 3u; ++channel) {
                float value = 0.0f;
                for (int offset = -radius; offset <= radius; ++offset) {
                    const uint32_t sampleY = static_cast<uint32_t>(std::clamp(
                        static_cast<int>(y) + offset, 0,
                        static_cast<int>(glowHeight) - 1));
                    value += g_softwareGlowScratch[GlowIndex(x, sampleY, channel)]
                        * weights[offset + 12];
                }
                g_softwareGlowBuffer[GlowIndex(x, y, channel)] = value;
            }
        }
    }
    static uint32_t glowLogCount = 0;
    if (glowLogCount++ < 8u) {
        sceClibPrintf(
            "[kisak-vita] scalar glow pass enabled=%u source=%ux%u radius=%d cutoff=%.6f desaturation=%.6f intensity=%.6f skyBleed=%.6f\n",
            static_cast<unsigned>(g_viewEffects.glowEnabled > 0.5f),
            static_cast<unsigned>(g_width), static_cast<unsigned>(g_height),
            radius, cutoff, desaturation,
            g_viewEffects.glowBloomIntensity0,
            std::isfinite(g_viewEffects.glowSkyBleedIntensity0)
                ? g_viewEffects.glowSkyBleedIntensity0 : 0.0f);
    }
    return true;
}

void ApplySoftwareFrameEffects() {
    if (!SoftwarePointersValid())
        return;
    const bool glowReady = PrepareSoftwareGlow();
    const float glowIntensity = glowReady
        ? std::clamp(std::isfinite(g_viewEffects.glowBloomIntensity0)
                         ? g_viewEffects.glowBloomIntensity0 : 0.0f,
                     0.0f, 8.0f)
        : 0.0f;
    static uint32_t filmStatsLogCount = 0;
    const bool filmEnabled = (g_viewEffects.flags & GFX_FRAME_LIGHTING_HAS_FILM) != 0;
    const bool collectFilmStats = filmEnabled && filmStatsLogCount < 4u;
    struct FilmProbe {
        uint32_t x;
        uint32_t y;
        float input[3];
        float output[3];
    };
    // Keep a few fixed source-space samples while validating the scalar
    // composition path.  The contact sheets showed bright openings and a
    // near-white Crossfire sky; these probes distinguish a source film curve
    // from an unfilled/incorrectly depth-tested world surface without adding
    // another capture buffer or changing the rendered pixels.
    FilmProbe probes[] = {
        {480u, 40u, {}, {}},   // sky / upper skyline
        {480u, 200u, {}, {}},  // distant buildings / openings
        {480u, 400u, {}, {}},  // ground / weapon boundary
        {100u, 300u, {}, {}},  // left wall / doorway
        {850u, 300u, {}, {}},  // right wall / prop
        {300u, 200u, {}, {}},  // bright left structure
        {400u, 300u, {}, {}},  // bright center structure
        {650u, 200u, {}, {}},  // dark center/right structure
        {850u, 200u, {}, {}},  // dark right structure
        {50u, 500u, {}, {}},   // near deck / rail
    };
    float inputMin[3] = {
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
    };
    float inputMax[3] = {
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
    };
    float outputMin[3] = {
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
    };
    float outputMax[3] = {
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
    };
    double inputSum[3] = {0.0, 0.0, 0.0};
    double outputSum[3] = {0.0, 0.0, 0.0};
    uint32_t inputAboveOne = 0;
    uint32_t outputClipped = 0;
    const size_t pixelCount = g_softwarePixelCount;
    // Match KisakCOD's R_UpdateColorManipulation/postfx_color constants.
    // The desktop renderer does not add brightness after contrast or perform
    // a second ad-hoc desaturation pass. It folds those values into
    // COLOR_BIAS, COLOR_TINT_BASE, and COLOR_TINT_DELTA, then the postfx
    // shader reconstructs the desaturated colour with COLOR_BIAS.a. Keeping
    // that algebra here is important for authored vision sets such as
    // Crossfire and Wet Work.
    const float filmDesaturation = std::clamp(g_viewEffects.desaturation,
                                              0.0f, 1.0f);
    const float desaturationUnit = std::max(filmDesaturation, 1.0f / 4096.0f);
    const float desaturationScale = 1.0f / desaturationUnit - 1.0f;
    const float contrast = std::clamp(g_viewEffects.contrast, 0.0f, 4.0f);
    float tintScale = contrast * desaturationUnit;
    float tintBias = g_viewEffects.brightness + 0.5f - contrast * 0.5f;
    if (g_viewEffects.invert > 0.0f) {
        tintScale = -tintScale;
        tintBias += 1.0f;
    }
    float tintBase[3]{};
    float tintDelta[3]{};
    for (uint32_t channel = 0; channel < 3; ++channel) {
        const float dark = std::isfinite(g_viewEffects.filmDarkTint[channel])
            ? g_viewEffects.filmDarkTint[channel] : 1.0f;
        const float light = std::isfinite(g_viewEffects.filmLightTint[channel])
            ? g_viewEffects.filmLightTint[channel] : 1.0f;
        tintBase[channel] = dark * tintScale;
        tintDelta[channel] = (light - dark) * tintScale;
    }
    // The desktop postfx material consumes the resolved scene in the same
    // native space as the source color constants. The old scalar path
    // quantized every draw into g_framebuffer before reaching this pass,
    // which clipped high-contrast maps such as Crossfire and made covered
    // geometry look flat. Keep that source space intact, but evaluate the
    // equation from the floating scene buffer and quantize only once here.
    const float exposure = std::max(
        0.0f, std::isfinite(g_viewEffects.exposure)
            ? g_viewEffects.exposure : 1.0f);
    // R_CalcGammaRamp uses exponent 1/r_gamma on the final display bytes.
    // Apply the same transfer after the source film equation and before the
    // ordered dither. It is independent of filmEnabled so menu and
    // no-vision frames receive the same global display calibration.
    const float displayGamma = std::clamp(
        std::isfinite(g_viewEffects.gamma) ? g_viewEffects.gamma : 0.8f,
        0.1f, 4.0f);
    const float displayExponent = 1.0f / displayGamma;
    for (uint32_t y = 0; y < g_height; ++y) {
        for (uint32_t x = 0; x < g_width; ++x) {
            const size_t pixelIndex = static_cast<size_t>(y) * g_width + x;
            uint32_t& pixel = g_framebuffer[pixelIndex];
            float color[3] = {
                g_softwareSceneColor[pixelIndex * 3u] * exposure,
                g_softwareSceneColor[pixelIndex * 3u + 1u] * exposure,
                g_softwareSceneColor[pixelIndex * 3u + 2u] * exposure,
            };
            if (collectFilmStats) {
                for (FilmProbe& probe : probes) {
                    if (probe.x == x && probe.y == y) {
                        std::memcpy(probe.input, color, sizeof(probe.input));
                        break;
                    }
                }
            }
            if (collectFilmStats) {
                bool pixelInputAboveOne = false;
                for (uint32_t channel = 0; channel < 3u; ++channel) {
                    inputMin[channel] = std::min(inputMin[channel], color[channel]);
                    inputMax[channel] = std::max(inputMax[channel], color[channel]);
                    inputSum[channel] += color[channel];
                    pixelInputAboveOne = pixelInputAboveOne || color[channel] > 1.0f;
                }
                if (pixelInputAboveOne)
                    ++inputAboveOne;
            }
            const float luminance = std::max(
                0.0f, color[0] * 0.2126f + color[1] * 0.7152f
                    + color[2] * 0.0722f);
            if (filmEnabled) {
                for (uint32_t channel = 0; channel < 3; ++channel) {
                // This is the scalar form of the source postfx shader:
                // ((scene + luma * COLOR_BIAS.a) *
                //  (COLOR_TINT_BASE + luma * COLOR_TINT_DELTA)) +
                //  COLOR_BIAS.rgb.
                const float filmColor = (color[channel]
                    + luminance * desaturationScale)
                    * (tintBase[channel] + luminance * tintDelta[channel])
                    + tintBias;
                    if (collectFilmStats && (filmColor < 0.0f || filmColor > 1.0f))
                        ++outputClipped;
                    color[channel] = std::clamp(filmColor, 0.0f, 1.0f);
                }
            }
            if (glowReady) {
                const uint32_t glowX = std::min(
                    g_softwareGlowWidth - 1u, x * g_softwareGlowWidth / g_width);
                const uint32_t glowY = std::min(
                    g_softwareGlowHeight - 1u, y * g_softwareGlowHeight / g_height);
                for (uint32_t channel = 0; channel < 3u; ++channel)
                    color[channel] += g_softwareGlowBuffer[
                        (static_cast<size_t>(glowY) * g_softwareGlowWidth + glowX)
                            * 3u + channel] * glowIntensity;
            }
            for (uint32_t channel = 0; channel < 3; ++channel) {
                color[channel] = std::pow(
                    std::clamp(color[channel], 0.0f, 1.0f), displayExponent);
                if (collectFilmStats) {
                    outputMin[channel] = std::min(outputMin[channel], color[channel]);
                    outputMax[channel] = std::max(outputMax[channel], color[channel]);
                    outputSum[channel] += color[channel];
                }
            }
            if (collectFilmStats) {
                for (FilmProbe& probe : probes) {
                    if (probe.x == x && probe.y == y) {
                        std::memcpy(probe.output, color, sizeof(probe.output));
                        break;
                    }
                }
            }
            // Keep the final scalar quantisation identical to the native
            // post shader. The shared 8x8 blue-noise table removes the
            // repeating crosshatch without changing source-space lighting.
            const float dither = DisplayDither8x8(x, y);
            pixel = PackColor(color[0] + dither, color[1] + dither,
                              color[2] + dither,
                              static_cast<float>((pixel >> 24u) & 0xFFu) / 255.0f);
            g_softwareSceneColor[pixelIndex * 3u] = color[0];
            g_softwareSceneColor[pixelIndex * 3u + 1u] = color[1];
            g_softwareSceneColor[pixelIndex * 3u + 2u] = color[2];
        }
    }
    if (collectFilmStats) {
        const double denominator = static_cast<double>(std::max<size_t>(1u, pixelCount));
        sceClibPrintf(
            "[kisak-vita] film stats frame=%u input=%.4f..%.4f,%.4f..%.4f,%.4f..%.4f mean=%.4f,%.4f,%.4f above1=%u output=%.4f..%.4f,%.4f..%.4f,%.4f..%.4f mean=%.4f,%.4f,%.4f clippedChannels=%u\n",
            static_cast<unsigned>(filmStatsLogCount),
            inputMin[0], inputMax[0], inputMin[1], inputMax[1],
            inputMin[2], inputMax[2],
            static_cast<float>(inputSum[0] / denominator),
            static_cast<float>(inputSum[1] / denominator),
            static_cast<float>(inputSum[2] / denominator),
            static_cast<unsigned>(inputAboveOne),
            outputMin[0], outputMax[0], outputMin[1], outputMax[1],
            outputMin[2], outputMax[2],
            static_cast<float>(outputSum[0] / denominator),
            static_cast<float>(outputSum[1] / denominator),
            static_cast<float>(outputSum[2] / denominator),
            static_cast<unsigned>(outputClipped));
        char probeDetail[1024]{};
        int probeOffset = std::snprintf(
            probeDetail, sizeof(probeDetail),
            "film probes frame=%u", static_cast<unsigned>(filmStatsLogCount));
        for (const FilmProbe& probe : probes) {
            if (probeOffset < 0 || static_cast<size_t>(probeOffset)
                    >= sizeof(probeDetail))
                break;
            probeOffset += std::snprintf(
                probeDetail + probeOffset,
                sizeof(probeDetail) - static_cast<size_t>(probeOffset),
                " p%u,%u=%.3f,%.3f,%.3f>%.3f,%.3f,%.3f",
                static_cast<unsigned>(probe.x), static_cast<unsigned>(probe.y),
                probe.input[0], probe.input[1], probe.input[2],
                probe.output[0], probe.output[1], probe.output[2]);
        }
        sceClibPrintf("[kisak-vita] %s\n", probeDetail);
        ++filmStatsLogCount;
    }
}

bool SoftwareDrawSkyCubemap(GfxTextureHandle textureHandle, float fovDegrees,
                            float pitchDegrees, float yawDegrees) {
    if (!SoftwarePointersValid() || textureHandle.value == 0
        || textureHandle.value >= g_textures.size())
        return false;
    const SoftwareTexture& texture = g_textures[textureHandle.value];
    if (!texture.cubeMap || texture.width == 0 || texture.height == 0)
        return false;
    constexpr float kPiOver180 = 0.01745329251994329577f;
    const float pitch = std::clamp(pitchDegrees, -89.9f, 89.9f) * kPiOver180;
    const float yaw = yawDegrees * kPiOver180;
    const float sp = std::sin(pitch), cp = std::cos(pitch);
    const float sy = std::sin(yaw), cy = std::cos(yaw);
    const float forward[3] = {cp * cy, cp * sy, -sp};
    const float right[3] = {sy, -cy, 0.0f};
    const float up[3] = {sp * cy, sp * sy, cp};
    constexpr float kPiOver360 = 0.00872664625997164788f;
    constexpr float kScreenAspect = 960.0f / 544.0f;
    const float fov = std::clamp(fovDegrees, 1.0f, 160.0f);
    const float tanHalfFovY = std::max(0.0001f,
                                      std::tan(fov * kPiOver360) * 0.75f);
    const float horizontalScale = 1.0f / (tanHalfFovY * kScreenAspect);
    const float verticalScale = 1.0f / tanHalfFovY;
    const float width = static_cast<float>(g_width);
    const float height = static_cast<float>(g_height);
    // The source sky material is an unlit cube sampler. Fill the cleared
    // scalar display directly with the six authored faces; unlike an ordinary
    // projected quad this keeps the sky direction tied to the same camera
    // FOV/yaw/pitch used by the BSP and static-model passes.
    for (uint32_t y = 0; y < g_height; ++y) {
        const float clipY = 1.0f - 2.0f
            * (static_cast<float>(y) + 0.5f) / height;
        for (uint32_t x = 0; x < g_width; ++x) {
            const float clipX = 2.0f
                * (static_cast<float>(x) + 0.5f) / width - 1.0f;
            const float ray[3] = {
                forward[0] + clipX / horizontalScale * right[0]
                    + clipY / verticalScale * up[0],
                forward[1] + clipX / horizontalScale * right[1]
                    + clipY / verticalScale * up[1],
                forward[2] + clipX / horizontalScale * right[2]
                    + clipY / verticalScale * up[2],
            };
            const size_t pixel = static_cast<size_t>(y) * g_width + x;
            const uint32_t sample = SampleSoftwareTextureCube(
                texture, ray[0], ray[1], ray[2]);
            float color[4]{};
            UnpackSoftwareColor(sample, color);
            g_softwareSceneColor[pixel * 3u] = color[0];
            g_softwareSceneColor[pixel * 3u + 1u] = color[1];
            g_softwareSceneColor[pixel * 3u + 2u] = color[2];
            g_framebuffer[pixel] = sample;
        }
    }
    return true;
}

void SoftwareStorePixelColor(size_t pixel, const float source[4], float alpha,
                             float depth) {
    if (!SoftwarePointersValid() || pixel >= g_softwarePixelCount
        || pixel >= g_depth.size()) {
        if (g_softwareInvalidStores < 8u)
            ++g_softwareInvalidStores;
        return;
    }
    const bool sourceState = SourceMaterialStateApplies(g_materialPass);
    const bool alphaTest = sourceState
        ? SourceStateUsesAlphaTest(g_materialPass)
        : (g_softwareSurfaceMode == GfxSurfaceMode::AlphaTest
           || g_softwareSurfaceMode == GfxSurfaceMode::AlphaTestBlend);
    if (alphaTest && !SoftwareAlphaTestPass(alpha))
        return;
    if (!source)
        return;
    float fogged[4] = {source[0], source[1], source[2], source[3]};
    ApplySoftwareFog(fogged, depth);
    float* destination = g_softwareSceneColor + pixel * 3u;
    const bool depthTest = sourceState
        ? SourceStateUsesDepthTest(g_materialPass) : g_softwareDepthTest;
    const bool depthWrite = sourceState
        ? SourceStateWritesDepth(g_materialPass) : g_softwareDepthWrite;
    if (depthTest) {
        if (!SoftwareSourceDepthPass(depth, g_depth[pixel], g_materialPass))
            return;
        if (depthWrite)
            g_depth[pixel] = depth;
    }
    if (SourceStateUsesBlend(g_materialPass)) {
        const uint32_t sourceBits = EffectiveSourceStateBits0(g_materialPass);
        const uint32_t sourceFactor = sourceBits & 0x0Fu;
        const uint32_t destinationFactor = (sourceBits >> 4u) & 0x0Fu;
        const uint32_t blendOperation = (sourceBits >> 8u) & 0x07u;
        const float sourceAlpha = std::clamp(fogged[3], 0.0f, 1.0f);
        const float destinationAlpha = static_cast<float>(
            (g_framebuffer[pixel] >> 24u) & 0xFFu) / 255.0f;
        const float destinationBefore[3] = {
            destination[0], destination[1], destination[2],
        };
        for (uint32_t channel = 0; channel < 3u; ++channel) {
            const float sourceTerm = fogged[channel]
                * SoftwareSourceBlendFactor(sourceFactor, channel, fogged,
                                             destinationBefore, sourceAlpha,
                                             destinationAlpha);
            const float destinationTerm = destinationBefore[channel]
                * SoftwareSourceBlendFactor(destinationFactor, channel, fogged,
                                             destinationBefore, sourceAlpha,
                                             destinationAlpha);
            destination[channel] = SoftwareSourceBlendOperation(
                blendOperation, sourceTerm, destinationTerm);
        }
        g_framebuffer[pixel] = PackColor(destination[0], destination[1],
                                         destination[2], 1.0f);
        return;
    }
    if (!sourceState && g_softwareSurfaceMode == GfxSurfaceMode::Additive) {
        for (uint32_t channel = 0; channel < 3u; ++channel)
            destination[channel] += fogged[channel] * alpha;
        g_framebuffer[pixel] = PackColor(destination[0], destination[1],
                                         destination[2], 1.0f);
        return;
    }
    const bool opaqueLike = sourceState ? !SourceStateUsesBlend(g_materialPass)
        : (g_softwareSurfaceMode == GfxSurfaceMode::Opaque
           || g_softwareSurfaceMode == GfxSurfaceMode::AlphaTest
           || g_softwareSurfaceMode == GfxSurfaceMode::Sky);
    if (opaqueLike) {
        for (uint32_t channel = 0; channel < 3u; ++channel)
            destination[channel] = fogged[channel];
        g_framebuffer[pixel] = PackColor(destination[0], destination[1],
                                         destination[2], fogged[3]);
        return;
    }
    if (alpha <= 0.0f)
        return;
    for (uint32_t channel = 0; channel < 3u; ++channel)
        destination[channel] = fogged[channel] * alpha
            + destination[channel] * (1.0f - alpha);
    g_framebuffer[pixel] = PackColor(destination[0], destination[1],
                                     destination[2], 1.0f);
}

void SoftwareStoreBlockColor(int x, int y, const float source[4], float alpha,
                             float depth, int step = kSoftwareRasterStep) {
    // Fill each coarse sample cell so the diagnostic fallback produces solid
    // surfaces instead of a dotted point cloud.
    if (!SoftwarePointersValid() || x < 0 || y < 0
        || x >= static_cast<int>(g_width) || y >= static_cast<int>(g_height)
        || step <= 0)
        return;
    const int blockWidth = std::min(step, static_cast<int>(g_width) - x);
    const int blockHeight = std::min(step, static_cast<int>(g_height) - y);
    if (blockWidth <= 0 || blockHeight <= 0)
        return;
    const bool sourceState = SourceMaterialStateApplies(g_materialPass);
    const bool alphaTest = sourceState
        ? SourceStateUsesAlphaTest(g_materialPass)
        : (g_softwareSurfaceMode == GfxSurfaceMode::AlphaTest
           || g_softwareSurfaceMode == GfxSurfaceMode::AlphaTestBlend);
    if (alphaTest && !SoftwareAlphaTestPass(alpha))
        return;
    if (!source)
        return;
    // Most world samples are opaque. Keep the coarse shading sample, but run
    // the source depth test for every destination pixel in the block. The old
    // first-pixel shortcut let a foreground wall/roof write across a whole
    // 4x4 cell after only its first depth value passed, which filled doorways
    // and covered interiors with the wrong surface.
    const bool opaqueLike = sourceState ? !SourceStateUsesBlend(g_materialPass)
        : (g_softwareSurfaceMode == GfxSurfaceMode::Opaque
           || g_softwareSurfaceMode == GfxSurfaceMode::AlphaTest
           || g_softwareSurfaceMode == GfxSurfaceMode::Sky);
    const bool depthTest = sourceState
        ? SourceStateUsesDepthTest(g_materialPass) : g_softwareDepthTest;
    const bool depthWrite = sourceState
        ? SourceStateWritesDepth(g_materialPass) : g_softwareDepthWrite;
    if (opaqueLike) {
        float fogged[4] = {source[0], source[1], source[2], source[3]};
        ApplySoftwareFog(fogged, depth);
        for (int blockY = 0; blockY < blockHeight; ++blockY) {
            const size_t row = static_cast<size_t>(y + blockY) * g_width
                + static_cast<size_t>(x);
            if (row >= g_softwarePixelCount
                || static_cast<size_t>(blockWidth) > g_softwarePixelCount - row)
                return;
            for (int blockX = 0; blockX < blockWidth; ++blockX) {
                const size_t pixel = row + static_cast<size_t>(blockX);
                if (depthTest && !SoftwareSourceDepthPass(
                        depth, g_depth[pixel], g_materialPass))
                    continue;
                g_softwareSceneColor[pixel * 3u] = fogged[0];
                g_softwareSceneColor[pixel * 3u + 1u] = fogged[1];
                g_softwareSceneColor[pixel * 3u + 2u] = fogged[2];
                g_framebuffer[pixel] = PackColor(
                    fogged[0], fogged[1], fogged[2], fogged[3]);
                if (depthTest && depthWrite)
                    g_depth[pixel] = depth;
            }
        }
        return;
    }
    for (int blockY = 0; blockY < step
         && y + blockY < static_cast<int>(g_height); ++blockY) {
        for (int blockX = 0; blockX < step
             && x + blockX < static_cast<int>(g_width); ++blockX) {
            const size_t pixel = static_cast<size_t>(y + blockY) * g_width
                + static_cast<size_t>(x + blockX);
            SoftwareStorePixelColor(pixel, source, alpha, depth);
        }
    }
}

void SoftwareDrawIndexed(const GfxDrawVertex* vertices, uint32_t vertexCount,
                         const uint16_t* indices, uint32_t indexCount) {
    if (!SoftwarePointersValid() || g_depth.size() != g_softwarePixelCount
        || !vertices || !indices || vertexCount == 0 || indexCount < 3)
        return;
    const float width = static_cast<float>(g_width);
    const float height = static_cast<float>(g_height);
    const auto ToScreenX = [width](float value) { return (value * 0.5f + 0.5f) * width; };
    const auto ToScreenY = [height](float value) { return (0.5f - value * 0.5f) * height; };
    // The compatibility frontend emits thousands of axis-aligned four-vertex
    // rectangles for labels, panels, and the infinite sky underlay. Handle
    // only those explicitly screen-space modes directly. World and XModel
    // meshes can also contain four-vertex quads; sending those through the UI
    // shortcut discards their perspective, depth, normals, and lightmaps.
    const bool axisAlignedSky = g_softwareSurfaceMode == GfxSurfaceMode::Sky;
    if (axisAlignedSky && vertexCount == 4 && indexCount == 6) {
        float left = width, right = 0.0f, top = height, bottom = 0.0f;
        for (uint32_t index = 0; index < 4; ++index) {
            left = std::min(left, ToScreenX(vertices[index].position[0]));
            right = std::max(right, ToScreenX(vertices[index].position[0]));
            top = std::min(top, ToScreenY(vertices[index].position[1]));
            bottom = std::max(bottom, ToScreenY(vertices[index].position[1]));
        }
        const int minX = std::max(0, static_cast<int>(std::floor(left)));
        const int maxX = std::min(static_cast<int>(g_width) - 1,
                                  static_cast<int>(std::ceil(right)));
        const int minY = std::max(0, static_cast<int>(std::floor(top)));
        const int maxY = std::min(static_cast<int>(g_height) - 1,
                                  static_cast<int>(std::ceil(bottom)));
        if (maxX < minX || maxY < minY)
            return;
        float solidSource[4] = {
            vertices[0].color[0], vertices[0].color[1],
            vertices[0].color[2], vertices[0].color[3],
        };
        // Textured UI/HUD quads use the same fast axis-aligned path as solid
        // panels.  Sample the bound software texture here rather than silently
        // flattening every authored image to the vertex tint; the accelerated
        // vitaGL path remains on its normal textured draw path.
        const bool textured = g_boundTexture != 0 && g_boundTexture < g_textures.size()
            && right > left && bottom > top;
        // Use a finer sample grid for authored UI/HUD images and glyphs.  The
        // large world fallback remains on the coarser bounded grid, while
        // menu labels and ammo/compass textures stay legible in emulator
        // captures without changing the bounded per-pass sample budget.
        const bool fontTexture = textured
            && g_textures[g_boundTexture].width == 512
            && g_textures[g_boundTexture].height == 1024;
        const int rasterStep = fontTexture ? 1 : (textured ? 2 : kSoftwareRasterStep);
        const size_t sampledColumns = static_cast<size_t>(
            (maxX - minX) / rasterStep + 1);
        const size_t sampledRows = static_cast<size_t>(
            (maxY - minY) / rasterStep + 1);
        const size_t rectanglePixels = sampledColumns * sampledRows;
        if (rectanglePixels == 0
            || rectanglePixels > kSoftwareRasterPixelBudget - g_softwareRasterPixels)
            return;
        // Account for the actual bounded sample grid rather than the full
        // rectangle area.  This lets a representative world texture render
        // broad surfaces without spending the entire budget on empty pixels
        // between step-2/step-4 samples.
        g_softwareRasterPixels += rectanglePixels;
        for (int y = minY; y <= maxY; y += rasterStep) {
            for (int x = minX; x <= maxX; x += rasterStep) {
                float source[4] = {
                    solidSource[0], solidSource[1], solidSource[2], 1.0f,
                };
                float alpha = std::clamp(vertices[0].color[3], 0.0f, 1.0f);
                if (textured) {
                    // Axis-aligned quads are used for both authored UI
                    // images and individual VITF glyphs.  Their texture
                    // coordinates are not necessarily the full [0,1]
                    // rectangle: glyphs carry a small atlas sub-rect.  The
                    // old scalar shortcut normalized screen coordinates and
                    // silently discarded those UVs, so every glyph sampled a
                    // different part of the complete atlas and rendered as
                    // pale blocks.  Interpolate the quad's authored corner
                    // UVs just like the accelerated rasterizer does.
                    const float horizontal = std::clamp(
                        (static_cast<float>(x) - left) / (right - left),
                        0.0f, 1.0f);
                    const float vertical = std::clamp(
                        (static_cast<float>(y) - top) / (bottom - top),
                        0.0f, 1.0f);
                    // Interpolate both UV channels from all four authored
                    // corners.  Ordinary UI quads are axis-aligned, but the
                    // fixed-family sky pass intentionally rotates the retail
                    // cube-face mapping; assuming U is always horizontal
                    // silently collapsed that pass to a flat strip.
                    const auto InterpolateQuadUv = [&](uint32_t channel) {
                        const float top = vertices[3].texCoord[channel]
                            + horizontal * (vertices[2].texCoord[channel]
                                - vertices[3].texCoord[channel]);
                        const float bottom = vertices[0].texCoord[channel]
                            + horizontal * (vertices[1].texCoord[channel]
                                - vertices[0].texCoord[channel]);
                        // Screen-space vertical zero is the top edge, so
                        // interpolate from the authored top row toward the
                        // bottom row as raster Y increases.
                        return top + vertical * (bottom - top);
                    };
                    const float u = InterpolateQuadUv(0);
                    const float v = InterpolateQuadUv(1);
                    const uint32_t texel = SampleSoftwareTexture(u, v);
                    const float tintR = std::clamp(vertices[0].color[0], 0.0f, 1.0f);
                    const float tintG = std::clamp(vertices[0].color[1], 0.0f, 1.0f);
                    const float tintB = std::clamp(vertices[0].color[2], 0.0f, 1.0f);
                    const float texelA = static_cast<float>((texel >> 24) & 0xFFu) / 255.0f;
                    source[0] = static_cast<float>(texel & 0xFFu) / 255.0f
                        * tintR;
                    source[1] = static_cast<float>((texel >> 8) & 0xFFu) / 255.0f
                        * tintG;
                    source[2] = static_cast<float>((texel >> 16) & 0xFFu) / 255.0f
                        * tintB;
                    alpha *= texelA;
                }
                static const float kNoDynamicLighting[3] = {
                    1.0f, 1.0f, 1.0f,
                };
                static const float kNoStaticLighting[3] = {
                    1.0f, 1.0f, 1.0f,
                };
                RecordSoftwareLightingProbe(
                    x, y, vertices[0].position[2], source,
                    kNoDynamicLighting, 0.0f, kNoStaticLighting,
                    kNoStaticLighting, source, 0.0f, 1.0f, -1.0f,
                    false, false);
                SoftwareStoreBlockColor(x, y, source, alpha,
                                        vertices[0].position[2], rasterStep);
            }
        }
        return;
    }
    for (uint32_t triangle = 0; triangle + 2 < indexCount; triangle += 3) {
        const uint16_t ia = indices[triangle];
        const uint16_t ib = indices[triangle + 1];
        const uint16_t ic = indices[triangle + 2];
        if (ia >= vertexCount || ib >= vertexCount || ic >= vertexCount)
            continue;
        const GfxDrawVertex& a = vertices[ia];
        const GfxDrawVertex& b = vertices[ib];
        const GfxDrawVertex& c = vertices[ic];
        const float ax = ToScreenX(a.position[0]), ay = ToScreenY(a.position[1]);
        const float bx = ToScreenX(b.position[0]), by = ToScreenY(b.position[1]);
        const float cx = ToScreenX(c.position[0]), cy = ToScreenY(c.position[1]);
        const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        if (!std::isfinite(area) || std::fabs(area) < 0.01f)
            continue;
        // D3D9's source state uses clockwise front faces.  The projected
        // screen coordinates below have a downward Y axis, so a positive
        // screen-space area is the authored clockwise/front winding.  Keep
        // the scalar diagnostic consistent with the VitaGL path instead of
        // rendering every world triangle double-sided.
        // D3D9's source state uses clockwise front faces.  The projected
        // screen coordinates below have a downward Y axis, so a positive
        // screen-space area is the authored clockwise/front winding. Keep
        // scalar culling consistent with the accelerated VitaGL path.
        if ((g_softwareCullMode == GfxCullMode::Back && area < 0.0f)
            || (g_softwareCullMode == GfxCullMode::Front && area > 0.0f))
            continue;
        const int minX = std::max(0, static_cast<int>(std::floor(std::min({ax, bx, cx}))));
        const int maxX = std::min(static_cast<int>(g_width) - 1,
                                  static_cast<int>(std::ceil(std::max({ax, bx, cx}))));
        const int minY = std::max(0, static_cast<int>(std::floor(std::min({ay, by, cy}))));
        const int maxY = std::min(static_cast<int>(g_height) - 1,
                                  static_cast<int>(std::ceil(std::max({ay, by, cy}))));
        if (maxX < minX || maxY < minY)
            continue;
        // Long, thin BSP triangles are common on Shipment's container seams,
        // rails, and trim. A fixed 4-pixel sample grid can miss them entirely
        // (leaving the cleared framebuffer visible as dark gaps). Refine only
        // narrow triangles; broad wall/floor triangles use the same bounded
        // one-pixel path so the fallback preserves continuous surfaces.
        const int boundsWidth = maxX - minX + 1;
        const int boundsHeight = maxY - minY + 1;
        const int rasterStep = (boundsWidth <= 18 || boundsHeight <= 18)
            ? 1 : kSoftwareRasterStep;
        const size_t sampleColumns = static_cast<size_t>(
            (maxX - minX) / rasterStep + 1);
        const size_t sampleRows = static_cast<size_t>(
            (maxY - minY) / rasterStep + 1);
        const size_t trianglePixels = sampleColumns * sampleRows;
        if (trianglePixels == 0
            || trianglePixels > kSoftwareRasterPixelBudget - g_softwareRasterPixels)
            continue;
        g_softwareRasterPixels += trianglePixels;
        for (int y = minY; y <= maxY; y += rasterStep) {
            for (int x = minX; x <= maxX; x += rasterStep) {
                const float px = static_cast<float>(x) + 0.5f * rasterStep;
                const float py = static_cast<float>(y) + 0.5f * rasterStep;
                const auto Barycentric = [&](float sampleX, float sampleY,
                                             float* outA, float* outB,
                                             float* outC) {
                    *outA = ((bx - sampleX) * (cy - sampleY)
                        - (by - sampleY) * (cx - sampleX)) / area;
                    *outB = ((cx - sampleX) * (ay - sampleY)
                        - (cy - sampleY) * (ax - sampleX)) / area;
                    *outC = 1.0f - *outA - *outB;
                };
                float wa = 0.0f, wb = 0.0f, wc = 0.0f;
                Barycentric(px, py, &wa, &wb, &wc);
                if (wa < 0.0f || wb < 0.0f || wc < 0.0f) {
                    // A center-only test leaves black cracks when a long
                    // BSP edge crosses a coarse sample cell without
                    // containing its center. Barycentric edge functions are
                    // linear, so checking the four corners is a cheap
                    // conservative coverage test for such cells.
                    const float right = std::min(
                        static_cast<float>(x + rasterStep), width);
                    const float bottom = std::min(
                        static_cast<float>(y + rasterStep), height);
                    float maxA = -std::numeric_limits<float>::infinity();
                    float maxB = -std::numeric_limits<float>::infinity();
                    float maxC = -std::numeric_limits<float>::infinity();
                    const float corners[4][2] = {
                        {static_cast<float>(x), static_cast<float>(y)},
                        {right, static_cast<float>(y)},
                        {static_cast<float>(x), bottom},
                        {right, bottom},
                    };
                    for (const auto& corner : corners) {
                        float cornerA = 0.0f, cornerB = 0.0f, cornerC = 0.0f;
                        Barycentric(corner[0], corner[1], &cornerA,
                                    &cornerB, &cornerC);
                        maxA = std::max(maxA, cornerA);
                        maxB = std::max(maxB, cornerB);
                        maxC = std::max(maxC, cornerC);
                    }
                    if (maxA < 0.0f || maxB < 0.0f || maxC < 0.0f)
                        continue;
                    // Keep the center's stable interpolation when it is
                    // inside; for edge-only coverage, project it onto the
                    // triangle so color/UV/depth remain finite.
                    wa = std::max(0.0f, wa);
                    wb = std::max(0.0f, wb);
                    wc = std::max(0.0f, wc);
                    const float weightSum = wa + wb + wc;
                    if (weightSum <= 0.0f)
                        continue;
                    wa /= weightSum;
                    wb /= weightSum;
                    wc /= weightSum;
                }
                // The projected world stores an encoded camera depth in the
                // third vertex component. Recover a reciprocal camera-Z
                // weight for world triangles so material UVs and colours are
                // perspective-correct instead of stretching across the large
                // BSP faces that dominate Shipment. Screen-space UI,
                // viewmodels, and FX use small explicit overlay depths and
                // intentionally stay affine.
                const bool perspectiveWorld = a.position[2] >= 0.25f
                    && b.position[2] >= 0.25f && c.position[2] >= 0.25f;
                if (perspectiveWorld) {
                    const float qa = std::max(0.000001f, 1.0f - a.position[2]);
                    const float qb = std::max(0.000001f, 1.0f - b.position[2]);
                    const float qc = std::max(0.000001f, 1.0f - c.position[2]);
                    const float sum = wa * qa + wb * qb + wc * qc;
                    if (sum <= 0.000001f || !std::isfinite(sum))
                        continue;
                    wa = wa * qa / sum;
                    wb = wb * qb / sum;
                    wc = wc * qc / sum;
                }
                const float z = wa * a.position[2] + wb * b.position[2] + wc * c.position[2];
                // Match the real GL_LEQUAL depth state while tolerating the
                // tiny interpolation-rounding difference between the two
                // triangles that make up a solid UI rectangle.
                if (!std::isfinite(z))
                    continue;
                const DecodedSourceSunVisibility sunVisibility =
                    DecodeSourceSunVisibility(
                        wa * a.shadowVisibility + wb * b.shadowVisibility
                            + wc * c.shadowVisibility);
                const float shadowVisibility = sunVisibility.shadowVisibility;
                const float localLightGridSunWeight =
                    sunVisibility.localLightGridSunWeight;
                float color[4]{};
                for (uint32_t channel = 0; channel < 4; ++channel)
                    color[channel] = wa * a.color[channel] + wb * b.color[channel]
                        + wc * c.color[channel];
                const float materialU =
                    wa * a.texCoord[0] + wb * b.texCoord[0] + wc * c.texCoord[0];
                const float materialV =
                    wa * a.texCoord[1] + wb * b.texCoord[1] + wc * c.texCoord[1];
                const uint32_t materialTexel = SampleSoftwareTexture(
                    materialU, materialV);
                for (uint32_t channel = 0; channel < 3; ++channel) {
                    color[channel] *= static_cast<float>(
                        (materialTexel >> (channel * 8u)) & 0xFFu) / 255.0f;
                }
                color[3] *= static_cast<float>((materialTexel >> 24) & 0xFFu)
                    / 255.0f;
                const float materialColor[3] = {
                    color[0], color[1], color[2],
                };
                const bool sourceUnlit = SourceTechniqueIsUnlit(g_materialPass);
                const bool bakedLighting = (g_materialPass.techniqueFlags0
                    & GFX_MATERIAL_PASS_BAKED_LIGHTING) != 0;
                const bool semanticPass = !sourceUnlit && (g_softwareSurfaceMode
                    != GfxSurfaceMode::Overlay || bakedLighting)
                    && (g_materialPass.colorTexture.value == 0
                        || g_materialPass.colorTexture.value == g_boundTexture);
                // The source BSP lit technique owns its static illumination in
                // the primary/secondary lightmap pair.  The frame/lightgrid
                // response is only a dynamic term for unlightmapped cards and
                // model/indoor materials; keep it out of the lightmap path so
                // the two authored lighting sources are not multiplied.
                const bool usePrimaryLightmap = !sourceUnlit
                    &&
                    (g_softwareLightmapSamplerFlags & 0x02u) != 0
                    && g_softwareLightmapPrimary != 0;
                const bool useSecondaryLightmap = !sourceUnlit
                    &&
                    (g_softwareLightmapSamplerFlags & 0x04u) != 0
                    && g_softwareLightmapSecondary != 0;
                const bool sourceLightmappedPass = usePrimaryLightmap
                    || useSecondaryLightmap;
                float interpolatedNormal[3] = {
                    wa * a.normal[0] + wb * b.normal[0] + wc * c.normal[0],
                    wa * a.normal[1] + wb * b.normal[1] + wc * c.normal[1],
                    wa * a.normal[2] + wb * b.normal[2] + wc * c.normal[2],
                };
                float sourceLightmapNormal[3] = {
                    interpolatedNormal[0], interpolatedNormal[1],
                    interpolatedNormal[2],
                };
                float sourceLightmapNormalXY[2] = {0.0f, 0.0f};
                float tangentNormal[3] = {0.0f, 0.0f, 1.0f};
                float dynamicFactors[3] = {1.0f, 1.0f, 1.0f};
                if (semanticPass) {
                    if (g_materialPass.normalTexture.value != 0) {
                        const uint32_t normalTexel = SampleSoftwareTextureHandle(
                            g_materialPass.normalTexture.value,
                            materialU, materialV, 1u);
                        const float encodedNormalX = static_cast<float>(
                            (normalTexel >> 24u) & 0xFFu) / 255.0f;
                        const float encodedNormalY = static_cast<float>(
                            (normalTexel >> 8u) & 0xFFu) / 255.0f;
                        tangentNormal[0] = encodedNormalX * 2.0f - 1.0f;
                        tangentNormal[1] = encodedNormalY * 2.0f - 1.0f;
                        // The retail lm_* shader decodes the normal-map XY
                        // channels with the same quantized range used by its
                        // secondary-light direction and attenuates them by
                        // color alpha before both secondary alignment and the
                        // source bumped-normal sun term.
                        const float sourceColorAlpha = std::clamp(
                            color[3], 0.0f, 1.0f);
                        sourceLightmapNormalXY[0] = encodedNormalX
                            * 4.07999992f - 2.07999992f;
                        sourceLightmapNormalXY[1] = encodedNormalY
                            * 4.06451607f - 2.06451607f;
                        sourceLightmapNormalXY[0] *= sourceColorAlpha;
                        sourceLightmapNormalXY[1] *= sourceColorAlpha;
                        // Retail COD4 normal images use DXT5nm (X in alpha,
                        // Y in green). Rebuild +Z and transform through the
                        // interpolated authored tangent frame.
                        tangentNormal[2] = std::sqrt(std::max(
                            0.0f, 1.0f - tangentNormal[0] * tangentNormal[0]
                                - tangentNormal[1] * tangentNormal[1]));
                        const float normalLengthSquared = interpolatedNormal[0]
                            * interpolatedNormal[0] + interpolatedNormal[1]
                            * interpolatedNormal[1] + interpolatedNormal[2]
                            * interpolatedNormal[2];
                        float tangent[3] = {
                            wa * a.tangent[0] + wb * b.tangent[0] + wc * c.tangent[0],
                            wa * a.tangent[1] + wb * b.tangent[1] + wc * c.tangent[1],
                            wa * a.tangent[2] + wb * b.tangent[2] + wc * c.tangent[2],
                        };
                        if (normalLengthSquared > 0.000001f
                            && std::isfinite(normalLengthSquared)) {
                            const float inverseNormal = 1.0f
                                / std::sqrt(normalLengthSquared);
                            for (float& component : interpolatedNormal)
                                component *= inverseNormal;
                            const float normalTangentDot = interpolatedNormal[0]
                                * tangent[0] + interpolatedNormal[1] * tangent[1]
                                + interpolatedNormal[2] * tangent[2];
                            for (uint32_t channel = 0; channel < 3; ++channel)
                                tangent[channel] -= interpolatedNormal[channel]
                                    * normalTangentDot;
                            const float tangentLengthSquared = tangent[0] * tangent[0]
                                + tangent[1] * tangent[1] + tangent[2] * tangent[2];
                            if (tangentLengthSquared > 0.000001f) {
                                const float inverseTangent = 1.0f
                                    / std::sqrt(tangentLengthSquared);
                                for (float& component : tangent)
                                    component *= inverseTangent;
                                const float bitangent[3] = {
                                    interpolatedNormal[1] * tangent[2]
                                        - interpolatedNormal[2] * tangent[1],
                                    interpolatedNormal[2] * tangent[0]
                                        - interpolatedNormal[0] * tangent[2],
                                    interpolatedNormal[0] * tangent[1]
                                        - interpolatedNormal[1] * tangent[0],
                                };
                                sourceLightmapNormal[0] = interpolatedNormal[0]
                                    + tangent[0] * sourceLightmapNormalXY[0]
                                    + bitangent[0] * sourceLightmapNormalXY[1];
                                sourceLightmapNormal[1] = interpolatedNormal[1]
                                    + tangent[1] * sourceLightmapNormalXY[0]
                                    + bitangent[1] * sourceLightmapNormalXY[1];
                                sourceLightmapNormal[2] = interpolatedNormal[2]
                                    + tangent[2] * sourceLightmapNormalXY[0]
                                    + bitangent[2] * sourceLightmapNormalXY[1];
                                const float sourceNormalLengthSquared =
                                    sourceLightmapNormal[0] * sourceLightmapNormal[0]
                                    + sourceLightmapNormal[1] * sourceLightmapNormal[1]
                                    + sourceLightmapNormal[2] * sourceLightmapNormal[2];
                                if (sourceNormalLengthSquared > 0.000001f
                                    && std::isfinite(sourceNormalLengthSquared)) {
                                    const float sourceNormalScale = 1.0f
                                        / std::sqrt(sourceNormalLengthSquared);
                                    for (float& component : sourceLightmapNormal)
                                        component *= sourceNormalScale;
                                }
                                interpolatedNormal[0] = tangent[0] * tangentNormal[0]
                                    + bitangent[0] * tangentNormal[1]
                                    + interpolatedNormal[0] * tangentNormal[2];
                                interpolatedNormal[1] = tangent[1] * tangentNormal[0]
                                    + bitangent[1] * tangentNormal[1]
                                    + interpolatedNormal[1] * tangentNormal[2];
                                interpolatedNormal[2] = tangent[2] * tangentNormal[0]
                                    + bitangent[2] * tangentNormal[1]
                                    + interpolatedNormal[2] * tangentNormal[2];
                            }
                        }
                    }
                    const float interpolatedWorldPosition[3] = {
                        wa * a.worldPosition[0] + wb * b.worldPosition[0]
                            + wc * c.worldPosition[0],
                        wa * a.worldPosition[1] + wb * b.worldPosition[1]
                            + wc * c.worldPosition[1],
                        wa * a.worldPosition[2] + wb * b.worldPosition[2]
                            + wc * c.worldPosition[2],
                    };
                    if (!sourceLightmappedPass && !bakedLighting && !sourceUnlit) {
                        EvaluateFrameLighting(interpolatedNormal,
                                               interpolatedWorldPosition,
                                               shadowVisibility,
                                               localLightGridSunWeight,
                                               dynamicFactors);
                        for (uint32_t channel = 0; channel < 3; ++channel)
                            color[channel] *= dynamicFactors[channel];
                    } else if ((sourceLightmappedPass || bakedLighting)
                               && !sourceUnlit) {
                        // Keep global frame/lightgrid response out of the
                        // authored lightmap, but retain the source's additive
                        // non-sun primary lights on BSP and baked model
                        // materials with the same attenuation as native.
                        float primaryLightResponse[3] = {0.0f, 0.0f, 0.0f};
                        EvaluatePrimaryLightResponse(
                            interpolatedNormal, interpolatedWorldPosition,
                            primaryLightResponse);
                        for (uint32_t channel = 0; channel < 3; ++channel)
                            color[channel] *= std::clamp(
                                1.0f + primaryLightResponse[channel],
                                0.0f, 1.35f);
                    }
                    if (g_materialPass.specularTexture.value != 0) {
                        const uint32_t specTexel = SampleSoftwareTextureHandle(
                            g_materialPass.specularTexture.value,
                            materialU, materialV, 2u);
                        float normalLengthSquared = interpolatedNormal[0]
                            * interpolatedNormal[0]
                            + interpolatedNormal[1] * interpolatedNormal[1]
                            + interpolatedNormal[2] * interpolatedNormal[2];
                        const float view[3] = {
                            g_frameLighting.cameraPosition[0]
                                - interpolatedWorldPosition[0],
                            g_frameLighting.cameraPosition[1]
                                - interpolatedWorldPosition[1],
                            g_frameLighting.cameraPosition[2]
                                - interpolatedWorldPosition[2],
                        };
                        const float viewLengthSquared = view[0] * view[0]
                            + view[1] * view[1] + view[2] * view[2];
                        const float sunLengthSquared =
                            g_frameLighting.sunDirection[0]
                                * g_frameLighting.sunDirection[0]
                            + g_frameLighting.sunDirection[1]
                                * g_frameLighting.sunDirection[1]
                            + g_frameLighting.sunDirection[2]
                                * g_frameLighting.sunDirection[2];
                        if (std::isfinite(normalLengthSquared)
                            && normalLengthSquared > 0.000001f
                            && std::isfinite(viewLengthSquared)
                            && viewLengthSquared > 0.000001f
                            && std::isfinite(sunLengthSquared)
                            && sunLengthSquared > 0.000001f) {
                            const float normalScale =
                                1.0f / std::sqrt(normalLengthSquared);
                            const float viewScale =
                                1.0f / std::sqrt(viewLengthSquared);
                            const float sunScale =
                                1.0f / std::sqrt(sunLengthSquared);
                            float normalizedNormal[3] = {
                                interpolatedNormal[0] * normalScale,
                                interpolatedNormal[1] * normalScale,
                                interpolatedNormal[2] * normalScale,
                            };
                            const float normalizedView[3] = {
                                view[0] * viewScale, view[1] * viewScale,
                                view[2] * viewScale,
                            };
                            const float normalizedSun[3] = {
                                g_frameLighting.sunDirection[0] * sunScale,
                                g_frameLighting.sunDirection[1] * sunScale,
                                g_frameLighting.sunDirection[2] * sunScale,
                            };
                            const float normalViewDot =
                                normalizedNormal[0] * normalizedView[0]
                                + normalizedNormal[1] * normalizedView[1]
                                + normalizedNormal[2] * normalizedView[2];
                            const float reflection[3] = {
                                normalizedView[0]
                                    - 2.0f * normalViewDot * normalizedNormal[0],
                                normalizedView[1]
                                    - 2.0f * normalViewDot * normalizedNormal[1],
                                normalizedView[2]
                                    - 2.0f * normalViewDot * normalizedNormal[2],
                            };
                            const float reflectionSun =
                                reflection[0] * normalizedSun[0]
                                + reflection[1] * normalizedSun[1]
                                + reflection[2] * normalizedSun[2];
                            const float specAlpha = static_cast<float>(
                                (specTexel >> 24u) & 0xFFu) / 255.0f;
                            const float specExponent = std::exp(
                                std::clamp(specAlpha, 0.0f, 1.0f)
                                    * 9.3775177f) + 7.0f;
                            const float specLog = (reflectionSun - 0.99925f)
                                * specExponent * 1.44269502f;
                            const float specular = specLog >= 0.0f
                                ? 1.0f : std::exp(std::max(specLog, -80.0f));
                            for (uint32_t channel = 0; channel < 3; ++channel) {
                                const float mask = static_cast<float>(
                                    (specTexel >> (channel * 8u)) & 0xFFu)
                                    / 255.0f;
                                color[channel] += std::clamp(specular * mask
                                    * g_frameLighting.sunColor[channel]
                                    * shadowVisibility,
                                    0.0f, 1.0f);
                            }
                            const uint32_t reflectionHandle =
                                g_materialPass.reflectionProbeTexture.value;
                            if (reflectionHandle < g_textures.size()
                                && g_textures[reflectionHandle].cubeMap) {
                                const uint32_t environmentTexel =
                                    SampleSoftwareTextureCube(
                                        g_textures[reflectionHandle],
                                        reflection[0], reflection[1], reflection[2]);
                                const float fresnel = std::pow(
                                    1.0f - std::clamp(normalViewDot, 0.0f, 1.0f),
                                    5.0f);
                                const float environmentWeight =
                                    (0.04f + 0.16f * fresnel) * shadowVisibility;
                                for (uint32_t channel = 0; channel < 3; ++channel) {
                                    const float mask = static_cast<float>(
                                        (specTexel >> (channel * 8u)) & 0xFFu)
                                        / 255.0f;
                                    const float environment = static_cast<float>(
                                        (environmentTexel >> (channel * 8u)) & 0xFFu)
                                        / 255.0f;
                                    color[channel] += std::clamp(
                                        environment * mask * environmentWeight,
                                        0.0f, 1.0f);
                                }
                            }
                        }
                    }
                }
                // The source lit world techniques sample the primary L8 map
                // and the top/bottom halves of the secondary A8R8G8B8 atlas
                // at the perspective-correct fragment coordinates. Keep the
                // recovered lm_* equation in the scalar oracle rather than
                // multiplying two generic lightmap floors.
                const float lightmapU =
                    wa * a.lightmapCoord[0] + wb * b.lightmapCoord[0]
                    + wc * c.lightmapCoord[0];
                const float lightmapV =
                    wa * a.lightmapCoord[1] + wb * b.lightmapCoord[1]
                    + wc * c.lightmapCoord[1];
                const bool dualLightmap = usePrimaryLightmap
                    && useSecondaryLightmap;
                float primaryIntensity = 0.0f;
                float secondaryRgb[3] = {0.0f, 0.0f, 0.0f};
                float staticLight[3] = {1.0f, 1.0f, 1.0f};
                float lightmapSunDot = 0.0f;
                if (usePrimaryLightmap && useSecondaryLightmap) {
                    const uint32_t primaryTexel = SampleSoftwareTextureHandle(
                        g_softwareLightmapPrimary, lightmapU, lightmapV, 1u);
                    const uint32_t secondaryTop = SampleSoftwareTextureHandle(
                        g_softwareLightmapSecondary, lightmapU,
                        lightmapV * 0.5f, 2u);
                    const uint32_t secondaryBottom = SampleSoftwareTextureHandle(
                        g_softwareLightmapSecondary, lightmapU,
                        lightmapV * 0.5f + 0.5f, 2u);
                    primaryIntensity = static_cast<float>(
                        primaryTexel & 0xFFu) / 255.0f;
                    const float secondaryTopAlpha = static_cast<float>(
                        (secondaryTop >> 24u) & 0xFFu) / 255.0f;
                    const float secondaryBottomAlpha = static_cast<float>(
                        (secondaryBottom >> 24u) & 0xFFu) / 255.0f;
                    const float secondaryX = secondaryTopAlpha * 4.07999992f
                        - 2.07999992f;
                    const float secondaryY = secondaryBottomAlpha * 4.06451607f
                        - 2.06451607f;
                    const float secondaryInverseLength = 1.0f / std::sqrt(
                        std::max(0.000001f, secondaryX * secondaryX
                            + secondaryY * secondaryY + 1.0f));
                    const float normalInverseLength = 1.0f / std::sqrt(
                        std::max(0.000001f,
                            sourceLightmapNormalXY[0] * sourceLightmapNormalXY[0]
                                + sourceLightmapNormalXY[1]
                                    * sourceLightmapNormalXY[1] + 1.0f));
                    const float alignment = std::clamp(
                        (secondaryX * sourceLightmapNormalXY[0]
                            + secondaryY * sourceLightmapNormalXY[1] + 1.0f)
                            * secondaryInverseLength * normalInverseLength,
                        0.0f, 1.0f);
                    secondaryRgb[0] = static_cast<float>(secondaryTop & 0xFFu)
                        / 255.0f * normalInverseLength
                        + static_cast<float>(secondaryBottom & 0xFFu)
                            / 255.0f * alignment;
                    secondaryRgb[1] = static_cast<float>(
                        (secondaryTop >> 8u) & 0xFFu) / 255.0f
                        * normalInverseLength
                        + static_cast<float>((secondaryBottom >> 8u) & 0xFFu)
                            / 255.0f * alignment;
                    secondaryRgb[2] = static_cast<float>(
                        (secondaryTop >> 16u) & 0xFFu) / 255.0f
                        * normalInverseLength
                        + static_cast<float>((secondaryBottom >> 16u) & 0xFFu)
                            / 255.0f * alignment;
                    const float normalLengthSquared = sourceLightmapNormal[0]
                        * sourceLightmapNormal[0]
                        + sourceLightmapNormal[1] * sourceLightmapNormal[1]
                        + sourceLightmapNormal[2] * sourceLightmapNormal[2];
                    const float sunLengthSquared =
                        g_frameLighting.sunDirection[0]
                            * g_frameLighting.sunDirection[0]
                        + g_frameLighting.sunDirection[1]
                            * g_frameLighting.sunDirection[1]
                        + g_frameLighting.sunDirection[2]
                            * g_frameLighting.sunDirection[2];
                    if ((g_frameLighting.flags & GFX_FRAME_LIGHTING_VALID) != 0
                        && std::isfinite(normalLengthSquared)
                        && normalLengthSquared > 0.000001f
                        && std::isfinite(sunLengthSquared)
                        && sunLengthSquared > 0.000001f) {
                        const float normalScale = 1.0f
                            / std::sqrt(normalLengthSquared);
                        const float sunScale = 1.0f / std::sqrt(sunLengthSquared);
                        lightmapSunDot = std::clamp(
                            (sourceLightmapNormal[0]
                                * g_frameLighting.sunDirection[0]
                                + sourceLightmapNormal[1]
                                    * g_frameLighting.sunDirection[1]
                                + sourceLightmapNormal[2]
                                    * g_frameLighting.sunDirection[2])
                                * normalScale * sunScale,
                            0.0f, 1.0f);
                    }
                    for (uint32_t channel = 0; channel < 3u; ++channel) {
                        staticLight[channel] = std::clamp(
                            secondaryRgb[channel] + primaryIntensity
                                * g_frameLighting.sunColor[channel] * lightmapSunDot
                                * shadowVisibility,
                            0.0f, 1.35f);
                        color[channel] *= staticLight[channel];
                    }
                    RecordFrameLightmapTelemetry(primaryIntensity,
                                                 secondaryRgb, staticLight);
                } else {
                    // Preserve the authored single-map variants for utility
                    // materials whose source state does not bind the full
                    // dual-lightmap pair.
                    if (usePrimaryLightmap) {
                        const uint32_t primaryTexel = SampleSoftwareTextureHandle(
                            g_softwareLightmapPrimary, lightmapU, lightmapV, 1u);
                        primaryIntensity = static_cast<float>(
                            primaryTexel & 0xFFu) / 255.0f;
                        const float normalLengthSquared =
                            sourceLightmapNormal[0] * sourceLightmapNormal[0]
                            + sourceLightmapNormal[1] * sourceLightmapNormal[1]
                            + sourceLightmapNormal[2] * sourceLightmapNormal[2];
                        const float sunLengthSquared =
                            g_frameLighting.sunDirection[0]
                                * g_frameLighting.sunDirection[0]
                            + g_frameLighting.sunDirection[1]
                                * g_frameLighting.sunDirection[1]
                            + g_frameLighting.sunDirection[2]
                                * g_frameLighting.sunDirection[2];
                        if ((g_frameLighting.flags & GFX_FRAME_LIGHTING_VALID) != 0
                            && std::isfinite(normalLengthSquared)
                            && normalLengthSquared > 0.000001f
                            && std::isfinite(sunLengthSquared)
                            && sunLengthSquared > 0.000001f) {
                            lightmapSunDot = std::clamp(
                                (sourceLightmapNormal[0]
                                    * g_frameLighting.sunDirection[0]
                                    + sourceLightmapNormal[1]
                                        * g_frameLighting.sunDirection[1]
                                    + sourceLightmapNormal[2]
                                        * g_frameLighting.sunDirection[2])
                                    / std::sqrt(normalLengthSquared
                                        * sunLengthSquared),
                                0.0f, 1.0f);
                        }
                        for (uint32_t channel = 0; channel < 3u; ++channel) {
                            staticLight[channel] = std::clamp(
                                primaryIntensity
                                    * g_frameLighting.sunColor[channel]
                                    * lightmapSunDot * shadowVisibility,
                                0.0f, 1.35f);
                            color[channel] *= staticLight[channel];
                        }
                    }
                    if (useSecondaryLightmap) {
                        const uint32_t secondaryTop = SampleSoftwareTextureHandle(
                            g_softwareLightmapSecondary, lightmapU,
                            lightmapV * 0.5f, 2u);
                        const uint32_t secondaryBottom = SampleSoftwareTextureHandle(
                            g_softwareLightmapSecondary, lightmapU,
                            lightmapV * 0.5f + 0.5f, 2u);
                        const float secondaryX = static_cast<float>(
                            (secondaryTop >> 24u) & 0xFFu) / 255.0f
                            * 4.07999992f - 2.07999992f;
                        const float secondaryY = static_cast<float>(
                            (secondaryBottom >> 24u) & 0xFFu) / 255.0f
                            * 4.06451607f - 2.06451607f;
                        const float secondaryInverseLength = 1.0f / std::sqrt(
                            std::max(0.000001f, secondaryX * secondaryX
                                + secondaryY * secondaryY + 1.0f));
                        const float normalInverseLength = 1.0f / std::sqrt(
                            std::max(0.000001f,
                                sourceLightmapNormalXY[0]
                                    * sourceLightmapNormalXY[0]
                                + sourceLightmapNormalXY[1]
                                    * sourceLightmapNormalXY[1] + 1.0f));
                        const float alignment = std::clamp(
                            (secondaryX * sourceLightmapNormalXY[0]
                                + secondaryY * sourceLightmapNormalXY[1] + 1.0f)
                                * secondaryInverseLength * normalInverseLength,
                            0.0f, 1.0f);
                        secondaryRgb[0] = static_cast<float>(secondaryTop & 0xFFu)
                            / 255.0f * normalInverseLength
                            + static_cast<float>(secondaryBottom & 0xFFu)
                                / 255.0f * alignment;
                        secondaryRgb[1] = static_cast<float>(
                            (secondaryTop >> 8u) & 0xFFu) / 255.0f
                            * normalInverseLength
                            + static_cast<float>((secondaryBottom >> 8u) & 0xFFu)
                                / 255.0f * alignment;
                        secondaryRgb[2] = static_cast<float>(
                            (secondaryTop >> 16u) & 0xFFu) / 255.0f
                            * normalInverseLength
                            + static_cast<float>((secondaryBottom >> 16u) & 0xFFu)
                                / 255.0f * alignment;
                        for (uint32_t channel = 0; channel < 3u; ++channel) {
                            staticLight[channel] = std::clamp(
                                secondaryRgb[channel], 0.0f, 1.35f);
                            color[channel] *= staticLight[channel];
                        }
                    }
                }
                RecordSoftwareLightingProbe(
                    x, y, z, materialColor, dynamicFactors,
                    primaryIntensity, secondaryRgb, staticLight, color,
                    lightmapSunDot, shadowVisibility, localLightGridSunWeight,
                    dualLightmap, semanticPass);
                SoftwareStoreBlockColor(
                    x, y, color, std::clamp(color[3], 0.0f, 1.0f), z,
                    rasterStep);
            }
        }
    }
}

void SetSurfaceMode(GfxSurfaceMode mode) {
    const GfxSurfaceMode previous = g_software ? g_softwareSurfaceMode
                                               : g_nativeSurfaceMode;
    if (previous != mode)
        ++g_frameStateChanges;
    if (g_software) {
        SoftwareSetSurfaceMode(mode);
        return;
    }
    g_nativeSurfaceMode = mode;
    // Changing the program invalidates the retained material/attribute cache,
    // including a transition that happens to keep the same surface enum.
    EndNativeStaticPipeline();
    // A lit world batch may have left the native source shader bound.  The
    // menu/HUD paths use VitaGL fixed-function arrays, so make the boundary
    // explicit instead of relying on the previous batch to restore it.
    glUseProgram(0);
    // Raw VITL state can disable color writes, enable stencil, or select a
    // non-additive blend equation.  Reset those pieces at every generic
    // surface boundary so a depth/prepass material cannot leak into the next
    // world, HUD, menu, sky, or viewmodel submission.
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_STENCIL_TEST);
    glDepthFunc(GL_LEQUAL);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    switch (mode) {
    case GfxSurfaceMode::Opaque:
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        if (g_vitaGlFogEnabled)
            glEnable(GL_FOG);
        break;
    case GfxSurfaceMode::AlphaTest:
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.5f);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        if (g_vitaGlFogEnabled)
            glEnable(GL_FOG);
        break;
    case GfxSurfaceMode::AlphaTestBlend:
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.5f);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        // Source b-family materials discard cutout texels, then blend the
        // surviving texels without writing depth.
        glDepthMask(GL_FALSE);
        if (g_vitaGlFogEnabled)
            glEnable(GL_FOG);
        break;
    case GfxSurfaceMode::AlphaBlend:
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_ALPHA_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        if (g_vitaGlFogEnabled)
            glEnable(GL_FOG);
        break;
    case GfxSurfaceMode::Additive:
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_ALPHA_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glDepthMask(GL_FALSE);
        if (g_vitaGlFogEnabled)
            glEnable(GL_FOG);
        break;
    case GfxSurfaceMode::Sky:
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_BLEND);
        glDepthMask(GL_FALSE);
        // `wc_sky` is the source infinite-distance cubemap pass.  Retail
        // sky pixels are not distance-fogged; enabling GL_FOG here applies
        // the far-plane fog factor to every texel and over-fogs the clouds.
        glDisable(GL_FOG);
        break;
    case GfxSurfaceMode::Overlay:
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_ALPHA_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glDisable(GL_FOG);
        break;
    }
}

void SetAlphaTestFunc(GfxAlphaTestFunc function) {
    const GfxAlphaTestFunc previous = g_software ? g_softwareAlphaTestFunc
                                                 : g_nativeAlphaTestFunc;
    if (previous != function)
        ++g_frameStateChanges;
    if (g_software) {
        SoftwareSetAlphaTestFunc(function);
        return;
    }
    g_nativeAlphaTestFunc = function;
    switch (function) {
    case GfxAlphaTestFunc::Greater0:
        glAlphaFunc(GL_GREATER, 0.0f);
        break;
    case GfxAlphaTestFunc::Less128:
        glAlphaFunc(GL_LESS, 0.5f);
        break;
    case GfxAlphaTestFunc::GreaterEqual128:
    default:
        glAlphaFunc(GL_GEQUAL, 0.5f);
        break;
    }
}

void SetCullMode(GfxCullMode mode) {
    const GfxCullMode previous = g_software ? g_softwareCullMode
                                            : g_nativeCullMode;
    if (previous != mode)
        ++g_frameStateChanges;
    if (g_software) {
        SoftwareSetCullMode(mode);
        return;
    }
    g_nativeCullMode = mode;
    // KisakCOD maps GFXS0_CULL_BACK to D3DCULL_CCW and
    // GFXS0_CULL_FRONT to D3DCULL_CW, i.e. clockwise is the source front
    // face.  Preserve that convention explicitly when entering the GL
    // backend so the authored cull bits have the same meaning on Vita.
    glFrontFace(GL_CW);
    switch (mode) {
    case GfxCullMode::None:
        glDisable(GL_CULL_FACE);
        break;
    case GfxCullMode::Back:
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        break;
    case GfxCullMode::Front:
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        break;
    }
}

GLenum NativeSourceBlendFactor(uint32_t encoded) {
    switch (encoded & 0x0Fu) {
    case 0u:
    case 1u:
        return GL_ZERO;
    case 2u:
        return GL_ONE;
    case 3u:
        return GL_SRC_COLOR;
    case 4u:
        return GL_ONE_MINUS_SRC_COLOR;
    case 5u:
        return GL_SRC_ALPHA;
    case 6u:
        return GL_ONE_MINUS_SRC_ALPHA;
    case 7u:
        return GL_DST_ALPHA;
    case 8u:
        return GL_ONE_MINUS_DST_ALPHA;
    case 9u:
        return GL_DST_COLOR;
    case 10u:
        return GL_ONE_MINUS_DST_COLOR;
    case 11u:
        return GL_SRC_ALPHA_SATURATE;
    default:
        // BlendFactor is not emitted by the recovered COD4 MP material maps;
        // keep an unknown future code visible rather than selecting a
        // platform-specific factor that could turn a surface black.
        return GL_ONE;
    }
}

GLenum NativeSourceBlendEquation(uint32_t encoded) {
    switch (encoded & 0x07u) {
    case 1u:
        return GL_FUNC_ADD;
    case 2u:
        return GL_FUNC_SUBTRACT;
    case 3u:
        return GL_FUNC_REVERSE_SUBTRACT;
    case 4u:
        return GL_MIN;
    case 5u:
        return GL_MAX;
    case 0u:
    default:
        return GL_FUNC_ADD;
    }
}

GLenum NativeSourceCompareFunction(uint32_t encoded) {
    switch (encoded & 0x07u) {
    case 0u: return GL_NEVER;
    case 1u: return GL_LESS;
    case 2u: return GL_EQUAL;
    case 3u: return GL_LEQUAL;
    case 4u: return GL_GREATER;
    case 5u: return GL_NOTEQUAL;
    case 6u: return GL_GEQUAL;
    case 7u: return GL_ALWAYS;
    default: return GL_ALWAYS;
    }
}

GLenum NativeSourceStencilOperation(uint32_t encoded) {
    switch (encoded & 0x07u) {
    case 1u: return GL_KEEP;
    case 2u: return GL_ZERO;
    case 3u: return GL_REPLACE;
    case 4u: return GL_INCR;
    case 5u: return GL_DECR;
    case 6u: return GL_INVERT;
    case 7u: return GL_INCR_WRAP;
    case 0u:
    default: return GL_DECR_WRAP;
    }
}

void ApplyNativeSourceState(const GfxMaterialPassState& state) {
    if (g_software || !SourceMaterialStateApplies(state))
        return;
    const uint32_t bits0 = EffectiveSourceStateBits0(state);
    const uint32_t bits1 = (state.techniqueFlags0 & GFX_MATERIAL_PASS_SOURCE_UI)
        ? (state.sourceStateBits1 | kSourceStateDepthTestDisable) & ~kSourceStateDepthWrite
        : state.sourceStateBits1;

    // The next draw may be fixed-function or the source lit shader. Applying
    // the state here keeps both paths behind one authored material boundary.
    glUseProgram(0);
    if ((bits0 & kSourceStateAlphaTestDisable) == 0u) {
        glEnable(GL_ALPHA_TEST);
        switch (bits0 & kSourceStateAlphaTestMask) {
        case 0x1000u:
            glAlphaFunc(GL_GREATER, 0.0f);
            break;
        case 0x2000u:
            glAlphaFunc(GL_LESS, 0.5f);
            break;
        case 0x3000u:
        default:
            glAlphaFunc(GL_GEQUAL, 0.5f);
            break;
        }
    } else {
        glDisable(GL_ALPHA_TEST);
    }

    if ((bits0 & kSourceStateBlendRgbMask) != 0u) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(
            NativeSourceBlendFactor(bits0),
            NativeSourceBlendFactor(bits0 >> 4u),
            NativeSourceBlendFactor(bits0 >> 16u),
            NativeSourceBlendFactor(bits0 >> 20u));
        glBlendEquationSeparate(
            NativeSourceBlendEquation(bits0 >> 8u),
            NativeSourceBlendEquation(bits0 >> 24u));
    } else {
        glDisable(GL_BLEND);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    }

    if ((bits1 & kSourceStateDepthTestDisable) != 0u)
        glDisable(GL_DEPTH_TEST);
    else {
        glEnable(GL_DEPTH_TEST);
        switch (bits1 & kSourceStateDepthFuncMask) {
        case 0x00000000u: glDepthFunc(GL_ALWAYS); break;
        case 0x00000004u: glDepthFunc(GL_LESS); break;
        case 0x00000008u: glDepthFunc(GL_EQUAL); break;
        case 0x0000000Cu:
        default: glDepthFunc(GL_LEQUAL); break;
        }
    }
    glDepthMask((bits1 & kSourceStateDepthWrite) != 0u ? GL_TRUE : GL_FALSE);

    const uint32_t cull = bits0 & 0xC000u;
    if (cull == 0x4000u)
        SetCullMode(GfxCullMode::None);
    else if (cull == 0x8000u)
        SetCullMode(GfxCullMode::Back);
    else if (cull == 0xC000u)
        SetCullMode(GfxCullMode::Front);

    glColorMask((bits0 & 0x08000000u) != 0u ? GL_TRUE : GL_FALSE,
                (bits0 & 0x08000000u) != 0u ? GL_TRUE : GL_FALSE,
                (bits0 & 0x08000000u) != 0u ? GL_TRUE : GL_FALSE,
                (bits0 & 0x10000000u) != 0u ? GL_TRUE : GL_FALSE);

    const uint32_t stencilMode = bits1 & 0xC0u;
    if (stencilMode == 0u) {
        glDisable(GL_STENCIL_TEST);
        return;
    }
    glEnable(GL_STENCIL_TEST);
    // Stencil ref/masks are command-buffer constants in the desktop path and
    // are not part of the pointer-free VITL material record. Preserve the
    // authored enable/function/operation fields and use the neutral source
    // ref/mask until a future VITS command descriptor carries those values.
    glStencilMaskSeparate(GL_FRONT, 0xFFFFFFFFu);
    glStencilMaskSeparate(GL_BACK, 0xFFFFFFFFu);
    if ((stencilMode & 0x40u) != 0u) {
        glStencilFuncSeparate(
            GL_FRONT, NativeSourceCompareFunction(bits1 >> 17u), 0, 0xFFFFFFFFu);
        glStencilOpSeparate(
            GL_FRONT, NativeSourceStencilOperation(bits1 >> 11u),
            NativeSourceStencilOperation(bits1 >> 14u),
            NativeSourceStencilOperation(bits1 >> 8u));
    }
    if ((stencilMode & 0x80u) != 0u) {
        glStencilFuncSeparate(
            GL_BACK, NativeSourceCompareFunction(bits1 >> 29u), 0, 0xFFFFFFFFu);
        glStencilOpSeparate(
            GL_BACK, NativeSourceStencilOperation(bits1 >> 23u),
            NativeSourceStencilOperation(bits1 >> 26u),
            NativeSourceStencilOperation(bits1 >> 20u));
    }
}

void SetFog(bool enabled, float startDistance, float density, const float color[3]) {
    if (g_software) {
        SoftwareSetFog(enabled, startDistance, density, color);
        return;
    }
    g_vitaGlFogEnabled = enabled && std::isfinite(density) && density > 0.0f;
    if (!g_vitaGlFogEnabled) {
        glDisable(GL_FOG);
        return;
    }
    const GLfloat fogColor[4] = {
        color && std::isfinite(color[0]) ? std::clamp(color[0], 0.0f, 1.0f) : 0.0f,
        color && std::isfinite(color[1]) ? std::clamp(color[1], 0.0f, 1.0f) : 0.0f,
        color && std::isfinite(color[2]) ? std::clamp(color[2], 0.0f, 1.0f) : 0.0f,
        1.0f,
    };
    glFogi(GL_FOG_MODE, GL_EXP);
    glFogf(GL_FOG_DENSITY, density);
    // GL_EXP ignores start/end, but setting them keeps the state faithful if
    // a VitaGL implementation promotes the mode to a start-aware variant.
    glFogf(GL_FOG_START, std::max(0.0f, startDistance));
    glFogf(GL_FOG_END, std::max(1.0f, startDistance + 1.0f));
    glFogfv(GL_FOG_COLOR, fogColor);
    glEnable(GL_FOG);
}

void SetFrameLighting(const GfxFrameLightingState* state) {
    // Evaluate BEFORE sanitization/clamping. Invalid source inputs must never
    // be mistaken for black lights by the optimization.
    g_rawLocalLightLoops = !state || !(state->flags & GFX_FRAME_LIGHTING_VALID)
        || GfxVitaNeedsLocalLightLoops(*state);
    if (!state) {
        g_frameLighting = {};
        SelectNativeLightShader();
        return;
    }
    g_frameLighting = *state;
    for (float& value : g_frameLighting.cameraPosition)
        value = std::isfinite(value) ? value : 0.0f;
    for (float& value : g_frameLighting.cameraForward)
        value = std::isfinite(value) ? value : 0.0f;
    for (float& value : g_frameLighting.cameraRight)
        value = std::isfinite(value) ? value : 0.0f;
    for (float& value : g_frameLighting.cameraUp)
        value = std::isfinite(value) ? value : 0.0f;
    g_frameLighting.cameraHorizontalScale = std::max(
        std::isfinite(state->cameraHorizontalScale)
            ? state->cameraHorizontalScale : 1.0f, 0.0001f);
    g_frameLighting.cameraVerticalScale = std::max(
        std::isfinite(state->cameraVerticalScale)
            ? state->cameraVerticalScale : 1.0f, 0.0001f);
    g_frameLighting.ambientScale = std::isfinite(state->ambientScale)
        ? state->ambientScale : 0.0f;
    g_frameLighting.diffuseFraction = std::isfinite(state->diffuseFraction)
        ? state->diffuseFraction : 0.0f;
    g_frameLighting.sunLight = std::isfinite(state->sunLight)
        ? state->sunLight : 0.0f;
    g_frameLighting.specularColorScale = std::clamp(
        std::isfinite(state->specularColorScale)
            ? state->specularColorScale : 1.0f,
        0.0f, 100.0f);
    g_frameLighting.lightGridSunWeight = std::clamp(
        std::isfinite(state->lightGridSunWeight) ? state->lightGridSunWeight : 1.0f,
        0.0f, 1.0f);
    g_frameLighting.indoorFactor = std::clamp(
        std::isfinite(state->indoorFactor) ? state->indoorFactor : 1.0f,
        0.20f, 1.0f);
    g_frameLighting.primaryLightCount = std::min(
        state->primaryLightCount, GFX_MAX_PRIMARY_LIGHTS);
    g_frameLighting.sunPrimaryLightIndex = g_frameLighting.primaryLightCount != 0
        ? std::min(state->sunPrimaryLightIndex,
                   g_frameLighting.primaryLightCount - 1u) : 0u;
    for (uint32_t index = 0; index < g_frameLighting.primaryLightCount; ++index) {
        GfxPrimaryLightDescriptor& light = g_frameLighting.primaryLights[index];
        const GfxPrimaryLightDescriptor& source = state->primaryLights[index];
        light = source;
        for (float& value : light.color)
            value = std::isfinite(value) ? value : 0.0f;
        for (float& value : light.direction)
            value = std::isfinite(value) ? value : 0.0f;
        for (float& value : light.origin)
            value = std::isfinite(value) ? value : 0.0f;
        light.radius = std::max(std::isfinite(light.radius) ? light.radius : 0.0f,
                                0.0f);
        light.cosHalfFovOuter = std::isfinite(light.cosHalfFovOuter)
            ? light.cosHalfFovOuter : 0.0f;
        light.cosHalfFovInner = std::isfinite(light.cosHalfFovInner)
            ? light.cosHalfFovInner : 0.0f;
        light.cosHalfFovExpanded = std::isfinite(light.cosHalfFovExpanded)
            ? light.cosHalfFovExpanded : 0.0f;
        light.rotationLimit = std::isfinite(light.rotationLimit)
            ? light.rotationLimit : 0.0f;
        light.translationLimit = std::isfinite(light.translationLimit)
            ? light.translationLimit : 0.0f;
        light.shadowVisibility = std::clamp(
            std::isfinite(light.shadowVisibility)
                ? light.shadowVisibility : 1.0f,
            0.0f, 1.0f);
    }
    g_frameLighting.dynamicLightCount = std::min(
        state->dynamicLightCount, GFX_MAX_DYNAMIC_LIGHTS);
    for (uint32_t index = 0; index < g_frameLighting.dynamicLightCount;
         ++index) {
        GfxPrimaryLightDescriptor& light =
            g_frameLighting.dynamicLights[index];
        const GfxPrimaryLightDescriptor& source = state->dynamicLights[index];
        light = source;
        for (float& value : light.color)
            value = std::isfinite(value) ? value : 0.0f;
        for (float& value : light.direction)
            value = std::isfinite(value) ? value : 0.0f;
        for (float& value : light.origin)
            value = std::isfinite(value) ? value : 0.0f;
        light.radius = std::max(
            std::isfinite(light.radius) ? light.radius : 0.0f, 0.0f);
        light.cosHalfFovOuter = std::isfinite(light.cosHalfFovOuter)
            ? light.cosHalfFovOuter : 0.0f;
        light.cosHalfFovInner = std::isfinite(light.cosHalfFovInner)
            ? light.cosHalfFovInner : 0.0f;
        light.cosHalfFovExpanded = std::isfinite(light.cosHalfFovExpanded)
            ? light.cosHalfFovExpanded : 0.0f;
        light.shadowVisibility = std::clamp(
            std::isfinite(light.shadowVisibility)
                ? light.shadowVisibility : 1.0f,
            0.0f, 1.0f);
    }
    SelectNativeLightShader();
    for (float& value : g_frameLighting.lightGridColor)
        value = std::clamp(std::isfinite(value) ? value : 1.0f, 0.0f, 2.0f);
    // Frame lighting and film state are authored together by the map
    // environment. Keep this synchronization at the primary frame-state
    // boundary as well as ApplyViewEffects so older callers that only submit
    // GfxFrameLightingState cannot silently disable the full-frame film pass.
    g_viewEffects.flags = state->flags
        & (GFX_FRAME_LIGHTING_HAS_FILM | GFX_FRAME_LIGHTING_HAS_GLOW);
    std::memcpy(g_viewEffects.filmLightTint, state->filmLightTint,
                sizeof(g_viewEffects.filmLightTint));
    std::memcpy(g_viewEffects.filmDarkTint, state->filmDarkTint,
                sizeof(g_viewEffects.filmDarkTint));
    g_viewEffects.contrast = std::isfinite(state->filmContrast)
        ? state->filmContrast : 1.0f;
    g_viewEffects.brightness = std::isfinite(state->filmBrightness)
        ? state->filmBrightness : 0.0f;
    g_viewEffects.desaturation = std::clamp(
        std::isfinite(state->filmDesaturation) ? state->filmDesaturation : 0.0f,
        0.0f, 1.0f);
    g_viewEffects.invert = std::clamp(
        std::isfinite(state->filmInvert) ? state->filmInvert : 0.0f,
        0.0f, 1.0f);
    g_viewEffects.exposure = std::isfinite(state->exposure)
        ? state->exposure : 1.0f;
    g_viewEffects.gamma = std::isfinite(state->gamma) ? state->gamma : 0.8f;
    g_viewEffects.glowEnabled = std::isfinite(state->glowEnabled)
        ? state->glowEnabled : 0.0f;
    g_viewEffects.glowRadius0 = std::clamp(
        std::isfinite(state->glowRadius0) ? state->glowRadius0 : 0.0f,
        0.0f, 128.0f);
    g_viewEffects.glowRadius1 = std::clamp(
        std::isfinite(state->glowRadius1) ? state->glowRadius1 : 0.0f,
        0.0f, 128.0f);
    g_viewEffects.glowBloomCutoff = std::clamp(
        std::isfinite(state->glowBloomCutoff) ? state->glowBloomCutoff : 0.0f,
        0.0f, 1.0f);
    g_viewEffects.glowBloomDesaturation = std::clamp(
        std::isfinite(state->glowBloomDesaturation)
            ? state->glowBloomDesaturation : 0.0f,
        0.0f, 1.0f);
    g_viewEffects.glowBloomIntensity0 = std::clamp(
        std::isfinite(state->glowBloomIntensity0)
            ? state->glowBloomIntensity0 : 0.0f,
        0.0f, 8.0f);
    g_viewEffects.glowBloomIntensity1 = std::clamp(
        std::isfinite(state->glowBloomIntensity1)
            ? state->glowBloomIntensity1 : 0.0f,
        0.0f, 8.0f);
    g_viewEffects.glowSkyBleedIntensity0 = std::clamp(
        std::isfinite(state->glowSkyBleedIntensity0)
            ? state->glowSkyBleedIntensity0 : 0.0f,
        0.0f, 8.0f);
    g_viewEffects.glowSkyBleedIntensity1 = std::clamp(
        std::isfinite(state->glowSkyBleedIntensity1)
            ? state->glowSkyBleedIntensity1 : 0.0f,
        0.0f, 8.0f);
}

void SetMaterialPass(const GfxMaterialPassState* state) {
    // A source material descriptor owns every shader texture/uniform used by
    // the immutable-world path.  Close any live static pipeline before the
    // descriptor changes so a later non-static submission cannot inherit its
    // vertex arrays, and advance the revision used by consecutive static
    // ranges to prove that their cached bindings are still current.
    if (!g_software)
        EndNativeStaticPipeline();
    if (g_materialPassRevision == UINT64_MAX)
        g_materialPassRevision = 1u;
    else
        ++g_materialPassRevision;
    if (!state) {
        g_materialPass = {};
        return;
    }
    g_materialPass = *state;
    ++g_frameMaterialPasses;
    ++g_frameStateChanges;
    if (state->normalTexture.value != 0)
        ++g_frameNormalPasses;
    if (state->specularTexture.value != 0)
        ++g_frameSpecularPasses;
    if ((state->lightmapSamplerFlags & 0x02u) != 0
        && state->lightmapPrimaryTexture.value != 0)
        ++g_framePrimaryLightmapPasses;
    if ((state->lightmapSamplerFlags & 0x04u) != 0
        && state->lightmapSecondaryTexture.value != 0)
        ++g_frameSecondaryLightmapPasses;
    if (g_software) {
        SoftwareSetSamplerState(0u, state->colorSamplerState);
        SoftwareSetSamplerState(1u, state->normalSamplerState);
        SoftwareSetSamplerState(2u, state->specularSamplerState);
        if (SourceMaterialStateApplies(*state)) {
            // SetSurfaceMode establishes the compatibility default before
            // this descriptor arrives. Replace only that default when VITL
            // carries the source state, preserving the explicit overlay
            // compositor for first-person/HUD draws.
            g_softwareAlphaTestFunc = SourceStateAlphaTestFunc(*state);
            g_softwareDepthTest = SourceStateUsesDepthTest(*state);
            g_softwareDepthWrite = SourceStateWritesDepth(*state);
        }
    } else {
        ApplyNativeSourceState(*state);
    }
}

void SetShadowPass(const GfxShadowPassState* state) {
    if (!state) {
        g_shadowPass = {};
        return;
    }
    g_shadowPass = *state;
    g_shadowPass.lightGridSunWeight = std::clamp(
        std::isfinite(state->lightGridSunWeight)
            ? state->lightGridSunWeight : 1.0f,
        0.0f, 1.0f);
    g_shadowPass.indoorFactor = std::clamp(
        std::isfinite(state->indoorFactor) ? state->indoorFactor : 1.0f,
        0.20f, 1.0f);
    g_shadowPass.shadowStrength = std::clamp(
        std::isfinite(state->shadowStrength) ? state->shadowStrength : 1.0f,
        0.0f, 1.0f);
    g_shadowPass.bias = std::isfinite(state->bias) ? state->bias : 0.0f;
    for (float& value : g_shadowPass.lightGridColor)
        value = std::clamp(std::isfinite(value) ? value : 1.0f, 0.0f, 2.0f);
    ++g_frameShadowPasses;
}

void SetEffectBatch(const GfxEffectBatchState* state) {
    if (!state) {
        g_effectBatch = {};
        return;
    }
    g_effectBatch = *state;
    for (float& value : g_effectBatch.color)
        value = std::clamp(std::isfinite(value) ? value : 1.0f, 0.0f, 1.0f);
    for (float& value : g_effectBatch.uvScaleBias)
        value = std::isfinite(value) ? value : 0.0f;
    g_effectBatch.placementCount = std::min(
        state->placementCount, GFX_MAX_EFFECT_PLACEMENTS);
    for (uint32_t index = 0; index < g_effectBatch.placementCount; ++index) {
        GfxEffectPlacementDescriptor& placement = g_effectBatch.placements[index];
        for (float& value : placement.origin)
            value = std::isfinite(value) ? value : 0.0f;
        for (float& value : placement.angles)
            value = std::isfinite(value) ? value : 0.0f;
        placement.delay = std::isfinite(placement.delay)
            ? placement.delay : 0.0f;
    }
    ++g_frameEffectBatches;
    if (g_software)
        SoftwareSetSamplerState(3u, state->colorSamplerState);
}

void ApplyViewEffects(const GfxViewEffectState* state) {
    if (!state) {
        g_viewEffects = {};
        return;
    }
    g_viewEffects = *state;
    g_viewEffects.contrast = std::isfinite(state->contrast)
        ? state->contrast : 1.0f;
    g_viewEffects.brightness = std::isfinite(state->brightness)
        ? state->brightness : 0.0f;
    g_viewEffects.desaturation = std::clamp(
        std::isfinite(state->desaturation) ? state->desaturation : 0.0f,
        0.0f, 1.0f);
    g_viewEffects.invert = std::clamp(
        std::isfinite(state->invert) ? state->invert : 0.0f, 0.0f, 1.0f);
    g_viewEffects.exposure = std::isfinite(state->exposure)
        ? state->exposure : 1.0f;
    g_viewEffects.gamma = std::isfinite(state->gamma) ? state->gamma : 0.8f;
    g_viewEffects.glowEnabled = std::isfinite(state->glowEnabled)
        ? state->glowEnabled : 0.0f;
    g_viewEffects.glowRadius0 = std::clamp(
        std::isfinite(state->glowRadius0) ? state->glowRadius0 : 0.0f,
        0.0f, 128.0f);
    g_viewEffects.glowRadius1 = std::clamp(
        std::isfinite(state->glowRadius1) ? state->glowRadius1 : 0.0f,
        0.0f, 128.0f);
    g_viewEffects.glowBloomCutoff = std::clamp(
        std::isfinite(state->glowBloomCutoff) ? state->glowBloomCutoff : 0.0f,
        0.0f, 1.0f);
    g_viewEffects.glowBloomDesaturation = std::clamp(
        std::isfinite(state->glowBloomDesaturation)
            ? state->glowBloomDesaturation : 0.0f,
        0.0f, 1.0f);
    g_viewEffects.glowBloomIntensity0 = std::clamp(
        std::isfinite(state->glowBloomIntensity0)
            ? state->glowBloomIntensity0 : 0.0f,
        0.0f, 8.0f);
    g_viewEffects.glowBloomIntensity1 = std::clamp(
        std::isfinite(state->glowBloomIntensity1)
            ? state->glowBloomIntensity1 : 0.0f,
        0.0f, 8.0f);
    g_viewEffects.glowSkyBleedIntensity0 = std::clamp(
        std::isfinite(state->glowSkyBleedIntensity0)
            ? state->glowSkyBleedIntensity0 : 0.0f,
        0.0f, 8.0f);
    g_viewEffects.glowSkyBleedIntensity1 = std::clamp(
        std::isfinite(state->glowSkyBleedIntensity1)
            ? state->glowSkyBleedIntensity1 : 0.0f,
        0.0f, 8.0f);
    static uint32_t viewEffectLogCount = 0;
    if (viewEffectLogCount++ < 8u) {
        sceClibPrintf(
            "[kisak-vita] view effects flags=0x%08x contrast=%.6f brightness=%.6f desaturation=%.6f invert=%.6f light=%.4f,%.4f,%.4f dark=%.4f,%.4f,%.4f\n",
            static_cast<unsigned>(g_viewEffects.flags),
            g_viewEffects.contrast, g_viewEffects.brightness,
            g_viewEffects.desaturation, g_viewEffects.invert,
            g_viewEffects.filmLightTint[0], g_viewEffects.filmLightTint[1],
            g_viewEffects.filmLightTint[2], g_viewEffects.filmDarkTint[0],
            g_viewEffects.filmDarkTint[1], g_viewEffects.filmDarkTint[2]);
    }
}

// The bootstrap submits already-normalized clip-space coordinates.  VitaGL
// keeps its fixed-function matrices process-global, so a previous frontend or
// world pass can leave a translation/scale active when the first-person HUD
// is composited.  Keep both the world and overlay passes on an explicit
// identity transform; this is a no-op for the scalar framebuffer path.
void ResetVitaGlMatrices() {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

bool Init(uint32_t width, uint32_t height, size_t transientBytes,
          size_t gpuPoolBytes) {
    VITA_CPU_LOADING("renderer-initialize");
    if (g_initialized)
        return true;
    if (width == 0 || height == 0)
        return false;
    // Set once before VitaGL creates its cleanup worker; no concurrent toggles.
    vglCod4TimingEnable(KISAK_VITA_BENCHMARK ? GL_TRUE : GL_FALSE);
    // Both native bounded passes and the scalar oracle use these dimensions.
    // Initializing them only in SoftwareInit left native prop/viewmodel/HUD
    // boundaries calling glViewport(0, 0, 0, 0).
    g_width = width;
    g_height = height;

    g_nativeTextures.fill({});
    g_nativeBuffers.fill({});
    g_nativeTextureBytes = 0;
    g_nativeStaticBufferBytes = 0;
    g_lastSwapWaitUsec = 0;
    g_nativeStaticPipelineActive = false;
    g_nativeStaticMaterialRevision = 0u;
    g_nativeStaticBufferDraw = false;
    g_nativeDynamicBufferDraw = false;
    g_nativeViewModelSpaceDraw = false;
    g_materialPassRevision = 1u;
    g_nativeTransientRing = {};
    g_frameTransientUploadDrops = 0u;
    g_frameTransientUploadBytes = 0u;
    g_transientUploadHighWaterBytes = 0u;

#if KISAK_VITA_ALLOW_SOFTWARE_FALLBACK
    // A fallback-enabled build is the bounded Vita3K diagnostic target.  Do
    // not touch vitaGL merely because a shader compiler module happens to be
    // installed: Vita3K 0.2.x still exposes stubbed GXM context/USSE calls,
    // and probing them first creates a partial context that can fault after
    // the guest returns.  The strict build (fallback disabled) below remains
    // the authoritative accelerated/GXM qualification path.
    sceClibPrintf("[kisak-vita] using opt-in display diagnostic build\\n");
    g_software = SoftwareInit(width, height);
    g_vitaGlFogEnabled = false;
    g_initialized = g_software;
    return g_initialized;
#endif

    // Vita3K 0.2.x does not ship the Vita shader compiler module and its GXM
    // exports are stubs.  Calling vitaGL in that environment starts a partial
    // context and can crash the emulator after the guest returns.  The retail
    // Vita firmware provides one of these modules, so use their presence as a
    // narrow runtime capability check before touching the vitaGL global state.
    SceIoStat shaderStat{};
    const bool hasShaderCompiler = sceIoGetstat("ur0:data/libshacccg.suprx", &shaderStat) >= 0
        || sceIoGetstat("ur0:data/external/libshacccg.suprx", &shaderStat) >= 0;
    if (!hasShaderCompiler) {
#if KISAK_VITA_ALLOW_SOFTWARE_FALLBACK
        sceClibPrintf("[kisak-vita] GXM shader compiler unavailable; using opt-in display diagnostic\\n");
        g_software = SoftwareInit(width, height);
        g_initialized = g_software;
        return g_initialized;
#else
        sceClibPrintf("[kisak-vita] accelerated vitaGL/GXM qualification requires libshacccg.suprx\\n");
        sceClibPrintf("[kisak-vita] install the legally extracted Vita shader compiler in ur0:data or ur0:data/external\\n");
        return false;
#endif
    }

#if KISAK_VITA_ALLOW_SOFTWARE_FALLBACK
    g_software = false;
#endif
    g_vitaGlFogEnabled = false;
    // Persistent map resources must remain in the explicit VitaGL pool.  The
    // old 16 MiB pool silently overflowed into newlib's general heap; growing
    // a compressed mip chain then called realloc on that shared heap and could
    // corrupt the source runtime before the frontend drew its first frame.
    // Disabling the spill also turns a real resource-budget miss into a clean
    // GL allocation failure that the caller can route to the bounded error
    // screen instead of continuing with partial state.
    vglUseExtraMem(GL_FALSE);
    // VitaGL otherwise reserves a 32 MiB circular pool even though this
    // renderer submits the world through persistent VBO/IBOs and uses the
    // rotating arena only for bounded UI/dynamic uploads. The content
    // contract budgets one MiB per in-flight frame for this remaining upload
    // arena (the native renderer has its own bounded dynamic ring); keep the
    // unused GL1 immediate pool disabled so it cannot reserve another
    // per-frame slice from it.
    vglSetDisplayBufferCount(static_cast<int>(kNativeDisplayBufferCount));
    vglSetCircularPoolSize(static_cast<unsigned int>(transientBytes));
    // The GXM vertex ring is command/uniform staging, not mesh storage. All
    // immutable meshes have their own buffers, and VitaGL owns a separate
    // uniform circular pool. Use its supported ring-size configuration to
    // reserve one MiB here; GXM still owns synchronization and partial renders.
    // Hardware qualification must measure the resulting submission waits.
    constexpr uint32_t vertexRingBytes = 1024u * 1024u;
    vglSetVertexBufferSize(vertexRingBytes);
    // The default parameter buffer consumes another 16 MiB outside the
    // mapped pool. Redistribute that existing reservation: a 4 MiB parameter
    // buffer leaves 12 MiB for the map's previously omitted prop textures.
    // Total graphics reservation stays unchanged (80 MiB for this caller).
    // GXM retains partial-render synchronization; no unsafe speed hack or
    // allocator spill is enabled. Hardware submission waits remain unqualified.
    constexpr uint32_t parameterBytes = 4u * 1024u * 1024u;
    const size_t mappedPoolBytes = gpuPoolBytes
        + SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE - parameterBytes;
    vglSetParamBufferSize(parameterBytes);
    // The viewmodel ring deliberately owns persistent CPU-mapped dynamic
    // VBOs.  Do not place GL_DYNAMIC_DRAW storage inside VitaGL's transient
    // circular pool: those pointers cease to identify an owned allocation
    // after the corresponding display-buffer slice is recycled. Client-array
    // and other GL_STREAM_DRAW uploads may continue to use scratch memory.
    vglSetupScratchMemory(GL_FALSE, GL_TRUE);
    sceClibPrintf(
        "[kisak-vita] vitaGL pools displayBuffers=%u transient=%u transientPerFrame=%u legacyImmediate=0 gpuMapped=%u externalSpill=0\n",
        static_cast<unsigned int>(kNativeDisplayBufferCount),
        static_cast<unsigned int>(transientBytes),
        static_cast<unsigned int>(transientBytes
                                  / kNativeDisplayBufferCount),
        static_cast<unsigned int>(mappedPoolBytes));
    sceClibPrintf("[kisak-vita] GXM reservation vertexRingWithinPool=%u parameterOutsidePool=%u totalPoolAndParameter=%llu\n",
        vertexRingBytes, parameterBytes,
        static_cast<unsigned long long>(mappedPoolBytes) + parameterBytes);
#if KISAK_VITA_CPU_RUNTIME
    kisak::vita::VitaCpuShaderCache_Initialize();
#endif
#if KISAK_VITA_CPU_RUNTIME
    // CPU queues add static main-memory residency. Keep the 76 MiB total,
    // with 16 MiB in dedicated CDRAM, leaving room for CPU reservations.
    constexpr size_t cdramPoolBytes=16u*1024u*1024u;
    const size_t ramPoolBytes=mappedPoolBytes-cdramPoolBytes;
    kisak::vita::VitaCpuProfile_Startup("graphics-ram-pool-bytes",int(ramPoolBytes));
    kisak::vita::VitaCpuProfile_Startup("graphics-cdram-pool-bytes",int(cdramPoolBytes));
#else
    constexpr size_t cdramPoolBytes=0;
    const size_t ramPoolBytes=mappedPoolBytes;
#endif
    const GLboolean resolutionFallback = vglInitWithCustomSizes(
        0,
        static_cast<unsigned int>(width),
        static_cast<unsigned int>(height),
        static_cast<unsigned int>(ramPoolBytes),
        static_cast<unsigned int>(cdramPoolBytes), 0, 0, SCE_GXM_MULTISAMPLE_NONE);
    const bool contextReady = gxm_context != nullptr;
#if KISAK_VITA_CPU_RUNTIME
    kisak::vita::VitaCpuProfile_Startup("gxm-initialization-result",vglGetInitializationError());
    g_nativeContextReady=contextReady;
#endif
    if (!contextReady) {
        sceClibPrintf("[kisak-vita] gfx backend init failed\\n");
#if KISAK_VITA_ALLOW_SOFTWARE_FALLBACK
        // Vita3K can load a real libshacccg.suprx yet still expose stubbed
        // sceGxm context/USSE calls.  Keep the fallback opt-in at build time,
        // but make it reachable after that capability probe as well as when
        // the compiler module is absent.  A retail Vita with a working GXM
        // context remains on the accelerated path; strict builds still
        // return false here and never silently downgrade.
        sceClibPrintf("[kisak-vita] accelerated init unavailable; using opt-in display diagnostic\\n");
        g_software = SoftwareInit(width, height);
        g_initialized = g_software;
        return g_initialized;
#else
        return false;
#endif
    }
    // vglInitWithCustomSizes returns GL_TRUE only when it had to clamp the
    // requested display size.  A normal 960x544 initialization therefore
    // returns GL_FALSE even though the native context is ready.
    g_initialized = true;
    if (resolutionFallback)
        sceClibPrintf("[kisak-vita] vitaGL clamped the native display size\\n");
    if (g_initialized) {
        // Keep the backend's fixed-function state explicit.  The shared
        // scene path can submit depth-tested indexed surfaces without
        // depending on vitaGL's process-global defaults.
        glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        ResetVitaGlMatrices();
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_TEXTURE_2D);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        if (!CreateNativeLitShader()) {
            sceClibPrintf("[kisak-vita] fatal: native source-lit shader compilation failed; release renderer will not fall back\\n");
            g_initialized = false;
            return false;
        }
#if KISAK_VITA_CLEAN_VIEWMODEL
        if (!GfxVitaViewmodel_Init()) {
            g_initialized = false;
            return false;
        }
#endif
        if (!CreateNativeTransientDrawRing()) {
            sceClibPrintf(
                "[kisak-vita] fatal: native transient draw ring allocation failed\\n");
            DestroyNativeLitShader();
            g_initialized = false;
            return false;
        }
#if !defined(KISAK_VITA_NATIVE_DIRECT_DISPLAY_PROBE)
        if (!CreateNativePostResources(width, height)) {
            sceClibPrintf("[kisak-vita] fatal: native scene target allocation failed\n");
            DestroyNativeTransientDrawRing();
            DestroyNativeLitShader();
            g_initialized = false;
            return false;
        }
        // VitaGL creates the scene depth surface and patches programs lazily.
        // Commit those permanent frame resources before loading map textures;
        // otherwise a nominally complete FBO can fail its first 2 MiB depth
        // allocation after the map has fragmented the graphics pool.
#if KISAK_VITA_CPU_RUNTIME
        // Warmup itself presents. Retire the boot display owner before the
        // first swap, retaining its displayed buffer until the frontend takes over.
        kisak::vita::VitaBootStatus_Stage(kisak::vita::VitaBootStage::Ready);
#endif
        for (uint32_t frame = 0; frame < kNativeDisplayBufferCount; ++frame) {
            if (g_benchmarkClear) { glBindFramebuffer(GL_FRAMEBUFFER, 0); glViewport(0,0,g_width,g_height); }
    else BindNativeSceneTarget();
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            ApplyNativePostEffects();
            { VITA_CPU_SCOPE("render-swap-call"); vglSwapBuffers(GL_FALSE); }
    vglCod4TimingRead(g_swapDetail);
        }
        { VITA_CPU_SCOPE("render-gpu-finish"); glFinish(); }
        const GLenum warmupError = glGetError();
        if (warmupError != GL_NO_ERROR) {
            sceClibPrintf("[kisak-vita] fatal: native scene warmup failed error=%x\n", warmupError);
            DestroyNativePostResources();
            DestroyNativeTransientDrawRing();
            DestroyNativeLitShader();
            g_initialized = false;
            return false;
        }
        sceClibPrintf("[kisak-vita] native scene warmup ready\n");
#else
        sceClibPrintf("[kisak-vita] native direct-display probe: post target bypassed\\n");
#endif
    }
    if (g_initialized) GfxVita_ReleaseShaderCompiler();
    return g_initialized;
}

void Shutdown() {
#if KISAK_VITA_CLEAN_VIEWMODEL
    GfxVitaViewmodel_Shutdown();
#endif
    if (g_software)
        SoftwareShutdown();
    else if (g_initialized
#if KISAK_VITA_CPU_RUNTIME
             || g_nativeContextReady
#endif
    ) {
#if KISAK_VITA_CPU_RUNTIME
        { VITA_CPU_SCOPE("render-gpu-finish"); glFinish(); }
#endif
        EndNativeStaticPipeline();
        glUseProgram(0);
        ResetExtraFixedFunctionTextureUnits();
        DestroyWorldTarget();
        if(g_worldCopy.program)glDeleteProgram(g_worldCopy.program);
        g_worldCopy={};
        DestroyNativePostResources();
        DestroyNativeTransientDrawRing();
        DestroyNativeLitShader();
    }
    DestroyNativeLightmapLayout(0);g_lightmapLayoutFailures=0;
    g_nativeTexturePool.Clear([](uint32_t native) { glDeleteTextures(1, &native); });
#if KISAK_VITA_CPU_RUNTIME
    kisak::vita::VitaTextureStore_Stop();
#endif
    if (!g_texturePageScratch.Clear([]{return sceGxmTransferFinish()>=0;})) {
#if KISAK_VITA_CPU_RUNTIME
        kisak::vita::VitaCpuProfile_Invalidate("texture-scratch-shutdown-transfer-failed");
#endif
    }
    g_pagedTextureBytes=0;g_texturePagingFault=false;g_texturePagingDisabled=false;g_textureLastUse.fill(0);
    g_cubeTextureHandles.clear();
    g_nativeTextures.fill({});
    g_poseBuffers.Clear(DeleteNativePoseExtra);
    g_poseLightBounds.Clear();
    g_nativeBuffers.fill({});
    g_nativeTextureBytes = 0;
    g_nativeStaticBufferBytes = 0;
    g_lastSwapWaitUsec = 0;
    g_initialized = false;
#if KISAK_VITA_CPU_RUNTIME
    g_nativeContextReady = false;
#endif
    g_width = g_height = 0;
#if KISAK_VITA_ALLOW_SOFTWARE_FALLBACK
    g_software = false;
#endif
    g_vitaGlFogEnabled = false;
    g_nativeStaticPipelineActive = false;
    g_nativeStaticMaterialRevision = 0u;
    g_nativeStaticBufferDraw = false;
    g_nativeDynamicBufferDraw = false;
    g_nativeViewModelSpaceDraw = false;
}

void BeginFrame() {
    g_drawLightBounds={};
    ++g_rendererFrame;
#if KISAK_VITA_CPU_RUNTIME
    g_gpuDrawProbe.Begin(kisak::vita::VitaCpuProfile_Context().generation,g_renderPassProbe.drawProbe?g_renderPassProbe.phase:0);
#endif
    g_weaponFamilyActual=g_weaponFamilyEligible=g_weaponFamilyInvalid=0;
    g_lightmapLayoutActual=g_lightmapLayoutEligible=g_lightmapLayoutInvalid=0;
    g_worldComponentsActual=g_worldComponentsEligible=g_worldComponentsInvalid=g_worldComponentsTargets=0;
    g_worldGlossActual=g_worldGlossEligible=g_worldGlossInvalid=g_worldGlossTargetActual=g_worldGlossTargets=0;
    g_worldBasisState.Reset();g_worldBasisActual=g_worldBasisEligible=g_worldBasisInvalid=g_worldBasisTargets=g_worldBasisTargetActual=g_worldBasisSeen=g_worldBasisTriangles=g_worldBasisFamilyTriangles=0;
    g_worldPrecisionActual=g_worldPrecisionEligible=g_worldPrecisionInvalid=g_worldPrecisionTargets=g_worldPrecisionTargetActual=0;
    g_worldMathActual=g_worldMathEligible=g_worldMathInvalid=g_worldMathTargets=g_worldMathTargetActual=0;
    g_worldLifetimeActual=g_worldLifetimeEligible=g_worldLifetimeInvalid=g_worldLifetimeTargets=g_worldLifetimeTargetActual=0;
    g_worldAlphaActual=g_worldAlphaEligible=g_worldAlphaInvalid=g_worldAlphaTargetActual=g_worldAlphaTargetEligible=0;
    g_weaponSpecularActual=g_weaponSpecularEligible=g_weaponSpecularInvalid=g_weaponSpecularNormalMaps=g_weaponSpecularMaps=0;
    g_weaponOpaqueActual=g_weaponOpaqueEligible=g_weaponOpaqueInvalid=g_weaponNormalDraws=g_weaponSpecularDraws=0;
    g_earlyAlphaActual[0]=g_earlyAlphaActual[1]=g_earlyAlphaEligible[0]=g_earlyAlphaEligible[1]=g_earlyAlphaInvalid=0;
    g_fastColorActual=g_fastColorEligible=g_fastClearActual=g_fastClearEligible=g_fastColorInvalid=0;
    g_hudActual=g_hudEligible=g_hudInvalid=g_hudViewportSeen=0;
    std::memset(g_hudViewport,0,sizeof(g_hudViewport));g_hudFbo=0;
    g_propShaderDraws=g_propShaderEligibleDraws=0;
    g_opaqueDraws[0]=g_opaqueDraws[1]=g_opaqueEligible[0]=g_opaqueEligible[1]=g_opaqueInvalid=0;
    g_specularDraws[0]=g_specularDraws[1]=g_specularInputDraws=g_reflectionInputDraws=0;
    g_prefetchActual[0]=g_prefetchActual[1]=g_prefetchInvalid=0;
    g_familyActual[0]=g_familyActual[1]=g_familyEligible[0]=g_familyEligible[1]=g_familyInvalid=g_familyCameraSeen=0;
    g_materialProbeMask=g_materialProbeDraws[0]=g_materialProbeDraws[1]=0;
    g_materialNormalDraws=g_materialSpecDraws=g_materialReflectionDraws=g_materialPrimaryDraws=g_materialSecondaryDraws=g_materialLocalDraws=0;
    g_propGroupSeen=g_propAttempts=g_propSubmitted=g_propClipped=0;
    g_propAttemptTriangles=g_propSubmittedTriangles=0;
    g_frameDrawCalls = 0;
    g_frameTriangles = 0;
    g_frameStateChanges = 0;
    g_lastSwapWaitUsec = 0;
    g_nativeHud = false; g_nativeViewmodel=false; g_compositionUsec=0; g_postSubmitUsec = 0;
    g_litFullDraws=g_litSunDraws=0;
    g_viewportSeen=g_viewportMismatches=0;
    std::memset(g_observedViewports,0,sizeof(g_observedViewports));
    g_sceneShaderDraws=g_simpleMaterialDraws=g_postCopyDraws=g_authoredPostDraws=0;
    g_noLocalLightDraws=g_noSpecularDraws=g_specularEligibleDraws=0;
    g_noNormalMapDraws=g_normalEligibleDraws=g_adaptiveFullLightDraws=0;
    g_noFogDraws=g_fogEligibleDraws=0; g_sceneFogStart=g_sceneFogDensity=0;
    g_frameNativeColorPasses = 0;
    g_frameLightmappedDraws = 0;
    g_frameMaterialPasses = 0;
    g_frameNormalPasses = 0;
    g_frameSpecularPasses = 0;
    g_framePrimaryLightmapPasses = 0;
    g_frameSecondaryLightmapPasses = 0;
    g_frameShadowPasses = 0;
    g_frameEffectBatches = 0;
    g_frameWeatherDraws = 0;
    g_frameWeatherDrops = 0;
    g_frameTransientUploadDrops = 0;
    g_frameTransientUploadBytes = 0u;
    ResetFrameLightmapTelemetry();
    if (g_software) {
        SoftwareBeginFrame();
        return;
    }
    // Pass epilogues normally close this cache.  Treat the frame boundary as
    // a hard ownership boundary as well: frame/view uniforms may change even
    // when the first material key happens to match the previous frame.
    EndNativeStaticPipeline();
    BeginNativeTransientDrawPage();
    SelectNativeLightShader();
    glUseProgram(0);
    ResetExtraFixedFunctionTextureUnits();
    ResetVitaGlMatrices();
#if !defined(KISAK_VITA_NATIVE_DIRECT_DISPLAY_PROBE)
    // Keep the whole native frame in the source-space scene target. The
    // final post pass returns to the Vita back buffer only after the HUD,
    // viewmodel, and weather overlays have been submitted.
    if (g_benchmarkClear) { glBindFramebuffer(GL_FRAMEBUFFER, 0); glViewport(0,0,g_width,g_height); }
    else BindNativeSceneTarget();
#endif
    SetSurfaceMode(GfxSurfaceMode::AlphaBlend);
    vglWaitVblankStart(GL_FALSE);
}

void Clear(float r, float g, float b, float a) {
    if (g_software) {
        SoftwareClear(r, g, b, a);
        return;
    }
    // Clear the native scene with an ordinary vertex-backed GPU triangle.
    // Synchronized captures show VitaGL's precompiled INDEX-only clear
    // leaving this FBO intact on the current emulator. The explicit pass
    // writes the same full color/depth values through the vertex-backed shader
    // submission path; it does not depend on any previous material or depth.
    const GfxSurfaceMode previousMode = g_nativeSurfaceMode;
    SetMaterialPass(nullptr);
    g_effectBatch = {};
    SetSurfaceMode(GfxSurfaceMode::Overlay);
    BindTexture(GfxTextureHandle{0});
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);
    GfxDrawVertex vertices[3]{};
    const float xy[3][2]{{-1, -1}, {3, -1}, {-1, 3}};
    const float color[4]{r, g, b, a};
    for (uint32_t i = 0; i < 3; ++i) {
        vertices[i].position[0] = xy[i][0];
        vertices[i].position[1] = xy[i][1];
        vertices[i].position[2] = 1.0f;
        std::memcpy(vertices[i].color, color, sizeof(color));
    }
    const uint16_t indices[3]{0, 1, 2};
    g_nativeClearDraw=true;
    DrawIndexed(vertices, 3, indices, 3);
    g_nativeClearDraw=false;
    SetSurfaceMode(previousMode);
}

uint32_t MipBytes(uint32_t format, uint32_t width, uint32_t height) {
    // D3DFMT_DXT1/3/5 are FOURCC values in the desktop VITL/VITI exporter.
    // Keep the translation local to the backend; no D3D headers enter Vita
    // source files.
    const uint32_t dxt1 = 0x31545844u;
    const uint32_t dxt3 = 0x33545844u;
    const uint32_t dxt5 = 0x35545844u;
    const uint32_t blocksWide = (width + 3u) / 4u;
    const uint32_t blocksHigh = (height + 3u) / 4u;
    if (format == dxt1)
        return blocksWide * blocksHigh * 8u;
    if (format == dxt3 || format == dxt5)
        return blocksWide * blocksHigh * 16u;
    if (format == 50u) // D3DFMT_L8, generated BSP primary lightmaps
        return width * height;
    if (format == 21u) // D3DFMT_A8R8G8B8, generated BSP secondary lightmaps
        return width * height * 4u;
    return 0;
}

GLenum VitaCompressedFormat(uint32_t format) {
    const uint32_t dxt1 = 0x31545844u;
    const uint32_t dxt3 = 0x33545844u;
    const uint32_t dxt5 = 0x35545844u;
    if (format == dxt1)
        return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
    if (format == dxt3)
        return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
    if (format == dxt5)
        return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
    return 0;
}

struct CompletedTextureApi {
    uint64_t releasedBytes=0;
    uint32_t Bound() { GLint id=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&id);return id; }
    void Bind(uint32_t id) { glBindTexture(GL_TEXTURE_2D,id); }
    void* Pixels() { return vglGetTexDataPointer(GL_TEXTURE_2D); }
    void Detach() { vglOverloadTexDataPointer(GL_TEXTURE_2D,nullptr); }
    void Delete(uint32_t id) { GLuint name=id;glDeleteTextures(1,&name); }
    void Free(void* pixels) { releasedBytes=vglMallocUsableSize(pixels);vglFree(pixels); }
};

GfxTextureHandle CreateTexture2D(const uint8_t *payload, size_t payloadBytes,
                                 uint32_t width, uint32_t height,
                                 uint32_t levelCount, uint32_t format,
                                 uint32_t flags) {
    VITA_CPU_LOADING("texture-upload");
    if (g_software)
        return SoftwareCreateTexture(payload, payloadBytes, width, height,
                                     levelCount, format, flags);
    GfxTextureHandle result{0};
    g_textureUploadError=GL_NO_ERROR;
    const auto Failed=[&](uint32_t reason,uint32_t glError=0,uint32_t face=0,uint32_t level=0) {
        g_textureUploadError=glError;
        kisak::vita::VitaCpuProfile_Counter("texture-upload-failure",reason,glError);
        kisak::vita::VitaCpuProfile_Counter("texture-upload-failure-size",(uint64_t(width)<<32)|height,payloadBytes);
        kisak::vita::VitaCpuProfile_Counter("texture-upload-failure-format",format,(uint64_t(face)<<32)|level);
        kisak::vita::VitaCpuProfile_Counter("texture-upload-failure-residency",g_nativeTextureBytes,g_nativeStaticBufferBytes);
        kisak::vita::VitaCpuProfile_Counter("texture-upload-failure-pool-free",vglMemFree(VGL_MEM_RAM),vglMemFree(VGL_MEM_VRAM));
        return GfxTextureHandle{0};
    };
    const GLenum internalFormat = VitaCompressedFormat(format);
    const bool isL8 = format == 50u;
    const bool isA8R8G8B8 = format == 21u;
    if (!payload || payloadBytes == 0 || width == 0 || height == 0
        || width > 4096 || height > 4096 || levelCount == 0 || levelCount > 16
        || (internalFormat == 0 && !isL8 && !isA8R8G8B8))
        return Failed(1);

    const bool cubeMap = (flags & 0x4u) != 0; // IMG_FLAG_CUBEMAP
    const auto TraceNativeCube = [&](const char* phase, uint32_t face) {
        // Keep the diagnostic trace focused on the authored sky cube.  The
        // package also contains many small reflection probes; logging every
        // face of every probe consumes the bounded Vita script trace before
        // the first world frame and obscures renderer phase markers.
        if (!cubeMap || width < 512u || height < 512u)
            return;
        char detail[160]{};
        std::snprintf(detail, sizeof(detail),
                      "phase=%s face=%u dimensions=%ux%u format=%u bytes=%u",
                      phase ? phase : "unknown", static_cast<unsigned>(face),
                      static_cast<unsigned>(width), static_cast<unsigned>(height),
                      static_cast<unsigned>(format),
                      static_cast<unsigned>(payloadBytes));
#if KISAK_VITA_CPU_RUNTIME
        kisak::vita::VitaCpuProfile_Counter("native-cube-upload-face",face,width);
#else
        kisak::vita::VitaScriptServicesTrace("native_cube_upload", detail);
#endif
        sceClibPrintf("[kisak-vita] native cube upload %s\\n", detail);
    };
    TraceNativeCube("begin", 0u);
    uint32_t mipWidth = width;
    uint32_t mipHeight = height;
    size_t faceRequiredBytes = 0;
    for (uint32_t level = 0; level < levelCount; ++level) {
        const uint32_t bytes = MipBytes(format, mipWidth, mipHeight);
        if (bytes == 0 || faceRequiredBytes > payloadBytes
            || bytes > payloadBytes - faceRequiredBytes)
            return Failed(2);
        faceRequiredBytes += bytes;
        mipWidth = std::max(1u, mipWidth / 2u);
        mipHeight = std::max(1u, mipHeight / 2u);
    }
    // The stock VitaGL transfer swizzlers operate on 4x4 blocks.  Passing
    // the terminal 2x2/1x1 DXT levels through sceGxmTransferCopy produces a
    // zero-sized block transfer in Vita3K (and is undefined on hardware),
    // even though those tail levels are valid desktop DXT payloads.  Keep
    // validating the complete source chain, but upload only the largest
    // prefix with valid transfer blocks.  The base image and all visible
    // filtering levels remain source-authored; the sampler simply clamps its
    // mip range to the last supported block level.
    uint32_t uploadLevelCount = 1u;
    mipWidth = width;
    mipHeight = height;
    for (uint32_t level = 1; level < levelCount; ++level) {
        const uint32_t nextWidth = std::max(1u, mipWidth / 2u);
        const uint32_t nextHeight = std::max(1u, mipHeight / 2u);
        if (nextWidth < 4u || nextHeight < 4u)
            break;
        uploadLevelCount = level + 1u;
        mipWidth = nextWidth;
        mipHeight = nextHeight;
    }
    const size_t requiredBytes = cubeMap ? faceRequiredBytes * 6u
                                         : faceRequiredBytes;
    if (faceRequiredBytes == 0 || requiredBytes > payloadBytes)
        return Failed(3);

    const kisak::vita::VitaTextureIdentity identity{
        kisak::vita::VitaSha256::Hash(payload, requiredBytes),
        width, height, levelCount, format, flags & 0x4u,
        static_cast<uint32_t>(requiredBytes),
        (!cubeMap && (flags&GFX_VITA_TEXTURE_FLAG_PAGEABLE)) ? 1u : 0u};
    if (!g_textureReloadLease) if (const uint32_t shared = g_nativeTexturePool.Acquire(identity)) {
        if (cubeMap) g_cubeTextureHandles.push_back(shared);
        return {shared};
    }
#if KISAK_VITA_CPU_RUNTIME
    if(!g_textureReloadLease && !cubeMap && (flags&GFX_VITA_TEXTURE_FLAG_PAGEABLE)) {
        VITA_CPU_LOADING("texture-backing-write");
        if(!kisak::vita::VitaTextureStore_LiveBytes())g_texturePageBudget=kisak::vita::kVitaTexturePageGpuBytes;
        if(!kisak::vita::VitaTextureStore_Start("ux0:data/CODVIT001/texture-stream.tmp"))return Failed(8);
        const uint32_t backing=kisak::vita::VitaTextureStore_Write(payload,uint32_t(requiredBytes));
        if(!backing)return Failed(9);
        const auto failedBacking=[&](uint32_t reason) {
            kisak::vita::VitaTextureStore_Release(backing);
            if (!kisak::vita::VitaTextureStore_LiveBytes())
                g_texturePageScratch.Clear([]{return sceGxmTransferFinish()>=0;});
            return Failed(reason);
        };
        const size_t previousScratch=g_texturePageScratch.Capacity();
        if (!g_texturePageScratch.Reserve(requiredBytes,[]{return sceGxmTransferFinish()>=0;})) return failedBacking(11);
        if (previousScratch!=g_texturePageScratch.Capacity())
            kisak::vita::VitaCpuProfile_Counter("texture-page-scratch-bytes",g_texturePageScratch.Capacity(),requiredBytes);
        uint32_t certificate=GfxVitaConstantGloss233(payload,requiredBytes,width,height,levelCount,format,flags)?233:0;
        if(GfxVitaGravelSpecIdentity(identity))certificate=kGfxVitaGravelSpecCertificate;
        const uint32_t lease=g_nativeTexturePool.Insert(identity,0,certificate,backing);
        if(!lease)return failedBacking(7);
        kisak::vita::VitaCpuProfile_Counter("texture-backed-bytes",requiredBytes,kisak::vita::VitaTextureStore_LiveBytes());
        return {lease};
    }
    if(g_textureReloadLease && (!g_nativeTexturePool.Identity(g_textureReloadLease)
        || !(identity==*g_nativeTexturePool.Identity(g_textureReloadLease))))return Failed(10);
#endif

    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (!texture)
        return Failed(4,glGetError());
    const GLenum target = cubeMap ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    glBindTexture(target, texture);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER,
                    uploadLevelCount > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_WRAP_S,
                    cubeMap ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glTexParameteri(target, GL_TEXTURE_WRAP_T,
                    cubeMap ? GL_CLAMP_TO_EDGE : GL_REPEAT);

    while (glGetError() != GL_NO_ERROR) {
        // Clear stale state from a previous draw before validating this
        // resource upload.
    }
    // VitaGL currently accepts only level zero for cubemap face targets.
    // Regular immutable 2D textures reserve their complete authored chain in
    // one allocation, eliminating per-level realloc peaks and fragmentation.
    if (cubeMap) {
        uploadLevelCount = 1u;
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    } else if (internalFormat != 0)
        vglSetCompressedTextureMipmapCountHint(uploadLevelCount);
    // VitaGL accepts BGRA for flat images but its cube uploader only accepts
    // RGBA. Reflection probes exported as D3DFMT_A8R8G8B8 need an exact byte
    // reorder at load time. Reuse one face-sized staging allocation; this
    // preserves every color/alpha byte and does not enter the frame loop.
    std::vector<uint8_t> cubeRgba;
    if (cubeMap && isA8R8G8B8)
        cubeRgba.resize(static_cast<size_t>(width) * height * 4u);
    const uint32_t faceCount = cubeMap ? 6u : 1u;
    GLenum uploadError = GL_NO_ERROR;
    uint32_t failedFace = 0, failedLevel = 0;
#if KISAK_VITA_NATIVE_CPU_TEXTURE_UPLOAD
    std::vector<uint32_t> nativeDecodedLevel;
#endif
    for (uint32_t face = 0; face < faceCount && uploadError == GL_NO_ERROR; ++face) {
        TraceNativeCube("face_begin", face);
        mipWidth = width;
        mipHeight = height;
        size_t offset = static_cast<size_t>(face) * faceRequiredBytes;
        const GLenum faceTarget = cubeMap
            ? static_cast<GLenum>(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face)
            : GL_TEXTURE_2D;
        for (uint32_t level = 0; level < uploadLevelCount; ++level) {
            const uint32_t bytes = MipBytes(format, mipWidth, mipHeight);
            if (internalFormat != 0) {
#if KISAK_VITA_NATIVE_CPU_TEXTURE_UPLOAD
                bool uploadedDecoded = false;
                if (!cubeMap
                    && DecodeNativeDxtLevelRgba8888(
                        payload + offset, bytes, mipWidth, mipHeight, format,
                        &nativeDecodedLevel)) {
                    glTexImage2D(faceTarget, static_cast<GLint>(level), GL_RGBA,
                                 static_cast<GLsizei>(mipWidth),
                                 static_cast<GLsizei>(mipHeight), 0, GL_RGBA,
                                 GL_UNSIGNED_BYTE, nativeDecodedLevel.data());
                    uploadedDecoded = true;
                    if (g_nativeCpuTextureUploadTraceCount < 24u) {
                        sceClibPrintf(
                            "[kisak-vita] native CPU texture upload level=%u dimensions=%ux%u format=%u bytes=%u\n",
                            static_cast<unsigned>(level),
                            static_cast<unsigned>(mipWidth),
                            static_cast<unsigned>(mipHeight),
                            static_cast<unsigned>(format),
                            static_cast<unsigned>(bytes));
                        ++g_nativeCpuTextureUploadTraceCount;
                    }
                }
                if (!uploadedDecoded)
#endif
                {
                    glCompressedTexImage2D(
                        faceTarget, static_cast<GLint>(level), internalFormat,
                        static_cast<GLsizei>(mipWidth),
                        static_cast<GLsizei>(mipHeight), 0,
                        static_cast<GLsizei>(bytes), payload + offset);
                }
            } else {
                const uint8_t* pixels = payload + offset;
                GLenum pixelFormat = isL8 ? GL_LUMINANCE : GL_BGRA;
                if (!cubeRgba.empty()) {
                    for (size_t byte = 0; byte < bytes; byte += 4u) {
                        cubeRgba[byte] = pixels[byte + 2u];
                        cubeRgba[byte + 1u] = pixels[byte + 1u];
                        cubeRgba[byte + 2u] = pixels[byte];
                        cubeRgba[byte + 3u] = pixels[byte + 3u];
                    }
                    pixels = cubeRgba.data();
                    pixelFormat = GL_RGBA;
                }
                const GLenum pixelType = GL_UNSIGNED_BYTE;
                glTexImage2D(faceTarget, static_cast<GLint>(level),
                             isL8 ? GL_LUMINANCE : GL_RGBA,
                             static_cast<GLsizei>(mipWidth), static_cast<GLsizei>(mipHeight),
                             0, pixelFormat, pixelType, pixels);
            }
            uploadError = glGetError();
            if (uploadError != GL_NO_ERROR) {
                // A failed base allocation leaves no valid storage for the
                // following mips. Stop at the first failure; continuing can
                // ask VitaGL to resize an uninitialized texture allocation.
                failedFace = face;
                failedLevel = level;
                break;
            }
            offset += bytes;
            mipWidth = std::max(1u, mipWidth / 2u);
            mipHeight = std::max(1u, mipHeight / 2u);
        }
        TraceNativeCube("face_return", face);
    }
    if (cubeMap && uploadError == GL_NO_ERROR) {
        // The pinned compressed-face uploader reapplies the 2D mip count
        // (one) after constructing a no-mip cube. GXM distinguishes that
        // from zero: one selects full mip-chain spacing between cube faces.
        // Reapply the public cube sampler after all faces are valid so the
        // descriptor again matches our six tightly packed base images.
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        uploadError = glGetError();
    }
    if (uploadError != GL_NO_ERROR) {
        sceClibPrintf(
            "[kisak-vita] native texture upload failed dimensions=%ux%u levels=%u format=%u bytes=%u gl=0x%04x face=%u level=%u\n",
            static_cast<unsigned>(width), static_cast<unsigned>(height),
            static_cast<unsigned>(uploadLevelCount),
            static_cast<unsigned>(format),
            static_cast<unsigned>(std::min<size_t>(payloadBytes, UINT32_MAX)),
            static_cast<unsigned>(uploadError), failedFace, failedLevel);
        sceClibPrintf(
            "[kisak-vita] native resource ledger textures=%llu buffers=%llu\n",
            static_cast<unsigned long long>(g_nativeTextureBytes),
            static_cast<unsigned long long>(g_nativeStaticBufferBytes));
        bool reclaimed=false;
        if(g_textureReloadLease && !cubeMap) {
            // A later mip can fail after the base allocated storage. It has
            // never been drawn, but transfers must finish before detach/free.
            VITA_CPU_LOADING("texture-page-failed-upload-retire");
            glFinish();
            if(sceGxmTransferFinish()>=0) {
                CompletedTextureApi reclaim;
                reclaimed=GfxVitaReclaimCompletedTexture(texture,reclaim);
            } else uploadError=GL_INVALID_OPERATION; // Unsafe to retry.
        }
        if(!reclaimed)glDeleteTextures(1, &texture);
        glBindTexture(target, 0);
        return Failed(5,uploadError,failedFace,failedLevel);
    }
    if (!TrackNativeResource(&g_nativeTextures, texture,
                        static_cast<uint32_t>(std::min<size_t>(
                            requiredBytes, UINT32_MAX)),
                        &g_nativeTextureBytes)) {
        glDeleteTextures(1, &texture);
        glBindTexture(target, 0);
        sceClibPrintf("[kisak-vita] native texture ledger capacity exceeded\n");
        return Failed(6);
    }
    uint32_t glossCertificate=0;
#if KISAK_VITA_CPU_RUNTIME
    {
        VITA_CPU_LOADING("world-gloss-texture-certification");
        if(GfxVitaConstantGloss233(payload,requiredBytes,width,height,levelCount,format,flags))glossCertificate=233;
    }
#endif
    if(GfxVitaGravelSpecIdentity(identity))glossCertificate=kGfxVitaGravelSpecCertificate;
    result.value = g_textureReloadLease
        ? (g_nativeTexturePool.SetResident(g_textureReloadLease,texture)?g_textureReloadLease:0)
        : g_nativeTexturePool.Insert(identity, texture,glossCertificate);
    if(result.value && glossCertificate)kisak::vita::VitaCpuProfile_Counter(glossCertificate==233?"world-gloss-texture-certified":"world-components-texture-certified",result.value,glossCertificate);
    if (!result.value) {
        ForgetNativeResource(&g_nativeTextures, texture, &g_nativeTextureBytes);
        glDeleteTextures(1, &texture);
        glBindTexture(target, 0);
        sceClibPrintf("[kisak-vita] native texture lease capacity exceeded\n");
        return Failed(7);
    }
    if (cubeMap)
        g_cubeTextureHandles.push_back(result.value);
    TraceNativeCube("complete", 6u);
    return result;
}

GLuint NativeTexture(uint32_t lease) {
    GLuint native=g_nativeTexturePool.Resolve(lease);
#if KISAK_VITA_CPU_RUNTIME
    const uint32_t backing=g_nativeTexturePool.Backing(lease);
    if(backing)g_textureLastUse[(backing&4095)-1]=++g_textureUseSerial;
    if(native || !backing)return native;
    if(g_texturePagingDisabled)return 0;
    const auto* identity=g_nativeTexturePool.Identity(lease);if(!identity)return 0;
    const auto key=*identity;
    const auto fail=[&]() {g_texturePagingDisabled=true;g_texturePagingFault.store(true,std::memory_order_release);
        kisak::vita::VitaCpuProfile_Invalidate("texture-paging-failed");return GLuint(0);};
    const auto pinned=[&](uint32_t candidate) {
        for(auto handle:{lease,g_boundTexture,g_textureDrawColor,g_materialPass.colorTexture.value,g_materialPass.normalTexture.value,g_materialPass.specularTexture.value,
            g_materialPass.lightmapPrimaryTexture.value,g_materialPass.lightmapSecondaryTexture.value,
            g_materialPass.reflectionProbeTexture.value})
            if(g_nativeTexturePool.SameImage(candidate,handle))return true;
        return false;
    };
    GLint active=0,bound=0;glGetIntegerv(GL_ACTIVE_TEXTURE,&active);glGetIntegerv(GL_TEXTURE_BINDING_2D,&bound);
    bool finished=false;
    const auto evictOne=[&]() {
        uint32_t victim=0;uint64_t oldest=UINT64_MAX;
        g_nativeTexturePool.VisitResident([&](uint32_t candidate) {
            const auto token=g_nativeTexturePool.Backing(candidate);
            const auto age=g_textureLastUse[(token&4095)-1];
            if(!pinned(candidate) && age<oldest){oldest=age;victim=candidate;}
        });
        if(!victim)return false;
        if(!finished){VITA_CPU_SCOPE("texture-eviction-gpu-wait");glFinish();finished=true;}
        const auto bytes=g_nativeTexturePool.Identity(victim)->bytes;
        const GLuint evicted=g_nativeTexturePool.Resolve(victim);
        CompletedTextureApi reclaim;
        // glFinish above proves quiescence. Ordinary glDeleteTextures queues
        // recent storage until a later frame, even after completion. Counting
        // those bytes as free forced allocation recovery/one-second GC waits
        // while this very frame was still being submitted.
        if(!GfxVitaReclaimCompletedTexture(evicted,reclaim))return false;
        if(static_cast<GLuint>(bound)==evicted)bound=0;
        g_nativeTexturePool.Evict(victim);
        ForgetNativeResource(&g_nativeTextures,evicted,&g_nativeTextureBytes);
        g_pagedTextureBytes-=bytes;
        kisak::vita::VitaCpuProfile_Counter("texture-page-reclaimed-bytes",reclaim.releasedBytes,bytes);
        kisak::vita::VitaCpuProfile_Counter("texture-page-evicted",bytes,g_pagedTextureBytes);
        return true;
    };
    // Active material pins are a hard lower bound. Under learned pressure an
    // indivisible image can require more than that soft target; try allocation
    // only if the original hard cache budget is still respected.
    while(g_pagedTextureBytes+key.bytes>g_texturePageBudget)
        if(!evictOne())break;
    if(g_pagedTextureBytes+key.bytes>kisak::vita::kVitaTexturePageGpuBytes)return fail();
    VITA_CPU_LOADING("texture-page-read-upload");
    {
        VITA_CPU_LOADING("texture-page-backing-read");
        if(!g_texturePageScratch.Data() || key.bytes>g_texturePageScratch.Capacity()
            || !kisak::vita::VitaTextureStore_Read(backing,g_texturePageScratch.Data(),key.bytes))return fail();
    }
    unsigned attempts=0;
    const bool created=GfxVitaUploadWithPagePressure(key.bytes,g_texturePageBudget,[&] {
        ++attempts;
        g_textureReloadLease=lease;
        const auto result=CreateTexture2D(g_texturePageScratch.Data(),key.bytes,key.width,key.height,key.levels,key.format,
            key.flags | (key.storageClass ? GFX_VITA_TEXTURE_FLAG_PAGEABLE : 0));
        g_textureReloadLease=0;
        if(result.value)return GfxVitaPageUpload::Complete;
        return g_textureUploadError==GL_OUT_OF_MEMORY ? GfxVitaPageUpload::OutOfMemory : GfxVitaPageUpload::Failed;
    },evictOne,[]{return g_pagedTextureBytes;});
    if(attempts>1)kisak::vita::VitaCpuProfile_Counter(created?"texture-page-pressure-recovered":"texture-page-pressure-exhausted",attempts,g_texturePageBudget);
    // The transfer engine can still read the upload source after glTexImage
    // returns. Finish transfers before the bounded CPU staging allocation dies.
    int transferResult;
    { VITA_CPU_LOADING("texture-page-transfer-wait"); transferResult=sceGxmTransferFinish(); }
    glActiveTexture(active);glBindTexture(GL_TEXTURE_2D,bound);
    if(created)g_pagedTextureBytes+=key.bytes;
    if(!created || transferResult<0)return fail();
    kisak::vita::VitaCpuProfile_Counter("texture-page-uploaded",key.bytes,g_pagedTextureBytes);
    native=g_nativeTexturePool.Resolve(lease);
#endif
    return native;
}

void DestroyTexture(GfxTextureHandle texture) {
    if (g_software) {
        SoftwareDestroyTexture(texture);
        return;
    }
    if (texture.value != 0) {
        if (g_boundTexture == texture.value)
            g_boundTexture = 0;
        RemoveCubeTextureHandle(texture.value);
        uint32_t backing=0;
        const auto* identity=g_nativeTexturePool.Identity(texture.value);
        const uint32_t bytes=identity?identity->bytes:0;
        const GLuint native = g_nativeTexturePool.Release(texture.value,&backing);
#if KISAK_VITA_CPU_RUNTIME
        if(backing){kisak::vita::VitaTextureStore_Release(backing);if(native)g_pagedTextureBytes-=bytes;
            if(!kisak::vita::VitaTextureStore_LiveBytes()) {
                g_texturePagingDisabled=false;
                if (!g_texturePageScratch.Clear([]{return sceGxmTransferFinish()>=0;})) {
                    g_texturePagingDisabled=true;g_texturePagingFault.store(true,std::memory_order_release);
                    kisak::vita::VitaCpuProfile_Invalidate("texture-scratch-release-transfer-failed");
                }
                kisak::vita::VitaCpuProfile_Counter("texture-page-scratch-bytes",g_texturePageScratch.Capacity(),0);
            }}
#endif
        if (native) {
            DestroyNativeLightmapLayout(native);
            ForgetNativeResource(&g_nativeTextures, native, &g_nativeTextureBytes);
            glDeleteTextures(1, &native);
        }
    }
}

void BindTexture(GfxTextureHandle texture) {
    if (g_boundTexture != texture.value)
        ++g_frameStateChanges;
    if (g_software) {
        SoftwareBindTexture(texture);
        return;
    }
    EndNativeStaticPipeline();
    g_boundTexture = texture.value;
    // UI, HUD, and ordinary model draws are fixed-function submissions.  A
    // preceding native lit draw must never consume their vertices with its
    // source shader or leave an auxiliary texture unit selected.
    glUseProgram(0);
    ResetExtraFixedFunctionTextureUnits();
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, NativeTexture(texture.value));
    if (texture.value != 0)
        glEnable(GL_TEXTURE_2D);
    else
        glDisable(GL_TEXTURE_2D);
}

void SetSamplerState(uint32_t textureUnit, uint8_t samplerState) {
    if (!GfxVitaSamplerUnitValid(g_software,textureUnit))
        return;
    const uint8_t previous = g_software ? g_softwareSamplerState[textureUnit]
                                        : g_nativeSamplerState[textureUnit];
    if (previous != samplerState)
        ++g_frameStateChanges;
    g_nativeSamplerState[textureUnit] = samplerState;
    if (g_software) {
        SoftwareSetSamplerState(textureUnit, samplerState);
        return;
    }
    // KisakCOD's sampler byte is shared by every backend-facing material
    // record: low bits select nearest/linear (or anisotropic), bits 3-4
    // select mip filtering, and bits 5-7 select U/V/W clamping. VitaGL only
    // exposes 2D texture state here, so W is intentionally ignored.
    if ((samplerState & 0x1Fu) == 0)
        samplerState = 0x0Bu;
    const uint8_t filter = samplerState & 0x07u;
    const bool linear = filter != 0x01u;
    const uint8_t mip = (samplerState >> 3u) & 0x03u;
    GLenum minFilter = linear ? GL_LINEAR : GL_NEAREST;
    if (mip == 0x01u)
        minFilter = linear ? GL_LINEAR_MIPMAP_NEAREST
                           : GL_NEAREST_MIPMAP_NEAREST;
    else if (mip == 0x02u)
        minFilter = linear ? GL_LINEAR_MIPMAP_LINEAR
                           : GL_NEAREST_MIPMAP_LINEAR;
    const GLenum magFilter = linear ? GL_LINEAR : GL_NEAREST;
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                    (samplerState & 0x20u) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                    (samplerState & 0x40u) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glActiveTexture(GL_TEXTURE0);
}

// The lit BSP bridge uses the extra fixed-function texture units for the
// source primary/secondary lightmaps. VitaGL retains enable/client-array
// state across draws, so a later prop/viewmodel/UI batch must not inherit a
// previous lightmap unit or its coordinates. Reset both units, including the
// unit that the current batch may not use, and leave the ordinary material
// unit active for the next draw.
void ResetExtraFixedFunctionTextureUnits() {
    // Unit 5 is reserved for the source reflectionProbeSampler in the native
    // lit shader. Cube sampling is selected by that shader; VitaGL does not
    // implement GL_TEXTURE_CUBE_MAP as a fixed-function enable capability.
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    for (const GLenum textureUnit : {GL_TEXTURE4, GL_TEXTURE3, GL_TEXTURE2,
                                     GL_TEXTURE1}) {
        glActiveTexture(textureUnit);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
        glDisable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
        // The pinned VitaGL exposes coordinate arrays 0..2. Material samplers
        // on units 3..5 use custom vertex attributes, not FFP client arrays.
        if (textureUnit <= GL_TEXTURE2) {
            glClientActiveTexture(textureUnit);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        }
    }
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
}

void EndNativeStaticPipeline() {
    if (!g_nativeStaticPipelineActive)
        return;
    for (GLuint attribute = 0u; attribute < 8u; ++attribute)
        glDisableVertexAttribArray(attribute);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    // Static source-lit draws keep fixed-function fog disabled because the
    // fragment shader evaluates the source fog equation.  Restore the state
    // expected by the following fixed-function/UI path at the cache boundary.
    if (g_vitaGlFogEnabled)
        glEnable(GL_FOG);
    g_nativeStaticPipelineActive = false;
    g_nativeStaticMaterialRevision = 0u;
}

bool NativeGenericAlphaTestEnabled() {
    if (g_effectBatch.flags != 0u)
        return g_effectBatch.surfaceMode == GfxSurfaceMode::AlphaTest
            || g_effectBatch.surfaceMode == GfxSurfaceMode::AlphaTestBlend;
    if (SourceMaterialStateApplies(g_materialPass))
        return SourceStateUsesAlphaTest(g_materialPass);
    return g_nativeSurfaceMode == GfxSurfaceMode::AlphaTest
        || g_nativeSurfaceMode == GfxSurfaceMode::AlphaTestBlend;
}

GfxAlphaTestFunc NativeGenericAlphaTestFunc() {
    if (g_effectBatch.flags != 0u)
        return g_effectBatch.alphaTestFunc;
    return SourceMaterialStateApplies(g_materialPass)
        ? SourceStateAlphaTestFunc(g_materialPass) : g_nativeAlphaTestFunc;
}

void BindDynamicViewModelAttributes(const GfxDrawVertex* vertices) {
    const GLsizei stride = static_cast<GLsizei>(
        sizeof(DynamicViewModelVertex));
    const auto VertexAttribute = [vertices](size_t offset) {
        return reinterpret_cast<const void*>(
            reinterpret_cast<uintptr_t>(vertices) + offset);
    };
    glEnableVertexAttribArray(0u);
    glEnableVertexAttribArray(1u);
    glEnableVertexAttribArray(2u);
    glDisableVertexAttribArray(3u);
    glEnableVertexAttribArray(4u);
    glEnableVertexAttribArray(5u);
    glEnableVertexAttribArray(6u);
    glDisableVertexAttribArray(7u);
    glVertexAttrib2f(3u, 0.0f, 0.0f);
    glVertexAttrib1f(7u, 1.0f);
    glVertexAttribPointer(
        0u, 3, GL_FLOAT, GL_FALSE, stride,
        VertexAttribute(offsetof(DynamicViewModelVertex, position)));
    glVertexAttribPointer(
        1u, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,
        VertexAttribute(offsetof(DynamicViewModelVertex, color)));
    glVertexAttribPointer(
        2u, 2, GL_FLOAT, GL_FALSE, stride,
        VertexAttribute(offsetof(DynamicViewModelVertex, texCoord)));
    glVertexAttribPointer(
        4u, 3, GL_BYTE, GL_TRUE, stride,
        VertexAttribute(offsetof(DynamicViewModelVertex, normal)));
    glVertexAttribPointer(
        5u, 3, GL_BYTE, GL_TRUE, stride,
        VertexAttribute(offsetof(DynamicViewModelVertex, tangent)));
    glVertexAttribPointer(
        6u, 3, GL_FLOAT, GL_FALSE, stride,
        VertexAttribute(offsetof(DynamicViewModelVertex, worldPosition)));
}

void BindStaticVertexAttributes(const GfxDrawVertex* vertices) {
    const auto attribute = [vertices](size_t offset) {
        return reinterpret_cast<const void*>(
            reinterpret_cast<uintptr_t>(vertices) + offset);
    };
    glDisableVertexAttribArray(0u);
    glVertexAttrib3f(0u, 0.0f, 0.0f, 0.0f);
    for (GLuint index = 1u; index < 8u; ++index)
        glEnableVertexAttribArray(index);
    if (g_nativeStaticInstancedDraw) {
        constexpr GLsizei stride = sizeof(VitaStaticModelVertex);
        glDisableVertexAttribArray(3u);
        glVertexAttrib2f(3u, 0.0f, 0.0f);
        glDisableVertexAttribArray(7u);
        glVertexAttrib1f(7u, 1.0f);
        glVertexAttribPointer(1u, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,
            attribute(offsetof(VitaStaticModelVertex, color)));
        glVertexAttribPointer(2u, 2, GL_FLOAT, GL_FALSE, stride,
            attribute(offsetof(VitaStaticModelVertex, texCoord)));
        glVertexAttribPointer(4u, 3, GL_BYTE, GL_TRUE, stride,
            attribute(offsetof(VitaStaticModelVertex, normal)));
        glVertexAttribPointer(5u, 3, GL_BYTE, GL_TRUE, stride,
            attribute(offsetof(VitaStaticModelVertex, tangent)));
        glVertexAttribPointer(6u, 3, GL_FLOAT, GL_FALSE, stride,
            attribute(offsetof(VitaStaticModelVertex, worldPosition)));
    } else {
        constexpr GLsizei stride = sizeof(StaticWorldVertex);
        glVertexAttribPointer(1u, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,
            attribute(offsetof(StaticWorldVertex, color)));
        glVertexAttribPointer(2u, 2, GL_FLOAT, GL_FALSE, stride,
            attribute(offsetof(StaticWorldVertex, texCoord)));
        glVertexAttribPointer(3u, 2, GL_FLOAT, GL_FALSE, stride,
            attribute(offsetof(StaticWorldVertex, lightmapCoord)));
        glVertexAttribPointer(4u, 3, GL_BYTE, GL_TRUE, stride,
            attribute(offsetof(StaticWorldVertex, normal)));
        glVertexAttribPointer(5u, 3, GL_BYTE, GL_TRUE, stride,
            attribute(offsetof(StaticWorldVertex, tangent)));
        glVertexAttribPointer(6u, 3, GL_FLOAT, GL_FALSE, stride,
            attribute(offsetof(StaticWorldVertex, worldPosition)));
        glVertexAttribPointer(7u, 1, GL_UNSIGNED_SHORT, GL_TRUE, stride,
            attribute(offsetof(StaticWorldVertex, shadowVisibility)));
    }
}

bool NativeWeaponOpaqueEligible() {
    return GfxVitaWeaponOpaqueEligible(g_nativeDynamicBufferDraw,g_nativeViewModelSpaceDraw,
        g_nativeWorldSpaceDraw,g_nativeStaticInstancedDraw,SourceMaterialStateApplies(g_materialPass),
        SourceStateUsesAlphaTest(g_materialPass),SourceStateUsesBlend(g_materialPass),
        g_materialPass.colorTexture.value!=0,(g_materialPass.techniqueFlags0&GFX_MATERIAL_PASS_BAKED_LIGHTING)!=0,
        g_nativeHud,g_materialPass.surfaceMode==GfxSurfaceMode::Sky,g_benchmarkShaderMode);
}

bool NativeFastColorEligible(bool worldSpace) {
    return !g_materialPass.normalTexture.value && !g_materialPass.specularTexture.value
        && !g_materialPass.reflectionProbeTexture.value
        && !((g_materialPass.lightmapSamplerFlags&2u) && g_materialPass.lightmapPrimaryTexture.value)
        && !((g_materialPass.lightmapSamplerFlags&4u) && g_materialPass.lightmapSecondaryTexture.value)
        && GfxVitaFastColorEligible(g_nativeStaticInstancedDraw,worldSpace,
        g_nativeViewModelSpaceDraw,(g_materialPass.techniqueFlags0&GFX_MATERIAL_PASS_BAKED_LIGHTING)!=0,
        g_materialPass.colorTexture.value!=0,NativeGenericAlphaTestEnabled(),SourceStateUsesBlend(g_materialPass),
        g_rawLocalLightLoops,g_adaptiveLighting,g_nativeHud,g_nativeSkyDrawTexture.value!=0,g_benchmarkShaderMode);
}

bool DrawIndexedNativeColorPass(const GfxDrawVertex* vertices,
                                uint32_t vertexCount,
                                const uint16_t* indices,
                                uint32_t indexCount) {
    const bool fastEligible=NativeFastColorEligible(g_nativeWorldSpaceDraw);
    SelectNativeLightShader(false,false,fastEligible,g_nativeClearDraw);
    if((g_renderPassProbe.fastColor || g_renderPassProbe.weaponOpaque || g_renderPassProbe.hud || g_renderPassProbe.earlyAlpha || g_renderPassProbe.fastDrawProbe || g_renderPipeline.weaponPipeline)) {
        g_fastColorEligible+=fastEligible;g_fastClearEligible+=g_nativeClearDraw;
    }
    if (g_nativeLitShader.program == 0)
        return false;
    if ((!NativeBufferDrawActive() && (!vertices || !indices))
        || vertexCount == 0 || indexCount < 3)
        return false;
    ++g_frameNativeColorPasses;
    const bool hudShader = g_nativeHudShader.program && g_nativeLitShader.program==g_nativeHudShader.program;

    // Generic color/effect/model submissions use the same source shader as
    // the lit family, but explicitly disable normal, specular, lightmap, and
    // frame-lighting inputs. This removes the old fixed-function-only native
    // route while keeping screen-space UI and untextured fallback geometry
    // valid through the has-color branch in the fragment shader.
    const bool staticDraw = g_nativeStaticBufferDraw;
    const GLuint colorTexture = staticDraw
        ? g_materialPass.colorTexture.value : g_boundTexture;
    TextureDrawPin drawPin(colorTexture);
    const bool hasColor = colorTexture != 0;
    const GfxSurfaceMode passSurface = g_effectBatch.flags != 0u
        ? g_effectBatch.surfaceMode
        : (staticDraw ? g_materialPass.surfaceMode : g_nativeSurfaceMode);
    const bool alphaTestEnabled = NativeGenericAlphaTestEnabled();
    const GfxAlphaTestFunc alphaTestFunc = NativeGenericAlphaTestFunc();
    const bool fogEnabled = g_vitaGlFogEnabled
        && passSurface != GfxSurfaceMode::Sky
        && passSurface != GfxSurfaceMode::Overlay;
    const bool reuseStaticMaterial = staticDraw
        && g_nativeStaticPipelineActive
        && g_nativeStaticMaterialRevision == g_materialPassRevision;

    // Color-only immutable ranges use the same persistent pipeline contract
    // as lightmapped ranges.  The old path rebuilt six texture units, eleven
    // uniforms, and the complete shader binding for every spatial batch even
    // when the authored material had not changed.  Shipment commonly has
    // several adjacent cells for one material, so that defeated the purpose
    // of the native archive and made submission cost scale with draw count.
    if (!reuseStaticMaterial) {
        ResetExtraFixedFunctionTextureUnits();
        glUseProgram(g_nativeLitShader.program);
        if (!hudShader) {
            UploadNativeLitUniforms();
            // The source UI technique is unlit in BOTH comparison phases.
            // The old generic path needlessly evaluated world lights here.
            if (NativeHudEligible()) SetNativeUniform1f(g_nativeLitShader.frameValid, 0.0f);
        }
        SetNativeUniform1f(g_nativeLitShader.hasColor,
                           hasColor ? 1.0f : 0.0f);
        if (!hudShader) {
        SetNativeUniform1f(g_nativeLitShader.hasNormal, 0.0f);
        SetNativeUniform1f(g_nativeLitShader.hasSpecular, 0.0f);
        SetNativeUniform1f(g_nativeLitShader.hasReflectionProbe,
                           g_nativeSkyDrawTexture.value != 0 ? 1.0f : 0.0f);
        SetNativeUniform1f(g_nativeLitShader.usePrimaryLightmap, 0.0f);
        SetNativeUniform1f(g_nativeLitShader.useSecondaryLightmap, 0.0f);
        SetNativeUniform1f(g_nativeLitShader.surfaceMode,
                           static_cast<float>(static_cast<uint8_t>(
                               passSurface)));
        }
        SetNativeUniform1f(g_nativeLitShader.alphaTestEnabled,
                           alphaTestEnabled ? 1.0f : 0.0f);
        SetNativeUniform1f(g_nativeLitShader.alphaTestFunc,
                           static_cast<float>(static_cast<uint8_t>(
                               alphaTestFunc)));
        if (!hudShader) {
        SetNativeUniform1f(g_nativeLitShader.bakedLighting, 1.0f);
        SetNativeUniform1f(g_nativeLitShader.fogEnabled,
                           fogEnabled ? 1.0f : 0.0f);

        if (g_nativeSkyDrawTexture.value != 0) {
            glActiveTexture(GL_TEXTURE5);
            glBindTexture(GL_TEXTURE_CUBE_MAP, NativeTexture(g_nativeSkyDrawTexture.value));
        }
        }
        glActiveTexture(GL_TEXTURE0);
        if (hasColor) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, NativeTexture(colorTexture));
            SetSamplerState(
                0u, staticDraw ? g_materialPass.colorSamplerState
                               : g_nativeSamplerState[0]);
        } else {
            glDisable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }

    if (g_nativeStaticBufferDraw) {
        BindStaticVertexAttributes(vertices);
    } else if (g_nativeViewModelSpaceDraw) {
      BindDynamicViewModelAttributes(vertices);
    } else {
      const GLsizei stride = static_cast<GLsizei>(sizeof(GfxDrawVertex));
      const auto VertexAttribute = [vertices](size_t offset) {
        return reinterpret_cast<const void*>(
            reinterpret_cast<uintptr_t>(vertices) + offset);
      };
    glEnableVertexAttribArray(0u);
    glEnableVertexAttribArray(1u);
    glEnableVertexAttribArray(2u);
    if (!hudShader) {
    glEnableVertexAttribArray(3u);
    glEnableVertexAttribArray(4u);
    glEnableVertexAttribArray(5u);
    glEnableVertexAttribArray(6u);
    glEnableVertexAttribArray(7u);
    }
    glVertexAttribPointer(0u, 3, GL_FLOAT, GL_FALSE, stride,
                          VertexAttribute(offsetof(GfxDrawVertex, position)));
    glVertexAttribPointer(1u, 4, GL_FLOAT, GL_FALSE, stride,
                          VertexAttribute(offsetof(GfxDrawVertex, color)));
    glVertexAttribPointer(2u, 2, GL_FLOAT, GL_FALSE, stride,
                          VertexAttribute(offsetof(GfxDrawVertex, texCoord)));
    if (!hudShader) {
    glVertexAttribPointer(3u, 2, GL_FLOAT, GL_FALSE, stride,
                          VertexAttribute(offsetof(GfxDrawVertex, lightmapCoord)));
    glVertexAttribPointer(4u, 3, GL_FLOAT, GL_FALSE, stride,
                          VertexAttribute(offsetof(GfxDrawVertex, normal)));
    glVertexAttribPointer(5u, 3, GL_FLOAT, GL_FALSE, stride,
                          VertexAttribute(offsetof(GfxDrawVertex, tangent)));
    glVertexAttribPointer(6u, 3, GL_FLOAT, GL_FALSE, stride,
                          VertexAttribute(offsetof(GfxDrawVertex, worldPosition)));
    glVertexAttribPointer(7u, 1, GL_FLOAT, GL_FALSE, stride,
                          VertexAttribute(offsetof(GfxDrawVertex, shadowVisibility)));
    }
    }

    // Fixed-function fog is not applied while a fragment shader is bound;
    // the source fog equation above is the only fog contribution for this
    // draw. Restore the compatibility state before returning to the caller.
    const bool restoreFog = g_vitaGlFogEnabled;
    if (restoreFog && !reuseStaticMaterial)
        glDisable(GL_FOG);
    if((g_renderPassProbe.fastColor || g_renderPassProbe.weaponOpaque || g_renderPassProbe.hud || g_renderPassProbe.earlyAlpha || g_renderPassProbe.fastDrawProbe || g_renderPipeline.weaponPipeline)) {
        GLint bound=0;glGetIntegerv(GL_CURRENT_PROGRAM,&bound);
        const bool color=g_nativeFastColor.program && GLuint(bound)==g_nativeFastColor.program;
        const bool clear=g_nativeFastClear.program && GLuint(bound)==g_nativeFastClear.program;
        g_fastColorActual+=color;g_fastClearActual+=clear;
        if((color && !fastEligible) || (clear && !g_nativeClearDraw)
            || ((color || clear) && !g_renderPassProbe.weaponOpaque && !g_renderPassProbe.hud && !g_renderPassProbe.earlyAlpha && !g_renderPassProbe.fastDrawProbe && !g_renderPipeline.weaponPipeline && g_renderPassProbe.phase!=2))++g_fastColorInvalid;
    }
    if (g_renderPassProbe.hud && NativeHudEligible()) {
        ++g_hudEligible;
        GLint bound=0;glGetIntegerv(GL_CURRENT_PROGRAM,&bound);
        const bool actual=g_nativeHudShader.program && GLuint(bound)==g_nativeHudShader.program;
        g_hudActual+=actual;
        if (actual != (g_renderPassProbe.phase==2) || !g_nativeHudShader.program) ++g_hudInvalid;
        if (!g_hudViewportSeen++) {
            glGetIntegerv(GL_VIEWPORT,g_hudViewport);
            glGetIntegerv(GL_FRAMEBUFFER_BINDING,&g_hudFbo);
            if (g_hudViewport[0] || g_hudViewport[1] || g_hudViewport[2]!=960 || g_hudViewport[3]!=544 || g_hudFbo) ++g_hudInvalid;
        }
    }
    SubmitNativeIndexedElements(indexCount, indices);
    if (staticDraw) {
        g_nativeStaticPipelineActive = true;
        g_nativeStaticMaterialRevision = g_materialPassRevision;
    } else {
        if (restoreFog)
            glEnable(GL_FOG);
        for (GLuint attribute = 0u; attribute < 8u; ++attribute)
            glDisableVertexAttribArray(attribute);
        glUseProgram(0);
        ResetExtraFixedFunctionTextureUnits();
    }
    return true;
}

void DrawIndexed(const GfxDrawVertex *vertices, uint32_t vertexCount,
                const uint16_t *indices, uint32_t indexCount) {
    if ((!NativeBufferDrawActive() && (!vertices || !indices))
        || vertexCount == 0 || indexCount < 3)
        return;
    if (g_software) {
        RecordDraw(indexCount);
        SoftwareDrawIndexed(vertices, vertexCount, indices, indexCount);
        return;
    }
    SelectNativeLightShader(false,false,NativeFastColorEligible(g_nativeWorldSpaceDraw),g_nativeClearDraw); // Before transient uploads bind their buffers.
    // CPU-authored UI/effect geometry is copied into this frame's persistent
    // page. Never hand stack/client pointers to VitaGL: its internal client
    // array copies share the circular allocator and can outlive the caller or
    // consume a second frame's scratch slice after a large map upload.
    if (!NativeBufferDrawActive()) {
        for (uint32_t index = 0; index < indexCount; ++index) {
            if (indices[index] >= vertexCount)
                return;
        }
        const GfxDrawVertex* vertexOffset = nullptr;
        const uint16_t* indexOffset = nullptr;
        if (!UploadNativeTransientDraw(vertices, vertexCount, indices,
                                       indexCount, &vertexOffset,
                                       &indexOffset))
            return;
        g_nativeDynamicBufferDraw = true;
        RecordDraw(indexCount);
        const bool submitted = DrawIndexedNativeColorPass(
            vertexOffset, vertexCount, indexOffset, indexCount);
        g_nativeDynamicBufferDraw = false;
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0u);
        glBindBuffer(GL_ARRAY_BUFFER, 0u);
        if (!submitted)
            ++g_frameTransientUploadDrops;
        return;
    }
    RecordDraw(indexCount);
    if (DrawIndexedNativeColorPass(vertices, vertexCount, indices, indexCount))
        return;

    // Keep fixed-function presentation deterministic after any source-lit
    // shader batch when the native shader could not be created. This is
    // especially important for the full-screen menu background, which
    // otherwise flickers or disappears on VitaGL.
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    ResetExtraFixedFunctionTextureUnits();
    glVertexPointer(3, GL_FLOAT, static_cast<GLsizei>(sizeof(GfxDrawVertex)),
                    vertices->position);
    glColorPointer(4, GL_FLOAT, static_cast<GLsizei>(sizeof(GfxDrawVertex)),
                   vertices->color);
    glTexCoordPointer(2, GL_FLOAT, static_cast<GLsizei>(sizeof(GfxDrawVertex)),
                      vertices->texCoord);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indexCount), GL_UNSIGNED_SHORT,
                   indices);
}

void DrawIndexedLightmapped(const GfxDrawVertex *vertices, uint32_t vertexCount,
                            const uint16_t *indices, uint32_t indexCount,
                            GfxTextureHandle texture,
                            GfxTextureHandle lightmapPrimaryTexture,
                            GfxTextureHandle lightmapSecondaryTexture,
                            GfxTextureHandle reflectionProbeTexture,
                            uint8_t lightmapSamplerFlags) {
    TextureDrawPin drawPin(texture.value);
    ++g_frameLightmappedDraws;
    if (g_software) {
        if (!vertices || !indices || vertexCount == 0 || indexCount < 3)
            return;
        RecordDraw(indexCount);
        // Keep the source-lit call alive on the scalar path as well.  The
        // software rasterizer samples the primary lightmap per fragment using
        // the authored lightmap UVs; this is materially closer to the
        // desktop Cg pass than the old shared-vertex CPU modulation.
        SoftwareBindTexture(texture);
        g_materialPass.colorTexture = texture;
        g_materialPass.lightmapPrimaryTexture = lightmapPrimaryTexture;
        g_materialPass.lightmapSecondaryTexture = lightmapSecondaryTexture;
        g_materialPass.reflectionProbeTexture = reflectionProbeTexture;
        g_materialPass.lightmapSamplerFlags = lightmapSamplerFlags;
        g_softwareLightmapPrimary = lightmapPrimaryTexture.value < g_textures.size()
            ? lightmapPrimaryTexture.value : 0u;
        g_softwareLightmapSecondary = lightmapSecondaryTexture.value < g_textures.size()
            ? lightmapSecondaryTexture.value : 0u;
        g_softwareLightmapSamplerFlags = lightmapSamplerFlags;
        if (g_softwareLightmapPrimary != 0)
            SoftwareSetSamplerState(1u, 0x62u);
        if (g_softwareLightmapSecondary != 0)
            SoftwareSetSamplerState(2u, 0x62u);
        SoftwareDrawIndexed(vertices, vertexCount, indices, indexCount);
        g_softwareLightmapPrimary = 0;
        g_softwareLightmapSecondary = 0;
        g_softwareLightmapSamplerFlags = 0;
        SoftwareBindTexture(GfxTextureHandle{0});
        return;
    }
    const bool usePrimary = (lightmapSamplerFlags & 0x02u) != 0
        && lightmapPrimaryTexture.value != 0;
    const bool useSecondary = (lightmapSamplerFlags & 0x04u) != 0
        && lightmapSecondaryTexture.value != 0;
    // The source unlit families use the bound color map as their complete
    // fragment output. If an older package accidentally carries lightmap
    // sampler bits on that record, keep the native path on the fixed-function
    // color route instead of sending it through the lit shader.
    const bool sourceUnlit = SourceTechniqueIsUnlit(g_materialPass);
    if (sourceUnlit)
        return DrawIndexed(vertices, vertexCount, indices, indexCount);
    if ((!NativeBufferDrawActive() && (!vertices || !indices))
        || vertexCount == 0 || indexCount < 3
        || texture.value == 0
        || (!usePrimary && !useSecondary
            && (g_materialPass.techniqueFlags0
                & GFX_MATERIAL_PASS_BAKED_LIGHTING) == 0))
        return DrawIndexed(vertices, vertexCount, indices, indexCount);
    if (!NativeBufferDrawActive()) {
        for (uint32_t index = 0; index < indexCount; ++index) {
            if (indices[index] >= vertexCount)
                return;
        }
    }

    g_materialPass.colorTexture = texture;
    g_materialPass.lightmapPrimaryTexture = lightmapPrimaryTexture;
    g_materialPass.lightmapSecondaryTexture = lightmapSecondaryTexture;
    g_materialPass.reflectionProbeTexture = reflectionProbeTexture;
    g_materialPass.lightmapSamplerFlags = lightmapSamplerFlags;
    const bool propEligible=GfxVitaPropShaderEligible(g_nativeStaticInstancedDraw,
        g_nativeWorldSpaceDraw,g_nativeViewModelSpaceDraw,
        (g_materialPass.techniqueFlags0&GFX_MATERIAL_PASS_BAKED_LIGHTING)!=0,
        usePrimary,useSecondary,texture.value!=0,g_rawLocalLightLoops,g_adaptiveLighting,
        g_nativeHud,g_materialPass.surfaceMode==GfxSurfaceMode::Sky,g_benchmarkShaderMode);
    if(propEligible)++g_propShaderEligibleDraws;
    const bool opaqueEligible=NativeOpaqueEligible(g_nativeStaticBufferDraw && g_nativeWorldSpaceDraw,g_nativeViewModelSpaceDraw);
    if(opaqueEligible)++g_opaqueEligible[g_nativeStaticInstancedDraw?1:0];
    if(g_renderPassProbe.family || g_renderPassProbe.prefetch || g_renderPassProbe.drawProbe || (g_renderPassProbe.fastColor || g_renderPassProbe.weaponOpaque || g_renderPassProbe.hud || g_renderPassProbe.earlyAlpha || g_renderPassProbe.fastDrawProbe || g_renderPipeline.weaponPipeline)) {const auto family=NativeMaterialFamily(opaqueEligible);if(family)++g_familyEligible[family-1];}
    const bool weaponEligible=NativeWeaponOpaqueEligible();
    const bool earlyAlphaEligible=NativeEarlyAlphaEligible(g_nativeStaticBufferDraw && g_nativeWorldSpaceDraw,g_nativeViewModelSpaceDraw);
    const bool worldAlphaEligible=NativeWorldAlphaEligible(g_nativeStaticBufferDraw && g_nativeWorldSpaceDraw,g_nativeViewModelSpaceDraw);
    const bool worldLifetimeEligible=NativeWorldLifetimeEligible(g_nativeStaticBufferDraw && g_nativeWorldSpaceDraw,g_nativeViewModelSpaceDraw);
    SelectNativeLightShader(propEligible,opaqueEligible,false,false,weaponEligible,earlyAlphaEligible,worldAlphaEligible,worldLifetimeEligible);

    const bool reuseStaticMaterial = g_nativeStaticBufferDraw
        && g_nativeStaticPipelineActive
        && g_nativeStaticMaterialRevision == g_materialPassRevision
        && (!g_renderPassProbe.lightmapLayout || g_nativeStaticLayoutLane==NativeUseLightmapLayout());

    if (g_nativeLitShader.program != 0) {
        if (!reuseStaticMaterial) {
            // Clear all auxiliary units only when the exact material changes;
            // a primary-only surface must not inherit a normal, specular, or
            // secondary-lightmap binding from the preceding material.  Static
            // ranges with the same revision deliberately retain this state.
            ResetExtraFixedFunctionTextureUnits();
            glUseProgram(g_nativeLitShader.program);
            UploadNativeLitUniforms();
            SetNativeUniform1f(g_nativeLitShader.hasNormal,
                               g_materialPass.normalTexture.value != 0 ? 1.0f : 0.0f);
            SetNativeUniform1f(g_nativeLitShader.hasColor, 1.0f);
            SetNativeUniform1f(g_nativeLitShader.hasSpecular,
                               g_materialPass.specularTexture.value != 0 ? 1.0f : 0.0f);
            SetNativeUniform1f(g_nativeLitShader.hasReflectionProbe,
                               g_materialPass.reflectionProbeTexture.value != 0
                                   ? 1.0f : 0.0f);
            SetNativeUniform1f(g_nativeLitShader.usePrimaryLightmap,
                               usePrimary ? 1.0f : 0.0f);
            SetNativeUniform1f(g_nativeLitShader.useSecondaryLightmap,
                               useSecondary ? 1.0f : 0.0f);
            SetNativeUniform1f(g_nativeLitShader.surfaceMode,
                               static_cast<float>(static_cast<uint8_t>(
                                   g_materialPass.surfaceMode)));
            const bool sourceAlphaTestEnabled = SourceMaterialStateApplies(
                g_materialPass) ? SourceStateUsesAlphaTest(g_materialPass)
                : (g_materialPass.surfaceMode == GfxSurfaceMode::AlphaTest
                   || g_materialPass.surfaceMode == GfxSurfaceMode::AlphaTestBlend);
            SetNativeUniform1f(g_nativeLitShader.alphaTestEnabled,
                               sourceAlphaTestEnabled ? 1.0f : 0.0f);
            SetNativeUniform1f(g_nativeLitShader.alphaTestFunc,
                               static_cast<float>(static_cast<uint8_t>(
                                   SourceMaterialStateApplies(g_materialPass)
                                       ? SourceStateAlphaTestFunc(g_materialPass)
                                       : g_materialPass.alphaTestFunc)));
            SetNativeUniform1f(g_nativeLitShader.bakedLighting,
                               (g_materialPass.techniqueFlags0
                                    & GFX_MATERIAL_PASS_BAKED_LIGHTING) != 0
                                   ? 1.0f : 0.0f);
            SetNativeUniform1f(g_nativeLitShader.fogEnabled,
                               (g_vitaGlFogEnabled
                                && g_materialPass.surfaceMode != GfxSurfaceMode::Sky
                                && g_materialPass.surfaceMode != GfxSurfaceMode::Overlay)
                                   ? 1.0f : 0.0f);

            const auto BindShaderTexture = [&](uint32_t unit,
                                               GfxTextureHandle handle,
                                               uint8_t samplerState) {
                const GLenum textureUnit = static_cast<GLenum>(GL_TEXTURE0 + unit);
                glActiveTexture(textureUnit);
                if (handle.value != 0) {
                    glEnable(GL_TEXTURE_2D);
                    glBindTexture(GL_TEXTURE_2D,(unit==3 || unit==4)?NativeLightmapTexture(handle.value):NativeTexture(handle.value));
                    SetSamplerState(unit, samplerState);
                    glActiveTexture(textureUnit);
                } else {
                    glDisable(GL_TEXTURE_2D);
                    glBindTexture(GL_TEXTURE_2D, 0);
                }
            };
            BindShaderTexture(0u, texture, g_materialPass.colorSamplerState);
            if(g_nativeLitShader.normalSampler>=0)BindShaderTexture(1u, g_materialPass.normalTexture,
                              g_materialPass.normalSamplerState);
            if(g_nativeLitShader.specularSampler>=0)BindShaderTexture(2u, g_materialPass.specularTexture,
                              g_materialPass.specularSamplerState);
            if(g_nativeLitShader.lightmapPrimarySampler>=0)BindShaderTexture(3u, lightmapPrimaryTexture, 0x62u);
            if(g_nativeLitShader.lightmapSecondarySampler>=0)BindShaderTexture(4u, lightmapSecondaryTexture, 0x62u);
            const GfxTextureHandle reflectionProbe =
                g_materialPass.reflectionProbeTexture;
            glActiveTexture(GL_TEXTURE5);
            glDisable(GL_TEXTURE_2D);
            if (reflectionProbe.value != 0 && g_nativeLitShader.reflectionProbeSampler>=0) {
                glBindTexture(GL_TEXTURE_CUBE_MAP, NativeTexture(reflectionProbe.value));
                glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            } else {
                glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
            }
        }

        RecordNativeLightmapLayout(lightmapPrimaryTexture,lightmapSecondaryTexture);
        NativeAuditWorldTextures(texture,lightmapPrimaryTexture,lightmapSecondaryTexture);

        if (g_nativeStaticBufferDraw) {
        BindStaticVertexAttributes(vertices);
    } else if (g_nativeViewModelSpaceDraw) {
          BindDynamicViewModelAttributes(vertices);
        } else {
        const GLsizei stride = static_cast<GLsizei>(sizeof(GfxDrawVertex));
        const auto VertexAttribute = [vertices](size_t offset) {
            return reinterpret_cast<const void*>(
                reinterpret_cast<uintptr_t>(vertices) + offset);
        };
        glEnableVertexAttribArray(0u);
        glEnableVertexAttribArray(1u);
        glEnableVertexAttribArray(2u);
        glEnableVertexAttribArray(3u);
        glEnableVertexAttribArray(4u);
        glEnableVertexAttribArray(5u);
        glEnableVertexAttribArray(6u);
        glEnableVertexAttribArray(7u);
        glVertexAttribPointer(0u, 3, GL_FLOAT, GL_FALSE, stride,
                              VertexAttribute(offsetof(GfxDrawVertex, position)));
        glVertexAttribPointer(1u, 4, GL_FLOAT, GL_FALSE, stride,
                              VertexAttribute(offsetof(GfxDrawVertex, color)));
        glVertexAttribPointer(2u, 2, GL_FLOAT, GL_FALSE, stride,
                              VertexAttribute(offsetof(GfxDrawVertex, texCoord)));
        glVertexAttribPointer(3u, 2, GL_FLOAT, GL_FALSE, stride,
                              VertexAttribute(offsetof(GfxDrawVertex, lightmapCoord)));
        glVertexAttribPointer(4u, 3, GL_FLOAT, GL_FALSE, stride,
                              VertexAttribute(offsetof(GfxDrawVertex, normal)));
        glVertexAttribPointer(5u, 3, GL_FLOAT, GL_FALSE, stride,
                              VertexAttribute(offsetof(GfxDrawVertex, tangent)));
        glVertexAttribPointer(6u, 3, GL_FLOAT, GL_FALSE, stride,
                              VertexAttribute(offsetof(GfxDrawVertex, worldPosition)));
        glVertexAttribPointer(7u, 1, GL_FLOAT, GL_FALSE, stride,
                              VertexAttribute(offsetof(GfxDrawVertex, shadowVisibility)));
        }

        // Fixed-function fog is not applied to a user fragment program. The
        // source fog equation is evaluated in the native fragment pass from
        // the same reciprocal depth encoding used by the scalar renderer.
        const bool restoreFog = g_vitaGlFogEnabled;
        if (restoreFog && !reuseStaticMaterial)
            glDisable(GL_FOG);
        RecordDraw(indexCount);
        if(g_renderPassProbe.earlyAlpha) {
            GLint bound=0;glGetIntegerv(GL_CURRENT_PROGRAM,&bound);
            const bool actual=(g_nativeEarlyAlpha[0].program && GLuint(bound)==g_nativeEarlyAlpha[0].program)
                || (g_nativeEarlyAlpha[1].program && GLuint(bound)==g_nativeEarlyAlpha[1].program);
            const unsigned category=g_nativeStaticInstancedDraw?1u:0u;
            g_earlyAlphaEligible[category]+=earlyAlphaEligible;g_earlyAlphaActual[category]+=actual;
            if(actual!=(earlyAlphaEligible && g_renderPassProbe.phase==2))++g_earlyAlphaInvalid;
        }
        if(g_renderPipeline.weaponPipeline && g_nativeViewModelSpaceDraw) {
            const bool eligible=weaponEligible && NativeMaterialFamily(weaponEligible)==2;
            GLint bound=0;glGetIntegerv(GL_CURRENT_PROGRAM,&bound);
            const bool actual=g_nativeMaterialFamily[1].program && GLuint(bound)==g_nativeMaterialFamily[1].program;
            g_weaponFamilyEligible+=eligible;g_weaponFamilyActual+=actual;
            if(actual!=(eligible && (g_renderPassProbe.phase==2 || g_renderPipeline.weaponVertex)))++g_weaponFamilyInvalid;
        }
        if(g_renderPassProbe.worldComponents) {
            GLint bound=0;glGetIntegerv(GL_CURRENT_PROGRAM,&bound);unsigned actual=0;
            for(unsigned i=0;i<2;++i)if(g_nativeWorldComponents[i].program && GLuint(bound)==g_nativeWorldComponents[i].program)actual=i+1;
            const bool eligible=NativeWorldComponentEligible(worldAlphaEligible);
            g_worldComponentsActual+=actual!=0;g_worldComponentsEligible+=eligible;
            if(actual!=(eligible?GfxVitaWorldComponentVariant(g_renderPassProbe.phase):0))++g_worldComponentsInvalid;
            if(eligible)g_worldComponentsTargets|=4u;
        }
        if(g_renderPassProbe.worldGloss) {
            GLint bound=0;glGetIntegerv(GL_CURRENT_PROGRAM,&bound);
            const bool actual=g_nativeWorldGloss.program && GLuint(bound)==g_nativeWorldGloss.program;
            const bool eligible=NativeWorldGlossEligible(worldLifetimeEligible);
            g_worldGlossActual+=actual;g_worldGlossEligible+=eligible;
            if(actual!=(eligible && g_renderPassProbe.phase==2))++g_worldGlossInvalid;
            const unsigned target=g_materialPass.sourceSurfaceIndex==261?1u:g_materialPass.sourceSurfaceIndex==507?2u:g_materialPass.sourceSurfaceIndex==588?4u:0u;
            if(eligible)g_worldGlossTargets|=target;if(actual)g_worldGlossTargetActual|=target;
        }
        if(g_renderPassProbe.worldBasis) {
            GLint bound=0;glGetIntegerv(GL_CURRENT_PROGRAM,&bound);
            unsigned actual=0;for(unsigned i=0;i<2;++i)if(g_nativeWorldBasis[i].program && GLuint(bound)==g_nativeWorldBasis[i].program)actual=i+1;
            const unsigned family=GfxVitaWorldMathLane(g_nativeStaticBufferDraw && g_nativeWorldSpaceDraw,
                g_nativeViewModelSpaceDraw,g_nativeStaticInstancedDraw,worldLifetimeEligible,worldAlphaEligible);
            const unsigned eligible=g_worldBasisState.active?family:0;
            g_worldBasisActual+=actual!=0;g_worldBasisEligible+=eligible!=0;
            if(actual!=(g_renderPassProbe.phase==2?eligible:0))++g_worldBasisInvalid;
            const unsigned target=g_materialPass.sourceSurfaceIndex==261?1u:g_materialPass.sourceSurfaceIndex==507?2u:g_materialPass.sourceSurfaceIndex==588?4u:0u;
            if(family){g_worldBasisSeen|=target;g_worldBasisFamilyTriangles+=indexCount/3;}
            if(eligible){g_worldBasisTargets|=target;g_worldBasisTriangles+=indexCount/3;if(actual)g_worldBasisTargetActual|=target;}
        }
        if(g_renderPassProbe.worldPrecision) {
            GLint bound=0;glGetIntegerv(GL_CURRENT_PROGRAM,&bound);
            unsigned actual=0;for(unsigned i=0;i<2;++i)if(g_nativeWorldPrecision[i].program && GLuint(bound)==g_nativeWorldPrecision[i].program)actual=i+1;
            const unsigned eligible=GfxVitaWorldPrecisionLane(g_nativeStaticBufferDraw && g_nativeWorldSpaceDraw,
                g_nativeViewModelSpaceDraw,g_nativeStaticInstancedDraw,worldLifetimeEligible,worldAlphaEligible);
            g_worldPrecisionActual+=actual!=0;g_worldPrecisionEligible+=eligible!=0;
            if(actual!=(g_renderPassProbe.phase==2?eligible:0))++g_worldPrecisionInvalid;
            const unsigned target=g_materialPass.sourceSurfaceIndex==261?1u:g_materialPass.sourceSurfaceIndex==507?2u:g_materialPass.sourceSurfaceIndex==588?4u:0u;
            if(eligible){g_worldPrecisionTargets|=target;if(actual)g_worldPrecisionTargetActual|=target;}
        }
        if(g_renderPassProbe.worldMath) {
            GLint bound=0;glGetIntegerv(GL_CURRENT_PROGRAM,&bound);
            unsigned actual=0;for(unsigned i=0;i<2;++i)if(g_nativeWorldMath[i].program && GLuint(bound)==g_nativeWorldMath[i].program)actual=i+1;
            const unsigned eligible=GfxVitaWorldMathLane(g_nativeStaticBufferDraw && g_nativeWorldSpaceDraw,
                g_nativeViewModelSpaceDraw,g_nativeStaticInstancedDraw,worldLifetimeEligible,worldAlphaEligible);
            g_worldMathActual+=actual!=0;g_worldMathEligible+=eligible!=0;
            if(actual!=(g_renderPassProbe.phase==2?eligible:0))++g_worldMathInvalid;
            const unsigned target=g_materialPass.sourceSurfaceIndex==261?1u:g_materialPass.sourceSurfaceIndex==507?2u:g_materialPass.sourceSurfaceIndex==588?4u:0u;
            if(eligible){g_worldMathTargets|=target;if(actual)g_worldMathTargetActual|=target;}
        }
        if(g_renderPassProbe.worldLifetime) {
            GLint bound=0;glGetIntegerv(GL_CURRENT_PROGRAM,&bound);
            const bool actual=g_nativeWorldLifetime.program && GLuint(bound)==g_nativeWorldLifetime.program;
            g_worldLifetimeActual+=actual;g_worldLifetimeEligible+=worldLifetimeEligible;
            if(actual!=(worldLifetimeEligible && g_renderPassProbe.phase==2))++g_worldLifetimeInvalid;
            const unsigned target=g_materialPass.sourceSurfaceIndex==261?1u:g_materialPass.sourceSurfaceIndex==507?2u:0u;
            if(worldLifetimeEligible){g_worldLifetimeTargets|=target;if(actual)g_worldLifetimeTargetActual|=target;}
        }
        if(g_renderPipeline.worldAlpha) {
            GLint bound=0;glGetIntegerv(GL_CURRENT_PROGRAM,&bound);
            const bool actual=(g_nativeWorldComponents[0].program && GLuint(bound)==g_nativeWorldComponents[0].program)
                || (g_nativeWorldComponents[1].program && GLuint(bound)==g_nativeWorldComponents[1].program)
                || (g_nativeWorldAlpha.program && GLuint(bound)==g_nativeWorldAlpha.program)
                || (g_renderPassProbe.worldPrecision && g_nativeWorldPrecision[1].program && GLuint(bound)==g_nativeWorldPrecision[1].program)
                || ((g_renderPassProbe.worldScaling || g_renderPipeline.ordinary) && g_nativeCheapWorld[1].program && GLuint(bound)==g_nativeCheapWorld[1].program)
                || (g_renderPassProbe.worldMath && g_nativeWorldMath[1].program && GLuint(bound)==g_nativeWorldMath[1].program)
                || (g_renderPassProbe.worldBasis && g_nativeWorldBasis[1].program && GLuint(bound)==g_nativeWorldBasis[1].program);
            g_worldAlphaEligible+=worldAlphaEligible;g_worldAlphaActual+=actual;
            if(actual!=(worldAlphaEligible && g_renderPipeline.worldAlphaActive))++g_worldAlphaInvalid;
            if(worldAlphaEligible && g_materialPass.sourceSurfaceIndex==588) {
                ++g_worldAlphaTargetEligible;g_worldAlphaTargetActual+=actual;
            }
        }
        if(g_renderPipeline.weaponSpecular && g_nativeViewModelSpaceDraw) {
            const bool eligible=NativeWeaponSpecularEligible(weaponEligible);
            GLint bound=0;glGetIntegerv(GL_CURRENT_PROGRAM,&bound);
            const bool actual=g_nativeWeaponSpecular.program && GLuint(bound)==g_nativeWeaponSpecular.program;
            g_weaponSpecularEligible+=eligible;g_weaponSpecularActual+=actual;
            if(actual!=(eligible && (g_renderPassProbe.phase==2 || g_renderPipeline.worldAlpha)))++g_weaponSpecularInvalid;
            if(eligible) {
                g_weaponSpecularNormalMaps+=g_materialPass.normalTexture.value!=0;
                g_weaponSpecularMaps+=g_materialPass.specularTexture.value!=0;
            }
        }
        if(g_renderPassProbe.weaponOpaque && g_nativeViewModelSpaceDraw) {
            GLint bound=0;glGetIntegerv(GL_CURRENT_PROGRAM,&bound);
            const bool opaque=(g_nativeOpaqueFullShader.program && GLuint(bound)==g_nativeOpaqueFullShader.program)
                || (g_nativeOpaqueSunShader.program && GLuint(bound)==g_nativeOpaqueSunShader.program);
            g_weaponOpaqueEligible+=weaponEligible;g_weaponOpaqueActual+=opaque;
            if(opaque && (!weaponEligible || g_renderPassProbe.phase!=2))++g_weaponOpaqueInvalid;
            if(weaponEligible) {
                g_weaponNormalDraws+=g_materialPass.normalTexture.value!=0;
                g_weaponSpecularDraws+=g_materialPass.specularTexture.value!=0;
            }
        }
        SubmitNativeIndexedElements(indexCount, indices);
        if (g_nativeStaticBufferDraw) {
            g_nativeStaticPipelineActive = true;
            g_nativeStaticMaterialRevision = g_materialPassRevision;
            g_nativeStaticLayoutLane=NativeUseLightmapLayout();
        } else {
            if (restoreFog)
                glEnable(GL_FOG);
            for (GLuint attribute = 0u; attribute < 8u; ++attribute)
                glDisableVertexAttribArray(attribute);
            glUseProgram(0);
            ResetExtraFixedFunctionTextureUnits();
        }
        return;
    }

    // DrawIndexedLightmapped is a complete source lit pass, not just a
    // texture-unit update.  Rebind the position and colour arrays here as
    // well: the preceding sky/HUD/model pass may have left client pointers
    // aimed at a different vertex stream, which otherwise makes the primary
    // lightmap modulate stale colours on VitaGL.
    glVertexPointer(3, GL_FLOAT, static_cast<GLsizei>(sizeof(GfxDrawVertex)),
                    vertices->position);
    glColorPointer(4, GL_FLOAT, static_cast<GLsizei>(sizeof(GfxDrawVertex)),
                   vertices->color);

    // VitaGL exposes the fixed-function multitexture entry points. Texture 0
    // carries the material color map; the authored primary and secondary
    // lightmaps follow in source sampler order. This keeps both map payloads
    // in the draw even while the eventual Cg lit-family shader replaces this
    // fixed-function bridge with KisakCOD's exact pixel equation.
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, NativeTexture(texture.value));
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glClientActiveTexture(GL_TEXTURE0);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, static_cast<GLsizei>(sizeof(GfxDrawVertex)),
                      vertices->texCoord);

    // R_DrawBspDrawSurfsLit/R_SetLightmap use source sampler state 0x62 for
    // both lightmap channels: linear filtering, no mip chain, clamp U/V.
    uint32_t lightmapUnit = 1u;
    const auto BindLightmap = [&](uint32_t unit, GfxTextureHandle handle) {
        const GLenum textureUnit = unit == 1u ? GL_TEXTURE1 : GL_TEXTURE2;
        glActiveTexture(textureUnit);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, NativeTexture(handle.value));
        SetSamplerState(unit, 0x62u);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glClientActiveTexture(textureUnit);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, static_cast<GLsizei>(sizeof(GfxDrawVertex)),
                          vertices->lightmapCoord);
    };
    if (usePrimary)
        BindLightmap(lightmapUnit++, lightmapPrimaryTexture);
    if (useSecondary)
        BindLightmap(lightmapUnit++, lightmapSecondaryTexture);

    RecordDraw(indexCount);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indexCount), GL_UNSIGNED_SHORT,
                   indices);

    ResetExtraFixedFunctionTextureUnits();
    glTexCoordPointer(2, GL_FLOAT, static_cast<GLsizei>(sizeof(GfxDrawVertex)),
                      vertices->texCoord);
}

GfxBufferHandle CreateStaticVertexBufferInternal(
    const GfxDrawVertex* vertices, uint32_t sourceVertexCount,
    const uint32_t* sourceIndices, uint32_t vertexCount) {
    GfxBufferHandle result{0};
    if (g_software || !g_initialized || !vertices || vertexCount == 0
        || sourceVertexCount == 0
        || vertexCount > UINT32_MAX / sizeof(StaticWorldVertex))
        return result;
    // Allocate the immutable VitaGL buffer first and pack directly into its
    // storage.  Crossfire's source-facing vertex stream is about twenty MiB;
    // materializing a second packed vector here raised the map-load high-water
    // by another eleven MiB and could exhaust newlib after the source runtime
    // had been rehydrated.  A newly-created static buffer has never been
    // submitted, so VitaGL's otherwise-unsynchronised map implementation is
    // safe at this one load-time boundary.
    for (uint32_t index = 0; index < vertexCount; ++index) {
        const uint32_t sourceIndex = sourceIndices ? sourceIndices[index] : index;
        if (sourceIndex >= sourceVertexCount)
            return result;
    }
    const uint64_t requestedBytes = static_cast<uint64_t>(vertexCount)
        * sizeof(StaticWorldVertex);
    const bool traceLargeAllocation = requestedBytes >= 4u * 1024u * 1024u;
    if (traceLargeAllocation) {
        sceClibPrintf(
            "[kisak-vita] static vertex allocation begin vertices=%u bytes=%llu textures=%llu buffers=%llu\n",
            static_cast<unsigned>(vertexCount),
            static_cast<unsigned long long>(requestedBytes),
            static_cast<unsigned long long>(g_nativeTextureBytes),
            static_cast<unsigned long long>(g_nativeStaticBufferBytes));
    }
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    if (buffer == 0) {
        if (traceLargeAllocation)
            sceClibPrintf(
                "[kisak-vita] static vertex allocation failed stage=gen error=0x%04x\n",
                static_cast<unsigned>(glGetError()));
        return result;
    }
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertexCount * sizeof(StaticWorldVertex)),
                 nullptr, GL_STATIC_DRAW);
    const GLenum allocationError = glGetError();
    if (allocationError != GL_NO_ERROR) {
        if (traceLargeAllocation)
            sceClibPrintf(
                "[kisak-vita] static vertex allocation failed stage=data buffer=%u error=0x%04x\n",
                static_cast<unsigned>(buffer),
                static_cast<unsigned>(allocationError));
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDeleteBuffers(1, &buffer);
        return result;
    }
    auto* packed = static_cast<StaticWorldVertex*>(
        glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY));
    const GLenum mapError = glGetError();
    if (!packed || mapError != GL_NO_ERROR) {
        if (traceLargeAllocation)
            sceClibPrintf(
                "[kisak-vita] static vertex allocation failed stage=map buffer=%u pointer=%p error=0x%04x\n",
                static_cast<unsigned>(buffer), packed,
                static_cast<unsigned>(mapError));
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDeleteBuffers(1, &buffer);
        return result;
    }
    const auto PackSnorm = GfxVitaWorldBasisSnorm;
    const auto PackUnorm8 = [](float value) -> uint8_t {
        const float finite = std::isfinite(value) ? value : 0.0f;
        return static_cast<uint8_t>(std::lround(
            std::clamp(finite, 0.0f, 1.0f) * 255.0f));
    };
    for (uint32_t index = 0; index < vertexCount; ++index) {
        const uint32_t sourceIndex = sourceIndices ? sourceIndices[index] : index;
        const GfxDrawVertex& source = vertices[sourceIndex];
        StaticWorldVertex& destination = packed[index];
        std::memcpy(destination.worldPosition, source.worldPosition,
                    sizeof(destination.worldPosition));
        std::memcpy(destination.texCoord, source.texCoord,
                    sizeof(destination.texCoord));
        std::memcpy(destination.lightmapCoord, source.lightmapCoord,
                    sizeof(destination.lightmapCoord));
        for (uint32_t component = 0; component < 3u; ++component) {
            destination.normal[component] = PackSnorm(source.normal[component]);
            destination.tangent[component] = PackSnorm(source.tangent[component]);
        }
        destination.normal[3] = 0;
        destination.tangent[3] = 0;
        for (uint32_t component = 0; component < 4u; ++component)
            destination.color[component] = PackUnorm8(source.color[component]);
        destination.shadowVisibility = static_cast<uint16_t>(std::lround(
            std::clamp(std::isfinite(source.shadowVisibility)
                           ? source.shadowVisibility : 1.0f,
                       0.0f, 1.0f) * 65535.0f));
        destination.padding = 0;
    }
    const GLboolean unmapped = glUnmapBuffer(GL_ARRAY_BUFFER);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (unmapped != GL_TRUE || glGetError() != GL_NO_ERROR) {
        if (traceLargeAllocation)
            sceClibPrintf(
                "[kisak-vita] static vertex allocation failed stage=unmap buffer=%u unmapped=%u\n",
                static_cast<unsigned>(buffer),
                static_cast<unsigned>(unmapped));
        glDeleteBuffers(1, &buffer);
        return result;
    }
    result.value = buffer;
    if (!TrackNativeResource(&g_nativeBuffers, result.value,
                        vertexCount * static_cast<uint32_t>(sizeof(StaticWorldVertex)),
                        &g_nativeStaticBufferBytes)) {
        glDeleteBuffers(1, &buffer);
        return {0};
    }
    if (traceLargeAllocation) {
        sceClibPrintf(
            "[kisak-vita] static vertex allocation ready buffer=%u bytes=%llu buffers=%llu\n",
            static_cast<unsigned>(buffer),
            static_cast<unsigned long long>(requestedBytes),
            static_cast<unsigned long long>(g_nativeStaticBufferBytes));
    }
    return result;
}

GfxBufferHandle CreateStaticVertexBuffer(const GfxDrawVertex* vertices,
                                         uint32_t vertexCount) {
    return CreateStaticVertexBufferInternal(
        vertices, vertexCount, nullptr, vertexCount);
}

GfxBufferHandle CreateStaticVertexBufferRemapped(
    const GfxDrawVertex* sourceVertices, uint32_t sourceVertexCount,
    const uint32_t* sourceIndices, uint32_t outputVertexCount) {
    if (!sourceIndices)
        return GfxBufferHandle{0};
    return CreateStaticVertexBufferInternal(
        sourceVertices, sourceVertexCount, sourceIndices, outputVertexCount);
}

GfxBufferHandle CreateInstancedModelVertexBuffer(const GfxDrawVertex* vertices,
                                                 uint32_t vertexCount) {
    if (g_software || !g_initialized || !vertices || !vertexCount
        || vertexCount > UINT32_MAX / sizeof(VitaStaticModelVertex))
        return {0};
    const uint32_t bytes = vertexCount * sizeof(VitaStaticModelVertex);
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    if (!buffer) return {0};
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, bytes, nullptr, GL_STATIC_DRAW);
    bool valid = glGetError() == GL_NO_ERROR;
    auto* packed = valid ? static_cast<VitaStaticModelVertex*>(
        glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY)) : nullptr;
    valid = valid && packed && glGetError() == GL_NO_ERROR;
    if (packed) {
        for (uint32_t index = 0; valid && index < vertexCount; ++index)
            valid = kisak::vita::VitaPackStaticModelVertex(vertices[index],
                                                          &packed[index]);
        const bool unmapped = glUnmapBuffer(GL_ARRAY_BUFFER) == GL_TRUE;
        valid = valid && unmapped && glGetError() == GL_NO_ERROR;
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (!valid || !TrackNativeResource(&g_nativeBuffers, buffer, bytes,
                                       &g_nativeStaticBufferBytes)) {
        glDeleteBuffers(1, &buffer);
        return {0};
    }
    return {buffer};
}

GfxBufferHandle CreateStaticIndexBuffer(const uint16_t* indices,
                                        uint32_t indexCount) {
    GfxBufferHandle result{0};
    if (g_software || !g_initialized || !indices || indexCount < 3
        || indexCount > UINT32_MAX / sizeof(uint16_t))
        return result;
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    if (buffer == 0)
        return result;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(indexCount * sizeof(uint16_t)),
                 indices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteBuffers(1, &buffer);
        return result;
    }
    result.value = buffer;
    if (!TrackNativeResource(&g_nativeBuffers, result.value,
                        indexCount * static_cast<uint32_t>(sizeof(uint16_t)),
                        &g_nativeStaticBufferBytes)) {
        glDeleteBuffers(1, &buffer);
        return {0};
    }
    return result;
}

GfxBufferHandle CreateStaticInstanceBuffer(
    const GfxStaticModelInstance* instances, uint32_t instanceCount) {
    GfxBufferHandle result{0};
    if (g_software || !g_initialized || !instances || instanceCount == 0
        || instanceCount > UINT32_MAX / sizeof(GfxStaticModelInstance))
        return result;
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    if (buffer == 0)
        return result;
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(instanceCount
                                * sizeof(GfxStaticModelInstance)),
        instances, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteBuffers(1, &buffer);
        return result;
    }
    result.value = buffer;
    if (!TrackNativeResource(
        &g_nativeBuffers, result.value,
        instanceCount * static_cast<uint32_t>(sizeof(GfxStaticModelInstance)),
        &g_nativeStaticBufferBytes)) {
        glDeleteBuffers(1, &buffer);
        return {0};
    }
    return result;
}

void DeleteNativePoseExtra(uint32_t handle) {
    ForgetNativeResource(&g_nativeBuffers,handle,&g_nativeStaticBufferBytes);
    const GLuint buffer=handle;glDeleteBuffers(1,&buffer);
}
GfxBufferHandle CreateDynamicVertexBuffer(uint32_t vertexCapacity) {
    GfxBufferHandle result{0};
    if (g_software || !g_initialized || vertexCapacity == 0
        || vertexCapacity > UINT32_MAX / sizeof(DynamicViewModelVertex))
        return result;
    EndNativeStaticPipeline();
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    if (buffer == 0)
        return result;
    const uint32_t bytes = vertexCapacity
        * static_cast<uint32_t>(sizeof(DynamicViewModelVertex));
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr,
                 GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (glGetError() != GL_NO_ERROR
        || !TrackNativeResource(&g_nativeBuffers, buffer, bytes,
                                &g_nativeStaticBufferBytes)) {
        glDeleteBuffers(1, &buffer);
        return result;
    }
    // Renderer-owned packing scratch is shared across dynamic buffers. Grow
    // while resources are staged, retaining the largest live staging size.
#if KISAK_VITA_CPU_RUNTIME
    if (!g_dynamicViewModelUpload.Reserve(vertexCapacity)) {
        ForgetNativeResource(&g_nativeBuffers, buffer, &g_nativeStaticBufferBytes);
        glDeleteBuffers(1, &buffer);
        return result;
    }
    kisak::vita::VitaCpuProfile_Counter("render-pose-scratch-bytes",
        g_dynamicViewModelUpload.size()*sizeof(DynamicViewModelVertex));
    const bool ringReady=g_poseBuffers.Reserve(buffer,bytes,[](uint32_t size)->uint32_t {
        GLuint extra=0;glGenBuffers(1,&extra);if(!extra)return 0;
        glBindBuffer(GL_ARRAY_BUFFER,extra);
        glBufferData(GL_ARRAY_BUFFER,size,nullptr,GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER,0);
        if(glGetError()!=GL_NO_ERROR || !TrackNativeResource(&g_nativeBuffers,extra,size,&g_nativeStaticBufferBytes)) {
            glDeleteBuffers(1,&extra);return 0;
        }
        return extra;
    },DeleteNativePoseExtra);
    kisak::vita::VitaCpuProfile_Counter("pose-buffer-ring-ready",ringReady,bytes);
    kisak::vita::VitaCpuProfile_Counter("pose-buffer-ring-memory",g_poseBuffers.ExtraBytes(),g_poseBuffers.ShadowBytes());
#endif
    result.value = buffer;
    return result;
}

// Callers validate handle, pointer and byte capacity once before reaching here.
bool UploadPackedViewmodel(GfxBufferHandle vertexBuffer,const DynamicViewModelVertex* vertices,uint32_t bytes,uint32_t capacity) {
    static uint32_t failureTraceCount=0;
    GfxVitaPoseBuffers::Upload upload{vertexBuffer.value,bytes,vertices};
#if KISAK_VITA_CPU_RUNTIME
    { VITA_CPU_SCOPE("render-pose-ring-stage");upload=g_poseBuffers.Prepare(vertexBuffer.value,vertices,bytes,g_poseSwapSerial); }
    if(!upload.handle)return false;
    ++g_poseRingUploads;g_poseRingFallbacks+=upload.fallback;g_poseRingRotations+=upload.rotated;
#endif
    GLenum error;
    {
    VITA_CPU_SCOPE("render-pose-buffer-update");
    { VITA_CPU_SCOPE("render-pose-end-pipeline"); EndNativeStaticPipeline(); }
    // GL errors are sticky and the optional texture staging path can leave a
    // recoverable allocation/format error behind. Do not misattribute that
    // stale state to this upload and route a successful persistent update
    // back through the catastrophic client-array fallback. Drain before the
    // operation, then inspect only errors produced by this update.
    { VITA_CPU_SCOPE("render-pose-stale-errors"); while (glGetError() != GL_NO_ERROR) {} }
    { VITA_CPU_SCOPE("render-pose-bind"); glBindBuffer(GL_ARRAY_BUFFER, upload.handle); }
    const uint64_t uploadStart=sceKernelGetProcessTimeWide();
    { VITA_CPU_SCOPE("render-pose-subdata");
        glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(upload.bytes),upload.data);
    }
    const uint64_t uploadUs=sceKernelGetProcessTimeWide()-uploadStart;
    { VITA_CPU_SCOPE("render-pose-unbind-check");
        glBindBuffer(GL_ARRAY_BUFFER, 0); error = glGetError();
    }
#if KISAK_VITA_CPU_RUNTIME
    // The physical ring still uses SubData's reader protection. No mapped
    // overwrite, new fence or deletion of live buffers occurs here.
    kisak::vita::VitaCpuProfile_Counter("render-pose-upload-bytes",bytes,capacity);
    if(uploadUs>10000)kisak::vita::VitaCpuProfile_Counter("render-pose-slow-subdata",uploadUs,vertexBuffer.value);
#endif
    }
    if (error != GL_NO_ERROR && failureTraceCount < 4u) {
        sceClibPrintf(
            "[kisak-vita] dynamic vertex update GL failure handle=%u bytes=%u capacity=%u gl=0x%04x\n",
            static_cast<unsigned>(vertexBuffer.value),
            static_cast<unsigned>(bytes), static_cast<unsigned>(capacity),
            static_cast<unsigned>(error));
        ++failureTraceCount;
    }
    return error == GL_NO_ERROR;
}

bool UpdateDynamicVertexBuffer(GfxBufferHandle vertexBuffer,
                               const GfxDrawVertex* vertices,
                               uint32_t vertexCount) {
    g_poseLightBounds.Forget(vertexBuffer.value);
    static uint32_t failureTraceCount = 0u;
    if (g_software || !g_initialized || vertexBuffer.value == 0 || !vertices
        || vertexCount == 0
        || vertexCount > UINT32_MAX / sizeof(DynamicViewModelVertex)) {
        if (failureTraceCount < 4u) {
            sceClibPrintf(
                "[kisak-vita] dynamic vertex update rejected handle=%u vertices=%u software=%u initialized=%u\n",
                static_cast<unsigned>(vertexBuffer.value),
                static_cast<unsigned>(vertexCount), g_software ? 1u : 0u,
                g_initialized ? 1u : 0u);
            ++failureTraceCount;
        }
        return false;
    }
    const uint32_t bytes = vertexCount
        * static_cast<uint32_t>(sizeof(DynamicViewModelVertex));
    const uint32_t capacity = NativeResourceSize(
        g_nativeBuffers, vertexBuffer.value);
    if (bytes > capacity) {
        if (failureTraceCount < 4u) {
            sceClibPrintf(
                "[kisak-vita] dynamic vertex update exceeds capacity handle=%u bytes=%u capacity=%u\n",
                static_cast<unsigned>(vertexBuffer.value),
                static_cast<unsigned>(bytes), static_cast<unsigned>(capacity));
            ++failureTraceCount;
        }
        return false;
    }
    {
    VITA_CPU_SCOPE("render-pose-scratch");
#if KISAK_VITA_CPU_RUNTIME
    // Every live buffer reserved its complete packing workspace at creation.
    // Unexpected oversized updates fail rather than allocate during gameplay.
    if (g_dynamicViewModelUpload.size() < vertexCount)
        return false;
#else
    if (g_dynamicViewModelUpload.size() < vertexCount)
        g_dynamicViewModelUpload.resize(vertexCount);
#endif
    }
#if KISAK_VITA_CPU_RUNTIME
    const auto PackSnorm = kisak::vita::PackVertexSnorm;
    const auto PackUnorm8 = kisak::vita::PackVertexUnorm8;
#else
    const auto PackSnorm = GfxVitaWorldBasisSnorm;
    const auto PackUnorm8 = [](float value) -> uint8_t {
        const float finite = std::isfinite(value) ? value : 0.0f;
        return static_cast<uint8_t>(std::lround(
            std::clamp(finite, 0.0f, 1.0f) * 255.0f));
    };
#endif
    {
    VITA_CPU_SCOPE("render-pose-pack");
    for (uint32_t index = 0u; index < vertexCount; ++index) {
        const GfxDrawVertex& source = vertices[index];
        DynamicViewModelVertex& destination =
            g_dynamicViewModelUpload[index];
        std::memcpy(destination.position, source.position,
                    sizeof(destination.position));
        std::memcpy(destination.texCoord, source.texCoord,
                    sizeof(destination.texCoord));
        std::memcpy(destination.worldPosition, source.worldPosition,
                    sizeof(destination.worldPosition));
        for (uint32_t component = 0u; component < 3u; ++component) {
            destination.normal[component] = PackSnorm(
                source.normal[component]);
            destination.tangent[component] = PackSnorm(
                source.tangent[component]);
        }
        destination.normal[3] = 0;
        destination.tangent[3] = 0;
        for (uint32_t component = 0u; component < 4u; ++component)
            destination.color[component] = PackUnorm8(
                source.color[component]);
    }
    }
    const bool uploaded=UploadPackedViewmodel(vertexBuffer,g_dynamicViewModelUpload.data(),bytes,capacity);
    if(uploaded)g_poseLightBounds.Store(vertexBuffer.value,g_dynamicViewModelUpload.data(),vertexCount);
    return uploaded;
}

void DestroyBuffer(GfxBufferHandle buffer) {
    g_poseLightBounds.Forget(buffer.value);
    if (g_software || buffer.value == 0)
        return;
    EndNativeStaticPipeline();
    g_poseBuffers.ReleaseExtra(buffer.value,DeleteNativePoseExtra);
    ForgetNativeResource(&g_nativeBuffers, buffer.value,
                         &g_nativeStaticBufferBytes);
    const GLuint nativeBuffer = buffer.value;
    glDeleteBuffers(1, &nativeBuffer);
}

void SetWorldViewProjection(const float matrix[16], float nearPlane) {
    if (!matrix || !std::isfinite(nearPlane) || !(nearPlane > 0.0f))
        return;
    // The transform is part of UploadNativeLitUniforms.  A caller may update
    // it without changing materials, so terminate a live cache before storing
    // the new matrix rather than rendering with the previous camera.
    EndNativeStaticPipeline();
    std::memcpy(g_nativeWorldViewProjection, matrix,
                sizeof(g_nativeWorldViewProjection));
    g_nativeWorldNearPlane = nearPlane;
    // Original R_ChangeDepthRange reserves the nearest 1/64 of the depth
    // buffer for first-person geometry and maps the world into the rest.
    glDepthRangef(0.015625f, 1.0f);
}

void SetDrawLightBounds(const GfxDrawLightBounds* bounds) {
    g_drawLightBounds=bounds?*bounds:GfxDrawLightBounds{};
}

bool DrawStaticIndexed(GfxBufferHandle vertexBuffer,
                       uint32_t firstVertex, uint32_t vertexCount,
                       GfxBufferHandle indexBuffer,
                       uint32_t firstIndex, uint32_t indexCount) {
    GfxVitaWorldBasisScope basisScope(g_worldBasisState,g_nativeStaticInstancedDraw);
    const unsigned lightLane=g_nativeStaticInstancedDraw?1u:0u;
    const bool hadLocalLights=g_rawLocalLightLoops;
    const bool lightBoundsValid=g_drawLightBounds.valid;
    GfxVitaDrawLocalLightScope lightScope(g_rawLocalLightLoops,g_drawLightBounds,g_frameLighting,
        g_adaptiveLighting && (g_frameLighting.flags&GFX_FRAME_LIGHTING_VALID) && !g_software);
    if(hadLocalLights) {
        ++g_localLightBounded[lightLane];
        g_localLightCulled[lightLane]+=!g_rawLocalLightLoops;
        g_localLightMissing[lightLane]+=!lightBoundsValid;
    }
    const uint32_t vertexStride = g_nativeStaticInstancedDraw
        ? sizeof(VitaStaticModelVertex) : sizeof(StaticWorldVertex);
    if (g_software || g_nativeLitShader.program == 0
        || vertexBuffer.value == 0 || indexBuffer.value == 0
        || vertexCount == 0 || indexCount < 3
        || firstVertex > UINT32_MAX / vertexStride
        || firstIndex > UINT32_MAX / sizeof(uint16_t))
        return false;

    if(!g_nativeStaticInstancedDraw || !g_propGroupOpen || !g_renderPassProbe.SuppressProps())
        ObserveGeometryViewport(g_nativeStaticInstancedDraw ? 1u : 0u);
    // Switching programs retires the preceding static pipeline and unbinds
    // its buffers. Resolve the exact draw specialization BEFORE binding this
    // draw's vertex/index buffers; its later lit-path selection is identical.
    SelectNativeLightShader(!SourceTechniqueIsUnlit(g_materialPass)
        && (g_materialPass.normalTexture.value || g_materialPass.specularTexture.value
            || g_materialPass.reflectionProbeTexture.value)
        && GfxVitaPropShaderEligible(g_nativeStaticInstancedDraw,true,false,
            (g_materialPass.techniqueFlags0&GFX_MATERIAL_PASS_BAKED_LIGHTING)!=0,
            (g_materialPass.lightmapSamplerFlags&0x02u) && g_materialPass.lightmapPrimaryTexture.value,
            (g_materialPass.lightmapSamplerFlags&0x04u) && g_materialPass.lightmapSecondaryTexture.value,
            g_materialPass.colorTexture.value!=0,g_rawLocalLightLoops,g_adaptiveLighting,
            g_nativeHud,g_materialPass.surfaceMode==GfxSurfaceMode::Sky,g_benchmarkShaderMode),NativeOpaqueEligible(true,false),NativeFastColorEligible(true),false,false,NativeEarlyAlphaEligible(true,false),NativeWorldAlphaEligible(true,false),NativeWorldLifetimeEligible(true,false));
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer.value);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer.value);
#if KISAK_VITA_PROFILE
    if (g_nativeCapture.pending && g_nativeCapture.inputContracts && !g_nativeCapture.replaying
        && g_materialPass.sourceStateValid && !g_nativeStaticInstancedDraw) {
        auto& capture = g_nativeCapture;
        if (capture.worldDraws.size() < 4096)
            capture.worldDraws.push_back({vertexBuffer, indexBuffer, firstVertex, vertexCount, firstIndex, indexCount, g_materialPass, g_materialPassRevision});
        if (!capture.capturedWorldBuffer) {
            capture.capturedWorldBuffer = vertexBuffer.value;
            const auto Dump = [](GLenum target, const char* path) {
                GLint bytes = 0;
                glGetBufferParameteriv(target, GL_BUFFER_SIZE, &bytes);
                if (bytes <= 0 || bytes > 24 * 1024 * 1024) return;
                const void* data = glMapBuffer(target, GL_READ_ONLY);
                if (data) {
                    if (FILE* file = std::fopen(path, "wb")) {
                        std::fwrite(data, 1, bytes, file);
                        std::fclose(file);
                    }
                    glUnmapBuffer(target);
                }
            };
            Dump(GL_ARRAY_BUFFER, "ux0:data/CODVIT001/logs/native-world-vertices.bin");
            Dump(GL_ELEMENT_ARRAY_BUFFER, "ux0:data/CODVIT001/logs/native-world-indices.bin");
        }
        if (capture.capturedWorldBuffer == vertexBuffer.value) {
            if (FILE* file = std::fopen("ux0:data/CODVIT001/logs/native-world-draws.csv", "ab")) {
                std::fprintf(file, "%u,%u,%u,%u,%u,%u,%u,%u\n", firstVertex, vertexCount,
                    firstIndex, indexCount, g_materialPass.sourceStateBits0, g_materialPass.sourceStateBits1,
                    g_materialPass.colorTexture.value, g_materialPass.sourceSurfaceIndex);
                std::fclose(file);
            }
        }
    }
#endif
    g_nativeStaticBufferDraw = true;
    g_nativeWorldSpaceDraw = true;
    const auto* vertices = reinterpret_cast<const GfxDrawVertex*>(
        static_cast<uintptr_t>(firstVertex * vertexStride));
    const auto* indices = reinterpret_cast<const uint16_t*>(
        static_cast<uintptr_t>(firstIndex * sizeof(uint16_t)));
    const bool hasPrimary = (g_materialPass.lightmapSamplerFlags & 0x02u) != 0
        && g_materialPass.lightmapPrimaryTexture.value != 0;
    const bool hasSecondary = (g_materialPass.lightmapSamplerFlags & 0x04u) != 0
        && g_materialPass.lightmapSecondaryTexture.value != 0;
    const bool hasSourceLitInputs = g_materialPass.colorTexture.value != 0
        && (hasPrimary || hasSecondary
            || g_materialPass.normalTexture.value != 0
            || g_materialPass.specularTexture.value != 0
            || g_materialPass.reflectionProbeTexture.value != 0);
    if (hasSourceLitInputs) {
        DrawIndexedLightmapped(
            vertices, vertexCount, indices, indexCount,
            g_materialPass.colorTexture,
            g_materialPass.lightmapPrimaryTexture,
            g_materialPass.lightmapSecondaryTexture,
            g_materialPass.reflectionProbeTexture,
            g_materialPass.lightmapSamplerFlags);
    } else {
        DrawIndexed(vertices, vertexCount, indices, indexCount);
    }
    g_nativeWorldSpaceDraw = false;
    g_nativeStaticBufferDraw = false;
    if (!g_nativeStaticPipelineActive) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    return true;
}

bool DrawStaticIndexedInstanced(
    GfxBufferHandle vertexBuffer,
    uint32_t firstVertex, uint32_t vertexCount,
    GfxBufferHandle indexBuffer,
    uint32_t firstIndex, uint32_t indexCount,
    GfxBufferHandle instanceBuffer,
    uint32_t firstInstance, uint32_t instanceCount) {
    if (g_software || !g_initialized || instanceBuffer.value == 0
        || instanceCount == 0
        || firstInstance > UINT32_MAX / sizeof(GfxStaticModelInstance)) {
        g_drawLightBounds={};
        return false;
    }

    const uintptr_t base = static_cast<uintptr_t>(firstInstance)
        * sizeof(GfxStaticModelInstance);
    const GLsizei stride = static_cast<GLsizei>(
        sizeof(GfxStaticModelInstance));
    glBindBuffer(GL_ARRAY_BUFFER, instanceBuffer.value);
    const auto InstanceAttribute = [base](size_t offset) {
        return reinterpret_cast<const void*>(base + offset);
    };
    struct AttributeBinding {
        GLuint index;
        size_t offset;
    };
    const AttributeBinding bindings[] = {
        {8u, offsetof(GfxStaticModelInstance, rotation)},
        {9u, offsetof(GfxStaticModelInstance, originScale)},
        {10u, offsetof(GfxStaticModelInstance, lightPositiveX)},
        {11u, offsetof(GfxStaticModelInstance, lightNegativeX)},
        {12u, offsetof(GfxStaticModelInstance, lightPositiveY)},
        {13u, offsetof(GfxStaticModelInstance, lightNegativeY)},
        {14u, offsetof(GfxStaticModelInstance, lightPositiveZ)},
        {15u, offsetof(GfxStaticModelInstance, lightNegativeZ)},
    };
    for (const AttributeBinding& binding : bindings) {
        glEnableVertexAttribArray(binding.index);
        glVertexAttribPointer(binding.index, 4, GL_FLOAT, GL_FALSE, stride,
                              InstanceAttribute(binding.offset));
        glVertexAttribDivisor(binding.index, 1u);
    }

    g_nativeStaticInstancedDraw = true;
    g_nativeStaticInstanceCount = instanceCount;
    const bool submitted = DrawStaticIndexed(
        vertexBuffer, firstVertex, vertexCount, indexBuffer, firstIndex,
        indexCount);
    g_nativeStaticInstancedDraw = false;
    g_nativeStaticInstanceCount = 1u;
    for (const AttributeBinding& binding : bindings) {
        glVertexAttribDivisor(binding.index, 0u);
        glDisableVertexAttribArray(binding.index);
    }
    if (!g_nativeStaticPipelineActive)
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    return submitted;
}

#if KISAK_VITA_PROFILE
void CaptureNativeSceneFrame(const char* stage);
#endif
bool DrawDynamicIndexed(GfxBufferHandle vertexBuffer,
                        uint32_t firstVertex, uint32_t vertexCount,
                        GfxBufferHandle indexBuffer,
                        uint32_t firstIndex, uint32_t indexCount,
                        bool sourceLit) {
    // Query the logical handle before resolving its rotating physical VBO.
    auto lightBounds=g_poseLightBounds.Find(vertexBuffer.value,firstVertex,vertexCount);
    const bool hadLocalLights=g_rawLocalLightLoops;
    const bool hadBounds=lightBounds.valid;
    GfxVitaDrawLocalLightScope lightScope(g_rawLocalLightLoops,lightBounds,g_frameLighting,
        sourceLit && g_adaptiveLighting && (g_frameLighting.flags&GFX_FRAME_LIGHTING_VALID) && !g_software);
    if(sourceLit && hadLocalLights) {
        ++g_poseLightBounded;g_poseLightCulled+=!g_rawLocalLightLoops;g_poseLightMissing+=!hadBounds;
    }
    if (g_software || !g_initialized || g_nativeLitShader.program == 0
        || vertexBuffer.value == 0 || indexBuffer.value == 0
        || vertexCount == 0 || indexCount < 3)
        return false;
    const uint64_t vertexEnd = static_cast<uint64_t>(firstVertex)
        + vertexCount;
    const uint64_t indexEnd = static_cast<uint64_t>(firstIndex) + indexCount;
    if (vertexEnd * sizeof(DynamicViewModelVertex)
            > NativeResourceSize(g_nativeBuffers, vertexBuffer.value)
        || indexEnd * sizeof(uint16_t)
            > NativeResourceSize(g_nativeBuffers, indexBuffer.value))
        return false;

    // A preceding BSP/model batch may have deliberately left its material
    // pipeline and buffers live. Dynamic viewmodel vertices use a different
    // layout and lifetime, so make the boundary explicit before rebinding.
    if(sourceLit) ObserveGeometryViewport(2u);
    vertexBuffer.value=g_poseBuffers.Draw(vertexBuffer.value,g_poseSwapSerial);
    EndNativeStaticPipeline();
#if KISAK_VITA_CLEAN_VIEWMODEL
    if (sourceLit) {
        RecordDraw(indexCount);
        return GfxVitaViewmodel_Draw(vertexBuffer.value, indexBuffer.value,
            firstVertex, firstIndex, indexCount,
            NativeTexture(g_materialPass.colorTexture.value));
    }
#endif
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer.value);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer.value);
    g_nativeDynamicBufferDraw = true;
    g_nativeWorldSpaceDraw = false;
    g_nativeViewModelSpaceDraw = true;
    if (sourceLit) glDepthRangef(0.0f, 0.015625f);
    const auto* vertices = reinterpret_cast<const GfxDrawVertex*>(
        static_cast<uintptr_t>(firstVertex
                               * sizeof(DynamicViewModelVertex)));
    const auto* indices = reinterpret_cast<const uint16_t*>(
        static_cast<uintptr_t>(firstIndex * sizeof(uint16_t)));
    if (sourceLit) {
        DrawIndexedLightmapped(
            vertices, vertexCount, indices, indexCount,
            g_materialPass.colorTexture, GfxTextureHandle{0},
            GfxTextureHandle{0}, GfxTextureHandle{0}, 0u);
    } else {
        DrawIndexed(vertices, vertexCount, indices, indexCount);
    }
#if KISAK_VITA_PROFILE
    if (g_nativeCapture.pending && g_nativeCapture.inputContracts && indexCount > 3000) {
        static uint32_t probedFrame = 0;
        if (probedFrame != g_rendererFrame) {
            probedFrame = g_rendererFrame;
            const auto savedPass = g_materialPass;
            const auto savedMode = g_nativeSurfaceMode;
            const auto savedCull = g_nativeCullMode;
            const auto savedTexture = g_boundTexture;
            sceClibPrintf("[kisak-vita] viewmodel draw state mode=%u source=%u bits=%08x,%08x blend=%u depth=%u\n",
                static_cast<unsigned>(savedMode), savedPass.sourceStateValid,
                savedPass.sourceStateBits0, savedPass.sourceStateBits1,
                static_cast<unsigned>(glIsEnabled(GL_BLEND)), static_cast<unsigned>(glIsEnabled(GL_DEPTH_TEST)));
            CaptureNativeSceneFrame("viewmodel-ordinary");
            DrawIndexedLightmapped(vertices, vertexCount, indices, indexCount,
                savedPass.colorTexture, {0}, {0}, {0}, 0);
            CaptureNativeSceneFrame("viewmodel-repeat");
            glDisable(GL_BLEND);
            DrawIndexedLightmapped(vertices, vertexCount, indices, indexCount,
                savedPass.colorTexture, {0}, {0}, {0}, 0);
            CaptureNativeSceneFrame("viewmodel-opaque");
            glDisable(GL_DEPTH_TEST);
            DrawIndexedLightmapped(vertices, vertexCount, indices, indexCount,
                savedPass.colorTexture, {0}, {0}, {0}, 0);
            CaptureNativeSceneFrame("viewmodel-no-depth");
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            glDepthRangef(0.0f, 1.0f);
            DrawIndexedLightmapped(vertices, vertexCount, indices, indexCount,
                savedPass.colorTexture, {0}, {0}, {0}, 0);
            CaptureNativeSceneFrame("viewmodel-full-depth-range");
            glDepthRangef(0.0f, 0.015625f);
            glDisable(GL_CULL_FACE);
            DrawIndexedLightmapped(vertices, vertexCount, indices, indexCount,
                savedPass.colorTexture, {0}, {0}, {0}, 0);
            CaptureNativeSceneFrame("viewmodel-opaque-nocull");
            SetMaterialPass(nullptr);
            BindTexture({0});
            DrawIndexed(vertices, vertexCount, indices, indexCount);
            CaptureNativeSceneFrame("viewmodel-color-only");
            SetSurfaceMode(savedMode);
            SetCullMode(savedCull);
            SetMaterialPass(&savedPass);
            BindTexture({savedTexture});
        }
    }
#endif
    g_nativeViewModelSpaceDraw = false;
    if (sourceLit) glDepthRangef(0.015625f, 1.0f);
    g_nativeDynamicBufferDraw = false;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

#if KISAK_VITA_PROFILE
void CaptureNativeSceneFrame(const char* stage) {
    if (!g_nativeCapture.pending || g_nativeCapture.completed || g_software
        || !g_nativePost.ready
        || (!g_nativeCapture.inputContracts && !g_nativeCapture.stageCaptures)) return;
    // Read the scene independently of its final composite. This makes a
    // stale/misbound presentation target distinguishable from material faults.
    if (g_benchmarkClear) { glBindFramebuffer(GL_FRAMEBUFFER, 0); glViewport(0,0,g_width,g_height); }
    else BindNativeSceneTarget();
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    { VITA_CPU_SCOPE("render-gpu-finish"); glFinish(); }
    // VitaGL ends/restarts a GXM scene for every glReadPixels call. Reading
    // one scanline at a time forced 544 scene boundaries per capture (tens
    // of seconds on Vita3K 4085). Keep bounded scratch and read 16 rows at
    // once. The stage PPM retains its existing FBO orientation.
    const bool world=g_gameplayFrame && !g_nativeViewmodel && g_worldTarget.framebuffer;
    const uint32_t readWidth=world?g_worldTarget.width:960u, readHeight=world?g_worldTarget.height:544u;
    constexpr uint32_t kReadbackRows = 16;
    std::array<uint8_t, 960 * 4 * kReadbackRows> rgba{};
    std::array<uint8_t, 960 * 3> rgb{};
    char path[128]{};
    std::snprintf(path, sizeof(path), "ux0:data/CODVIT001/logs/native-%s.ppm", stage);
    FILE* file = std::fopen(path, "wb");
    bool ok = file != nullptr;
    if (file) {
        ok = std::fprintf(file, "P6\n%u %u\n255\n",readWidth,readHeight) > 0;
        while (glGetError() != GL_NO_ERROR) {}
        for (uint32_t row = 0; row < readHeight && ok; row += kReadbackRows) {
            const uint32_t rows = std::min(kReadbackRows, readHeight - row);
            glReadPixels(0, readHeight - row - rows, readWidth, rows,
                         GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            ok = glGetError() == GL_NO_ERROR;
            for (uint32_t localRow = 0; localRow < rows && ok; ++localRow) {
                for (uint32_t x = 0; x < readWidth; ++x)
                    std::memcpy(rgb.data() + x * 3,
                                rgba.data() + (localRow * readWidth + x) * 4, 3);
                ok = std::fwrite(rgb.data(), 1, readWidth*3u, file) == readWidth*3u;
            }
        }
        if (std::fclose(file) != 0) ok = false;
    }
    sceClibPrintf("[kisak-vita] native scene readback %s stage=%s sourceTime=%d framebuffer=%u texture=%u\n",
        ok ? "complete" : "failed", stage, g_nativeCapture.sourceTime,
        g_nativePost.sceneFramebuffer, g_nativePost.sceneTexture);
}

void CaptureNativeInputContracts() {
    if (!g_nativeCapture.pending || g_nativeCapture.completed || g_software
        || !g_nativePost.ready) return;
    // Opt-in capture controls: the same exact block colors through a linear
    // image and DXT1, then the same quad through persistent vertices at zero
    // and a byte offset above the GXM attribute's 16-bit offset range.
    const auto savedMode = g_nativeSurfaceMode;
    const auto savedPass = g_materialPass;
    const auto savedLighting = g_frameLighting;
    const bool savedFog = g_vitaGlFogEnabled;
    float savedMatrix[16];
    std::copy_n(g_nativeWorldViewProjection, 16, savedMatrix);
    const float savedNear = g_nativeWorldNearPlane;
    g_frameLighting = {};
    g_vitaGlFogEnabled = false;
    std::array<uint8_t, 64 * 64 * 4> rgba{};
    std::array<uint8_t, 16 * 16 * 8> dxt{};
    for (uint32_t y = 0; y < 16; ++y) {
        for (uint32_t x = 0; x < 16; ++x) {
            const uint16_t color = static_cast<uint16_t>(
                ((x * 2) << 11) | ((y * 4) << 5) | 16);
            std::memcpy(dxt.data() + (y * 16 + x) * 8, &color, 2);
            for (uint32_t py = 0; py < 4; ++py) {
                for (uint32_t px = 0; px < 4; ++px) {
                    auto* p = rgba.data() + ((y * 4 + py) * 64 + x * 4 + px) * 4;
                    p[0] = (16 << 3) | (16 >> 2); // BGRA input
                    p[1] = (y * 4 << 2) | (y * 4 >> 4);
                    p[2] = (x * 2 << 3) | (x * 2 >> 2);
                    p[3] = 255;
                }
            }
        }
    }
    const auto linear = CreateTexture2D(rgba.data(), rgba.size(), 64, 64, 1, 21, 0);
    const auto compressed = CreateTexture2D(dxt.data(), dxt.size(), 64, 64, 1, 0x31545844u, 0);
    GfxDrawVertex vertices[4]{};
    const float corners[4][2]{{-0.9f, -0.8f}, {0.9f, -0.8f},
                              {0.9f, 0.8f}, {-0.9f, 0.8f}};
    for (uint32_t i = 0; i < 4; ++i) {
        std::copy_n(corners[i], 2, vertices[i].position);
        vertices[i].position[2] = 0.5f;
        std::copy_n(vertices[i].position, 3, vertices[i].worldPosition);
        std::fill_n(vertices[i].color, 4, 1.0f);
        vertices[i].texCoord[0] = i == 1 || i == 2 ? 1.0f : 0.0f;
        vertices[i].texCoord[1] = i >= 2 ? 1.0f : 0.0f;
    }
    const uint16_t indices[6]{0, 1, 2, 0, 2, 3};
    const auto Prepare = [&]() {
        Clear(0.0f, 0.0f, 0.0f, 1.0f);
        SetMaterialPass(nullptr);
        SetSurfaceMode(GfxSurfaceMode::Overlay);
        SetCullMode(GfxCullMode::None);
    };
    Prepare();
    BindTexture(linear);
    SetSamplerState(0, 0x61);
    DrawIndexed(vertices, 4, indices, 6);
    CaptureNativeSceneFrame("contract-linear");
    Prepare();
    BindTexture(compressed);
    SetSamplerState(0, 0x61);
    DrawIndexed(vertices, 4, indices, 6);
    CaptureNativeSceneFrame("contract-dxt");
    std::vector<uint8_t> largeDxt(1024 * 1024 / 2);
    for (uint32_t y = 0; y < 256; ++y) {
        for (uint32_t x = 0; x < 256; ++x) {
            std::memcpy(largeDxt.data() + (y * 256 + x) * 8,
                        dxt.data() + ((y / 16) * 16 + x / 16) * 8, 8);
        }
    }
    const auto largeCompressed = CreateTexture2D(largeDxt.data(), largeDxt.size(),
                                                1024, 1024, 1, 0x31545844u, 0);
    Prepare();
    BindTexture(largeCompressed);
    SetSamplerState(0, 0x61);
    DrawIndexed(vertices, 4, indices, 6);
    CaptureNativeSceneFrame("contract-dxt-large");
    DestroyTexture(largeCompressed);
    std::vector<GfxDrawVertex> padded(2052);
    std::copy_n(vertices, 4, padded.begin());
    std::copy_n(vertices, 4, padded.begin() + 2048);
    const auto vbo = CreateStaticVertexBuffer(padded.data(), padded.size());
    const auto ibo = CreateStaticIndexBuffer(indices, 6);
    const float matrix[16]{0.75f, 0, 0, 0, 0, 0.75f, 0, 0,
                           0, 0, 1, 0, 0.1f, -0.1f, 0, 1};
    for (uint32_t offset : {0u, 2048u}) {
        Prepare();
        SetWorldViewProjection(matrix, 2.0f);
        GfxMaterialPassState pass{};
        pass.colorTexture = linear;
        pass.colorSamplerState = 0x61;
        pass.surfaceMode = GfxSurfaceMode::Overlay;
        SetMaterialPass(&pass);
        DrawStaticIndexed(vbo, offset, 4, ibo, 0, 6);
        CaptureNativeSceneFrame(offset ? "contract-static-offset" : "contract-static-zero");
    }
    EndNativeStaticPipeline();
    DestroyBuffer(vbo);
    DestroyBuffer(ibo);
    DestroyTexture(linear);
    DestroyTexture(compressed);
    // Actual perspective and occlusion controls. The earlier affine quad
    // cannot expose broken homogeneous clipping or lost depth writes.
    kisak::vita::VitaRenderPacket view{};
    view.cameraAxis[0][1] = 1;
    view.cameraAxis[1][0] = -1;
    view.cameraAxis[2][2] = 1;
    view.nearPlane = 4;
    view.tanHalfFovX = view.tanHalfFovY = 1;
    kisak::vita::VitaBuildViewProjection(&view);
    GfxDrawVertex depthVertices[8]{};
    for (uint32_t i = 0; i < 8; ++i) {
        const bool near = i < 4;
        const uint32_t corner = i % 4;
        const float halfSize = near ? 4.0f : 16.0f;
        depthVertices[i].worldPosition[0] = (corner == 1 || corner == 2) ? halfSize : -halfSize;
        depthVertices[i].worldPosition[1] = near ? 8.0f : 16.0f;
        depthVertices[i].worldPosition[2] = corner >= 2 ? halfSize : -halfSize;
        depthVertices[i].color[near ? 1 : 0] = 1;
        depthVertices[i].color[3] = 1;
    }
    const auto depthVbo = CreateStaticVertexBuffer(depthVertices, 8);
    const auto depthIbo = CreateStaticIndexBuffer(indices, 6);
    for (const bool reverse : {false, true}) {
        Prepare();
        SetWorldViewProjection(view.viewProjection, view.nearPlane);
        SetSurfaceMode(GfxSurfaceMode::Opaque);
        GfxMaterialPassState depthPass{};
        depthPass.surfaceMode = GfxSurfaceMode::Opaque;
        SetMaterialPass(&depthPass);
        DrawStaticIndexed(depthVbo, reverse ? 4 : 0, 4, depthIbo, 0, 6);
        DrawStaticIndexed(depthVbo, reverse ? 0 : 4, 4, depthIbo, 0, 6);
        CaptureNativeSceneFrame(reverse ? "contract-depth-reverse" : "contract-depth");
    }
    EndNativeStaticPipeline();
    for (const GLenum winding : {GL_CW, GL_CCW}) {
        Prepare();
        SetWorldViewProjection(view.viewProjection, view.nearPlane);
        SetSurfaceMode(GfxSurfaceMode::Opaque);
        SetCullMode(GfxCullMode::Back);
        glFrontFace(winding);
        GfxMaterialPassState cullPass{};
        cullPass.surfaceMode = GfxSurfaceMode::Opaque;
        SetMaterialPass(&cullPass);
        DrawStaticIndexed(depthVbo, 0, 4, depthIbo, 0, 6);
        CaptureNativeSceneFrame(winding == GL_CW ? "contract-cull-cw" : "contract-cull-ccw");
    }
    EndNativeStaticPipeline();
    DestroyBuffer(depthVbo);
    DestroyBuffer(depthIbo);
    for (auto& vertex : depthVertices) {
        vertex.worldPosition[1] = -8;
    }
    const auto behindVbo = CreateStaticVertexBuffer(depthVertices, 4);
    const auto behindIbo = CreateStaticIndexBuffer(indices, 6);
    Prepare();
    SetWorldViewProjection(view.viewProjection, view.nearPlane);
    SetSurfaceMode(GfxSurfaceMode::Opaque);
    GfxMaterialPassState behindPass{};
    behindPass.surfaceMode = GfxSurfaceMode::Opaque;
    SetMaterialPass(&behindPass);
    DrawStaticIndexed(behindVbo, 0, 4, behindIbo, 0, 6);
    CaptureNativeSceneFrame("contract-behind");
    EndNativeStaticPipeline();
    DestroyBuffer(behindVbo);
    DestroyBuffer(behindIbo);
    // Exercise the packed dynamic first-person stream independently of the
    // static world layout. Camera Z must become clip W, and the two constant
    // attributes must not shift the enabled interleaved attributes.
    for (uint32_t i = 0; i < 4; ++i) {
        auto& vertex = depthVertices[i];
        vertex = {};
        vertex.position[0] = (i == 1 || i == 2) ? 4 : -4;
        vertex.position[1] = i >= 2 ? 4 : -4;
        vertex.position[2] = 8;
        vertex.color[1] = vertex.color[3] = 1;
    }
    const auto dynamicVbo = CreateDynamicVertexBuffer(4);
    const auto dynamicIbo = CreateStaticIndexBuffer(indices, 6);
    UpdateDynamicVertexBuffer(dynamicVbo, depthVertices, 4);
    Prepare();
    SetSurfaceMode(GfxSurfaceMode::Overlay);
    SetMaterialPass(nullptr);
    DrawDynamicIndexed(dynamicVbo, 0, 4, dynamicIbo, 0, 6, false);
    CaptureNativeSceneFrame("contract-dynamic");
    for (uint32_t i = 0; i < 4; ++i) {
        depthVertices[i].position[2] = (i == 0 || i == 3) ? -2 : 8;
        depthVertices[i].worldPosition[0] = depthVertices[i].position[0];
        depthVertices[i].worldPosition[1] = depthVertices[i].position[2];
        depthVertices[i].worldPosition[2] = depthVertices[i].position[1];
    }
    UpdateDynamicVertexBuffer(dynamicVbo, depthVertices, 4);
    Prepare();
    SetSurfaceMode(GfxSurfaceMode::Overlay);
    SetMaterialPass(nullptr);
    DrawDynamicIndexed(dynamicVbo, 0, 4, dynamicIbo, 0, 6, false);
    CaptureNativeSceneFrame("contract-dynamic-crossing");
    const auto crossingVbo = CreateStaticVertexBuffer(depthVertices, 4);
    Prepare();
    SetWorldViewProjection(view.viewProjection, view.nearPlane);
    SetSurfaceMode(GfxSurfaceMode::Opaque);
    SetMaterialPass(&behindPass);
    DrawStaticIndexed(crossingVbo, 0, 4, dynamicIbo, 0, 6);
    CaptureNativeSceneFrame("contract-world-crossing");
    DestroyBuffer(crossingVbo);
    DestroyBuffer(dynamicVbo);
    DestroyBuffer(dynamicIbo);
    SetWorldViewProjection(savedMatrix, savedNear);
    g_frameLighting = savedLighting;
    g_vitaGlFogEnabled = savedFog;
    Clear(14.0f / 255.0f, 19.0f / 255.0f, 24.0f / 255.0f, 1.0f);
    SetMaterialPass(&savedPass);
    SetSurfaceMode(savedMode);
}

void CaptureNativeFrame() {
    auto& request = g_nativeCapture;
    if (!request.pending || request.completed || g_software) return;
    request.pending = false;
    request.completed = true;
    // Keep just one mapped scanline, then wait for each GPU transfer before
    // reusing it. CPU-only reads can see stale guest RAM on Vita3K even when
    // the host render surface has completed. No full-frame GPU copy is needed.
    std::array<uint8_t, 960 * 3> rgb{};
    if (!g_nativePost.ready || g_nativePost.width != 960 || g_nativePost.height != 544) {
        sceClibPrintf("[kisak-vita] native capture failed: native scene dimensions unavailable\n");
        return;
    }
    const char* path = "ux0:data/CODVIT001/logs/native-frame.ppm";
    FILE* file = std::fopen(path, "wb");
    auto* rgba = static_cast<uint8_t*>(vglMemalign(4096, 960 * 4));
    bool ok = file != nullptr && rgba != nullptr;
    if (file) {
        ok = std::fprintf(file, "P6\n960 544\n255\n") > 0;
        ok = ok && rgba != nullptr;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        { VITA_CPU_SCOPE("render-gpu-finish"); glFinish(); }
        while (glGetError() != GL_NO_ERROR) {}
        for (uint32_t row = 0; row < 544 && ok; ++row) {
            const int copied = sceGxmTransferCopy(960, 1, 0, 0, SCE_GXM_TRANSFER_COLORKEY_NONE,
                SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, SCE_GXM_TRANSFER_LINEAR,
                gxm_color_surfaces_addr[gxm_back_buffer_index], 0, row, DISPLAY_STRIDE * 4,
                SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, SCE_GXM_TRANSFER_LINEAR,
                rgba, 0, 0, 960 * 4, nullptr, 0, nullptr);
            const int finished = sceGxmTransferFinish();
            ok = copied >= 0 && finished >= 0;
            for (uint32_t x = 0; x < 960; ++x)
                std::memcpy(rgb.data() + x * 3, rgba + x * 4, 3);
            if (ok) ok = std::fwrite(rgb.data(), 1, rgb.size(), file) == rgb.size();
        }
        if (std::fclose(file) != 0) ok = false;
    }
    if (rgba) vglFree(rgba);
    FILE* metadata = std::fopen("ux0:data/CODVIT001/logs/native-frame.json", "wb");
    if (metadata) {
        const int written = std::fprintf(metadata,
            "{\"schemaVersion\":1,\"native\":true,\"complete\":%s,\"width\":960,\"height\":544,"
            "\"mapGeneration\":%u,\"frameGeneration\":%u,\"rendererFrame\":%u,\"sourceServerTime\":%d,"
            "\"cameraOrigin\":[%.9g,%.9g,%.9g],\"cameraAxis\":[[%.9g,%.9g,%.9g],[%.9g,%.9g,%.9g],[%.9g,%.9g,%.9g]],"
            "\"nearPlane\":%.9g,\"tanHalfFovX\":%.9g,\"tanHalfFovY\":%.9g,"
            "\"diagnosticWorldReplay\":%s,"
            "\"diagnosticReadback\":true,\"performanceQualified\":false,\"visuallyQualified\":false}\n",
            ok ? "true" : "false", request.mapGeneration, request.frameGeneration,
            g_rendererFrame, request.sourceTime, request.origin[0], request.origin[1], request.origin[2],
            request.axis[0][0], request.axis[0][1], request.axis[0][2],
            request.axis[1][0], request.axis[1][1], request.axis[1][2],
            request.axis[2][0], request.axis[2][1], request.axis[2][2],
            request.nearPlane, request.tanHalfFovX, request.tanHalfFovY,
            request.inputContracts ? "true" : "false");
        const int closed = std::fclose(metadata);
        if (written <= 0 || closed != 0) ok = false;
    } else ok = false;
    sceClibPrintf("[kisak-vita] native capture %s sourceTime=%d rendererFrame=%u file=%s diagnosticReadback=1\n",
        ok ? "complete" : "failed", request.sourceTime, g_rendererFrame, path);
}
#endif

void EndFrame() {
    VITA_CPU_SCOPE("render-end-and-present");
#if KISAK_VITA_CPU_RUNTIME
    if(g_propGroupOpen) {
        kisak::vita::VitaCpuProfile_Counter("render-probe-props-group-invalid",2);
        GfxVita_PropWorkloadGroup(false); // Rejected/incomplete snapshot cleanup.
    }
#endif
    if (!g_software && g_rendererFrame <= 3u) {
        sceClibPrintf("[kisak-vita] texture sharing images=%u leases=%u resident=%llu shared=%llu\n",
            g_nativeTexturePool.ImageCount(), g_nativeTexturePool.LeaseCount(),
            static_cast<unsigned long long>(g_nativeTexturePool.ResidentBytes()),
            static_cast<unsigned long long>(g_nativeTexturePool.SharedBytes()));
    }
    if (g_rendererFrame <= 3u || (g_rendererFrame % 120u) == 0u) {
        sceClibPrintf(
            "[kisak-vita] renderer telemetry frame=%u map=%u draws=%u triangles=%u states=%u nativeColor=%u lit=%u materials=%u normal=%u specular=%u lmPrimary=%u lmSecondary=%u shadowPass=%u effectBatches=%u indoor=%u shadows=%u weather=%u drops=%u transientDrops=%u transientBytes=%u transientHighWater=%u textureBytes=%llu staticBufferBytes=%llu colorMode=%s\n",
            static_cast<unsigned>(g_rendererFrame),
            static_cast<unsigned>(g_frameLighting.mapIndex),
            static_cast<unsigned>(g_frameDrawCalls),
            static_cast<unsigned>(g_frameTriangles),
            static_cast<unsigned>(g_frameStateChanges),
            static_cast<unsigned>(g_frameNativeColorPasses),
            static_cast<unsigned>(g_frameLightmappedDraws),
            static_cast<unsigned>(g_frameMaterialPasses),
            static_cast<unsigned>(g_frameNormalPasses),
            static_cast<unsigned>(g_frameSpecularPasses),
            static_cast<unsigned>(g_framePrimaryLightmapPasses),
            static_cast<unsigned>(g_frameSecondaryLightmapPasses),
            static_cast<unsigned>(g_frameShadowPasses),
            static_cast<unsigned>(g_frameEffectBatches),
            static_cast<unsigned>((g_frameLighting.flags
                                   & GFX_FRAME_LIGHTING_INDOOR) != 0),
            static_cast<unsigned>((g_frameLighting.flags
                                   & GFX_FRAME_LIGHTING_HAS_SHADOWS) != 0),
            static_cast<unsigned>(g_frameWeatherDraws),
            static_cast<unsigned>(g_frameWeatherDrops),
            static_cast<unsigned>(g_frameTransientUploadDrops),
            static_cast<unsigned>(g_frameTransientUploadBytes),
            static_cast<unsigned>(g_transientUploadHighWaterBytes),
            static_cast<unsigned long long>(g_software
                ? static_cast<uint64_t>(g_textureBytes)
                : g_nativeTextureBytes),
            static_cast<unsigned long long>(g_nativeStaticBufferBytes),
            g_software ? "scalar-source-fp32-film-dither"
                       : (g_nativePost.ready
                           ? "vitagl-gxm-source-lit-post"
                           : "vitagl-gxm-source-lit-direct"));
        if (g_frameDualLightmapFragments != 0) {
            const float inverseCount = 1.0f
                / static_cast<float>(g_frameDualLightmapFragments);
            sceClibPrintf(
                "[kisak-vita] lightmap telemetry frame=%u dualFragments=%llu primary=%.4f..%.4f/%.4f secondaryR=%.4f..%.4f/%.4f secondaryG=%.4f..%.4f/%.4f secondaryB=%.4f..%.4f/%.4f staticR=%.4f..%.4f/%.4f staticG=%.4f..%.4f/%.4f staticB=%.4f..%.4f/%.4f\n",
                static_cast<unsigned>(g_rendererFrame),
                static_cast<unsigned long long>(g_frameDualLightmapFragments),
                g_frameLightmapPrimaryMin, g_frameLightmapPrimaryMax,
                g_frameLightmapPrimarySum * inverseCount,
                g_frameLightmapSecondaryMin[0], g_frameLightmapSecondaryMax[0],
                g_frameLightmapSecondarySum[0] * inverseCount,
                g_frameLightmapSecondaryMin[1], g_frameLightmapSecondaryMax[1],
                g_frameLightmapSecondarySum[1] * inverseCount,
                g_frameLightmapSecondaryMin[2], g_frameLightmapSecondaryMax[2],
                g_frameLightmapSecondarySum[2] * inverseCount,
                g_frameStaticLightMin[0], g_frameStaticLightMax[0],
                g_frameStaticLightSum[0] * inverseCount,
                g_frameStaticLightMin[1], g_frameStaticLightMax[1],
                g_frameStaticLightSum[1] * inverseCount,
                g_frameStaticLightMin[2], g_frameStaticLightMax[2],
                g_frameStaticLightSum[2] * inverseCount);
        }
        for (const SoftwareLightingProbe& probe : g_softwareLightingProbes) {
            if (!probe.valid)
                continue;
            sceClibPrintf(
                "[kisak-vita] lighting probe p%u,%u depth=%.5f mode=%u dual=%u semantic=%u lmFlags=0x%02x bound=%u color=%u surface=%u material=%.4f,%.4f,%.4f dynamic=%.4f,%.4f,%.4f primary=%.4f secondary=%.4f,%.4f,%.4f static=%.4f,%.4f,%.4f final=%.4f,%.4f,%.4f sunDot=%.4f shadow=%.4f gridSun=%.4f\n",
                static_cast<unsigned>(probe.x),
                static_cast<unsigned>(probe.y), probe.depth,
                static_cast<unsigned>(probe.surfaceMode),
                static_cast<unsigned>(probe.dualLightmap),
                static_cast<unsigned>(probe.semanticPass),
                static_cast<unsigned>(probe.lightmapFlags),
                static_cast<unsigned>(probe.boundTexture),
                static_cast<unsigned>(probe.colorTexture),
                static_cast<unsigned>(probe.sourceSurfaceIndex),
                probe.material[0], probe.material[1], probe.material[2],
                probe.dynamic[0], probe.dynamic[1], probe.dynamic[2],
                probe.primary, probe.secondary[0], probe.secondary[1],
                probe.secondary[2], probe.staticLight[0],
                probe.staticLight[1], probe.staticLight[2],
                probe.finalColor[0], probe.finalColor[1],
                probe.finalColor[2], probe.sunDot, probe.shadow,
                probe.gridSunWeight);
        }
    }
    if (g_software) {
        SoftwareEndFrame();
        return;
    }
    EndNativeTransientDrawPage();
#if KISAK_VITA_PROFILE
    CaptureNativeSceneFrame("scene");
#endif
#if !defined(KISAK_VITA_NATIVE_DIRECT_DISPLAY_PROBE)
    if (g_nativePost.ready && !g_nativeHud && !g_benchmarkClear) {
        const uint64_t postStart = sceKernelGetProcessTimeWide();
        ApplyNativePostEffects();
        g_postSubmitUsec += static_cast<uint32_t>(sceKernelGetProcessTimeWide() - postStart);
    }
#endif
#if KISAK_VITA_PROFILE
    CaptureNativeFrame();
#endif
    GfxVita_RenderPassBoundary(kisak::vita::VitaRenderPass::Hud);
    kisak::vita::VitaDisplayProbe_SetPhase((g_renderPassProbe.graphicsProfile || g_renderPassProbe.passIsolation)?4:g_renderPassProbe.displayQueue?g_renderPassProbe.phase:0);
    const uint64_t swapStart = sceKernelGetProcessTimeWide();
    { VITA_CPU_SCOPE("render-swap-call"); vglSwapBuffers(GL_FALSE); }
    ++g_poseSwapSerial;
    vglCod4TimingRead(g_swapDetail);
    const uint64_t swapUsec = sceKernelGetProcessTimeWide() - swapStart;
    g_lastSwapWaitUsec = static_cast<uint32_t>(std::min<uint64_t>(
        swapUsec, UINT32_MAX));
    kisak::vita::VitaDisplayProbe_SetPhase(0);
    kisak::vita::VitaDisplayProbe_Drain();
    kisak::vita::VitaGraphicsProfile_End();
#if KISAK_VITA_CPU_RUNTIME
    if(g_gameplayFrame) {
        kisak::vita::VitaCpuProfile_Counter("render-local-light-world",g_localLightCulled[0],g_localLightBounded[0]);
        kisak::vita::VitaCpuProfile_Counter("render-local-light-props",g_localLightCulled[1],g_localLightBounded[1]);
        kisak::vita::VitaCpuProfile_Counter("render-local-light-missing-bounds",g_localLightMissing[0],g_localLightMissing[1]);
        kisak::vita::VitaCpuProfile_Counter("render-local-light-weapon",g_poseLightCulled,g_poseLightBounded);
        kisak::vita::VitaCpuProfile_Counter("render-local-light-weapon-missing",g_poseLightMissing,0);
        kisak::vita::VitaCpuProfile_Counter("render-prop-shader-draws",g_propShaderDraws,g_propShaderEligibleDraws);
        if(!g_renderPassProbe.requestedPhase && !g_cpuDiagnostic) {
            kisak::vita::VitaCpuProfile_Counter("render-simple-world-draws",g_simpleWorldActual,g_simpleWorldEligible);
            kisak::vita::VitaCpuProfile_Counter("render-simple-world-state",g_simpleWorldRequested,g_cheapWorldReady);
            kisak::vita::VitaCpuProfile_Counter("render-simple-world-invalid",g_simpleWorldInvalid,0);
            kisak::vita::VitaCpuProfile_Counter("render-pipeline-family",g_familyActual[0]+g_familyActual[1],g_familyEligible[0]+g_familyEligible[1]);
            kisak::vita::VitaCpuProfile_Counter("render-pipeline-fast-color",g_fastColorActual,g_fastColorEligible);
            kisak::vita::VitaCpuProfile_Counter("render-pipeline-fast-clear",g_fastClearActual,g_fastClearEligible);
            kisak::vita::VitaCpuProfile_Counter("render-pipeline-weapon",g_weaponFamilyActual,g_weaponFamilyEligible);
            kisak::vita::VitaCpuProfile_Counter("render-pipeline-world-alpha",g_worldAlphaActual,g_worldAlphaEligible);
            kisak::vita::VitaCpuProfile_Counter("render-pipeline-invalid",g_familyInvalid+g_fastColorInvalid+g_weaponFamilyInvalid+g_worldAlphaInvalid+g_weaponSpecularInvalid,0);
        }
        kisak::vita::VitaCpuProfile_Counter("render-pose-ring-use",g_poseRingUploads,g_poseRingFallbacks);
        kisak::vita::VitaCpuProfile_Counter("render-pose-ring-rotation",g_poseRingRotations,g_poseBuffers.ExtraBytes());
    }
    if(g_renderPassProbe.phase) {
        kisak::vita::VitaCpuProfile_Counter("render-probe-frame",g_renderPassProbe.phase,g_renderPassProbe.seen);
        kisak::vita::VitaCpuProfile_Counter("render-probe-kind",g_renderPassProbe.worldScaling?34:g_renderPassProbe.worldPrecision?33:g_renderPassProbe.passIsolation?32:g_renderPassProbe.composition?31:g_renderPassProbe.graphicsProfile?30:g_renderPassProbe.worldComponents?29:g_renderPassProbe.worldGloss?27:g_renderPassProbe.worldBasis?26:g_renderPassProbe.worldMath?25:g_renderPassProbe.lightmapLayout?24:g_renderPassProbe.worldLifetime?23:g_renderPassProbe.currentDrawProbe?21:g_renderPipeline.worldAlpha?20:g_renderPipeline.weaponSpecular?19:g_renderPipeline.weaponVertex?18:g_renderPipeline.weaponPipeline?17:g_renderPassProbe.fastDrawProbe?16:g_renderPassProbe.earlyAlpha?15:g_renderPassProbe.hud?14:g_renderPassProbe.weaponOpaque?13:g_renderPassProbe.fastColor?12:g_renderPassProbe.drawProbe?11:g_renderPassProbe.prefetch?10:g_renderPassProbe.displayQueue?9:g_renderPassProbe.family?8:g_renderPassProbe.baseline?7:g_renderPassProbe.specular?6:g_renderPassProbe.material?5:g_renderPassProbe.depthOrder?4:g_renderPassProbe.opaque?3:(g_renderPassProbe.workload?2:(g_renderPassProbe.comparison?1:0)),0);
        kisak::vita::VitaCpuProfile_Counter("render-probe-prop-shader",g_propShaderDraws,g_propShaderEligibleDraws);
        if(g_renderPassProbe.worldScaling) {
            kisak::vita::VitaCpuProfile_Counter("render-world-scale-sky",g_worldScaling.skyPrepared,g_worldScaling.skySuppressed);
            kisak::vita::VitaCpuProfile_Counter("render-world-scale-draws",g_worldScaling.actual,g_worldScaling.eligible);
            kisak::vita::VitaCpuProfile_Counter("render-world-scale-valid",g_worldScaling.invalid,g_cheapWorldReady);
            kisak::vita::VitaCpuProfile_Counter("render-world-scale-targets",g_worldScaling.targetActual,g_worldScaling.targetEligible);
            kisak::vita::VitaCpuProfile_Counter("render-world-scale-world",g_worldScaling.worldPrepared,g_worldScaleActive);
            kisak::vita::VitaCpuProfile_Counter("render-world-scale-size",g_worldTarget.width,g_worldTarget.height);
            for(unsigned i=0;i<3;++i) {
                char name[48];std::snprintf(name,sizeof(name),"render-world-scale-viewport-%u",i);
                kisak::vita::VitaCpuProfile_Counter(name,g_observedViewports[i][2],g_observedViewports[i][3]);
            }
        }
        if(g_renderPassProbe.passIsolation) {
            kisak::vita::VitaCpuProfile_Counter("render-isolation-health",g_passIsolation.errors,g_passIsolation.phase);
            for(unsigned i=0;i<7;++i) {
                char label[64];std::snprintf(label,sizeof(label),"render-isolation-draws-%u",i);
                kisak::vita::VitaCpuProfile_Counter(label,g_passIsolation.prepared[i],g_passIsolation.suppressed[i]);
            }
        }
        if(g_renderPassProbe.opaque || g_renderPassProbe.depthOrder || g_renderPassProbe.material || g_renderPassProbe.specular || g_renderPassProbe.baseline || g_renderPassProbe.family || g_renderPassProbe.displayQueue || g_renderPassProbe.prefetch || g_renderPassProbe.drawProbe || (g_renderPassProbe.fastColor || g_renderPassProbe.weaponOpaque || g_renderPassProbe.hud || g_renderPassProbe.earlyAlpha || g_renderPassProbe.fastDrawProbe || g_renderPipeline.weaponPipeline)) {
            kisak::vita::VitaCpuProfile_Counter("render-probe-opaque-world",g_opaqueDraws[0],g_opaqueEligible[0]);
            kisak::vita::VitaCpuProfile_Counter("render-probe-opaque-props",g_opaqueDraws[1],g_opaqueEligible[1]);
            kisak::vita::VitaCpuProfile_Counter("render-probe-opaque-invalid",g_opaqueInvalid,0);
        }
        if(g_renderPassProbe.displayQueue) {
            kisak::vita::VitaCpuProfile_Counter("render-probe-camera-xy",kisak::vita::VitaGpuFloatBits(g_familyCamera[0]),kisak::vita::VitaGpuFloatBits(g_familyCamera[1]));
            kisak::vita::VitaCpuProfile_Counter("render-probe-camera-z",kisak::vita::VitaGpuFloatBits(g_familyCamera[2]),g_familyCameraSeen);
        }
        if(g_renderPassProbe.family || g_renderPassProbe.prefetch || g_renderPassProbe.drawProbe || (g_renderPassProbe.fastColor || g_renderPassProbe.weaponOpaque || g_renderPassProbe.hud || g_renderPassProbe.earlyAlpha || g_renderPassProbe.fastDrawProbe || g_renderPipeline.weaponPipeline)) {
            kisak::vita::VitaCpuProfile_Counter("render-probe-family-ready",g_familyReady,3);
            kisak::vita::VitaCpuProfile_Counter("render-probe-family-lightmapped",g_familyActual[0],g_familyEligible[0]);
            kisak::vita::VitaCpuProfile_Counter("render-probe-family-baked",g_familyActual[1],g_familyEligible[1]);
            kisak::vita::VitaCpuProfile_Counter("render-probe-family-invalid",g_familyInvalid,0);
            kisak::vita::VitaCpuProfile_Counter("render-probe-camera-xy",kisak::vita::VitaGpuFloatBits(g_familyCamera[0]),kisak::vita::VitaGpuFloatBits(g_familyCamera[1]));
            kisak::vita::VitaCpuProfile_Counter("render-probe-camera-z",kisak::vita::VitaGpuFloatBits(g_familyCamera[2]),g_familyCameraSeen);
        }
        if(g_renderPassProbe.prefetch) {
            kisak::vita::VitaCpuProfile_Counter("render-probe-prefetch-ready",g_prefetchReady,3);
            kisak::vita::VitaCpuProfile_Counter("render-probe-prefetch-lightmapped",g_prefetchActual[0],g_familyEligible[0]);
            kisak::vita::VitaCpuProfile_Counter("render-probe-prefetch-baked",g_prefetchActual[1],g_familyEligible[1]);
            kisak::vita::VitaCpuProfile_Counter("render-probe-prefetch-invalid",g_prefetchInvalid,0);
        }
        if(g_renderPassProbe.specular) {
            kisak::vita::VitaCpuProfile_Counter("render-probe-specular-ready",g_specularReady,3);
            kisak::vita::VitaCpuProfile_Counter("render-probe-specular-world",g_specularDraws[0],g_opaqueEligible[0]);
            kisak::vita::VitaCpuProfile_Counter("render-probe-specular-props",g_specularDraws[1],g_opaqueEligible[1]);
            kisak::vita::VitaCpuProfile_Counter("render-probe-specular-inputs",g_specularInputDraws,g_reflectionInputDraws);
        }
        if(g_renderPassProbe.material) {
            kisak::vita::VitaCpuProfile_Counter("render-probe-material-mode",g_materialProbeMask,g_materialProbeReady);
            kisak::vita::VitaCpuProfile_Counter("render-probe-material-world",g_materialProbeDraws[0],g_opaqueEligible[0]);
            kisak::vita::VitaCpuProfile_Counter("render-probe-material-props",g_materialProbeDraws[1],g_opaqueEligible[1]);
            kisak::vita::VitaCpuProfile_Counter("render-probe-material-features",g_materialNormalDraws,g_materialSpecDraws);
            kisak::vita::VitaCpuProfile_Counter("render-probe-material-lightmaps",g_materialPrimaryDraws,g_materialSecondaryDraws);
            kisak::vita::VitaCpuProfile_Counter("render-probe-material-reflections",g_materialReflectionDraws,g_materialLocalDraws);
        }
        if(g_renderPassProbe.workload) {
            kisak::vita::VitaCpuProfile_Counter("render-probe-prop-attempts",g_propAttempts,g_propAttemptTriangles);
            kisak::vita::VitaCpuProfile_Counter("render-probe-prop-submitted",g_propSubmitted,g_propSubmittedTriangles);
            kisak::vita::VitaCpuProfile_Counter("render-probe-prop-clip",g_propGroupSeen,g_propClipped);
        }
        kisak::vita::VitaCpuProfile_Counter("render-probe-total-us",sceKernelGetProcessTimeWide()-g_renderPassProbe.beginUs,g_renderPassProbe.waitUs);
        kisak::vita::VitaCpuProfile_Counter("render-probe-swap-us",swapUsec,g_swapDetail[1]);
        kisak::vita::VitaCpuProfile_Counter("render-probe-geometry",g_frameDrawCalls,g_frameTriangles);
        kisak::vita::VitaCpuProfile_Counter("render-probe-viewports",g_viewportSeen,g_viewportMismatches);
        if(!g_renderPassProbe.Complete())kisak::vita::VitaCpuProfile_Counter("render-probe-incomplete",g_renderPassProbe.phase,g_renderPassProbe.seen);
        if((g_renderPassProbe.fastColor || g_renderPassProbe.weaponOpaque || g_renderPassProbe.hud || g_renderPassProbe.earlyAlpha || g_renderPassProbe.fastDrawProbe || g_renderPipeline.weaponPipeline)) {
            kisak::vita::VitaCpuProfile_Counter("render-probe-fast-ready",g_nativeFastColor.program!=0,g_nativeFastClear.program!=0);
            kisak::vita::VitaCpuProfile_Counter("render-probe-fast-color",g_fastColorActual,g_fastColorEligible);
            kisak::vita::VitaCpuProfile_Counter("render-probe-fast-clear",g_fastClearActual,g_fastClearEligible);
            kisak::vita::VitaCpuProfile_Counter("render-probe-fast-invalid",g_fastColorInvalid,0);
        }
        if(g_renderPassProbe.hud) {
            kisak::vita::VitaCpuProfile_Counter("render-probe-hud-draws",g_hudActual,g_hudEligible);
            kisak::vita::VitaCpuProfile_Counter("render-probe-hud-valid",g_hudViewportSeen,g_hudInvalid);
            kisak::vita::VitaCpuProfile_Counter("render-probe-hud-viewport",uint64_t(uint32_t(g_hudViewport[2]))<<32|uint32_t(g_hudViewport[3]),uint32_t(g_hudFbo));
        }
        if(g_renderPassProbe.earlyAlpha) {
            kisak::vita::VitaCpuProfile_Counter("render-probe-alpha-ready",g_earlyAlphaReady,3);
            kisak::vita::VitaCpuProfile_Counter("render-probe-alpha-world",g_earlyAlphaActual[0],g_earlyAlphaEligible[0]);
            kisak::vita::VitaCpuProfile_Counter("render-probe-alpha-props",g_earlyAlphaActual[1],g_earlyAlphaEligible[1]);
            kisak::vita::VitaCpuProfile_Counter("render-probe-alpha-invalid",g_earlyAlphaInvalid,0);
        }
        if(g_renderPipeline.weaponPipeline) {
            kisak::vita::VitaCpuProfile_Counter("render-probe-weapon-family",g_weaponFamilyActual,g_weaponFamilyEligible);
            kisak::vita::VitaCpuProfile_Counter("render-probe-weapon-family-valid",g_weaponFamilyInvalid,g_familyReady);
        }
        if(g_renderPassProbe.lightmapLayout) {
            kisak::vita::VitaCpuProfile_Counter("render-probe-lightmap-layout",g_lightmapLayoutActual,g_lightmapLayoutEligible);
            kisak::vita::VitaCpuProfile_Counter("render-probe-lightmap-layout-valid",g_lightmapLayoutInvalid,g_lightmapLayoutFailures);
            kisak::vita::VitaCpuProfile_Counter("render-probe-lightmap-layout-storage",g_lightmapLayoutBytes,8u*1024*1024);
            kisak::vita::VitaCpuProfile_Counter("render-probe-lightmap-inplace",sizeof(g_lightmapWorkspace),0);
        }
        if(g_renderPassProbe.worldComponents) {
            kisak::vita::VitaCpuProfile_Counter("render-probe-world-components",g_worldComponentsActual,g_worldComponentsEligible);
            kisak::vita::VitaCpuProfile_Counter("render-probe-world-components-valid",g_worldComponentsInvalid,g_worldComponentsReady);
            kisak::vita::VitaCpuProfile_Counter("render-probe-world-components-target",g_worldComponentsTargets,GfxVitaWorldComponentVariant(g_renderPassProbe.phase));
        }
        if(g_renderPassProbe.worldGloss) {
            kisak::vita::VitaCpuProfile_Counter("render-probe-world-gloss",g_worldGlossActual,g_worldGlossEligible);
            kisak::vita::VitaCpuProfile_Counter("render-probe-world-gloss-valid",g_worldGlossInvalid,g_nativeWorldGloss.program!=0);
            kisak::vita::VitaCpuProfile_Counter("render-probe-world-gloss-targets",g_worldGlossTargetActual,g_worldGlossTargets);
        }
        if(g_renderPassProbe.worldBasis) {
            kisak::vita::VitaCpuProfile_Counter("render-probe-world-basis",g_worldBasisActual,g_worldBasisEligible);
            kisak::vita::VitaCpuProfile_Counter("render-probe-world-basis-valid",g_worldBasisInvalid,g_worldBasisReady);
            kisak::vita::VitaCpuProfile_Counter("render-probe-world-basis-targets",g_worldBasisTargetActual,g_worldBasisTargets);
            kisak::vita::VitaCpuProfile_Counter("render-probe-world-basis-seen",g_worldBasisSeen,1);
            kisak::vita::VitaCpuProfile_Counter("render-probe-world-basis-triangles",g_worldBasisTriangles,g_worldBasisFamilyTriangles);
        }
        if(g_renderPassProbe.worldPrecision) {
            kisak::vita::VitaCpuProfile_Counter("render-probe-world-precision",g_worldPrecisionActual,g_worldPrecisionEligible);
            kisak::vita::VitaCpuProfile_Counter("render-probe-world-precision-valid",g_worldPrecisionInvalid,g_worldPrecisionReady);
            kisak::vita::VitaCpuProfile_Counter("render-probe-world-precision-targets",g_worldPrecisionTargetActual,g_worldPrecisionTargets);
        }
        if(g_renderPassProbe.worldMath) {
            kisak::vita::VitaCpuProfile_Counter("render-probe-world-math",g_worldMathActual,g_worldMathEligible);
            kisak::vita::VitaCpuProfile_Counter("render-probe-world-math-valid",g_worldMathInvalid,g_worldMathReady);
            kisak::vita::VitaCpuProfile_Counter("render-probe-world-math-targets",g_worldMathTargetActual,g_worldMathTargets);
        }
        if(g_renderPassProbe.worldLifetime) {
            kisak::vita::VitaCpuProfile_Counter("render-probe-world-lifetime",g_worldLifetimeActual,g_worldLifetimeEligible);
            kisak::vita::VitaCpuProfile_Counter("render-probe-world-lifetime-valid",g_worldLifetimeInvalid,g_nativeWorldLifetime.program!=0);
            kisak::vita::VitaCpuProfile_Counter("render-probe-world-lifetime-targets",g_worldLifetimeTargetActual,g_worldLifetimeTargets);
        }
        if(g_renderPipeline.worldAlpha) {
            kisak::vita::VitaCpuProfile_Counter("render-probe-world-alpha",g_worldAlphaActual,g_worldAlphaEligible);
            kisak::vita::VitaCpuProfile_Counter("render-probe-world-alpha-valid",g_worldAlphaInvalid,g_nativeWorldAlpha.program!=0);
            kisak::vita::VitaCpuProfile_Counter("render-probe-world-alpha-target",g_worldAlphaTargetActual,g_worldAlphaTargetEligible);
        }
        if(g_renderPipeline.weaponSpecular) {
            kisak::vita::VitaCpuProfile_Counter("render-probe-weapon-specular",g_weaponSpecularActual,g_weaponSpecularEligible);
            kisak::vita::VitaCpuProfile_Counter("render-probe-weapon-specular-valid",g_weaponSpecularInvalid,g_nativeWeaponSpecular.program!=0);
            kisak::vita::VitaCpuProfile_Counter("render-probe-weapon-specular-maps",g_weaponSpecularNormalMaps,g_weaponSpecularMaps);
        }
        if(g_renderPassProbe.weaponOpaque) {
            kisak::vita::VitaCpuProfile_Counter("render-probe-weapon-ready",g_nativeOpaqueFullShader.program!=0,g_nativeOpaqueSunShader.program!=0);
            kisak::vita::VitaCpuProfile_Counter("render-probe-weapon-opaque",g_weaponOpaqueActual,g_weaponOpaqueEligible);
            kisak::vita::VitaCpuProfile_Counter("render-probe-weapon-invalid",g_weaponOpaqueInvalid,0);
            kisak::vita::VitaCpuProfile_Counter("render-probe-weapon-maps",g_weaponNormalDraws,g_weaponSpecularDraws);
        }
        if(g_renderPassProbe.drawProbe)ReportGpuDraws();
        g_renderPassProbe.phase=0;
    }
#endif
}

const GfxBackendApi kApi = { Init, Shutdown, BeginFrame, Clear, CreateTexture2D,
                             DestroyTexture, BindTexture, SetSamplerState, SetCullMode,
                             DrawIndexed,
                             DrawIndexedLightmapped, SetSurfaceMode, SetAlphaTestFunc,
                             SetFog, SetFrameLighting, SetMaterialPass,
                             SetShadowPass, SetEffectBatch,
                             ApplyViewEffects,
                             CreateStaticVertexBuffer,
                             CreateStaticVertexBufferRemapped,
                             CreateStaticIndexBuffer,
                             CreateStaticInstanceBuffer,
                             CreateDynamicVertexBuffer,
                             UpdateDynamicVertexBuffer, DestroyBuffer,
                             SetWorldViewProjection, DrawStaticIndexed,
                             DrawStaticIndexedInstanced,
                             DrawDynamicIndexed,
                             EndFrame, CreateInstancedModelVertexBuffer, SetDrawLightBounds };

} // namespace

bool GfxVita_UpdatePackedViewmodel(GfxBufferHandle buffer,const GfxVitaViewmodelVertex* vertices,uint32_t count) {
#if KISAK_VITA_CPU_RUNTIME
    if(kisak::vita::VitaCpuRender_OnSource())return false;
#endif
    g_poseLightBounds.Forget(buffer.value);
    if(g_software || !g_initialized || !buffer.value || !vertices || !count
        || count>UINT32_MAX/sizeof(DynamicViewModelVertex))return false;
    const uint32_t bytes=count*static_cast<uint32_t>(sizeof(DynamicViewModelVertex));
    const uint32_t capacity=NativeResourceSize(g_nativeBuffers,buffer.value);
    if(bytes>capacity)return false;
    const bool uploaded=UploadPackedViewmodel(buffer,vertices,bytes,capacity);
    if(uploaded)g_poseLightBounds.Store(buffer.value,vertices,count);
    return uploaded;
}

bool GfxVita_ConsumeTexturePagingFailure() {return g_texturePagingFault.exchange(false,std::memory_order_acq_rel);}

const GfxBackendApi* GfxVita_GetBackend() {
#if KISAK_VITA_CPU_RUNTIME
    return kisak::vita::VitaCpuRender_Backend(&kApi);
#endif
    return &kApi;
}

#if KISAK_VITA_PROFILE
void GfxVita_CaptureProfileStage(const char* stage) {
    if (!g_nativeCapture.inputContracts && !g_nativeCapture.stageCaptures) return;
    CaptureNativeSceneFrame(stage);
    if (!g_nativeCapture.inputContracts) return;
    if (stage && std::strcmp(stage, "clear") == 0)
        CaptureNativeInputContracts();
    if (stage && std::strcmp(stage, "world") == 0 && g_nativeCapture.pending
        && !g_nativeCapture.worldDraws.empty()) {
        const auto savedPass = g_materialPass;
        const auto savedMode = g_nativeSurfaceMode;
        const auto savedCull = g_nativeCullMode;
        g_nativeCapture.replaying = true;
        for (int variant = 0; variant < 6; ++variant) {
            Clear(0, 0, 0, 1);
            uint64_t previousRevision = 0;
            for (const auto& draw : g_nativeCapture.worldDraws) {
                if (variant == 0 || previousRevision != draw.materialRevision) {
                    SetSurfaceMode(draw.material.surfaceMode);
                    SetMaterialPass(&draw.material);
                    previousRevision = draw.materialRevision;
                }
                if (variant == 2) glUseProgram(g_nativeLitShader.program);
                if (variant == 3) {
                    ApplyNativeSourceState(draw.material);
                    glUseProgram(g_nativeLitShader.program);
                }
                if (variant == 4) {
                    glUseProgram(g_nativeLitShader.program);
                    g_nativeWorldSpaceDraw = true;
                    UploadNativeLitUniforms();
                    g_nativeWorldSpaceDraw = false;
                }
                if (variant == 5) EndNativeStaticPipeline();
                DrawStaticIndexed(draw.vertices, draw.firstVertex, draw.vertexCount,
                    draw.indices, draw.firstIndex, draw.indexCount);
            }
            static const char* names[]{"world-control-state", "world-control-cached",
                "world-control-program", "world-control-raster", "world-control-uniforms", "world-control-reset"};
            CaptureNativeSceneFrame(names[variant]);
        }
        g_nativeCapture.triangleReplay = false;
        g_nativeCapture.replaying = false;
        SetMaterialPass(&savedPass);
        SetSurfaceMode(savedMode);
        SetCullMode(savedCull);
    }
}

void GfxVita_QueueProfileCapture(const kisak::vita::VitaRenderPacket& packet) {
    auto& request = g_nativeCapture;
    if (!packet.sourceViewValid || request.completed || g_software) return;
    if (!request.checked) {
        request.checked = true;
        if (FILE* file = std::fopen("ux0:data/CODVIT001/tests/native-capture.req", "rb")) {
            unsigned time = 0, contracts = 0, stages = 0, synchronous = 0;
            request.enabled = std::fscanf(file, "%u %u %u %u", &time, &contracts, &stages, &synchronous) >= 1 && time <= 120000;
            request.inputContracts = contracts == 1;
            request.stageCaptures = stages == 1;
            request.synchronousDraws = synchronous == 1;
            request.requestedTime = static_cast<int32_t>(time);
            std::fclose(file);
        }
    }
    if (!request.enabled || packet.sourceServerTime < request.requestedTime) return;
    request.pending = true;
    request.sourceTime = packet.sourceServerTime;
    request.mapGeneration = packet.mapGeneration;
    request.frameGeneration = packet.frameGeneration;
    std::memcpy(request.origin, packet.cameraOrigin, sizeof(request.origin));
    std::memcpy(request.axis, packet.cameraAxis, sizeof(request.axis));
    request.nearPlane = packet.nearPlane;
    request.tanHalfFovX = packet.tanHalfFovX;
    request.tanHalfFovY = packet.tanHalfFovY;
}
#endif

void GfxVita_ReleaseGameplayTargets() {
#if KISAK_VITA_CPU_RUNTIME
    if(kisak::vita::VitaCpuRender_OnSource()) { kisak::vita::VitaCpuRender_Call([](void*){GfxVita_ReleaseGameplayTargets();},nullptr);return; }
#endif
    if (g_software || !g_initialized || (!g_gameplayTargets && !g_worldTarget.framebuffer)) return;
    EndNativeStaticPipeline(); { VITA_CPU_SCOPE("render-gpu-finish"); glFinish(); }
#if KISAK_VITA_CPU_RUNTIME
    // Compatible targets remain resident for the next selected-map load.
    if(g_worldScaleActive)GfxVitaWorldScalingTarget(0,g_worldTarget,g_worldScaleTarget,g_worldScaleActive);
    g_gameplayTargets=false;g_gameplayFrame=false;g_nativeViewmodel=false;
    glBindFramebuffer(GL_FRAMEBUFFER,0);
#else
    DestroyWorldTarget();
#endif
}
void GfxVita_SetGameplayFrame(bool active) {
#if KISAK_VITA_CPU_RUNTIME
    if(kisak::vita::VitaCpuRender_OnSource()) { kisak::vita::VitaCpuRender_SetGameplay(active);return; }
#endif
    if(!active && g_worldScaleActive)GfxVitaWorldScalingTarget(0,g_worldTarget,g_worldScaleTarget,g_worldScaleActive);
    g_gameplayFrame=active && g_gameplayTargets;
}
void GfxVita_SetDrawSurfaceIndex(uint32_t surfaceIndex,bool constantBasis) {
#if KISAK_VITA_CPU_RUNTIME
    if(kisak::vita::VitaCpuRender_OnSource()) {
        struct Command {uint32_t index;bool constant;};const Command command{surfaceIndex,constantBasis};
        kisak::vita::VitaCpuRender_Emit(&command,sizeof(command),[](const void* data){
            const auto& c=*static_cast<const Command*>(data);GfxVita_SetDrawSurfaceIndex(c.index,c.constant);
        });
        return;
    }
#endif
    g_materialPass.sourceSurfaceIndex=surfaceIndex;g_worldBasisState.pending=constantBasis;
}
void GfxVita_BeginNativeViewmodel() {
#if KISAK_VITA_CPU_RUNTIME
    if(kisak::vita::VitaCpuRender_OnSource()) { kisak::vita::VitaCpuRender_Emit(nullptr,0,[](const void*){GfxVita_BeginNativeViewmodel();});return; }
#endif
    if (g_software || !g_gameplayFrame || g_nativeViewmodel) return;
    GfxVita_RenderPassBoundary(kisak::vita::VitaRenderPass::PropsEffects);
    const uint64_t start=sceKernelGetProcessTimeWide();
    EndNativeStaticPipeline();
    g_nativeViewmodel=true;
    BindNativeSceneTarget();
    if (g_worldTarget.framebuffer) {
        // Reference phases keep the original two draws. Candidate phase copies
        // identical world colour and initializes far depth in one vertex-backed
        // draw; lighting, texture filtering and the authored post pass stay intact.
        const bool fused=g_renderPassProbe.composition && g_renderPassProbe.phase==2;
        const bool audit=g_renderPassProbe.composition || g_renderPassProbe.passIsolation;
        GLfloat depthBefore[2]{};if(audit)glGetFloatv(GL_DEPTH_RANGE,depthBefore);
        if (!fused) Clear(0,0,0,1);
        else {
            SetMaterialPass(nullptr);g_effectBatch={};
            SetSurfaceMode(g_nativeSurfaceMode); // same state reset as Clear
        }
        glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
        glDisable(GL_ALPHA_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
        glDisable(GL_FOG); glDisable(GL_SCISSOR_TEST);
        glUseProgram(g_worldCopy.program);
        glActiveTexture(GL_TEXTURE0); glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D,g_worldTarget.texture);
        glUniform1i(g_worldCopy.sampler,0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,0);
        glBindBuffer(GL_ARRAY_BUFFER,g_nativePost.compositeVertexBuffer);
        glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
        glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,4*sizeof(float),nullptr);
        glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,4*sizeof(float),reinterpret_cast<void*>(2*sizeof(float)));
        GfxVitaCompositionState depthDuring;GLint depthFunction=0;
        GfxVitaCompositionDepth(fused,
            [](float* range){glGetFloatv(GL_DEPTH_RANGE,range);},
            [](float nearDepth,float farDepth){glDepthRangef(nearDepth,farDepth);},
            [](bool always){glEnable(GL_DEPTH_TEST);glDepthMask(GL_TRUE);glDepthFunc(always?GL_ALWAYS:GL_LEQUAL);},
            [&]{
                if(audit) {
                    GLboolean mask=GL_FALSE,color[4]{};
                    glGetFloatv(GL_DEPTH_RANGE,depthDuring.range);glGetBooleanv(GL_DEPTH_WRITEMASK,&mask);
                    glGetBooleanv(GL_COLOR_WRITEMASK,color);glGetIntegerv(GL_DEPTH_FUNC,&depthFunction);
                    depthDuring.depth=glIsEnabled(GL_DEPTH_TEST);depthDuring.write=mask;depthDuring.always=depthFunction==GL_ALWAYS;
                    for(unsigned i=0;i<4;++i)if(color[i])depthDuring.colorMask|=1u<<i;
                    depthDuring.stencil=glIsEnabled(GL_STENCIL_TEST);depthDuring.blend=glIsEnabled(GL_BLEND);
                    depthDuring.alpha=glIsEnabled(GL_ALPHA_TEST);depthDuring.scissor=glIsEnabled(GL_SCISSOR_TEST);
                }
                glDrawArrays(GL_TRIANGLE_STRIP,0,4);
            });
        if(audit) {
            GLfloat depthAfter[2]{};glGetFloatv(GL_DEPTH_RANGE,depthAfter);
            const unsigned failures=GfxVitaCompositionCheck(fused,depthBefore,depthDuring,depthAfter);
            const auto counter=kisak::vita::VitaCpuProfile_Counter;
            const auto bits=kisak::vita::VitaGpuFloatBits;
            counter("render-probe-composition-copy",1,fused?1:0);
            counter("render-probe-composition-valid",failures,uint64_t(g_nativePost.width)*g_nativePost.height);
            counter("render-composition-depth-before",bits(depthBefore[0]),bits(depthBefore[1]));
            counter("render-composition-depth-during",bits(depthDuring.range[0]),bits(depthDuring.range[1]));
            counter("render-composition-depth-after",bits(depthAfter[0]),bits(depthAfter[1]));
            counter("render-composition-depth-state",depthDuring.Flags(),uint32_t(depthFunction));
        }
        ++g_frameDrawCalls; g_frameTriangles+=2;
        glDisableVertexAttribArray(1); glDisableVertexAttribArray(0);
        glBindBuffer(GL_ARRAY_BUFFER,0); glUseProgram(0);
        glBindTexture(GL_TEXTURE_2D,0); glDisable(GL_TEXTURE_2D);
        ResetExtraFixedFunctionTextureUnits();
        glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY);
        glClientActiveTexture(GL_TEXTURE0); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE);
    }
    g_compositionUsec+=static_cast<uint32_t>(sceKernelGetProcessTimeWide()-start);
    GfxVita_RenderPassBoundary(kisak::vita::VitaRenderPass::Composition);
}
bool GfxVita_PrepareLightmapLayouts(const uint32_t* textureLeases,uint32_t count) {
    if(!textureLeases || !count || count>8)return false;
#if KISAK_VITA_CPU_RUNTIME
    if(kisak::vita::VitaCpuRender_OnSource()) {
        struct Args {std::array<uint32_t,8> leases{};uint32_t count=0;bool result=false;};Args args;
        std::copy_n(textureLeases,count,args.leases.begin());args.count=count;
        kisak::vita::VitaCpuRender_Call([](void* p){auto& a=*static_cast<Args*>(p);a.result=GfxVita_PrepareLightmapLayouts(a.leases.data(),a.count);},&args);return args.result;
    }
#endif
    if(g_software || !g_initialized)return false;
    if(!g_lightmapLayoutBytes)g_lightmapLayoutFault=false;
    if(!SetNativeLightmapLayout(false))return false;
    DestroyNativeLightmapLayout(0);g_lightmapLayoutFailures=0;g_lightmapLayoutFault=false;g_lightmapTransitionClock.Reset();
    kisak::vita::VitaCpuProfile_Counter("lightmap-layout-inplace-schema",1,sizeof(g_lightmapWorkspace));
    const bool ready=GfxVitaPrepareLightmapBatch(textureLeases,count,
        [](uint32_t lease){return PrepareNativeLightmapLayout({lease});},
        []{DestroyNativeLightmapLayout(0);});
    kisak::vita::VitaCpuProfile_Counter("lightmap-layout-batch-ready",ready,count);
    return ready;
}
void GfxVita_SetSimpleWorldLighting(bool enabled) {
#if KISAK_VITA_CPU_RUNTIME
    if(kisak::vita::VitaCpuRender_OnSource()) {
        kisak::vita::VitaCpuRender_Call([](void* data){GfxVita_SetSimpleWorldLighting(*static_cast<bool*>(data));},&enabled);return;
    }
#endif
    g_simpleWorldRequested=enabled;
    kisak::vita::VitaCpuProfile_Counter("world-lighting-setting",enabled,g_cheapWorldReady);
}
void GfxVita_ReleaseShaderCompiler() {
#if KISAK_VITA_CPU_RUNTIME
    if (kisak::vita::VitaCpuRender_OnSource()) {
        kisak::vita::VitaCpuRender_Call([](void*) { GfxVita_ReleaseShaderCompiler(); }, nullptr);
        return;
    }
#endif
    if (g_software || !g_initialized) return;
    VITA_CPU_LOADING("shader-compiler-release");
    const auto before = mallinfo();
    // Pinned VitaGL restarts vitaShaRK on a later shader compile request. Existing
    // shader/program objects own their compiled bytes, independently of it.
    glReleaseShaderCompiler();
    const auto after = mallinfo();
    kisak::vita::VitaCpuProfile_Counter("shader-compiler-heap-before-after",
        static_cast<uint64_t>(before.uordblks), static_cast<uint64_t>(after.uordblks));
}

bool GfxVita_PrepareGameplayTargets(uint32_t width,uint32_t height,bool adaptiveLighting) {
#if KISAK_VITA_CPU_RUNTIME
    if(kisak::vita::VitaCpuRender_OnSource()) {
        struct Args {uint32_t width,height;bool adaptive,result;};Args args{width,height,adaptiveLighting,false};
        kisak::vita::VitaCpuRender_Call([](void* p){auto& a=*static_cast<Args*>(p);a.result=GfxVita_PrepareGameplayTargets(a.width,a.height,a.adaptive);},&args);return args.result;
    }
#endif
    if (g_software || !g_initialized) return false;
    if (!((width==960 && height==544)||(width==720 && height==408)||(width==480 && height==272))) return false;
#if KISAK_VITA_CPU_RUNTIME
    if(g_nativePost.width==960 && g_nativePost.height==544
        && ((width==960 && !g_worldTarget.framebuffer)
            || (g_worldTarget.framebuffer && g_worldTarget.width==width && g_worldTarget.height==height))) {
        VITA_CPU_LOADING("gameplay-target-reuse");
        EndNativeStaticPipeline();{ VITA_CPU_SCOPE("render-gpu-finish"); glFinish(); }
        g_benchmarkShaderMode=0;g_benchmarkClear=false;g_benchmarkScene=false;
        g_adaptiveLighting=adaptiveLighting;g_gameplayTargets=true;
        GfxVita_SetGameplayFrame(true);
        BeginFrame();Clear(0,0,0,1);GfxVita_BeginNativeViewmodel();EndFrame();
        { VITA_CPU_SCOPE("render-gpu-finish"); glFinish(); }GfxVita_SetGameplayFrame(false);
        return glGetError()==GL_NO_ERROR;
    }
#endif
    return GfxVitaTargetTransaction(width,height,
    [] { EndNativeStaticPipeline(); { VITA_CPU_SCOPE("render-gpu-finish"); glFinish(); } DestroyWorldTarget(); },
    [&] {
    g_benchmarkShaderMode=0; g_benchmarkClear=false; g_benchmarkScene=false;
    g_adaptiveLighting=adaptiveLighting;
    if (g_nativePost.width!=960 || g_nativePost.height!=544) {
        DestroyNativePostResources();
        if (!CreateNativePostResources(960,544)) return false;
    }
    if (width!=960) {
        static constexpr const char* copy=R"VITA_SHADER(
#version 100
precision highp float;
uniform sampler2D u_scene;
varying vec2 v_texcoord;
void main() { gl_FragColor=vec4(texture2D(u_scene,v_texcoord).rgb,1.0); }
)VITA_SHADER";
        if(!g_worldCopy.program) {
        VITA_CPU_LOADING("world-composition-program-create");
        GLuint vs=0,fs=0;
        if (!CompileNativeShader(GL_VERTEX_SHADER,kNativePostVertexShader,&vs)
            || !CompileNativeShader(GL_FRAGMENT_SHADER,copy,&fs)) {
            if(vs) glDeleteShader(vs); if(fs) glDeleteShader(fs); return false;
        }
        g_worldCopy.program=glCreateProgram();
        const GLuint program=g_worldCopy.program;
        if(!program) { glDeleteShader(vs); glDeleteShader(fs); return false; }
        glAttachShader(program,vs); glAttachShader(program,fs);
        glBindAttribLocation(program,0,"a_position"); glBindAttribLocation(program,1,"a_texcoord");
        glLinkProgram(program); glDeleteShader(vs); glDeleteShader(fs);
        GLint linked=0; glGetProgramiv(program,GL_LINK_STATUS,&linked);
        if(!linked) { glDeleteProgram(program);g_worldCopy={};return false; }
        g_worldCopy.sampler=glGetUniformLocation(program,"u_scene");
        }
        if(!CreateWorldTarget(g_worldTarget,width,height))return false;
        if(width==720) {
            VITA_CPU_LOADING("world-scaling-target-create");
            if(!CreateWorldTarget(g_worldScaleTarget,480,272))return false;
        }
    }
    return true;
    }, [&] {
    g_gameplayTargets=true;
    // Force deferred GXM target/pipeline setup at the load boundary, before
    // map textures consume the remaining contiguous allocation budget.
    GfxVita_SetGameplayFrame(true);
    for (unsigned i=0;i<3;++i) {
        BeginFrame(); Clear(0,0,0,1); GfxVita_BeginNativeViewmodel(); EndFrame();
    }
    { VITA_CPU_SCOPE("render-gpu-finish"); glFinish(); }
    if(g_worldScaleTarget.framebuffer) {
        std::swap(g_worldTarget,g_worldScaleTarget);
        for(unsigned i=0;i<3;++i){BeginFrame();Clear(0,0,0,1);GfxVita_BeginNativeViewmodel();EndFrame();}
        { VITA_CPU_SCOPE("render-gpu-finish"); glFinish(); }
        std::swap(g_worldTarget,g_worldScaleTarget);
    }
    const bool valid=glGetError()==GL_NO_ERROR;
    GfxVita_SetGameplayFrame(false);
    if(!valid) DestroyWorldTarget();
    return valid;
    });
}

bool GfxVita_SetBenchmarkTarget(uint32_t width, uint32_t height, bool enabled, bool clearOnly, uint32_t shaderMode) {
#if KISAK_VITA_CPU_RUNTIME
    if(kisak::vita::VitaCpuRender_OnSource()) {
        struct Args {uint32_t width,height,mode;bool enabled,clear,result;};Args args{width,height,shaderMode,enabled,clearOnly,false};
        kisak::vita::VitaCpuRender_Call([](void* p){auto& a=*static_cast<Args*>(p);a.result=GfxVita_SetBenchmarkTarget(a.width,a.height,a.enabled,a.clear,a.mode);},&args);return args.result;
    }
#endif
    if (shaderMode > 7 || (!enabled && shaderMode)) return false;
#if !KISAK_VITA_BENCHMARK
    if (shaderMode) return false;
#endif
    if (g_software || !g_initialized) return false;
    if (!((width==960 && height==544) || (width==720 && height==408) || (width==480 && height==272))) return false;
    const bool postChanged=(g_benchmarkShaderMode==1) != (shaderMode==1);
#if KISAK_VITA_BENCHMARK
    if (shaderMode==2 && !g_nativeSimpleShader.program
        && !CreateNativeLitShaderProgram(g_nativeSimpleShader,kBenchmarkSimpleMaterialFragment)) return false;
    if (shaderMode==7) {
        std::string source(kNativeLitFragmentShader);
        const auto versionEnd=source.find('\n',source.find("#version"));
        source.insert(versionEnd+1,"#define VITA_NO_FOG 1\n");
        if (!g_nativeNoFogShader.program
            && !CreateNativeLitShaderProgram(g_nativeNoFogShader,source.c_str())) return false;
        source.insert(versionEnd+1,"#define VITA_SUN_ONLY 1\n");
        if (!g_nativeSunNoFogShader.program
            && !CreateNativeLitShaderProgram(g_nativeSunNoFogShader,source.c_str())) return false;
    }
    if (shaderMode==6) {
        std::string source(kNativeLitFragmentShader);
        const auto versionEnd=source.find('\n',source.find("#version"));
        source.insert(versionEnd+1,"#define VITA_NO_NORMAL_MAP 1\n");
        if (!g_nativeNoNormalShader.program
            && !CreateNativeLitShaderProgram(g_nativeNoNormalShader,source.c_str())) return false;
        source.insert(versionEnd+1,"#define VITA_SUN_ONLY 1\n");
        if (!g_nativeSunNoNormalShader.program
            && !CreateNativeLitShaderProgram(g_nativeSunNoNormalShader,source.c_str())) return false;
    }
    if (shaderMode==4) {
        std::string source(kNativeLitFragmentShader);
        const auto versionEnd=source.find('\n',source.find("#version"));
        source.insert(versionEnd+1,"#define VITA_NO_SPECULAR 1\n");
        if (!g_nativeNoSpecularShader.program
            && !CreateNativeLitShaderProgram(g_nativeNoSpecularShader,source.c_str())) return false;
        source.insert(versionEnd+1,"#define VITA_SUN_ONLY 1\n");
        if (!g_nativeSunNoSpecularShader.program
            && !CreateNativeLitShaderProgram(g_nativeSunNoSpecularShader,source.c_str())) return false;
    }
#endif
    EndNativeStaticPipeline();
    g_adaptiveLighting=false; // Historical diagnostic baselines retain their original dispatch.
    g_benchmarkShaderMode=shaderMode;
    SelectNativeLightShader();
    if (postChanged || g_nativePost.width != width || g_nativePost.height != height) {
        // Test boundary only: wait before releasing the old target. Neither
        // this wait nor resource/shader creation belongs to measurements.
        EndNativeStaticPipeline(); { VITA_CPU_SCOPE("render-gpu-finish"); glFinish(); }
        DestroyWorldTarget();
        if(g_worldCopy.program)glDeleteProgram(g_worldCopy.program);
        g_worldCopy={};
        DestroyNativePostResources();
        if (!CreateNativePostResources(width,height)) return false;
    }
    g_benchmarkScene=enabled; g_benchmarkClear=enabled && clearOnly; g_nativeHud=false;
    return true;
}
void GfxVita_BeginNativeHud() {
#if KISAK_VITA_CPU_RUNTIME
    if(kisak::vita::VitaCpuRender_OnSource()) { kisak::vita::VitaCpuRender_Emit(nullptr,0,[](const void*){GfxVita_BeginNativeHud();});return; }
#endif
    if (g_software || (!g_benchmarkScene && !g_gameplayTargets) || g_nativeHud || g_benchmarkClear) return;
    GfxVita_BeginNativeViewmodel();
    GfxVita_RenderPassBoundary(kisak::vita::VitaRenderPass::Weapon);
    const uint64_t start=sceKernelGetProcessTimeWide();
    ApplyNativePostEffects();
    g_postSubmitUsec += static_cast<uint32_t>(sceKernelGetProcessTimeWide()-start);
    GfxVita_RenderPassBoundary(kisak::vita::VitaRenderPass::Post);
    g_nativeHud=true;
    SelectNativeLightShader(); // UI keeps source colors, alpha tests and blend state.
    SetMaterialPass(nullptr);
    SetSurfaceMode(GfxSurfaceMode::Overlay);
}

bool GfxVita_GetFrameCounters(GfxVitaFrameCounters* output) {
#if KISAK_VITA_CPU_RUNTIME
    if(kisak::vita::VitaCpuRender_OnSource()) { if(!output)return false; *output=kisak::vita::VitaCpuRender_Read().counters;return true; }
#endif
    if (!output || !g_initialized)
        return false;
    output->frameNumber = g_rendererFrame;
    output->drawCalls = g_frameDrawCalls;
    output->triangles = g_frameTriangles;
    output->stateChanges = g_frameStateChanges;
    output->materialPasses = g_frameMaterialPasses;
    output->effectBatches = g_frameEffectBatches;
    output->transientUploadDrops = g_frameTransientUploadDrops;
    output->transientUploadBytes = g_frameTransientUploadBytes;
    output->transientUploadHighWaterBytes = g_transientUploadHighWaterBytes;
    output->swapWaitUsec = g_lastSwapWaitUsec;
    std::memcpy(output->swapDetail, g_swapDetail, sizeof(g_swapDetail));
    output->postSubmitUsec = g_postSubmitUsec;
    output->shaderMode=g_benchmarkShaderMode; output->sceneShaderDraws=g_sceneShaderDraws;
    output->simpleMaterialDraws=g_simpleMaterialDraws; output->postCopyDraws=g_postCopyDraws;
    output->noLocalLightDraws=g_noLocalLightDraws; output->noSpecularDraws=g_noSpecularDraws;
    output->specularEligibleDraws=g_specularEligibleDraws;
    output->noNormalMapDraws=g_noNormalMapDraws; output->normalEligibleDraws=g_normalEligibleDraws;
    output->adaptiveFullLightDraws=g_adaptiveFullLightDraws;
    output->noFogDraws=g_noFogDraws; output->fogEligibleDraws=g_fogEligibleDraws;
    output->fogEnabled=g_fogEligibleDraws!=0;
    output->fogStart=g_sceneFogStart; output->fogDensity=g_sceneFogDensity;
    output->authoredPostDraws=g_authoredPostDraws;
    vglCod4ConfigRead(output->config);
    output->sunOnlyShader = g_nativeLitShader.program == g_nativeSunOnlyShader.program
        || (g_nativePropShader.program && g_nativeLitShader.program==g_nativePropShader.program)
        || (g_nativeOpaqueSunShader.program && g_nativeLitShader.program==g_nativeOpaqueSunShader.program) || (g_nativeSpecularShader[1].program && g_nativeLitShader.program==g_nativeSpecularShader[1].program) || NativeBoundMaterialFamily() || NativeBoundPrefetch();
    output->bloomActive = (g_viewEffects.flags & GFX_FRAME_LIGHTING_HAS_GLOW) && g_viewEffects.glowEnabled > 0.5f && g_viewEffects.glowBloomIntensity0 > 0 && g_viewEffects.glowRadius0 > 0;
    output->sceneWidth = g_gameplayFrame && g_worldTarget.width ? g_worldTarget.width : g_nativePost.width;
    output->sceneHeight = g_gameplayFrame && g_worldTarget.height ? g_worldTarget.height : g_nativePost.height;
    output->viewmodelWidth=g_gameplayTargets ? 960u : output->sceneWidth;
    output->viewmodelHeight=g_gameplayTargets ? 544u : output->sceneHeight;
    output->compositionUsec=g_compositionUsec;
    output->viewportSeen=g_viewportSeen; output->viewportMismatches=g_viewportMismatches;
    std::memcpy(output->observedViewports,g_observedViewports,sizeof(g_observedViewports));
    output->litFullDraws=g_litFullDraws; output->litSunDraws=g_litSunDraws;
    output->adaptiveLighting=g_adaptiveLighting;
    output->targetLogicalBytes = uint64_t(g_nativePost.width) * g_nativePost.height * 6u + uint64_t(g_worldTarget.width)*g_worldTarget.height*6u + uint64_t(g_worldScaleTarget.width)*g_worldScaleTarget.height*6u;
    output->shadowPasses = g_frameShadowPasses;
    output->lightCount = g_frameLighting.primaryLightCount + g_frameLighting.dynamicLightCount;
    output->nonSunLightInputs=0;
    if (g_frameLighting.flags & GFX_FRAME_LIGHTING_VALID) {
        const auto hasColor=[](const GfxPrimaryLightDescriptor& light) {
            return light.color[0]>0 || light.color[1]>0 || light.color[2]>0;
        };
        for (uint32_t i=0;i<g_frameLighting.primaryLightCount;++i)
            if (i!=g_frameLighting.sunPrimaryLightIndex && hasColor(g_frameLighting.primaryLights[i]))
                ++output->nonSunLightInputs;
        for (uint32_t i=0;i<g_frameLighting.dynamicLightCount;++i)
            if (hasColor(g_frameLighting.dynamicLights[i])) ++output->nonSunLightInputs;
    }
    output->textureResidentBytes = g_software
        ? static_cast<uint64_t>(g_textureBytes) : g_nativeTextureBytes;
    output->staticBufferResidentBytes = g_nativeStaticBufferBytes;
    return true;
}

bool GfxVita_SampleImageNearest(const uint8_t* payload, size_t payloadBytes,
                                uint32_t width, uint32_t height,
                                uint32_t levelCount, uint32_t format,
                                float u, float v, float output[4]) {
    if (!output || !payload || payloadBytes == 0 || width == 0 || height == 0
        || width > 4096 || height > 4096 || levelCount == 0 || levelCount > 16) {
        return false;
    }
    for (uint32_t component = 0; component < 4; ++component)
        output[component] = 1.0f;
    // Material UVs in the desktop renderer use the sampler's repeat mode.
    // Keep the same wrapping for the pointer-free CPU sample, including
    // negative coordinates on tiled ground/container surfaces.
    const auto Wrap = [](float value) {
        if (!std::isfinite(value))
            return 0.0f;
        value -= std::floor(value);
        return value < 0.0f ? value + 1.0f : value;
    };
    const float wrappedU = Wrap(u);
    const float wrappedV = Wrap(v);
    const uint32_t x = std::min(width - 1,
                                static_cast<uint32_t>(wrappedU * width));
    const uint32_t y = std::min(height - 1,
                                static_cast<uint32_t>(wrappedV * height));
    uint32_t value = 0xFFFFFFFFu;
    if (format == 50u) { // D3DFMT_L8
        const size_t required = static_cast<size_t>(width) * height;
        if (required > payloadBytes)
            return false;
        const uint32_t sample = payload[static_cast<size_t>(y) * width + x];
        value = sample | (sample << 8u) | (sample << 16u) | 0xFF000000u;
    } else if (format == 21u) { // D3DFMT_A8R8G8B8
        const size_t pixel = static_cast<size_t>(y) * width + x;
        const size_t required = (pixel + 1u) * sizeof(uint32_t);
        if (required > payloadBytes)
            return false;
        std::memcpy(&value, payload + pixel * sizeof(uint32_t), sizeof(value));
        value = CanonicalizeA8R8G8B8(value);
    } else if (format == 0x31545844u || format == 0x33545844u
               || format == 0x35545844u) {
        const uint32_t blockBytes = format == 0x31545844u ? 8u : 16u;
        const uint32_t blocksWide = (width + 3u) / 4u;
        const size_t blockOffset = (static_cast<size_t>(y / 4u) * blocksWide
                                    + x / 4u) * blockBytes;
        if (blockOffset > payloadBytes || blockBytes > payloadBytes - blockOffset)
            return false;
        // This helper feeds the source normal/specular semantic bridge. Do
        // not route it through the scalar display texture's RGBA4444 store:
        // the authored DXT block already contains the full 8-bit decoded
        // channel values needed by DXT5nm tangent reconstruction and masks.
        uint32_t decoded[16]{};
        DecodeDxtBlockRgba8888(payload + blockOffset, format, decoded, 4u,
                               0u, 0u);
        value = decoded[(y & 3u) * 4u + (x & 3u)];
    } else {
        return false;
    }
    output[0] = static_cast<float>(value & 0xFFu) / 255.0f;
    output[1] = static_cast<float>((value >> 8u) & 0xFFu) / 255.0f;
    output[2] = static_cast<float>((value >> 16u) & 0xFFu) / 255.0f;
    output[3] = static_cast<float>((value >> 24u) & 0xFFu) / 255.0f;
    return true;
}

bool GfxVita_DrawSkyCubemap(GfxTextureHandle texture, float fovDegrees,
                            float pitchDegrees, float yawDegrees,
                            const float (*cameraAxis)[3]) {
#if KISAK_VITA_CPU_RUNTIME
    if(kisak::vita::VitaCpuRender_OnSource()) {
        if(!kisak::vita::VitaCpuRender_IsCube(texture))return false;
        struct Args {GfxTextureHandle texture;float fov,pitch,yaw,axis[3][3];bool hasAxis;};
        Args args{texture,fovDegrees,pitchDegrees,yawDegrees,{},cameraAxis!=nullptr};
        if(cameraAxis)std::memcpy(args.axis,cameraAxis,sizeof(args.axis));
        return kisak::vita::VitaCpuRender_Copy(args,+[](const Args& a){GfxVita_DrawSkyCubemap(a.texture,a.fov,a.pitch,a.yaw,a.hasAxis?a.axis:nullptr);});
    }
#endif
    if (g_software) {
        if (!SoftwareDrawSkyCubemap(texture, fovDegrees, pitchDegrees,
                                    yawDegrees))
            return false;
        // Keep the following BSP pass in its normal opaque state. The direct
        // scalar fill does not touch depth, matching the source sky pass.
        g_softwareSurfaceMode = GfxSurfaceMode::Opaque;
        g_softwareDepthTest = true;
        g_softwareDepthWrite = true;
        return true;
    }
    if (!g_initialized || !IsCubeTextureHandle(texture.value))
        return false;
    constexpr float kPiOver180 = 0.01745329251994329577f;
    constexpr float kPiOver360 = 0.00872664625997164788f;
    constexpr float kScreenAspect = 960.0f / 544.0f;
    const float pitch = std::clamp(pitchDegrees, -89.9f, 89.9f) * kPiOver180;
    const float yaw = yawDegrees * kPiOver180;
    const float sp = std::sin(pitch), cp = std::cos(pitch);
    const float sy = std::sin(yaw), cy = std::cos(yaw);
    float forward[3] = {cp * cy, cp * sy, -sp};
    float right[3] = {sy, -cy, 0.0f};
    float up[3] = {sp * cy, sp * sy, cp};
    if (cameraAxis) {
        std::copy_n(cameraAxis[0], 3, forward);
        std::copy_n(cameraAxis[2], 3, up);
        for (int i = 0; i < 3; ++i) right[i] = -cameraAxis[1][i];
    }
    const float fov = std::clamp(fovDegrees, 1.0f, 160.0f);
    const float tanHalfFovY = std::max(0.0001f,
                                      std::tan(fov * kPiOver360) * 0.75f);
    const float horizontalScale = 1.0f / (tanHalfFovY * kScreenAspect);
    const float verticalScale = 1.0f / tanHalfFovY;
    const float clipCorners[4][2] = {
        {-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f},
    };
    GfxDrawVertex vertices[4]{};
    for (uint32_t index = 0; index < 4u; ++index) {
        const float clipX = clipCorners[index][0];
        const float clipY = clipCorners[index][1];
        vertices[index].position[0] = clipX;
        vertices[index].position[1] = clipY;
        vertices[index].position[2] = 1.0f;
        vertices[index].color[0] = 1.0f;
        vertices[index].color[1] = 1.0f;
        vertices[index].color[2] = 1.0f;
        vertices[index].color[3] = 1.0f;
        vertices[index].worldPosition[0] = forward[0]
            + clipX / horizontalScale * right[0]
            + clipY / verticalScale * up[0];
        vertices[index].worldPosition[1] = forward[1]
            + clipX / horizontalScale * right[1]
            + clipY / verticalScale * up[1];
        vertices[index].worldPosition[2] = forward[2]
            + clipX / horizontalScale * right[2]
            + clipY / verticalScale * up[2];
    }
    static constexpr uint16_t kSkyIndices[] = {0, 1, 2, 0, 2, 3};
    SetMaterialPass(nullptr);
    SetSurfaceMode(GfxSurfaceMode::Sky);
    SetCullMode(GfxCullMode::None);
    BindTexture(GfxTextureHandle{0});
    g_nativeSkyDrawTexture = texture;
    DrawIndexed(vertices, 4, kSkyIndices, 6);
    g_nativeSkyDrawTexture = {};
    BindTexture(GfxTextureHandle{0});
    SetSurfaceMode(GfxSurfaceMode::Opaque);
#if KISAK_VITA_PROFILE
    CaptureNativeSceneFrame("sky");
#endif
    return true;
}

void GfxVita_DrawWeather(GfxTextureHandle texture, float rate, float slant,
                         float coverage, uint32_t timeMsec,
                         uint32_t sourceSeed,
                         uint32_t authoredPlacementCount,
                         const GfxEffectBatchState* authoredState) {
#if KISAK_VITA_CPU_RUNTIME
    if(kisak::vita::VitaCpuRender_OnSource()) {
        struct Args {GfxTextureHandle texture;float rate,slant,coverage;uint32_t time,seed,count;bool hasState;GfxEffectBatchState state;};
        const Args args{texture,rate,slant,coverage,timeMsec,sourceSeed,authoredPlacementCount,authoredState!=nullptr,authoredState?*authoredState:GfxEffectBatchState{}};
        kisak::vita::VitaCpuRender_Copy(args,+[](const Args& a){GfxVita_DrawWeather(a.texture,a.rate,a.slant,a.coverage,a.time,a.seed,a.count,a.hasState?&a.state:nullptr);});return;
    }
#endif
    if (!g_initialized || rate <= 0.0f || coverage <= 0.0f)
        return;
    constexpr uint32_t kMaxDrops = 128u;
    const uint32_t dropCount = std::clamp(static_cast<uint32_t>(
        24.0f + std::clamp(rate, 0.0f, 1.0f) * 88.0f
            * std::clamp(coverage, 0.0f, 1.0f)), 8u, kMaxDrops);
    ++g_frameWeatherDraws;
    g_frameWeatherDrops += dropCount;
    GfxDrawVertex vertices[kMaxDrops * 4u]{};
    uint16_t indices[kMaxDrops * 6u]{};
    const auto Hash = [](uint32_t value) {
        value ^= value >> 16u;
        value *= 0x7feb352du;
        value ^= value >> 15u;
        value *= 0x846ca68bu;
        return value ^ (value >> 16u);
    };
    const uint32_t anchorCount = authoredState
        ? std::min(authoredState->placementCount, GFX_MAX_EFFECT_PLACEMENTS)
        : 0u;
    const float horizontalScale = std::isfinite(
        g_frameLighting.cameraHorizontalScale)
        && g_frameLighting.cameraHorizontalScale > 0.0f
        ? g_frameLighting.cameraHorizontalScale : 1.0f;
    const float verticalScale = std::isfinite(g_frameLighting.cameraVerticalScale)
        && g_frameLighting.cameraVerticalScale > 0.0f
        ? g_frameLighting.cameraVerticalScale : 1.0f;
    const bool canProjectAnchors = anchorCount != 0
        && (g_frameLighting.flags & GFX_FRAME_LIGHTING_VALID) != 0;
    uint32_t projectedAnchorCount = 0;
    for (uint32_t drop = 0; drop < dropCount; ++drop) {
        const uint32_t placementIndex = anchorCount != 0
            ? drop % anchorCount : 0u;
        const GfxEffectPlacementDescriptor* placement = canProjectAnchors
            ? &authoredState->placements[placementIndex] : nullptr;
        const float delayMsec = placement
            ? std::min(std::fabs(placement->delay) * 1000.0f,
                       static_cast<float>(std::numeric_limits<uint32_t>::max()))
            : 0.0f;
        const uint32_t placementDelay = static_cast<uint32_t>(delayMsec);
        const uint32_t seed = Hash(drop * 0x9e3779b9u + timeMsec / 70u
                                   + g_frameLighting.mapIndex * 131u
                                   + sourceSeed
                                   + authoredPlacementCount * 0x45d9f3bu
                                   + placementDelay);
        float x = static_cast<float>(seed & 0xFFFFu) / 32767.5f - 1.0f;
        float y = static_cast<float>((seed >> 16u) & 0xFFFFu)
            / 32767.5f - 1.0f;
        if (placement) {
            const float relative[3] = {
                placement->origin[0] - g_frameLighting.cameraPosition[0],
                placement->origin[1] - g_frameLighting.cameraPosition[1],
                placement->origin[2] - g_frameLighting.cameraPosition[2],
            };
            const float cameraX = relative[0] * g_frameLighting.cameraRight[0]
                + relative[1] * g_frameLighting.cameraRight[1]
                + relative[2] * g_frameLighting.cameraRight[2];
            const float cameraY = relative[0] * g_frameLighting.cameraUp[0]
                + relative[1] * g_frameLighting.cameraUp[1]
                + relative[2] * g_frameLighting.cameraUp[2];
            const float cameraZ = relative[0] * g_frameLighting.cameraForward[0]
                + relative[1] * g_frameLighting.cameraForward[1]
                + relative[2] * g_frameLighting.cameraForward[2];
            if (std::isfinite(cameraX) && std::isfinite(cameraY)
                && std::isfinite(cameraZ) && cameraZ > 2.0f) {
                const float anchorX = cameraX / cameraZ * horizontalScale;
                const float anchorY = cameraY / cameraZ * verticalScale;
                const float jitterX = static_cast<float>((seed >> 3u) & 0xFFFFu)
                    / 32767.5f - 1.0f;
                const float jitterY = static_cast<float>((seed >> 19u) & 0x1FFFu)
                    / 4095.5f - 1.0f;
                x = anchorX + jitterX * 0.42f;
                y = anchorY + jitterY * 0.30f;
                ++projectedAnchorCount;
            }
        }
        const float length = 0.055f + static_cast<float>(seed & 0xFFu)
            / 255.0f * 0.075f;
        const float width = 0.0025f + static_cast<float>((seed >> 8u) & 0x1Fu)
            / 31.0f * 0.0025f;
        const float dx = std::clamp(slant, -1.0f, 1.0f) * length * 0.35f;
        const uint32_t vertex = drop * 4u;
        const float alpha = 0.12f + std::clamp(coverage, 0.0f, 1.0f) * 0.18f;
        // The source rain material supplies the streak colour. Keep the
        // vertex tint neutral when that authored image is resident; the
        // blue-grey colour remains the explicit compatibility fallback when
        // an older package has no weather image payload.
        const float color[4] = {
            texture.value != 0 ? 1.0f : 0.60f,
            texture.value != 0 ? 1.0f : 0.72f,
            texture.value != 0 ? 1.0f : 0.82f,
            alpha,
        };
        const float texCoords[4][2] = {
            {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f},
        };
        const float points[4][2] = {
            {x - width, y + length}, {x + width, y + length},
            {x + dx + width, y - length}, {x + dx - width, y - length},
        };
        for (uint32_t corner = 0; corner < 4u; ++corner) {
            vertices[vertex + corner].position[0] = points[corner][0];
            vertices[vertex + corner].position[1] = points[corner][1];
            vertices[vertex + corner].position[2] = 0.05f;
            std::memcpy(vertices[vertex + corner].color, color,
                        sizeof(vertices[vertex + corner].color));
            if (texture.value != 0)
                std::memcpy(vertices[vertex + corner].texCoord,
                            texCoords[corner],
                            sizeof(vertices[vertex + corner].texCoord));
        }
        const uint32_t index = drop * 6u;
        indices[index + 0] = static_cast<uint16_t>(vertex + 0u);
        indices[index + 1] = static_cast<uint16_t>(vertex + 1u);
        indices[index + 2] = static_cast<uint16_t>(vertex + 2u);
        indices[index + 3] = static_cast<uint16_t>(vertex + 0u);
        indices[index + 4] = static_cast<uint16_t>(vertex + 2u);
        indices[index + 5] = static_cast<uint16_t>(vertex + 3u);
    }
    SetSurfaceMode(GfxSurfaceMode::AlphaBlend);
    SetCullMode(GfxCullMode::None);
    BindTexture(texture);
    GfxEffectBatchState effect{};
    if (authoredState)
        effect = *authoredState;
    effect.flags = GFX_EFFECT_BATCH_WEATHER | GFX_EFFECT_BATCH_PARTICLE;
    effect.colorTexture = texture;
    effect.colorSamplerState = 0x0Bu;
    effect.surfaceMode = GfxSurfaceMode::AlphaBlend;
    effect.alphaTestFunc = GfxAlphaTestFunc::Greater0;
    effect.color[0] = texture.value != 0 ? 1.0f : 0.60f;
    effect.color[1] = texture.value != 0 ? 1.0f : 0.72f;
    effect.color[2] = texture.value != 0 ? 1.0f : 0.82f;
    effect.color[3] = 0.12f + std::clamp(coverage, 0.0f, 1.0f) * 0.18f;
    effect.placementCount = anchorCount;
    SetEffectBatch(&effect);
    SetSamplerState(0u, effect.colorSamplerState);
    DrawIndexed(vertices, dropCount * 4u, indices, dropCount * 6u);
    BindTexture(GfxTextureHandle{0});
    SetEffectBatch(nullptr);
    if (g_software)
        g_softwareSurfaceMode = GfxSurfaceMode::Opaque;
    static bool weatherLogged = false;
    if (!weatherLogged) {
        weatherLogged = true;
        sceClibPrintf("[kisak-vita] authored weather pass rain drops=%u texture=%u sourceSeed=%u anchors=%u projected=%u\n",
                      static_cast<unsigned>(dropCount),
                      static_cast<unsigned>(texture.value),
                      static_cast<unsigned>(sourceSeed),
                      static_cast<unsigned>(anchorCount != 0
                                                ? anchorCount
                                                : authoredPlacementCount),
                      static_cast<unsigned>(projectedAnchorCount));
    }
}

bool GfxVita_IsSoftwareFallback() {
    return g_software;
}

void GfxVita_SetSoftwareFileLogEnabled(bool enabled) {
#if KISAK_VITA_CPU_RUNTIME
    if(kisak::vita::VitaCpuRender_OnSource()) {
        kisak::vita::VitaCpuRender_Call(+[](void* p){GfxVita_SetSoftwareFileLogEnabled(*static_cast<bool*>(p));},&enabled);return;
    }
#endif
    g_softwareFileLogEnabled = enabled;
}

void GfxVita_BeginBoundedPass(GfxSurfaceMode mode) {
#if KISAK_VITA_CPU_RUNTIME
    if(kisak::vita::VitaCpuRender_OnSource()) { kisak::vita::VitaCpuRender_Copy(mode,+[](const GfxSurfaceMode& value){GfxVita_BeginBoundedPass(value);});return; }
#endif
    if (g_software) {
        g_softwareRasterPixels = 0;
        g_materialPass = {};
        // Make the pass self-contained: a world/prop caller may have left a
        // different depth-tested mode or texture state active. Opaque prop
        // draws keep depth rejection, while the overlay pass always
        // composites over the sampled world on the scalar path.
        g_softwareDepthTest = mode != GfxSurfaceMode::Overlay;
        g_softwareDepthWrite = mode == GfxSurfaceMode::Opaque
            || mode == GfxSurfaceMode::AlphaTest;
        g_softwareSurfaceMode = mode;
        g_softwareAlphaTestFunc = GfxAlphaTestFunc::GreaterEqual128;
        return;
    }
    ResetVitaGlMatrices();
    const auto viewport=ActivePassViewport();
    glViewport(0,0,viewport.width,viewport.height);
}

void GfxVita_BeginOverlayPass() {
#if KISAK_VITA_CPU_RUNTIME
    if(kisak::vita::VitaCpuRender_OnSource()) { kisak::vita::VitaCpuRender_Emit(nullptr,0,[](const void*){GfxVita_BeginOverlayPass();});return; }
#endif
    GfxVita_BeginBoundedPass(GfxSurfaceMode::Overlay);
}

void GfxVita_WaitIdle() {
#if KISAK_VITA_CPU_RUNTIME
    if(kisak::vita::VitaCpuRender_OnSource()) {kisak::vita::VitaCpuRender_Drain();return;}
#endif
    if(g_initialized && !g_software){EndNativeStaticPipeline();{ VITA_CPU_SCOPE("render-gpu-finish"); glFinish(); }}
}
void GfxVita_SetCpuDiagnostic(bool active){g_cpuDiagnostic=active;}
bool GfxVita_SceneSubmissionEnabled(){return !g_cpuDiagnostic || g_nativeHud;}
bool GfxVita_ProfileDrawSubmission() {
#if KISAK_VITA_CPU_RUNTIME
    if(g_gameplayFrame && !g_renderPassProbe.requestedPhase && !g_cpuDiagnostic) {
        const bool world=NativeWorldScalingPass() && g_nativeStaticBufferDraw && g_nativeWorldSpaceDraw
            && !g_nativeViewModelSpaceDraw && !g_nativeStaticInstancedDraw && !g_nativeHud && !g_nativeClearDraw;
        const unsigned lane=GfxVitaWorldPrecisionLane(world,false,false,NativeWorldLifetimeEligible(world,false),NativeWorldAlphaEligible(world,false));
        GLint program=0;glGetIntegerv(GL_CURRENT_PROGRAM,&program);unsigned actual=0;
        for(unsigned i=0;i<2;++i)if(g_nativeCheapWorld[i].program && GLuint(program)==g_nativeCheapWorld[i].program)actual=i+1;
        g_simpleWorldEligible+=lane!=0;g_simpleWorldActual+=actual!=0;
        if(actual!=(g_simpleWorldRequested && g_cheapWorldReady==3?lane:0))++g_simpleWorldInvalid;
    }
    if(g_renderPassProbe.worldScaling) {
        const bool world=GpuDrawPass()==unsigned(kisak::vita::VitaRenderPass::World) && g_nativeStaticBufferDraw && g_nativeWorldSpaceDraw && !g_nativeViewModelSpaceDraw && !g_nativeStaticInstancedDraw && !g_nativeHud && !g_nativeClearDraw;
        const bool sky=GpuDrawPass()==unsigned(kisak::vita::VitaRenderPass::World) && !g_nativeClearDraw && !g_nativeHud && (g_nativeSkyDrawTexture.value!=0 || g_materialPass.surfaceMode==GfxSurfaceMode::Sky);
        const unsigned lane=GfxVitaWorldPrecisionLane(world,false,false,NativeWorldLifetimeEligible(world,false),NativeWorldAlphaEligible(world,false));
        GLint program=0;glGetIntegerv(GL_CURRENT_PROGRAM,&program);unsigned actual=0;
        for(unsigned i=0;i<2;++i)if(g_nativeCheapWorld[i].program && GLuint(program)==g_nativeCheapWorld[i].program)actual=i+1;
        if(!g_worldScaling.Draw(sky,world,lane,actual,g_cheapWorldReady,g_materialPass.sourceSurfaceIndex))return false;
    }
    return g_passIsolation.Draw(GpuDrawPass(),g_nativeClearDraw);
#else
    return true;
#endif
}

uint64_t GfxVita_LightmapTransitionTime(bool* failed) {
    if(failed)*failed=g_lightmapLayoutFault.load();
    return g_lightmapTransitionClock.PausedUs(sceKernelGetProcessTimeWide());
}
void GfxVita_BeginRenderProbe(uint32_t phase) {
    g_poseRingUploads=g_poseRingFallbacks=g_poseRingRotations=0;
    g_simpleWorldActual=g_simpleWorldEligible=g_simpleWorldInvalid=0;
    g_poseLightBounded=g_poseLightCulled=g_poseLightMissing=0;
    for(unsigned i=0;i<2;++i)g_localLightBounded[i]=g_localLightCulled[i]=g_localLightMissing[i]=0;
    g_renderPipeline=kisak::vita::VitaRenderPipeline::Select(g_gameplayFrame,g_cpuDiagnostic,phase);
    if(!g_software) {
        const bool swizzled=g_gameplayFrame && kisak::vita::VitaLightmapLayoutMode(phase) && (phase&0xffu)==2;
        if(!SetNativeLightmapLayout(swizzled))g_cpuDiagnostic=true;
    }
    g_renderPassProbe.Begin(!g_software && g_gameplayFrame?phase:0,sceKernelGetProcessTimeWide());
    #if KISAK_VITA_CPU_RUNTIME
    g_passIsolation.Begin(g_renderPassProbe.requestedPhase);
    g_worldScaling.Begin(g_renderPassProbe.requestedPhase);
    if(!GfxVitaWorldScalingTarget(g_renderPassProbe.requestedPhase,g_worldTarget,g_worldScaleTarget,g_worldScaleActive))++g_worldScaling.invalid;
#endif
    kisak::vita::VitaGraphicsProfile_Begin(g_renderPassProbe.graphicsProfile || g_renderPassProbe.passIsolation?g_renderPassProbe.phase:0,!g_renderPassProbe.passIsolation);
}
void GfxVita_RenderPassBoundary(kisak::vita::VitaRenderPass pass) {
#if KISAK_VITA_CPU_RUNTIME
    if(kisak::vita::VitaCpuRender_OnSource()) {
        kisak::vita::VitaCpuRender_Copy(pass,+[](const kisak::vita::VitaRenderPass& value){GfxVita_RenderPassBoundary(value);});return;
    }
    if(!g_renderPassProbe.Observe(pass))return;
    static const char* names[]={"render-pass-setup","render-pass-world","render-pass-props-fx","render-pass-composition","render-pass-weapon","render-pass-post","render-pass-hud"};
    static const char* geometry[]={"render-pass-setup-geometry","render-pass-world-geometry","render-pass-props-fx-geometry","render-pass-composition-geometry","render-pass-weapon-geometry","render-pass-post-geometry","render-pass-hud-geometry"};
    const auto at=sceKernelGetProcessTimeWide();
    // The pinned VitaGL glFinish submits the current scene then waits for GXM.
    // No target, shader, GL bindings or source state is changed here. Diagnostic
    // scene breaks serialize the GPU and include their own store/reload cost.
    if(g_renderPassProbe.Fenced())glFinish();
    const auto finished=sceKernelGetProcessTimeWide();
    if(g_renderPassProbe.Fenced()) {
        const auto error=glGetError();
        if(error!=GL_NO_ERROR)kisak::vita::VitaCpuProfile_Counter("render-probe-gl-error",error,static_cast<uint32_t>(pass));
    }
    const auto wait=g_renderPassProbe.Fenced()?finished-at:0;
    const auto index=static_cast<uint32_t>(pass);
    kisak::vita::VitaCpuProfile_Counter(names[index],at-g_renderPassProbe.lastUs,wait);
    kisak::vita::VitaCpuProfile_Counter(geometry[index],g_frameDrawCalls-g_renderPassProbe.draws,g_frameTriangles-g_renderPassProbe.triangles);
    if(g_renderPassProbe.graphicsProfile || g_renderPassProbe.passIsolation) {
        GLint target=0,viewport[4]={};glGetIntegerv(GL_FRAMEBUFFER_BINDING,&target);glGetIntegerv(GL_VIEWPORT,viewport);
        kisak::vita::VitaGraphicsProfile_Pass(index+1,uint32_t(target),uint32_t(viewport[2]),uint32_t(viewport[3]));
    }
    g_renderPassProbe.draws=g_frameDrawCalls;g_renderPassProbe.triangles=g_frameTriangles;
    g_renderPassProbe.waitUs+=wait;g_renderPassProbe.lastUs=sceKernelGetProcessTimeWide();
#endif
}

void GfxVita_PropWorkloadGroup(bool begin) {
#if KISAK_VITA_CPU_RUNTIME
    if(kisak::vita::VitaCpuRender_OnSource()) {
        kisak::vita::VitaCpuRender_Copy(begin,+[](const bool& value){GfxVita_PropWorkloadGroup(value);});return;
    }
    if(!g_renderPassProbe.workload)return;
    const auto set=[](bool enabled,const int* box) {
        glScissor(box[0],box[1],box[2],box[3]);
        if(enabled)glEnable(GL_SCISSOR_TEST);else glDisable(GL_SCISSOR_TEST);
    };
    if(begin) {
        if(g_propGroupOpen || g_propGroupSeen) {kisak::vita::VitaCpuProfile_Counter("render-probe-props-group-invalid",3);return;}
        g_propGroupOpen=true;g_propGroupSeen=1;g_propGroupStart=sceKernelGetProcessTimeWide();
        if(g_renderPassProbe.ClipProps())g_propScissor.Begin([](bool& enabled,int* box) {
            enabled=glIsEnabled(GL_SCISSOR_TEST)!=0;glGetIntegerv(GL_SCISSOR_BOX,box);
        },set);
    } else {
        if(!g_propGroupOpen){kisak::vita::VitaCpuProfile_Counter("render-probe-props-group-invalid",4);return;}
        g_propScissor.Restore(set);g_propGroupOpen=false;g_propGroupSeen|=2;
        const auto error=glGetError();
        if(error!=GL_NO_ERROR)kisak::vita::VitaCpuProfile_Counter("render-probe-gl-error",error,7);
        kisak::vita::VitaCpuProfile_Counter("render-probe-props-group-us",sceKernelGetProcessTimeWide()-g_propGroupStart);
    }
#endif
}
