#pragma once

#include <cstddef>
#include <cstdint>

// Backend-neutral resource identifiers.  Vita and x86 both use four-byte
// values here; native D3D/vitaGL objects never cross this boundary.
struct GfxTextureHandle { uint32_t value; };
struct GfxBufferHandle { uint32_t value; };
struct GfxShaderHandle { uint32_t value; };
// Source-space bounds copied into a snapshot, consumed by the next static draw.
// Missing/invalid bounds always retain the full lighting workload.
struct GfxDrawLightBounds { float minimum[3]{}, maximum[3]{}; bool valid=false; };
static_assert(sizeof(GfxTextureHandle) == 4);
static_assert(sizeof(GfxBufferHandle) == 4);
static_assert(sizeof(GfxShaderHandle) == 4);

enum class GfxSurfaceMode : uint8_t {
    Opaque = 0,
    AlphaTest,
    AlphaBlend,
    Additive,
    Sky,
    // Screen-space/viewmodel overlays are composited after the world and
    // must not be rejected by world depth, while preserving the depth buffer
    // for any subsequent pass.
    Overlay,
    // KisakCOD's lit blend family can enable alpha test and RGB blending at
    // the same time (for example wc_l_sm_b0c0*). Keep that authored state
    // distinct so cutout pixels are discarded before the remaining texels
    // participate in the source blend equation.
    AlphaTestBlend = 6,
};

// The source GfxStateBits alpha-test field is not a single fixed 0.5
// threshold. Preserve the three comparisons used by COD4's material state
// maps so the scalar oracle and VitaGL path can consume the same authored
// cutout rule.
enum class GfxAlphaTestFunc : uint8_t {
    Greater0 = 0,       // GT0
    Less128,             // LT128
    GreaterEqual128,     // GE128
};

// The desktop material state stores culling separately from the blend/depth
// family.  Keep the three source choices explicit at the backend boundary so
// VitaGL and the scalar diagnostic path can consume the authored winding
// policy without inferring it from a material name.
enum class GfxCullMode : uint8_t {
    None = 0,
    Back,
    Front,
};

enum class GfxWeatherMode : uint8_t {
    None = 0,
    Rain,
};

enum GfxFrameLightingFlags : uint32_t {
    GFX_FRAME_LIGHTING_VALID = 1u << 0,
    GFX_FRAME_LIGHTING_INDOOR = 1u << 1,
    GFX_FRAME_LIGHTING_HAS_SHADOWS = 1u << 2,
    GFX_FRAME_LIGHTING_HAS_FILM = 1u << 3,
    GFX_FRAME_LIGHTING_HAS_GLOW = 1u << 4,
};

constexpr uint32_t GFX_MAX_PRIMARY_LIGHTS = 16;
// Authored FX lights are short-lived scene additions rather than entries in
// the map's ComPrimaryLight array. Keep their descriptor pointer-free and
// bounded so the scalar oracle and VitaGL/GXM path consume the same frame
// state without growing the source primary-light ABI.
constexpr uint32_t GFX_MAX_DYNAMIC_LIGHTS = 4;

// Pointer-free copy of the authored ComPrimaryLight fields consumed by the
// Vita scalar/native lighting equations. The source defName is intentionally
// excluded: it is a lookup/debug string, not a lighting input.
struct GfxPrimaryLightDescriptor {
    uint8_t type = 0;
    uint8_t canUseShadowMap = 0;
    uint8_t exponent = 0;
    uint8_t unused = 0;
    float color[3] = {0.0f, 0.0f, 0.0f};
    float direction[3] = {0.0f, 0.0f, 0.0f};
    float origin[3] = {0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    float cosHalfFovOuter = 0.0f;
    float cosHalfFovInner = 0.0f;
    float cosHalfFovExpanded = 0.0f;
    float rotationLimit = 0.0f;
    float translationLimit = 0.0f;
    // Source shadow-map/cookie visibility sampled by the Vita scene bridge.
    // Directional sun visibility remains per receiver in GfxDrawVertex;
    // this bounded scalar covers shadow-capable local spot/omni lights while
    // keeping the draw-vertex ABI pointer-free and unchanged.
    float shadowVisibility = 1.0f;
};

// Backend-neutral, source-derived camera/environment state. The Vita
// renderer consumes this record in both the scalar diagnostic and native
// paths so map atmosphere is not replaced by a global tint.
struct GfxFrameLightingState {
    uint32_t mapIndex = 0;
    uint32_t flags = 0;
    float cameraPosition[3] = {0.0f, 0.0f, 0.0f};
    // Camera basis and projected scale are source-derived view inputs for
    // world-space effects such as weather. Keeping them beside the eye point
    // lets the scalar oracle and VitaGL path project the same authored FX
    // anchors without passing a desktop camera pointer through the backend.
    float cameraForward[3] = {1.0f, 0.0f, 0.0f};
    float cameraRight[3] = {0.0f, -1.0f, 0.0f};
    float cameraUp[3] = {0.0f, 0.0f, 1.0f};
    float cameraHorizontalScale = 1.0f;
    float cameraVerticalScale = 1.0f;
    float sunDirection[3] = {0.0f, 0.0f, 0.0f};
    float sunColor[3] = {0.0f, 0.0f, 0.0f};
    float ambientColor[3] = {0.0f, 0.0f, 0.0f};
    float diffuseColor[3] = {0.0f, 0.0f, 0.0f};
    float fogColor[3] = {0.0f, 0.0f, 0.0f};
    float ambientScale = 0.0f;
    float diffuseFraction = 0.0f;
    float sunLight = 0.0f;
    // Source map-script r_specularColorScale applied to SUN_SPECULAR by
    // R_SetSunConstants. Keep this separate from diffuse sunColor.
    float specularColorScale = 1.0f;
    // Interpolated gndLt sample for the current camera.  The source uses it
    // for model/indoor response; carrying it beside the frame state keeps
    // covered spaces darker without a platform-specific map tint.
    float lightGridColor[3] = {1.0f, 1.0f, 1.0f};
    float lightGridSunWeight = 1.0f;
    float indoorFactor = 1.0f;
    float fogStart = 0.0f;
    float fogDensity = 0.0f;
    float exposure = 1.0f;
    // KisakCOD's default r_gamma is 0.8. The desktop backend applies the
    // reciprocal exponent through the final hardware gamma ramp, after the
    // film/post chain; retain that source-space value here for Vita paths.
    float gamma = 0.8f;
    float filmLightTint[3] = {1.0f, 1.0f, 1.0f};
    float filmDarkTint[3] = {1.0f, 1.0f, 1.0f};
    float filmContrast = 1.0f;
    float filmBrightness = 0.0f;
    float filmDesaturation = 0.0f;
    float filmInvert = 0.0f;
    float glowEnabled = 0.0f;
    float glowRadius0 = 0.0f;
    float glowRadius1 = 0.0f;
    float glowBloomCutoff = 0.0f;
    float glowBloomDesaturation = 0.0f;
    float glowBloomIntensity0 = 0.0f;
    float glowBloomIntensity1 = 0.0f;
    float glowSkyBleedIntensity0 = 0.0f;
    float glowSkyBleedIntensity1 = 0.0f;
    float rainRate = 0.0f;
    float rainSlant = 0.0f;
    float rainCoverage = 0.0f;
    float wetness = 0.0f;
    GfxWeatherMode weather = GfxWeatherMode::None;
    uint32_t primaryLightCount = 0;
    uint32_t sunPrimaryLightIndex = 0;
    GfxPrimaryLightDescriptor primaryLights[GFX_MAX_PRIMARY_LIGHTS]{};
    uint32_t dynamicLightCount = 0;
    GfxPrimaryLightDescriptor dynamicLights[GFX_MAX_DYNAMIC_LIGHTS]{};
};

// Complete source material pass state. Texture handles are backend-owned
// numeric IDs; the descriptor contains no VitaGL/D3D object pointers.
struct GfxMaterialPassState {
    GfxTextureHandle colorTexture{0};
    GfxTextureHandle normalTexture{0};
    GfxTextureHandle specularTexture{0};
    // Source code reflectionProbeSampler input. This is a cubemap handle;
    // keeping it in the pointer-free pass descriptor lets the scalar oracle
    // and VitaGL/GXM shader consume the same authored environment sample.
    GfxTextureHandle reflectionProbeTexture{0};
    GfxTextureHandle lightmapPrimaryTexture{0};
    GfxTextureHandle lightmapSecondaryTexture{0};
    uint8_t colorSamplerState = 0;
    uint8_t normalSamplerState = 0;
    uint8_t specularSamplerState = 0;
    uint8_t lightmapSamplerFlags = 0;
    uint32_t techniqueFlags0 = 0;
    uint32_t techniqueFlags1 = 0;
    GfxSurfaceMode surfaceMode = GfxSurfaceMode::Opaque;
    GfxAlphaTestFunc alphaTestFunc = GfxAlphaTestFunc::GreaterEqual128;
    GfxCullMode cullMode = GfxCullMode::None;
    // Optional source identity for scalar diagnostics.  This is deliberately
    // numeric/pointer-free so the same descriptor remains valid on VitaGL;
    // world submissions fill it with the VITW surface index and non-world
    // callers leave the sentinel value.
    uint32_t sourceSurfaceIndex = 0xFFFFFFFFu;
    // VITL v4's resolved lit-pass GfxStateBits. The compact mode/cull fields
    // above cannot represent source blend factors/equations or the exact
    // depth test/write policy, so keep the authored pair pointer-free for
    // both the scalar oracle and the VitaGL/GXM path.
    uint32_t sourceStateBits0 = 0;
    uint32_t sourceStateBits1 = 0;
    uint8_t sourceStateValid = 0;
};

// Internal material-pass selectors that supplement the authored VITL state.
// Static XModels arrive at the backend with the complete source model-light
// and ground/lightgrid response already baked into vertex colour. The native
// lit shader uses this bit to apply semantic normal/specular response without
// multiplying that authored model-light response a second time.
enum GfxMaterialPassTechniqueFlags0 : uint32_t {
    GFX_MATERIAL_PASS_BAKED_LIGHTING = 1u << 31,
    // Source technique-family selectors. These are separate from the
    // authored VITL state pair: unlit world techniques still carry blend
    // state, but their shader output must not receive the lit
    // lightmap/lightgrid/normal response first.
    GFX_MATERIAL_PASS_SOURCE_UNLIT = 1u << 30,
    GFX_MATERIAL_PASS_SOURCE_UNLIT_MULTIPLY = 1u << 29,
    GFX_MATERIAL_PASS_SOURCE_UNLIT_ADDITIVE = 1u << 28,
    GFX_MATERIAL_PASS_SOURCE_UNLIT_DISTFALLOFF = 1u << 27,
    // Source 2D commands retain authored alpha/blend/sampler state. Their
    // screen-space compositor always leaves world depth untouched.
    GFX_MATERIAL_PASS_SOURCE_UI = 1u << 26,
};

struct GfxViewEffectState {
    uint32_t flags = 0;
    float filmLightTint[3] = {1.0f, 1.0f, 1.0f};
    float filmDarkTint[3] = {1.0f, 1.0f, 1.0f};
    float contrast = 1.0f;
    float brightness = 0.0f;
    float desaturation = 0.0f;
    float invert = 0.0f;
    float exposure = 1.0f;
    // This is the source r_gamma value, not a texture color-space exponent.
    // Backends apply 1/gamma only at final display composition.
    float gamma = 0.8f;
    float glowEnabled = 0.0f;
    float glowRadius0 = 0.0f;
    float glowRadius1 = 0.0f;
    float glowBloomCutoff = 0.0f;
    float glowBloomDesaturation = 0.0f;
    float glowBloomIntensity0 = 0.0f;
    float glowBloomIntensity1 = 0.0f;
    float glowSkyBleedIntensity0 = 0.0f;
    float glowSkyBleedIntensity1 = 0.0f;
};

// Pointer-free shadow state shared by the scalar oracle and the accelerated
// Vita path.  The current retail closure exposes light-grid visibility rather
// than a native shadow-map pointer, so keep the source sun/grid result here;
// later shadow-map/cookie records can extend this descriptor without changing
// the scene/backend ownership boundary.
enum GfxShadowPassFlags : uint32_t {
    GFX_SHADOW_PASS_ENABLED = 1u << 0,
    GFX_SHADOW_PASS_HAS_SUN = 1u << 1,
    GFX_SHADOW_PASS_HAS_LIGHTGRID = 1u << 2,
};

struct GfxShadowPassState {
    uint32_t mapIndex = 0;
    uint32_t flags = 0;
    float sunDirection[3] = {0.0f, 0.0f, 0.0f};
    float lightGridColor[3] = {1.0f, 1.0f, 1.0f};
    float lightGridSunWeight = 1.0f;
    float indoorFactor = 1.0f;
    float shadowStrength = 1.0f;
    float bias = 0.0f;
};

// Pointer-free effect submission state.  FX such as rain, decals, muzzle
// flashes, and overlays can select their authored blend/sampler family
// without smuggling native texture/material pointers across the backend ABI.
enum GfxEffectBatchFlags : uint32_t {
    GFX_EFFECT_BATCH_WEATHER = 1u << 0,
    GFX_EFFECT_BATCH_PARTICLE = 1u << 1,
    GFX_EFFECT_BATCH_DECAL = 1u << 2,
    GFX_EFFECT_BATCH_OVERLAY = 1u << 3,
    // Source-authored map FX, as opposed to client-side weapon/UI effects.
    GFX_EFFECT_BATCH_SOURCE_AUTHORED = 1u << 4,
    GFX_EFFECT_BATCH_SMOKE = 1u << 5,
    GFX_EFFECT_BATCH_DUST = 1u << 6,
    GFX_EFFECT_BATCH_FIRE = 1u << 7,
    GFX_EFFECT_BATCH_PAPER = 1u << 8,
    GFX_EFFECT_BATCH_WATER = 1u << 9,
    GFX_EFFECT_BATCH_FOLIAGE = 1u << 10,
    GFX_EFFECT_BATCH_LIGHT = 1u << 11,
    // The source FX tail path uses a velocity-facing quad rather than the
    // camera-facing billboard path. Keep that distinction in the shared
    // effect descriptor so the accelerated backend can select the same
    // technique when it gains native FX geometry submission.
    GFX_EFFECT_BATCH_TAIL = 1u << 12,
};

constexpr uint32_t GFX_MAX_EFFECT_PLACEMENTS = 64;

struct GfxEffectPlacementDescriptor {
    uint32_t flags = 0;
    float origin[3] = {0.0f, 0.0f, 0.0f};
    float angles[3] = {0.0f, 0.0f, 0.0f};
    float delay = 0.0f;
};

struct GfxEffectBatchState {
    uint32_t flags = 0;
    GfxTextureHandle colorTexture{0};
    uint8_t colorSamplerState = 0;
    GfxSurfaceMode surfaceMode = GfxSurfaceMode::AlphaBlend;
    GfxAlphaTestFunc alphaTestFunc = GfxAlphaTestFunc::Greater0;
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float uvScaleBias[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    uint32_t placementCount = 0;
    GfxEffectPlacementDescriptor placements[GFX_MAX_EFFECT_PLACEMENTS]{};
};

struct GfxDrawVertex
{
    float position[3];
    float color[4];
    // Optional world/material UVs. Geometry-only callers leave them zero;
    // textured Vita passes can consume the same backend-neutral vertex.
    float texCoord[2];
    // Optional static-lightmap UVs. The Vita fixed-function backend currently
    // applies primary lightmap intensity on the CPU before submission, while
    // retaining the second channel for a future multitexture path.
    float lightmapCoord[2];
    // World-sidecar normals/tangents decoded from KisakCOD's PackedUnitVec
    // records.  The fixed-function Vita backend does not currently expose a
    // normal array, but keeping the authored basis on the backend-neutral
    // vertex lets the scalar and accelerated paths share the same source
    // sunlight evaluation before submission.
    float normal[3];
    float tangent[3];
    // Optional source-space position retained beside the projected position.
    // Dynamic point/spot-light equations need this value per fragment; UI and
    // legacy callers may leave it zero.
    float worldPosition[3];
    // Source sun visibility for this receiver.  World projection derives it
    // from the pointer-free collision sidecar and both backends use it to
    // attenuate only the authored directional sun term (ambient/local lights
    // remain available indoors).  Non-world callers inherit the clear-value
    // default of one. For world vertices the scalar is a compact pair: the
    // high byte is sun visibility and the low byte is the optional local
    // light-grid sun weight (255 means no local override).
    float shadowVisibility = 1.0f;
};

// Pointer-free immutable rigid-model instance consumed by the native Vita
// renderer.  The source model-light texture is represented by its six axis
// lobes; this keeps per-placement light-grid identity while allowing repeated
// props to share one mesh.  Rotation is a normalized source quaternion and
// originScale.w is the authored uniform model scale.
struct GfxStaticModelInstance {
    float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float originScale[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float lightPositiveX[4] = {1.0f, 1.0f, 1.0f, 0.0f};
    float lightNegativeX[4] = {1.0f, 1.0f, 1.0f, 0.0f};
    float lightPositiveY[4] = {1.0f, 1.0f, 1.0f, 0.0f};
    float lightNegativeY[4] = {1.0f, 1.0f, 1.0f, 0.0f};
    float lightPositiveZ[4] = {1.0f, 1.0f, 1.0f, 0.0f};
    float lightNegativeZ[4] = {1.0f, 1.0f, 1.0f, 0.0f};
};
static_assert(sizeof(GfxStaticModelInstance) == 128);

struct GfxBackendApi
{
    // transientBytes is the complete frame-rotating upload arena and
    // gpuPoolBytes is the complete backend-owned GPU-mapped allocation pool.
    // Backends must not silently spill persistent resources into the general
    // game heap after either budget has been exhausted.
    bool (*Init)(uint32_t width, uint32_t height, size_t transientBytes,
                 size_t gpuPoolBytes);
    void (*Shutdown)();
    void (*BeginFrame)();
    void (*Clear)(float r, float g, float b, float a);
    // Texture handles are backend-owned IDs. The payload is already decoded
    // into a pointer-free VITI record; backends may reject formats they do
    // not support without exposing native objects to the scene layer.
    GfxTextureHandle (*CreateTexture2D)(const uint8_t *payload, size_t payloadBytes,
                                        uint32_t width, uint32_t height,
                                        uint32_t levelCount, uint32_t format,
                                        uint32_t flags);
    void (*DestroyTexture)(GfxTextureHandle texture);
    void (*BindTexture)(GfxTextureHandle texture);
    // Apply the source MaterialTextureDef sampler state to a texture unit.
    // KisakCOD encodes filter/mipmap/addressing in the low byte; keeping this
    // at the backend boundary lets scalar and VitaGL paths consume the same
    // authored state without exposing native GL/D3D objects to the scene.
    void (*SetSamplerState)(uint32_t textureUnit, uint8_t samplerState);
    // Apply the source GfxStateBits cull mode.  D3D9's front-face convention
    // is retained by the Vita backend; callers use Back/Front in the same
    // semantic direction as the authored desktop state.
    void (*SetCullMode)(GfxCullMode mode);
    void (*DrawIndexed)(const GfxDrawVertex *vertices, uint32_t vertexCount,
                        const uint16_t *indices, uint32_t indexCount);
    // Optional source-lit submission. The first coordinate channel samples
    // the material image and the second samples the authored lightmap UVs.
    // KisakCOD's lit techniques can bind both the primary L8 lightmap and the
    // secondary A8R8G8B8 lightmap; the source custom-sampler bits are passed
    // verbatim so a native backend can select the same shader inputs instead
    // of inferring them from a material name. Backends without multitexture
    // support may leave this null.
    void (*DrawIndexedLightmapped)(const GfxDrawVertex *vertices, uint32_t vertexCount,
                                   const uint16_t *indices, uint32_t indexCount,
                                   GfxTextureHandle texture,
                                   GfxTextureHandle lightmapPrimaryTexture,
                                   GfxTextureHandle lightmapSecondaryTexture,
                                   GfxTextureHandle reflectionProbeTexture,
                                   uint8_t lightmapSamplerFlags);
    void (*SetSurfaceMode)(GfxSurfaceMode mode);
    void (*SetAlphaTestFunc)(GfxAlphaTestFunc function);
    // Apply the authored renderer fog state.  The values originate at the
    // retail GSC setExpFog call; both the VitaGL and scalar backends consume
    // the same bounded parameters.
    void (*SetFog)(bool enabled, float startDistance, float density,
                   const float color[3]);
    // Install source-derived frame lighting/material/post state. These are
    // optional for legacy callers, but the Vita world path uses them for both
    // the native and scalar renderers.
    void (*SetFrameLighting)(const GfxFrameLightingState* state);
    void (*SetMaterialPass)(const GfxMaterialPassState* state);
    void (*SetShadowPass)(const GfxShadowPassState* state);
    void (*SetEffectBatch)(const GfxEffectBatchState* state);
    void (*ApplyViewEffects)(const GfxViewEffectState* state);
    // Immutable map geometry lives in backend-owned buffers for the complete
    // lifetime of a map.  The native path transforms a_worldposition in the
    // vertex shader and treats firstVertex/firstIndex as byte-buffer offsets;
    // the scalar oracle deliberately leaves these entry points null/failed.
    GfxBufferHandle (*CreateStaticVertexBuffer)(const GfxDrawVertex* vertices,
                                                uint32_t vertexCount);
    // Offline/native cluster builders may reorder or duplicate source
    // vertices without materializing a second 84-byte GfxDrawVertex stream.
    // sourceIndices maps each packed output vertex to the immutable source
    // array. The native backend validates every entry before allocating.
    GfxBufferHandle (*CreateStaticVertexBufferRemapped)(
        const GfxDrawVertex* sourceVertices, uint32_t sourceVertexCount,
        const uint32_t* sourceIndices, uint32_t outputVertexCount);
    GfxBufferHandle (*CreateStaticIndexBuffer)(const uint16_t* indices,
                                               uint32_t indexCount);
    GfxBufferHandle (*CreateStaticInstanceBuffer)(
        const GfxStaticModelInstance* instances, uint32_t instanceCount);
    // Skinned first-person meshes retain immutable source index buffers while
    // rotating a bounded set of CPU-written vertex buffers.  This avoids the
    // client-array path copying the complete hands/weapon stream once for
    // every material surface.  Dynamic buffers use the GfxDrawVertex ABI and
    // therefore remain clip-space submissions; unlike static world buffers,
    // they are never transformed by the world vertex-shader branch.
    GfxBufferHandle (*CreateDynamicVertexBuffer)(uint32_t vertexCapacity);
    bool (*UpdateDynamicVertexBuffer)(GfxBufferHandle vertexBuffer,
                                      const GfxDrawVertex* vertices,
                                      uint32_t vertexCount);
    void (*DestroyBuffer)(GfxBufferHandle buffer);
    void (*SetWorldViewProjection)(const float matrix[16], float nearPlane);
    bool (*DrawStaticIndexed)(GfxBufferHandle vertexBuffer,
                              uint32_t firstVertex, uint32_t vertexCount,
                              GfxBufferHandle indexBuffer,
                              uint32_t firstIndex, uint32_t indexCount);
    bool (*DrawStaticIndexedInstanced)(
        GfxBufferHandle vertexBuffer,
        uint32_t firstVertex, uint32_t vertexCount,
        GfxBufferHandle indexBuffer,
        uint32_t firstIndex, uint32_t indexCount,
        GfxBufferHandle instanceBuffer,
        uint32_t firstInstance, uint32_t instanceCount);
    bool (*DrawDynamicIndexed)(GfxBufferHandle vertexBuffer,
                               uint32_t firstVertex, uint32_t vertexCount,
                               GfxBufferHandle indexBuffer,
                               uint32_t firstIndex, uint32_t indexCount,
                               bool sourceLit);
    void (*EndFrame)();
    // Compact immutable rigid meshes for DrawStaticIndexedInstanced. These
    // omit only constant lightmap/visibility attributes; source positions
    // and texture coordinates retain their complete float precision.
    GfxBufferHandle (*CreateInstancedModelVertexBuffer)(
        const GfxDrawVertex* vertices, uint32_t vertexCount);
    void (*SetDrawLightBounds)(const GfxDrawLightBounds* bounds);
};
