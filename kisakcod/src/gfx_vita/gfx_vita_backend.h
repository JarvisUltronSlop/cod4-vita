#pragma once

#include <gfx/gfx_backend.h>
#include <vita/vita_render_pass_probe.h>

struct GfxVitaViewmodelVertex;
// Renderer owner only; consumes the complete packed array before returning.
bool GfxVita_UpdatePackedViewmodel(GfxBufferHandle buffer,const GfxVitaViewmodelVertex* vertices,uint32_t count);

void GfxVita_BeginRenderProbe(uint32_t phase);
void GfxVita_PropWorkloadGroup(bool begin);
void GfxVita_RenderPassBoundary(kisak::vita::VitaRenderPass pass);

const GfxBackendApi* GfxVita_GetBackend();
// Immutable world textures may use a bounded GPU working set with exact backing.
constexpr uint32_t GFX_VITA_TEXTURE_FLAG_PAGEABLE = 0x00800000u;
bool GfxVita_ConsumeTexturePagingFailure();

// Private image flag consumed only by the Vita3K scalar diagnostic.  Callers
// may OR it into the source image flags for broad authored gradients (menu or
// loading backgrounds); the accelerated vitaGL path ignores the high bit and
// still receives the original compressed image chain.
constexpr uint32_t GFX_VITA_TEXTURE_FLAG_SCALAR_HIGH_PRECISION = 0x80000000u;
// Scalar-only source decode hint.  Source-authored colour maps can retain
// their compressed base-level payload and be decoded at sample time,
// preserving the authored 8-bit texels without allocating a second RGBA8888
// thumbnail.  The accelerated Vita path ignores this bit and uploads the
// normal mip chain.
constexpr uint32_t GFX_VITA_TEXTURE_FLAG_SCALAR_SOURCE_DECODE = 0x02000000u;
// Scalar-only world residency hint.  The source payload is still the exact
// KisakCOD image; Vita3K decodes it to a bounded 512px diagnostic copy so the
// complete BSP can coexist with the authored model/HUD closure and projection
// scratch.  The accelerated Vita path ignores this bit and uploads the
// original compressed chain unchanged.
constexpr uint32_t GFX_VITA_TEXTURE_FLAG_SCALAR_WORLD_RESAMPLE = 0x40000000u;
// Large-map scalar residency hint. Crossfire-class BSPs keep their complete
// authored material table, but decode ordinary world/model images to a bounded
// diagnostic copy so the full vertex stream, props, HUD, and frontend can
// coexist in Vita3K's guest heap. The accelerated Vita path ignores this bit.
constexpr uint32_t GFX_VITA_TEXTURE_FLAG_SCALAR_LARGE_MAP = 0x20000000u;
// Large-map model-only compact residency hint. The selected map's complete
// placed-model color closure can fit the same scalar budget when each model
// image uses a smaller diagnostic copy; world textures keep the larger
// large-map limit above. The accelerated Vita path ignores this bit.
constexpr uint32_t GFX_VITA_TEXTURE_FLAG_SCALAR_MODEL_COMPACT = 0x08000000u;
// Extra scalar residency hint for the largest stock BSPs.  This only reduces
// the diagnostic copy's decoded texel size; the authored package and the
// accelerated Vita upload path remain unchanged.
constexpr uint32_t GFX_VITA_TEXTURE_FLAG_SCALAR_HUGE_MAP = 0x10000000u;
// Scalar-only lightmap residency hint.  Large BSP atlases use a smaller
// source sampling grid before their bounded destination is filtered; this
// avoids a transient full 1024px RGBA lightmap while preserving authored UV
// proportions and the source lightmap equation.
constexpr uint32_t GFX_VITA_TEXTURE_FLAG_SCALAR_LIGHTMAP = 0x04000000u;
// Scalar-only first-person model residency hint. Viewhands and the selected
// weapon keep a larger, RGBA8888 diagnostic copy on the emulator path so a
// dark authored weapon texture is not further crushed by the large-map
// RGBA4444 budget. The accelerated Vita path ignores this bit and uploads the
// original compressed image chain.
constexpr uint32_t GFX_VITA_TEXTURE_FLAG_SCALAR_VIEWMODEL = 0x01000000u;

// True only when the Vita3K/display diagnostic path is active.  Physical Vita
// firmware continues through the normal vitaGL/GXM backend.
bool GfxVita_IsSoftwareFallback();

// Low-overhead counters captured at the backend boundary.  They describe the
// frame that most recently completed EndFrame and never expose native GL/GXM
// objects to the application layer.
struct GfxVitaFrameCounters {
    uint32_t frameNumber = 0;
    uint32_t drawCalls = 0;
    uint32_t triangles = 0;
    uint32_t stateChanges = 0;
    uint32_t materialPasses = 0;
    uint32_t effectBatches = 0;
    uint32_t transientUploadDrops = 0;
    uint32_t transientUploadBytes = 0;
    uint32_t transientUploadHighWaterBytes = 0;
    uint32_t swapWaitUsec = 0;
    uint32_t swapDetail[8]{};
    uint32_t config[3]{};
    bool sunOnlyShader=false, bloomActive=false;
    uint32_t postSubmitUsec=0, sceneWidth=0, sceneHeight=0, shadowPasses=0, lightCount=0;
    uint64_t targetLogicalBytes=0;
    uint32_t viewmodelWidth=0, viewmodelHeight=0, compositionUsec=0;
    uint32_t viewportSeen=0, viewportMismatches=0;
    int observedViewports[3][4]{}; // world, static props, viewmodel; zero if not observed
    bool adaptiveLighting=false;
    uint32_t litFullDraws=0, litSunDraws=0;
    uint32_t shaderMode=0, sceneShaderDraws=0, simpleMaterialDraws=0, postCopyDraws=0, authoredPostDraws=0;
    uint32_t noLocalLightDraws=0, noSpecularDraws=0, specularEligibleDraws=0, nonSunLightInputs=0;
    uint32_t noNormalMapDraws=0, normalEligibleDraws=0, adaptiveFullLightDraws=0;
    uint32_t noFogDraws=0, fogEligibleDraws=0;
    bool fogEnabled=false; float fogStart=0, fogDensity=0;
    uint64_t textureResidentBytes = 0;
    uint64_t staticBufferResidentBytes = 0;
};

bool GfxVita_GetFrameCounters(GfxVitaFrameCounters* output);
// Test boundary only; display resolution and camera FOV remain unchanged.
bool GfxVita_SetBenchmarkTarget(uint32_t width, uint32_t height, bool enabled, bool clearOnly, uint32_t shaderMode=0);
void GfxVita_BeginNativeHud();
// Map-load transaction; no target creation occurs in a gameplay frame.
bool GfxVita_PrepareGameplayTargets(uint32_t width, uint32_t height, bool adaptiveLighting);
// Loading boundary: release compiler scratch/module, retaining linked programs.
// Source callers synchronously dispatch to the renderer owner.
void GfxVita_ReleaseShaderCompiler();
// Applies on the renderer owner at map setup; explicit tests retain their own policy.
void GfxVita_SetSimpleWorldLighting(bool enabled);
void GfxVita_ReleaseGameplayTargets();
void GfxVita_SetGameplayFrame(bool active);
void GfxVita_BeginNativeViewmodel();
// Renderer metadata only; no material/GL rebinding. Source callers emit a copied command.
void GfxVita_SetDrawSurfaceIndex(uint32_t surfaceIndex,bool constantBasis=false);

#if KISAK_VITA_PROFILE
namespace kisak::vita { struct VitaRenderPacket; }
// Opt-in guest framebuffer evidence. No host window/process capture; never
// linked into Release and never usable as a performance qualification run.
void GfxVita_QueueProfileCapture(const kisak::vita::VitaRenderPacket& packet);
void GfxVita_CaptureProfileStage(const char* stage);
#endif

// The fallback file log is optional diagnostics. Very large scalar maps can
// exhaust Vita3K's guest heap before its fopen shim can return a valid FILE;
// callers may disable only that sink while keeping stdout telemetry/capture.
void GfxVita_SetSoftwareFileLogEnabled(bool enabled);

// Start a fresh bounded scalar-raster budget for a projected composition pass.
// The world may consume the full fidelity budget while the fallback is being
// tuned, but props and overlays must remain visible and interactive in captures.
// Opaque prop passes retain depth testing; the default overlay pass disables it.
void GfxVita_BeginBoundedPass(GfxSurfaceMode mode);

// Convenience wrapper for screen-space viewmodel/HUD composition.
void GfxVita_BeginOverlayPass();

// Sample one texel from a pointer-free VITI payload using the same base-level
// format accepted by CreateTexture2D.  The Vita renderer uses this only for a
// bounded CPU material-lighting bridge while the native shader family is being
// adapted; callers receive normalized RGBA channels and never see a native
// texture pointer.
bool GfxVita_SampleImageNearest(const uint8_t* payload, size_t payloadBytes,
                                uint32_t width, uint32_t height,
                                uint32_t levelCount, uint32_t format,
                                float u, float v, float output[4]);

// Draw an authored six-face sky image using the same camera basis as the
// source BSP view. Returns false for an ordinary 2D texture so callers can
// retain their legacy quad fallback. Both the scalar diagnostic and vitaGL
// path consume the same VITI cube payload and camera angles.
bool GfxVita_DrawSkyCubemap(GfxTextureHandle texture, float fovDegrees,
                            float pitchDegrees, float yawDegrees,
                            const float (*cameraAxis)[3] = nullptr);

// Draws the deterministic screen-space precipitation batch selected by the
// map's rawfile FX closure. It intentionally uses the same DrawIndexed path
// as other effects so scalar and native captures have identical pass order.
void GfxVita_DrawWeather(GfxTextureHandle texture, float rate, float slant,
                         float coverage, uint32_t timeMsec,
                         uint32_t sourceSeed = 0u,
                         uint32_t authoredPlacementCount = 0u,
                         const GfxEffectBatchState* authoredState = nullptr);

// Loading-only after required assets and GPU drain: register existing immutable content.
// Copies the bounded lease list to the renderer owner; no live source pointers retained.
bool GfxVita_PrepareLightmapLayouts(const uint32_t* textureLeases,uint32_t count);
uint64_t GfxVita_LightmapTransitionTime(bool* failed);

// Renderer-owner draw gate used by the bounded pass-isolation diagnostic.
bool GfxVita_ProfileDrawSubmission();
