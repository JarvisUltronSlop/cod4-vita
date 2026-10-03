#include <vita/vita_source_platform.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstring>

#include <EffectsCore/fx_system.h>
#include <client_mp/client_mp.h>
#include <database/database.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_debug.h>
#include <gfx_d3d/r_font.h>
#include <gfx_d3d/r_dvars.h>
#include <gfx_d3d/r_marks.h>
#include <gfx_d3d/r_scene.h>
#include <physics/phys_local.h>
#include <qcommon/qcommon.h>
#include <server_mp/server_mp.h>
#include <sound/snd_public.h>
#include <ui_mp/ui_mp.h>
#include <universal/com_files.h>
#include <universal/com_sndalias.h>
#include <xanim/xanim.h>

namespace kisak::vita {
namespace {

VitaSourcePlatformHooks g_hooks{};
bool g_bound = false;
uint64_t g_missingAssetKeys[512]{};
uint32_t g_missingAssetCount = 0;

struct FogState {
    bool enabled = false;
    float start = 0.0f;
    float density = 0.0f;
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    int startTime = 0;
    int transitionTime = 0;
};

FogState g_fog{};
uint32_t g_soundSequence = 1;
uint32_t g_aliasRandom = 0x51A7C0D4u;

} // namespace

void VitaSourcePlatform_Bind(const VitaSourcePlatformHooks& hooks) {
    g_hooks = hooks;
    g_bound = hooks.findAsset != nullptr;
    std::fill_n(g_missingAssetKeys, 512, uint64_t(0));
    g_missingAssetCount = 0;
    VitaSourcePlatform_ResetSessionState();
}

void VitaSourcePlatform_Clear() {
    g_hooks = {};
    g_bound = false;
    VitaSourcePlatform_ResetSessionState();
}

bool VitaSourcePlatform_IsBound() { return g_bound; }

void* VitaSourcePlatform_FindAsset(uint32_t assetType, const char* name) {
    // COD4's canonical FxImpactTable deliberately has the empty asset name.
    // Reject only a null pointer here; the typed registry decides which asset
    // classes may publish an empty identity.
    if (!g_hooks.findAsset || !name)
        return nullptr;
    void* asset = g_hooks.findAsset(assetType, name, g_hooks.context);
    if (!asset) {
        uint64_t key = 14695981039346656037ull ^ assetType;
        for (const auto* at = reinterpret_cast<const unsigned char*>(name); *at; ++at)
            key = (key ^ *at) * 1099511628211ull;
        if (!key) key = 1;
        const uint32_t first = static_cast<uint32_t>(key) % 512;
        for (uint32_t probe = 0; probe < 512; ++probe) {
            uint64_t& slot = g_missingAssetKeys[(first + probe) % 512];
            if (slot == key) break;
            if (!slot) {
                slot = key;
                ++g_missingAssetCount;
                Com_PrintWarning(16, "Vita source asset missing type=0x%x name='%s'\n", assetType, name);
                break;
            }
        }
    }
    return asset;
}

int VitaSourcePlatform_Sound(VitaSourceSoundAction action,
                             const char* aliasName, const float* origin,
                             int localClientNum) {
    if (!g_hooks.sound)
        return 0;
    return g_hooks.sound(action, aliasName ? aliasName : "", origin,
                         localClientNum, g_hooks.context);
}

void VitaSourcePlatform_LoadingPulse(const char* stage) {
    if (g_hooks.loadingPulse)
        g_hooks.loadingPulse(stage ? stage : "", g_hooks.context);
}

void VitaSourcePlatform_SetFog(bool enabled, float start, float density,
                               uint8_t red, uint8_t green, uint8_t blue,
                               int startTime, int transitionTime) {
    g_fog.enabled = enabled;
    g_fog.start = start;
    g_fog.density = density;
    g_fog.red = red;
    g_fog.green = green;
    g_fog.blue = blue;
    g_fog.startTime = startTime;
    g_fog.transitionTime = transitionTime;
    if (g_hooks.fogChanged) {
        g_hooks.fogChanged(enabled, start, density, red, green, blue,
                           startTime, transitionTime, g_hooks.context);
    }
}

void VitaSourcePlatform_SwitchFog(bool enabled, int startTime,
                                  int transitionTime) {
    VitaSourcePlatform_SetFog(enabled, g_fog.start, g_fog.density,
                              g_fog.red, g_fog.green, g_fog.blue,
                              startTime, transitionTime);
}

uint32_t VitaSourcePlatform_WorldChecksum() { return g_hooks.worldChecksum; }

const char* VitaSourcePlatform_FastfileNames() {
    return g_hooks.referencedFastfileNames
        ? g_hooks.referencedFastfileNames : "";
}

const char* VitaSourcePlatform_FastfileChecksums() {
    return g_hooks.referencedFastfileChecksums
        ? g_hooks.referencedFastfileChecksums : "";
}

void VitaSourcePlatform_ResetSessionState() {
    g_fog = {};
    g_soundSequence = 1;
    g_aliasRandom = 0x51A7C0D4u;
}

} // namespace kisak::vita

// -------------------------------------------------------------------------
// Original KisakCOD platform entry points
// -------------------------------------------------------------------------

int com_expectedHunkUsage = 0;
int com_frameTime = 0;
int fs_checksumFeed = 0;
int sv_serverId_value = 0;

uint32_t __cdecl Sys_GetCpuCount() {
    // The source engine is admitted to one execution thread. Vita I/O and
    // asset-preparation workers have separate queues and never execute source
    // gameplay globals; this is not the physical-device telemetry query.
    return 1;
}

namespace {
dvar_t g_vitaNextMap{};
dvar_t g_vitaFsDebug{};
}

const dvar_t* nextmap = &g_vitaNextMap;
const dvar_s* fs_debug = &g_vitaFsDebug;

void kisak::vita::VitaSourcePlatform_RegisterDvars() {
    VitaSourcePlatform_RegisterCapabilities();
    Com_InitDvars();
    fs_basepath = Dvar_RegisterString("fs_basepath", "ux0:data/CODVIT001",
        DVAR_INIT | DVAR_AUTOEXEC, "Base game path");
    fs_gameDirVar = Dvar_RegisterString("fs_game", "",
        DVAR_SERVERINFO | DVAR_SYSTEMINFO | DVAR_INIT, "Game data directory");
    r_modelVertColor = Dvar_RegisterBool("r_modelVertColor", true, 0xA0u,
        "Set to 0 to replace all model vertex colors with white when loaded");
    nextmap = Dvar_RegisterString("nextmap", "", DVAR_NOFLAG, "Next map command");
    fs_debug = Dvar_RegisterInt("fs_debug", 0, 0, 2, DVAR_NOFLAG, "File system diagnostics");
}

double DB_GetLoadedFraction() {
    const auto& hooks = kisak::vita::g_hooks;
    return hooks.loadingFraction ? std::clamp(hooks.loadingFraction(hooks.context), 0.0f, 1.0f) : 0.0f;
}

char* __cdecl FS_LoadedIwdNames() {
    static char empty[] = "";
    return empty;
}

char* __cdecl FS_LoadedIwdChecksums() {
    static char empty[] = "";
    return empty;
}

char* __cdecl FS_ReferencedIwdNames() {
    static char empty[] = "";
    return empty;
}

char* __cdecl FS_ReferencedIwdChecksums() {
    static char empty[] = "";
    return empty;
}

void __cdecl FS_Restart(int, int checksumFeed) {
    // KVAP files are prevalidated and stay mounted; only the source pure
    // checksum epoch changes for a new local session.
    fs_checksumFeed = checksumFeed;
}

char* __cdecl DB_ReferencedFFNameList() {
    return const_cast<char*>(kisak::vita::VitaSourcePlatform_FastfileNames());
}

char* __cdecl DB_ReferencedFFChecksums() {
    return const_cast<char*>(
        kisak::vita::VitaSourcePlatform_FastfileChecksums());
}

void __cdecl DB_UpdateDebugZone() {}
void __cdecl ProfLoad_Activate() {}
void __cdecl ProfLoad_Deactivate() {}
void __cdecl Sys_BeginLoadThreadPriorities() {}
void __cdecl Sys_EndLoadThreadPriorities() {}

uint32_t __cdecl R_GetLocalClientNum() { return 0; }

void __cdecl R_LoadWorld(char* name, int* checksum, int) {
    GfxWorld* world = static_cast<GfxWorld*>(
        kisak::vita::VitaSourcePlatform_FindAsset(ASSET_TYPE_GFXWORLD, name));
    if (checksum)
        *checksum = world ? static_cast<int>(world->checksum)
                          : static_cast<int>(
                                kisak::vita::VitaSourcePlatform_WorldChecksum());
}

GfxBrushModel* __cdecl R_GetBrushModel(uint32_t modelIndex) {
    GfxWorld* world = static_cast<GfxWorld*>(
        kisak::vita::VitaSourcePlatform_FindAsset(ASSET_TYPE_GFXWORLD,
                                                   "__active__"));
    return world && world->models && modelIndex < static_cast<uint32_t>(world->modelCount)
        ? &world->models[modelIndex] : nullptr;
}

void __cdecl R_SetFogFromServer(float start, uint8_t red, uint8_t green,
                                uint8_t blue, float density) {
    kisak::vita::VitaSourcePlatform_SetFog(
        true, start, density, red, green, blue, 0, 0);
}

void __cdecl R_SwitchFog(uint32_t fogvar, int startTime,
                         int transitionTime) {
    // FOG_NONE is zero and FOG_SERVER is one in the source renderer.
    kisak::vita::VitaSourcePlatform_SwitchFog(
        fogvar != 0, startTime, transitionTime);
}

void __cdecl R_GetAverageLightingAtPoint(const float*, uint8_t* outColor) {
    if (!outColor)
        return;
    // Neutral fallback is used only when an EffectsCore element asks for a
    // light-grid sample absent from the typed registry.  The native packet
    // renderer applies the authored map light grid to normal geometry.
    outColor[0] = 128;
    outColor[1] = 128;
    outColor[2] = 128;
    outColor[3] = 255;
}

void R_WarnOncePerFrame(GfxWarningType warningType, ...) {
    // Rate limiting lives in the low-overhead Vita perf ring.  The source
    // warning type is retained so captures identify the exact bounded pool.
    (void)warningType;
}

void __cdecl R_BeginRemoteScreenUpdate() {}
void __cdecl R_EndRemoteScreenUpdate() {}
void __cdecl R_SyncRenderThread() {}
void __cdecl R_Shutdown(int) {}

void __cdecl R_MarkFragments_Begin(
    MarkInfo* markInfo, MarkFragmentsAgainstEnum markAgainst,
    const float* origin, const float (*)[3], float radius,
    const float* viewOffset, Material* material) {
    if (!markInfo)
        return;
    std::memset(markInfo, 0, sizeof(*markInfo));
    markInfo->markAgainst = markAgainst;
    markInfo->radius = radius;
    markInfo->material = material;
    if (origin)
        std::memcpy(markInfo->origin, origin, sizeof(markInfo->origin));
    if (viewOffset)
        std::memcpy(markInfo->viewOffset, viewOffset,
                    sizeof(markInfo->viewOffset));
}

char __cdecl R_MarkFragments_AddDObj(MarkInfo*, DObj_s*, cpose_t*, uint16_t) {
    return 0;
}

char __cdecl R_MarkFragments_AddBModel(MarkInfo*, GfxBrushModel*, cpose_t*,
                                       uint16_t) {
    return 0;
}

void __cdecl R_MarkFragments_Go(
    MarkInfo*, void(__cdecl*)(void*, int, FxMarkTri*, int, FxMarkPoint*,
                              const float*, const float*),
    void*, int, FxMarkTri*, int, FxMarkPoint*) {
    // Decal projection is a renderer boundary.  The first source-owned
    // session intentionally admits zero projected mark triangles until the
    // GPU archive's clustered decal collector is bound; impact events and
    // their non-mark EffectsCore elements are still produced by source code.
}

void __cdecl SCR_StopCinematic(int) {}

