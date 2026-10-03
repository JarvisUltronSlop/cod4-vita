#include "vita_map_assets.h"
#include <vita/vita_source_session.h>
#include <vita/vita_gpu_diagnostic_anchor.h>
#include <game_mp/g_public_mp.h>
#include <vita/vita_cpu_profile.h>
#include <vita/vita_cpu_replay.h>
#include <vita/vita_cpu_render.h>
#include <vita/vita_cpu_file.h>
#include <vita/vita_cpu_audio.h>
#include <vita/vita_source_model_backend.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <psp2/kernel/processmgr.h>

#include <client_mp/client_mp.h>
#include <client/client.h>
#include <cgame_mp/cg_local_mp.h>
#include <gfx_d3d/r_reflection_probe.h>
#include <sound/snd_local.h>
#include <ragdoll/ragdoll.h>
#include <physics/phys_local.h>
#include <game_mp/g_main_mp.h>
#include <qcommon/cmd.h>
#include <qcommon/msg_mp.h>
#include <qcommon/net_chan_mp.h>
#include <qcommon/qcommon.h>
#include <script/scr_stringlist.h>
#include <script/scr_variable.h>
#include <script/scr_vm.h>
#include <server_mp/server_mp.h>
#include <stringed/stringed_hooks.h>
#include <ui_mp/ui_mp.h>
#include <vita/vita_animation_bundle.h>
#include <vita/vita_effect_bundle.h>
#include <vita/vita_impact_fx_bundle.h>
#include <vita/vita_net_compat.h>
#include <vita/vita_image_bundle.h>
#include <vita/vita_material_bundle.h>
#include <vita/vita_model_bundle.h>
#include <vita/vita_rawfile_bundle.h>
#include <vita/vita_script_services.h>
#include <vita/vita_shipment_collision.h>
#include <vita/vita_shipment_entities.h>
#include <vita/vita_shipment_weapons.h>
#include <vita/vita_stringtable_bundle.h>
#include <vita/vita_sound_bundle.h>
#include <vita/vita_source_render.h>
#include <xanim/dobj.h>
#include <xanim/xanim.h>
#if defined(KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT)
#include <vita/vita_source_input_smoke.h>
#endif

namespace kisak::vita {
namespace {

constexpr int kLoopbackMessageBytes = 0x20000;
constexpr uint32_t kMaximumPacketsPerPump = 128;
uint8_t g_clientMessageBytes[kLoopbackMessageBytes] = {};
uint8_t g_serverMessageBytes[kLoopbackMessageBytes] = {};
#if defined(KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT)
VitaSourceInputSmoke g_inputSmoke;

void TraceSourceInputState(const char* owner, const playerState_s& ps) {
    const bool hasWeapon = ps.weapon && ps.weapon < BG_GetNumWeapons();
    const int clip = hasWeapon ? BG_ClipForWeapon(ps.weapon) : 0;
    const int ammo = hasWeapon ? BG_AmmoForWeapon(ps.weapon) : 0;
    Com_Printf(0, "Vita source input state owner=%s phase=%u time=%d pm=%d flags=%x health=%d weapon=%u weaponState=%d weaponTime=%d weaponDelay=%d anim=%d clip=%d ammo=%d ads=%.6f pos=%.6f,%.6f,%.6f vel=%.6f,%.6f,%.6f angles=%.6f,%.6f,%.6f events=%d\n",
        owner, g_inputSmoke.Phase(), ps.commandTime, ps.pm_type, ps.pm_flags, ps.stats[0],
        ps.weapon, ps.weaponstate, ps.weaponTime, ps.weaponDelay, ps.weapAnim,
        ps.ammoclip[clip], ps.ammo[ammo], ps.fWeaponPosFrac,
        ps.origin[0], ps.origin[1], ps.origin[2], ps.velocity[0], ps.velocity[1], ps.velocity[2],
        ps.viewangles[0], ps.viewangles[1], ps.viewangles[2], ps.eventSequence);
}
#endif

uint32_t PresentationWeaponSlot(uint32_t weaponIndex) {
    if (!weaponIndex || weaponIndex >= BG_GetNumWeapons())
        return 0;
    const WeaponDef* definition = BG_GetWeaponDef(weaponIndex);
    if (!definition || !definition->szInternalName)
        return 0;
    if (!std::strcmp(definition->szInternalName, "mp5_mp"))
        return 2;
    if (!std::strcmp(definition->szInternalName, "usp_mp"))
        return 3;
    if (!std::strcmp(definition->szInternalName, "frag_grenade_mp"))
        return 4;
    if (!std::strcmp(definition->szInternalName, "flash_grenade_mp"))
        return 5;
    return 0;
}

template <typename Function>
bool InvokeSourceRoot(Function function, const char* name="source-call") {
    VITA_CPU_SCOPE(name);
    void* const profileBoundary=kisak::vita::VitaCpuProfile_ScopeCheckpoint();
    VitaSourceErrorBoundary boundary{};
    VitaScriptServicesPushErrorBoundary(&boundary);
    const bool completed = ::setjmp(boundary.jump) == 0;
    if (completed)
        function();
    // Com_Error can skip inner C++ scopes with longjmp. Do not retain their
    // stack addresses after crossing the source error boundary.
    kisak::vita::VitaCpuProfile_RestoreScopes(profileBoundary);
    VitaScriptServicesPopErrorBoundary(&boundary);
    return completed;
}

uint32_t PumpEndpoint(netsrc_t endpoint, uint8_t* bytes) {
    msg_t message{};
    netadr_t from{};
    uint32_t count = 0;
    MSG_Init(&message, bytes, kLoopbackMessageBytes);
    while (count < kMaximumPacketsPerPump
           && NET_GetLoopPacket(endpoint, &from, &message)) {
        if (endpoint == NS_SERVER) {
            if (com_sv_running && com_sv_running->current.enabled)
                SV_PacketEvent(from, &message);
        } else {
            CL_PacketEvent(0, from, &message, Sys_Milliseconds());
        }
        ++count;
        message.cursize = 0;
        message.readcount = 0;
        message.bit = 0;
        message.overflowed = false;
    }
    return count;
}

} // namespace

bool VitaSourceSession::NormalizeMapName(const char* input, char output[32]) {
    if (!input || !output)
        return false;
    uint32_t mapIndex=0;
    if(!VitaFindMapIndex(input,&mapIndex))return false;
    std::snprintf(output,32,"mp_%s",kVitaMpMaps[mapIndex].name);
    output[31] = '\0';
    return true;
}

bool VitaSourceSession::Fail(const char* message) {
    status_.state = VitaSourceSessionState::Failed;
    std::strncpy(status_.error, message ? message : "source session failed",
                 sizeof(status_.error) - 1u);
    status_.error[sizeof(status_.error) - 1u] = '\0';
    VitaScriptServicesTrace("source_session_failed", status_.error);
    return false;
}

bool VitaSourceSession::SourceBoundarySucceeded(const char* stage) {
    status_.runtimeScriptErrors = VitaScriptServicesRuntimeErrorCount() - runtimeErrorBaseline_;
    if (!VitaScriptServicesHadError())
        return true;
    char failure[sizeof(status_.error)] = {};
    std::snprintf(failure, sizeof(failure), "%s: %s",
                  stage ? stage : "source",
                  VitaScriptServicesLastError());
    return Fail(failure);
}

bool VitaSourceSession::Initialize(
    VitaAssetRegistry* registry, const VitaSourcePlatformHooks& services,
    uint32_t worldChecksum, const char* fastfileNames,
    const char* fastfileChecksums) {
    if (status_.state != VitaSourceSessionState::Cold || !registry
        || registry->Stats().capacityBytes == 0)
        return Fail("source session initialize contract is invalid");

    registry_ = registry;
#if KISAK_VITA_CPU_PROFILE
    VitaCpuReplay_SourceInitialize();
#endif
    cachedWeaponCount_=UINT32_MAX;
    cachedFragIndex_=0;
    runtimeErrorBaseline_ = VitaScriptServicesRuntimeErrorCount();
    VitaScriptServicesClearError();
    VitaLoopback_Reset();
    VitaSourceLoopback_Reset();
    VitaSourcePlatform_Bind(registry_->MakePlatformHooks(
        services, worldChecksum, fastfileNames, fastfileChecksums));
    if (!VitaSourcePlatform_IsBound())
        return Fail("source asset registry did not bind");

    InvokeSourceRoot([] { SL_Init(); });
    if (!SourceBoundarySucceeded("SL_Init"))
        return false;
    sourceStringsInitialized_ = true;

    // SV_SpawnServer executes the config command buffer before bringing up
    // the game VM.  The desktop Com_Init root initializes both pieces before
    // SV_Init/CL_Init; the bounded Vita composition root must preserve that
    // dependency even though it intentionally does not invoke all of
    // Com_Init.  Without Cmd_Init, Cmd_ExecuteSingleCommand walks a null
    // command list while processing the first startup command.  Vita3K can
    // continue after that guest fault with damaged registers, making later
    // XAnim/malloc failures look like the cause instead of fallout.
    InvokeSourceRoot([] {
        Cbuf_Init();
        Cmd_Init();
    });
    if (!SourceBoundarySucceeded("command system initialization"))
        return false;

    InvokeSourceRoot([] {
        Dvar_Init();
        VitaSourcePlatform_RegisterDvars();
        SEH_InitLanguage();
    });
    if (!SourceBoundarySucceeded("Dvar_Init"))
        return false;
    sourceDvarsInitialized_ = true;

    // The normal Com_Init root owns these three source subsystems before it
    // enters either SV_Init or CL_Init.  The Vita composition root cannot
    // rely on zero-initialized globals here: Scr_VM_Init immediately consumes
    // the variable free lists built by Scr_InitVariables, while game startup
    // creates XAnim/DObj state for mantle and the first-person model.  Keeping
    // each root behind its own error boundary also reports the actual failed
    // dependency instead of allowing a null free-list head to surface later
    // as the misleading retail "maximum number of script variables" error.
    InvokeSourceRoot([] {
        Scr_InitVariables();
        Scr_Init();
        Scr_Settings(0, 0, 0);
    });
    if (!SourceBoundarySucceeded("script runtime initialization"))
        return false;
    sourceScriptInitialized_ = true;

    InvokeSourceRoot([] { XAnimInit(); });
    if (!SourceBoundarySucceeded("XAnim initialization"))
        return false;
    sourceXAnimInitialized_ = true;

    InvokeSourceRoot([] {
        DObjInit();
        // The desktop map-load root calls Com_Restart before SV_SpawnServer.
        // Its DObj handle pool is distinct from DObjInit's shared strings.
        // Initialize both here before any server entity creates a DObj.
        Com_InitDObj();
    });
    if (!SourceBoundarySucceeded("DObj initialization"))
        return false;
    sourceDObjInitialized_ = true;

    // Preserve the MP ordering from Com_Init.  This is not socket startup:
    // the Vita platform boundary remains loopback-only, while the recovered
    // source root owns qport plus every netchan/message diagnostic dvar used
    // by Netchan_Transmit and Netchan_Process.  Skipping this root leaves
    // showpackets/showdrop null and turns their first field access into a
    // guest access near address 0xC.
    InvokeSourceRoot([] { Netchan_Init(1); });
    if (!SourceBoundarySucceeded("Netchan_Init"))
        return false;

    InvokeSourceRoot([] { SV_Init(); });
    if (!SourceBoundarySucceeded("SV_Init"))
        return false;
    serverRootInitialized_ = true;

    // CL_InitOnceForAllClients is the original MP source registration root,
    // not a Vita timeout workaround.  It installs the command/input dvars,
    // client timeout/packet policy, and the complete cgame dvar set consumed
    // by the real connection, snapshot, prediction, and usercmd paths.  Its
    // Vita compile branch omits only desktop console/renderer/ragdoll
    // presentation services owned by the native composition root.
    InvokeSourceRoot([] {
        Con_InitChannels();
        Con_Init();
        CL_InitOnceForAllClients();
        UI_RegisterDvars();
    });
    if (!SourceBoundarySucceeded("CL_InitOnceForAllClients"))
        return false;
    InvokeSourceRoot([] { CL_Init(0); });
    if (!SourceBoundarySucceeded("CL_Init"))
        return false;
    clientRootInitialized_ = true;

    // The vertical slice deliberately exposes only one local FFA client.
    InvokeSourceRoot([] {
        Dvar_SetStringByName("g_gametype", const_cast<char*>("dm"));
        Dvar_SetIntByName("ui_maxclients", 1);
        Dvar_SetIntByName("sv_maxclients", 1);
        Dvar_SetIntByName("sv_fps", 20);
        // Certified KVAP manifests replace the desktop IWD pure-check
        // protocol.  Leaving sv_pure enabled would prevent SV_UserMove from
        // admitting the first local usercmd and strand the real client in
        // CS_CLIENTLOADING even though every pack hash was already verified.
        Dvar_SetBoolByName("sv_pure", false);
        Dvar_SetBoolByName("onlinegame", false);
        Dvar_SetBoolByName("onlinegameandhost", false);
        Dvar_SetBoolByName("systemlink", false);
    });
    if (!SourceBoundarySucceeded("offline session configuration"))
        return false;

    status_ = {};
    status_.state = VitaSourceSessionState::Initialized;
    return true;
}

bool VitaSourceSession::MountSourceModelAssets(
    const VitaImageBundle& images, const VitaMaterialBundle& materials,
    const VitaModelBundle& models) {
    if (status_.state != VitaSourceSessionState::Initialized || !registry_
        || !sourceStringsInitialized_ || !sourceDataMounted_
        || sourceModelAssetsMounted_)
        return Fail("source model assets requested outside clean initialized state");

    VitaSourceModelClosureReport report{};
    VitaScriptServicesClearError();
    InvokeSourceRoot([&] {
        report = registry_->RehydrateSourceModelClosure(
            images, materials, models, VitaAssetLifetime::Map,
            false /* GPU archive owns pixel residency */,
            true /* model bundle outlives the map registry */);
    });
    if (!SourceBoundarySucceeded("rehydrate source model closure"))
        return false;
    if (!report.ready) {
        const VitaSourceAssetMountReport* failed = !report.images.ready
            ? &report.images : (!report.materials.ready
                ? &report.materials : &report.models);
        const VitaMissingSourceAsset* missing = failed->missingCount
            ? &failed->missing[0] : nullptr;
        char failure[sizeof(status_.error)] = {};
        std::snprintf(failure, sizeof(failure),
                      "source asset type %u failed at %.72s (%u/%u%s%.64s)",
                      failed->assetType,
                      failed->failedAsset[0] ? failed->failedAsset : "unknown",
                      failed->mountedCount, failed->sourceCount,
                      missing ? ": " : "", missing ? missing->name : "");
        return Fail(failure);
    }
    sourceModelAssetsMounted_ = true;
    return true;
}

bool VitaSourceSession::MountSourceData(
    const VitaRawFileBundle& rawfiles,
    const VitaStringTableBundle& stringTables) {
    if (status_.state != VitaSourceSessionState::Initialized || !registry_
        || !sourceStringsInitialized_ || sourceDataMounted_
        || sourceModelAssetsMounted_ || sourceAnimationsMounted_
        || sourceEffectsMounted_ || sourceWeaponsMounted_
        || sourceMapRootsMounted_)
        return Fail("source data requested outside clean initialized state");
    if (!rawfiles.Loaded() || rawfiles.Count() == 0)
        return Fail("source VITR rawfile closure is empty");

    VitaSourceAssetMountReport tables{};
    VitaScriptServicesClearError();
    InvokeSourceRoot([&] {
        tables = registry_->RehydrateSourceStringTables(
            stringTables, VitaAssetLifetime::Map);
    });
    if (!SourceBoundarySucceeded("rehydrate source string tables"))
        return false;
    if (!tables.ready) {
        char failure[sizeof(status_.error)] = {};
        std::snprintf(failure, sizeof(failure),
                      "source string table failed at %.96s (%u/%u)",
                      tables.failedAsset[0] ? tables.failedAsset : "unknown",
                      tables.mountedCount, tables.sourceCount);
        return Fail(failure);
    }

    VitaScriptServicesBeginRawFiles();
    bool complete = true;
    const char* failedName = nullptr;
    for (uint32_t index = 0; index < rawfiles.Count(); ++index) {
        const VitaRawFileRecord* record = rawfiles.Record(index);
        if (!record || record->name.empty()) {
            complete = false;
            failedName = "<missing-record>";
            break;
        }
        const char* bytes = record->bytes.empty()
            ? "" : reinterpret_cast<const char*>(record->bytes.data());
        if (!VitaScriptServicesInstallRawFile(
                record->name.c_str(), bytes, record->bytes.size())) {
            complete = false;
            failedName = record->name.c_str();
            break;
        }
    }
    if (!complete || !SourceBoundarySucceeded("mount source rawfiles")) {
        char failure[sizeof(status_.error)] = {};
        std::snprintf(failure, sizeof(failure),
                      "source rawfile failed at %.112s%s%s",
                      failedName ? failedName : "unknown",
                      VitaScriptServicesHadError() ? ": " : "",
                      VitaScriptServicesHadError()
                          ? VitaScriptServicesLastError() : "");
        VitaScriptServicesClearRawFile();
        return Fail(failure);
    }
    sourceDataMounted_ = true;
    // The desktop client registers ragdolls after its filesystem is ready.
    // Our original ragdoll.cfg commands can only resolve after VITR is mounted.
    InvokeSourceRoot([] { Ragdoll_Register(); });
    if (!SourceBoundarySucceeded("Ragdoll_Register")) return false;
    return true;
}

bool VitaSourceSession::MountSourceAnimations(
    const VitaAnimationBundle& animations) {
    if (status_.state != VitaSourceSessionState::Initialized || !registry_
        || !sourceStringsInitialized_ || !sourceModelAssetsMounted_
        || sourceAnimationsMounted_)
        return Fail("source animations requested outside clean initialized state");

    VitaSourceAssetMountReport report{};
    VitaScriptServicesClearError();
    InvokeSourceRoot([&] {
        report = registry_->RehydrateSourceAnimations(
            animations, VitaAssetLifetime::Map, true);
    });
    if (!SourceBoundarySucceeded("rehydrate source XAnimParts closure"))
        return false;
    if (!report.ready) {
        char failure[sizeof(status_.error)] = {};
        const VitaMissingSourceAsset* missing = report.missingCount
            ? &report.missing[0] : nullptr;
        std::snprintf(failure, sizeof(failure),
                      "source animation failed at %.72s (%u/%u%s%.64s)",
                      report.failedAsset[0] ? report.failedAsset : "unknown",
                      report.mountedCount, report.sourceCount,
                      missing ? ": " : "", missing ? missing->name : "");
        return Fail(failure);
    }
    sourceAnimationsMounted_ = true;
    return true;
}

bool VitaSourceSession::MountSourceSounds(const VitaSoundBundle& sounds) {
    if (status_.state != VitaSourceSessionState::Initialized || !registry_
        || !sourceDataMounted_ || sourceSoundInitialized_)
        return Fail("source sounds requested outside initialized data state");
    const auto report = registry_->RehydrateSourceSounds(sounds);
    if (!report.ready) {
        char error[sizeof(status_.error)]{};
        std::snprintf(error, sizeof(error), "source sound closure failed at %.96s (%u/%u)",
            report.failedAsset[0] ? report.failedAsset : "missing VSAL catalog", report.mountedCount, report.sourceCount);
        return Fail(error);
    }
    sounds.FreezePreparation();
    VitaScriptServicesClearError();
    sourceSoundInitialized_ = true; // also owns cleanup after partial driver startup
    bool driverReady = false;
    InvokeSourceRoot([&] { driverReady = SND_InitDriver() != 0; });
    if (!SourceBoundarySucceeded("SND_InitDriver") || !driverReady)
        return Fail("source OpenAL output could not initialize");
    InvokeSourceRoot([] { SND_Init(); });
    return SourceBoundarySucceeded("SND_Init");
}

bool VitaSourceSession::MountSourceEffects(
    const VitaEffectBundle& effects, const VitaImpactFxBundle& impacts) {
    if (status_.state != VitaSourceSessionState::Initialized || !registry_
        || !sourceStringsInitialized_ || !sourceModelAssetsMounted_
        || !sourceAnimationsMounted_ || sourceEffectsMounted_)
        return Fail("source effects requested outside clean initialized state");

    VitaSourceAssetMountReport report{};
    VitaScriptServicesClearError();
    InvokeSourceRoot([&] {
        report = registry_->RehydrateSourceEffects(
            effects, impacts, VitaAssetLifetime::Map);
    });
    if (!SourceBoundarySucceeded("rehydrate source FxEffectDef closure"))
        return false;
    if (!report.ready) {
        char failure[sizeof(status_.error)] = {};
        const VitaMissingSourceAsset* missing = report.missingCount
            ? &report.missing[0] : nullptr;
#if KISAK_VITA_CPU_RUNTIME
        if (registry_->AllocationLimitReached()) {
            std::snprintf(failure,sizeof(failure),"source registry memory limit during effect %.72s",
                report.failedAsset[0]?report.failedAsset:"unknown");
            return Fail(failure);
        }
#endif
        std::snprintf(failure, sizeof(failure),
                      "source effect/impact table failed at %.72s (%u/%u%s%.64s)",
                      report.failedAsset[0] ? report.failedAsset : "unknown",
                      report.mountedCount, report.sourceCount,
                      missing ? ": " : "", missing ? missing->name : "");
        return Fail(failure);
    }
    sourceEffectsMounted_ = true;
    return true;
}

bool VitaSourceSession::MountSourceFonts(const VitaFontBundle& fonts) {
    if (status_.state != VitaSourceSessionState::Initialized || !registry_
        || !sourceModelAssetsMounted_ || sourceFontsMounted_)
        return Fail("source fonts requested outside initialized material state");
    const auto report = registry_->RehydrateSourceFonts(fonts);
    if (!report.ready) {
        char error[sizeof(status_.error)]{};
        std::snprintf(error, sizeof(error), "source font closure failed at %.70s%s%.70s",
            report.failedAsset, report.missingCount ? ": " : "",
            report.missingCount ? report.missing[0].name : "");
        return Fail(error);
    }
    sourceFontsMounted_ = true;
    return true;
}

bool VitaSourceSession::MountSourceWeapons(
    const ShipmentWeaponTable& weapons) {
    if (status_.state != VitaSourceSessionState::Initialized || !registry_
        || !sourceStringsInitialized_ || !sourceModelAssetsMounted_
        || !sourceAnimationsMounted_ || !sourceEffectsMounted_)
        return Fail("source weapons requested outside initialized state");

    VitaWeaponMountReport report{};
    VitaScriptServicesClearError();
    InvokeSourceRoot([&] {
        report = registry_->RehydrateSourceWeapons(
            weapons, VitaAssetLifetime::Map);
    });
    if (!SourceBoundarySucceeded("rehydrate source weapons"))
        return false;
    if (!report.ready) {
        char failure[sizeof(status_.error)] = {};
        const VitaMissingSourceAsset* missing = report.missingCount
            ? &report.missing[0] : nullptr;
        std::snprintf(failure, sizeof(failure),
                      "weapon closure failed at %s (%u mounted%s%s)",
                      report.failedWeapon[0] ? report.failedWeapon : "unknown",
                      report.mountedCount, missing ? ": " : "",
                      missing ? missing->name : "");
        return Fail(failure);
    }
    sourceWeaponsMounted_ = true;
    return true;
}

bool VitaSourceSession::MountMapRoots(
    const char* mapName, const ShipmentCollision& collision,
    const ShipmentMapEntities& entities, uint32_t checksum) {
    if (status_.state != VitaSourceSessionState::Initialized || !registry_)
        return Fail("source map roots requested outside initialized state");
    char normalized[32] = {};
    if (!NormalizeMapName(mapName, normalized))
        return Fail("map root is not certified for the Vita vertical slice");

    const VitaMapRootReport report = registry_->RehydrateMapRoots(
        normalized, collision, entities, checksum, VitaAssetLifetime::Map);
    if (!report.ready)
        return Fail(report.error[0] ? report.error
                                   : "source map root construction failed");
    sourceMapRootsMounted_ = true;
    std::strncpy(mountedRootMap_, normalized, sizeof(mountedRootMap_) - 1u);
    return true;
}

bool VitaSourceSession::StartMap(const char* mapName) {
    VITA_CPU_LOADING("source-start-map");
    if (status_.state != VitaSourceSessionState::Initialized || !registry_)
        return Fail("source map start requested outside initialized state");
    if (!sourceWeaponsMounted_)
        return Fail("source VITP v5 weapons are not mounted");
    if (!sourceDataMounted_)
        return Fail("source VITR/VITT data closure is not mounted");
    if (!sourceSoundInitialized_)
        return Fail("source VSAL/VITS sound closure is not mounted");
    if (!sourceModelAssetsMounted_)
        return Fail("source VITX v5 model closure is not mounted");
    if (!sourceFontsMounted_)
        return Fail("source VITF v2 font closure is not mounted");
    if (!sourceAnimationsMounted_)
        return Fail("source VITQ v3 animation closure is not mounted");
    if (!sourceEffectsMounted_)
        return Fail("source VFXA v3/VIFX v1 effect closure is not mounted");
    char normalized[32] = {};
    if (!NormalizeMapName(mapName, normalized))
        return Fail("map is not certified for the Vita vertical slice");
    if (!sourceMapRootsMounted_ || std::strcmp(normalized, mountedRootMap_))
        return Fail("matching VITC v3 source map roots are not mounted");

    const VitaSourceClosureReport closure =
        registry_->ValidateOfflineMap(normalized);
    if (!closure.ready) {
        char failure[sizeof(status_.error)] = {};
        const VitaMissingSourceAsset* missing = closure.missingCount
            ? &closure.missing[0] : nullptr;
        std::snprintf(failure, sizeof(failure),
                      "source asset closure incomplete (%u missing%s%s)",
                      closure.missingCount,
                      missing ? ": " : "",
                      missing ? missing->name : "");
        return Fail(failure);
    }

    VitaScriptServicesClearError();
    status_.state = VitaSourceSessionState::Loading;
    InvokeSourceRoot([] {
        CL_InitRenderer();
        Con_OneTimeInit();
        // Cgame notifications and HUD text use the original shared UI font
        // selectors even while the outer frontend owns menu navigation.
        // Mount their complete source cache after the typed fonts/materials.
        UI_AssetCache();
    });
    if (!SourceBoundarySucceeded("CL_InitRenderer"))
        return false;
    static uint32_t mapGeneration = 0;
    VitaSourceRender_BeginMap(++mapGeneration);
    std::strncpy(status_.mapName, normalized,
                 sizeof(status_.mapName) - 1u);
    char mutableMapName[32] = {};
    std::strncpy(mutableMapName, normalized, sizeof(mutableMapName) - 1u);
    InvokeSourceRoot([&] {
        Dvar_SetStringByName("g_gametype", const_cast<char*>("dm"));
        Dvar_SetIntByName("ui_maxclients", 1);
        Dvar_SetIntByName("sv_maxclients", 1);
        Dvar_SetIntByName("sv_fps", 20);
        SV_SpawnServer(mutableMapName);
    });
    if (!SourceBoundarySucceeded("SV_SpawnServer") || !SV_Loaded())
        return status_.state == VitaSourceSessionState::Failed
            ? false : Fail("source server did not enter SS_GAME");

    tickAccumulatorMs_ = 0;
    clientAccumulatorMs_ = 0;
    status_.serverTicks = 0;
    status_.loopbackPackets = 0;
    status_.connectionElapsedMs = 0;
    status_.clientConnectionState =
        static_cast<int32_t>(clientUIActives[0].connectionState);
    status_.serverClientState = svs.clients ? svs.clients[0].header.state : 0;
    status_.connectPacketCount = static_cast<uint32_t>(
        std::max(clientConnections[0].connectPacketCount, 0));
    status_.serverMessageSequence = static_cast<uint32_t>(
        std::max(clientConnections[0].serverMessageSequence, 0));
    status_.state = VitaSourceSessionState::Connecting;
    initialJoinElapsedMs_ = 0;
    initialJoinStage_ = 0;
#if defined(KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT)
    if (!g_inputSmoke.Load()) return Fail("invalid diagnostic source input fixture");
#endif
    return true;
}

void VitaSourceSession::AdvanceInitialJoin() {
    if (status_.initialPlayerSpawned) return;
    const cg_s* cg = CG_GetLocalClientGlobals(0);
    const char* menu = cg->scriptMainMenu;
    const char* response = nullptr;
    uint8_t nextStage = initialJoinStage_;
    if (initialJoinStage_ == 0 && !std::strncmp(menu, "team_", 5)) {
        response = "autoassign";
        nextStage = 1;
    } else if (initialJoinStage_ <= 1 && !std::strncmp(menu, "class_", 6)) {
        menu = "changeclass_offline";
        response = "offline_class1_mp,0";
        nextStage = 2;
    }
    if (response) {
        // Same reliable command/configstring protocol as CL_OpenScriptMenu_f.
        // The native frontend has already accepted offline FFA; original GSC
        // receives its team/class choices and owns the complete spawn/loadout.
        for (int index = 0; index < 32; ++index) {
            if (std::strcmp(CL_GetConfigString(0, 1970 + index), menu)) continue;
            char command[128]{};
            std::snprintf(command, sizeof(command), "mr %d %d %s",
                Dvar_GetInt("sv_serverId"), index, response);
            CL_AddReliableCommand(0, command);
            // The response follows the native menu protocol. Close its popup
            // without generating a second response or leaving a waiting menu
            // to reopen invisibly after the frontend has entered gameplay.
            CG_CloseScriptMenu(0, false);
            initialJoinStage_ = nextStage;
            Com_Printf(0, "Vita source join menu=%s response=%s\n", menu, response);
            break;
        }
    }
    const playerState_s& ps = cg->predictedPlayerState;
    if (initialJoinStage_ >= 2 && ps.pm_type == PM_NORMAL && ps.stats[0] > 0 && ps.weapon) {
        if (initialJoinStage_ == 2) {
            CG_CloseScriptMenu(0, false);
            UI_CloseAllMenus(0);
            // UI_SetActiveMenu returns early when no menus were registered;
            // release only the UI catcher even in that case.
            Key_RemoveCatcher(0, ~16);
            initialJoinStage_ = 3;
            // CG_DrawActiveFrame must run once after closing the popups. Its
            // last copied UI packet may still contain the class menu.
            Com_Printf(0, "Vita source initial join menus closed\n");
            return;
        }
        if (ps.viewmodelIndex > 0) {
            const char* hands = CL_GetConfigString(0, ps.viewmodelIndex + CS_MODELS);
            if (hands) std::snprintf(status_.handModelName, sizeof(status_.handModelName), "%s", hands);
        }
        status_.initialPlayerSpawned = true;
        Com_Printf(0, "Vita source initial spawn time=%d weapon=%s health=%d\n",
            ps.commandTime, BG_GetWeaponDef(ps.weapon)->szInternalName, ps.stats[0]);
    }
}

uint32_t VitaSourceSession::PumpLoopback() {
    uint32_t total = 0;
    // Alternate endpoints because processing one handshake packet commonly
    // emits its reply onto the other endpoint. Bound the work even if a
    // corrupt stream creates a reply loop.
    for (uint32_t pass = 0; pass < 8; ++pass) {
        const uint32_t server = PumpEndpoint(NS_SERVER, g_serverMessageBytes);
        const uint32_t client = PumpEndpoint(NS_CLIENT1, g_clientMessageBytes);
        total += server + client;
        if (!server && !client)
            break;
    }
    status_.loopbackPackets += total;
    return total;
}

bool VitaSourceSession::Frame(uint32_t elapsedMs, bool advanceClient) {
    VITA_CPU_SCOPE("source-frame");
    status_.frameServerTicks = status_.backlogMs = 0;
    status_.clFrameUsec = status_.cgameUsec = 0;
    status_.serverUsec = status_.clientUsec = status_.loopbackUsec =
        status_.presentationUsec = status_.audioUsec = 0;
#if defined(KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT)
    static uint32_t sourceFrameSequence = 0;
    const uint32_t sourceFrame = ++sourceFrameSequence;
    const auto traceFrame = [this, sourceFrame](const char* phase) {
        if (sourceFrame <= 96) {
            std::printf(
                "[kisak-vita] source frame=%u phase=%s state=%u conn=%d ticks=%u packets=%u accumulator=%u\n",
                static_cast<unsigned>(sourceFrame), phase,
                static_cast<unsigned>(status_.state),
                static_cast<int>(clientUIActives[0].connectionState),
                static_cast<unsigned>(status_.serverTicks),
                static_cast<unsigned>(status_.loopbackPackets),
                static_cast<unsigned>(tickAccumulatorMs_));
        }
    };
    traceFrame("enter");
#else
    const auto traceFrame = [](const char*) {};
#endif
    if (status_.state == VitaSourceSessionState::Paused)
        return true;
    if (status_.state != VitaSourceSessionState::Connecting
        && status_.state != VitaSourceSessionState::Active)
        return Fail("source frame requested outside a running session");
#if KISAK_VITA_CPU_PROFILE
    if(VitaCpuReplay_TapeActive()) {
        // Correctness replay uses identical 30 Hz source-call boundaries in
        // both versions. The independent ordinary-play schedule is untouched.
        if(!advanceClient)return true;
        if(!VitaCpuReplay_NextFrame(elapsedMs))return Fail("source replay tape exhausted");
    }
#endif
    if (elapsedMs > kMaximumRunningFrameMs)
        return Fail("running clock exceeded 60 seconds without a platform suspend event");

    clientAccumulatorMs_ += elapsedMs;
    cls.realFrametime = static_cast<int>(clientAccumulatorMs_);
    cls.frametime = static_cast<int>(clientAccumulatorMs_);
    cls.realtime += static_cast<int>(elapsedMs);
    com_frameTime += static_cast<int>(elapsedMs);

    VitaScriptServicesClearError();
    traceFrame("pre_loopback_begin");
    uint64_t phaseStart = sceKernelGetProcessTimeWide();
    InvokeSourceRoot([this] { PumpLoopback(); });
    status_.loopbackUsec += sceKernelGetProcessTimeWide() - phaseStart;
    traceFrame("pre_loopback_end");
    if (!SourceBoundarySucceeded("pre-client loopback"))
        return false;
    if (advanceClient) {
    traceFrame("client_begin");
    phaseStart = sceKernelGetProcessTimeWide();
    InvokeSourceRoot([] { CL_Frame(0); }, "client-command");
    status_.clientUsec += sceKernelGetProcessTimeWide() - phaseStart;
    status_.clFrameUsec = status_.clientUsec;
    traceFrame("client_end");
    if (!SourceBoundarySucceeded("CL_Frame"))
        return false;
    // Cgame owns command creation, prediction, entity events, view animation,
    // camera/HUD calculation and the renderer packet. Preserve the original
    // CL_Input call inside CG_DrawActiveFrame exactly once per client frame.
    if (clientUIActives[0].connectionState == CA_ACTIVE) {
        traceFrame("cgame_begin");
        phaseStart = sceKernelGetProcessTimeWide();
#if defined(KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT)
        InvokeSourceRoot([this] {
            const auto& ps = CG_GetLocalClientGlobals(0)->predictedPlayerState;
            g_inputSmoke.Apply(clients[0].serverTime, status_.initialPlayerSpawned
                && !(ps.pm_flags & (PMF_FROZEN | PMF_RESPAWNED)));
        });
        if (!SourceBoundarySucceeded("source input smoke")) return false;
#endif
        InvokeSourceRoot([] {
            CG_DrawActiveFrame(0, clients[0].serverTime, static_cast<DemoType>(0),
                CUBEMAPSHOT_NONE, 0, 1);
            CL_SendCmd(0);
#if KISAK_VITA_CPU_PROFILE
            VitaCpuReplay_Command(clients[0].cmds[clients[0].cmdNumber & 127],clients[0].cmdNumber);
#endif
        });
        status_.clientUsec += sceKernelGetProcessTimeWide() - phaseStart;
        status_.cgameUsec = status_.clientUsec - status_.clFrameUsec;
        traceFrame("cgame_end");
        if (!SourceBoundarySucceeded("CG_DrawActiveFrame")) return false;
        ++status_.completedClientFrames;
        InvokeSourceRoot([this] { AdvanceInitialJoin(); });
        if (!SourceBoundarySucceeded("source initial menu responses")) return false;
        if (!status_.initialPlayerSpawned) {
            initialJoinElapsedMs_ += clientAccumulatorMs_;
            if (initialJoinElapsedMs_ > kConnectionTimeoutMs)
                return Fail("source initial team/class/spawn response timed out");
        }
    }
    clientAccumulatorMs_ = 0;
    }
    traceFrame("post_client_loopback_begin");
    phaseStart = sceKernelGetProcessTimeWide();
    InvokeSourceRoot([this] { PumpLoopback(); });
    status_.loopbackUsec += sceKernelGetProcessTimeWide() - phaseStart;
    traceFrame("post_client_loopback_end");
    if (!SourceBoundarySucceeded("post-client loopback"))
        return false;

    // Keep connection diagnostics cheap enough for every release frame.  The
    // public report exposes only counters and enum values; no packet payload
    // or retail data is copied into the telemetry path.
    status_.clientConnectionState =
        static_cast<int32_t>(clientUIActives[0].connectionState);
    status_.serverClientState = svs.clients ? svs.clients[0].header.state : 0;
    status_.connectPacketCount = static_cast<uint32_t>(
        std::max(clientConnections[0].connectPacketCount, 0));
    status_.serverMessageSequence = static_cast<uint32_t>(
        std::max(clientConnections[0].serverMessageSequence, 0));

    tickAccumulatorMs_ += elapsedMs;
    while (tickAccumulatorMs_ >= kServerTickMs) {
        traceFrame("server_begin");
        phaseStart = sceKernelGetProcessTimeWide();
        InvokeSourceRoot([] { SV_Frame(kServerTickMs); }, "server-tick");
        status_.serverUsec += sceKernelGetProcessTimeWide() - phaseStart;
        traceFrame("server_end");
        if (!SourceBoundarySucceeded("SV_Frame"))
            return false;
        tickAccumulatorMs_ -= kServerTickMs;
        ++status_.serverTicks;
        ++status_.frameServerTicks;
#if KISAK_VITA_CPU_PROFILE
        if(level.clients)VitaCpuReplay_State("server",level.clients[0].ps,status_.serverTicks);
        VitaCpuReplay_ServerWorld(status_.serverTicks);
#endif
        traceFrame("post_server_loopback_begin");
        phaseStart = sceKernelGetProcessTimeWide();
        InvokeSourceRoot([this] { PumpLoopback(); });
        status_.loopbackUsec += sceKernelGetProcessTimeWide() - phaseStart;
        traceFrame("post_server_loopback_end");
        if (!SourceBoundarySucceeded("post-server loopback"))
            return false;
    }

    status_.backlogMs = tickAccumulatorMs_;
    if (clientUIActives[0].connectionState == CA_ACTIVE) {
        status_.state = VitaSourceSessionState::Active;
        status_.connectionElapsedMs = 0;
        phaseStart = sceKernelGetProcessTimeWide();
        if (advanceClient) {
            UpdatePresentation();
#if KISAK_VITA_CPU_PROFILE
            VitaCpuReplay_State("client",CG_GetLocalClientGlobals(0)->predictedPlayerState,status_.completedClientFrames);
#endif
        }
        status_.presentationUsec += sceKernelGetProcessTimeWide() - phaseStart;
    } else {
        status_.state = VitaSourceSessionState::Connecting;
        status_.connectionElapsedMs += elapsedMs;
        if (status_.connectionElapsedMs > kConnectionTimeoutMs)
            return Fail("source loopback connection timed out");
    }
    phaseStart = sceKernelGetProcessTimeWide();
    if (advanceClient) InvokeSourceRoot([] { SND_Update(); }, "sound-control");
    status_.audioUsec = sceKernelGetProcessTimeWide() - phaseStart;
    if (!SourceBoundarySucceeded("SND_Update")) return false;
#if defined(KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT)
    if (g_inputSmoke.Recording()) {
        const usercmd_s& cmd = clients[0].cmds[clients[0].cmdNumber & 127];
        const auto& render = VitaSourceRender_Packet();
        Com_Printf(0, "Vita source input cmd phase=%u number=%d time=%d buttons=%x weapon=%u offhand=%u move=%d,%d angles=%d,%d,%d entities=%u ui=%u fx=%u rejected=%u\n",
            g_inputSmoke.Phase(), clients[0].cmdNumber, cmd.serverTime, cmd.buttons, cmd.weapon,
            cmd.offHandIndex, static_cast<int>(static_cast<signed char>(cmd.forwardmove)),
            static_cast<int>(static_cast<signed char>(cmd.rightmove)), cmd.angles[0], cmd.angles[1], cmd.angles[2],
            render.dynamicEntityCount, render.uiCommandCount, render.effectDrawCount, render.rejectedCommands);
        TraceSourceInputState("client", CG_GetLocalClientGlobals(0)->predictedPlayerState);
        if (level.clients) TraceSourceInputState("server", level.clients[0].ps);
    }
#endif
    traceFrame("return");
    return true;
}

void VitaSourceSession::UpdatePresentation() {
    if (clientUIActives[0].connectionState != CA_ACTIVE
        || !clients[0].snap.valid)
        return;
    const playerState_s& state = CG_GetLocalClientGlobals(0)->predictedPlayerState;
    ShipmentDmPlayerState next{};
    next.clientNum = static_cast<uint32_t>(std::max(0, state.clientNum));
    next.connected = true;
    next.alive = state.stats[0] > 0 && state.pm_type < PM_DEAD;
    std::memcpy(next.origin, state.origin, sizeof(next.origin));
    std::memcpy(next.velocity, state.velocity, sizeof(next.velocity));
    std::memcpy(next.angles, state.viewangles, sizeof(next.angles));
    next.health = std::max(0, state.stats[0]);
    next.weaponSlot = PresentationWeaponSlot(state.weapon);
    next.weapAnim = state.weapAnim;
    next.weaponState = static_cast<int32_t>(state.weaponstate);
    next.weaponPosFrac = std::clamp(state.fWeaponPosFrac, 0.0f, 1.0f);
    next.viewHeight = std::clamp(state.viewHeightCurrent, 8.0f, 60.0f);
    next.ads = (state.pm_flags & PMF_SIGHT_AIMING) != 0;
    next.prone = (state.pm_flags & PMF_PRONE) != 0;
    next.crouched = (state.pm_flags & PMF_DUCKED) != 0;
    next.sprinting = (state.pm_flags & PMF_SPRINTING) != 0;
    next.jumping = (state.pm_flags & PMF_JUMPING) != 0;
    next.firing = state.weaponstate == WEAPON_FIRING;
    next.reloading = state.weaponstate == WEAPON_RELOADING
        || state.weaponstate == WEAPON_RELOADING_INTERUPT
        || state.weaponstate == WEAPON_RELOAD_START
        || state.weaponstate == WEAPON_RELOAD_START_INTERUPT
        || state.weaponstate == WEAPON_RELOAD_END;
    next.melee = state.weaponstate >= WEAPON_MELEE_INIT
        && state.weaponstate <= WEAPON_MELEE_END;
    next.grenade = state.weaponstate >= WEAPON_OFFHAND_INIT
        && state.weaponstate <= WEAPON_OFFHAND_END;
    if (state.weapon && state.weapon < BG_GetNumWeapons()) {
        const int clip = BG_ClipForWeapon(state.weapon);
        const int ammo = BG_AmmoForWeapon(state.weapon);
        if (clip >= 0 && clip < static_cast<int>(std::size(state.ammoclip)))
            next.clipAmmo = state.ammoclip[clip];
        if (ammo >= 0 && ammo < static_cast<int>(std::size(state.ammo)))
            next.reserveAmmo = state.ammo[ammo];
    }
#if KISAK_VITA_CPU_RUNTIME
    const uint32_t weaponCount=BG_GetNumWeapons();
    if(cachedWeaponCount_!=weaponCount) {
        cachedFragIndex_=BG_FindWeaponIndexForName("frag_grenade_mp");
        cachedWeaponCount_=weaponCount;
    }
    const uint32_t frag=cachedFragIndex_;
#else
    const uint32_t frag = BG_FindWeaponIndexForName("frag_grenade_mp");
#endif
    if (frag) {
        const int fragClip = BG_ClipForWeapon(frag);
        if (fragClip >= 0
            && fragClip < static_cast<int>(std::size(state.ammoclip)))
            next.grenadeCount = std::max(0, state.ammoclip[fragClip]);
    }
    const int32_t actionEnd = state.commandTime
        + std::max(state.weaponTime, state.weaponDelay);
    next.nextFireTime = actionEnd;
    if (next.reloading) {
        next.reloadStartTime = previousWeaponState_ == state.weaponstate
            && playerPresentation_.reloadStartTime != 0
            ? playerPresentation_.reloadStartTime : state.commandTime;
        next.reloadEndTime = actionEnd;
    }
    if (state.weaponShotCount != previousWeaponShotCount_)
        next.muzzleFlashEndTime = state.commandTime + 50;
    else
        next.muzzleFlashEndTime = playerPresentation_.muzzleFlashEndTime;

    if (level.clients && state.clientNum >= 0
        && state.clientNum < level.maxclients) {
        const gclient_s& sourceClient = level.clients[state.clientNum];
        next.team = static_cast<uint32_t>(sourceClient.sess.cs.team);
        next.score = sourceClient.sess.score;
        next.deaths = sourceClient.sess.deaths;
    }
    playerPresentation_ = next;
    previousWeaponState_ = state.weaponstate;
    previousWeaponShotCount_ = state.weaponShotCount;

    matchPresentation_ = {};
    matchPresentation_.phase = state.pm_type == PM_INTERMISSION
        ? ShipmentDmMatchPhase::Complete : ShipmentDmMatchPhase::Active;
    matchPresentation_.elapsedMsec = std::max(0, level.time - level.startTime);
    matchPresentation_.scoreLimit = 0;
    matchPresentation_.timeLimitMsec = 0;
    matchPresentation_.killScore = 1;
    if (matchPresentation_.phase == ShipmentDmMatchPhase::Complete)
        matchPresentation_.winnerClientNum = next.clientNum;

    grenadePresentation_ = {};
    const int entityCount = std::clamp(level.num_entities, 0, MAX_GENTITIES);
    for (int entityIndex = 0; entityIndex < entityCount; ++entityIndex) {
        const gentity_s& entity = g_entities[entityIndex];
        if (!entity.r.inuse || entity.s.eType != ET_MISSILE
            || entity.s.weapon <= 0
            || static_cast<uint32_t>(entity.s.weapon) >= BG_GetNumWeapons())
            continue;
        const WeaponDef* weapon = BG_GetWeaponDef(entity.s.weapon);
        if (!weapon || (weapon->offhandClass != OFFHAND_CLASS_FRAG_GRENADE
                        && weapon->offhandClass != OFFHAND_CLASS_FLASH_GRENADE
                        && weapon->offhandClass != OFFHAND_CLASS_SMOKE_GRENADE))
            continue;
        if (entity.parent.isDefined()
            && entity.parent.entnum() != static_cast<uint32_t>(state.clientNum))
            continue;
        grenadePresentation_.active = true;
        grenadePresentation_.ownerClientNum = next.clientNum;
        grenadePresentation_.spawnTime = entity.missile.timeOfBirth;
        std::memcpy(grenadePresentation_.origin, entity.r.currentOrigin,
                    sizeof(grenadePresentation_.origin));
        BG_EvaluateTrajectoryDelta(&entity.s.lerp.pos, level.time,
                                   grenadePresentation_.velocity);
        break;
    }
}

bool VitaSourceSession::PlaceGpuDiagnosticPlayer(const ShipmentMapEntities& entities) {
    if(VitaCpuCurrentRole()!=VitaCpuRole::Source || !Active() || !status_.initialPlayerSpawned
        || std::strcmp(status_.mapName,"mp_shipment") || entities.DeathmatchSpawns().empty()
        || !level.clients || !g_entities[0].client || !g_entities[0].r.inuse
        || g_entities[0].client->sess.connected==CON_DISCONNECTED)
        return Fail("GPU diagnostic source player/spawn unavailable");
    auto spawn=entities.DeathmatchSpawns().front();
    if(!VitaGpuAnchorValid(spawn.origin,spawn.angles))return Fail("GPU diagnostic authored spawn invalid");
    InvokeSourceRoot([&]{TeleportPlayer(&g_entities[0],spawn.origin,spawn.angles);},"gpu-diagnostic-place");
    if(!SourceBoundarySucceeded("GPU diagnostic source placement"))return false;
    VitaCpuProfile_Counter("gpu-anchor-origin-xy",VitaGpuFloatBits(spawn.origin[0]),VitaGpuFloatBits(spawn.origin[1]));
    VitaCpuProfile_Counter("gpu-anchor-origin-z-yaw",VitaGpuFloatBits(spawn.origin[2]),VitaGpuFloatBits(spawn.angles[1]));
    VitaCpuProfile_Counter("gpu-anchor-angles",VitaGpuFloatBits(spawn.angles[0]),VitaGpuFloatBits(spawn.angles[2]));
    return true;
}

bool VitaSourceSession::GpuDiagnosticPlayerReady(const ShipmentMapEntities& entities) const {
    if(VitaCpuCurrentRole()!=VitaCpuRole::Source || !Active() || !level.clients
        || !status_.initialPlayerSpawned || entities.DeathmatchSpawns().empty())return false;
    const auto& spawn=entities.DeathmatchSpawns().front();
    const auto& server=level.clients[0].ps;const auto& client=CG_GetLocalClientGlobals(0)->predictedPlayerState;
    return server.pm_type==PM_NORMAL && client.pm_type==PM_NORMAL && server.stats[0]>0 && client.stats[0]>0
        && server.weapon && server.weapon==client.weapon
        && VitaGpuAnchorReady(spawn.origin,spawn.angles,server.origin,client.origin,
            server.viewangles,client.viewangles,server.velocity,client.velocity);
}

bool VitaSourceSession::CopyPlayerPresentation(
    ShipmentDmPlayerState* output) const {
    if (!output || status_.state != VitaSourceSessionState::Active
        || !playerPresentation_.connected)
        return false;
    *output = playerPresentation_;
    return true;
}

bool VitaSourceSession::CopyMatchPresentation(
    ShipmentDmMatchState* output) const {
    if (!output || status_.state != VitaSourceSessionState::Active)
        return false;
    *output = matchPresentation_;
    return true;
}

bool VitaSourceSession::CopyGrenadePresentation(
    ShipmentDmProjectileState* output) const {
    if (!output || status_.state != VitaSourceSessionState::Active)
        return false;
    *output = grenadePresentation_;
    return output->active;
}

bool VitaSourceSession::SetScoreboardVisible(bool visible) {
    if (!Active()) return false;
    // Original commands request real scores and own fades/layout on source.
    InvokeSourceRoot([visible] { if (visible) CG_ScoresDown(0); else CG_ScoresUp(0); });
    return SourceBoundarySucceeded("source scoreboard");
}

bool VitaSourceSession::SetPaused(bool paused) {
    if (paused) {
        if (status_.state != VitaSourceSessionState::Active
            && status_.state != VitaSourceSessionState::Connecting)
            return false;
        VitaScriptServicesClearError();
        InvokeSourceRoot([] {
            Dvar_SetInt(const_cast<dvar_s*>(cl_paused), 1);
            Dvar_SetInt(const_cast<dvar_s*>(sv_paused), 1);
            SND_UpdatePause();
        });
        if (!SourceBoundarySucceeded("pause source session"))
            return false;
        status_.state = VitaSourceSessionState::Paused;
        return true;
    }
    if (status_.state != VitaSourceSessionState::Paused)
        return false;
    VitaScriptServicesClearError();
    InvokeSourceRoot([] {
        Dvar_SetInt(const_cast<dvar_s*>(cl_paused), 0);
        Dvar_SetInt(const_cast<dvar_s*>(sv_paused), 0);
        SND_UpdatePause();
    });
    if (!SourceBoundarySucceeded("resume source session"))
        return false;
    status_.state = clientUIActives[0].connectionState == CA_ACTIVE
        ? VitaSourceSessionState::Active
        : VitaSourceSessionState::Connecting;
    return true;
}

bool VitaSourceSession::DiagnosticInputComplete() const {
#if defined(KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT)
    return g_inputSmoke.Completed();
#else
    return false;
#endif
}

void VitaSourceSession::Stop() {
    // Loading/renderer waits service existing streams. Stop source channels
    // before those waits: draining decoders alone leaves OpenAL queues and
    // channel ownership alive across Main and the next map generation.
    bool audioStopped = true;
    if (sourceSoundInitialized_) {
        VitaScriptServicesClearError();
        InvokeSourceRoot([] { SND_StopSounds(SND_STOP_ALL); });
        audioStopped = SourceBoundarySucceeded("stop source sounds");
    }
#if KISAK_VITA_CPU_RUNTIME
    VitaCpuAudio_Drain();
    if (sourceSoundInitialized_)
        VitaCpuProfile_Counter("source-audio-stopped", audioStopped,
                               VitaCpuAudio_ReadStatus().activeStreams);
    VitaCpuRender_Drain();
    VitaCpuFiles_Drain();
#endif
    VitaSourceModel_Reset();
#if defined(KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT)
    g_inputSmoke.Reset();
#endif
    if (status_.state == VitaSourceSessionState::Cold
        || status_.state == VitaSourceSessionState::Initialized)
        return;
    // Com_Restart tears cgame down before the server/game globals. Its
    // weapon DObjs and XAnim trees still need the current BG weapon table.
    // Disconnecting the loopback connection alone only clears client state.
    bool clientStopped = true;
    if (clientRootInitialized_ && clientUIActives[0].cgameInitCalled) {
        VitaScriptServicesClearError();
        InvokeSourceRoot([] { CL_ShutdownCGame(0); });
        clientStopped = SourceBoundarySucceeded("stop source cgame");
        if (clientStopped) cls.hunkUsersStarted = 0;
    }
    VitaScriptServicesClearError();
    InvokeSourceRoot([this] {
        if (cl_paused)
            Dvar_SetInt(const_cast<dvar_s*>(cl_paused), 0);
        if (sv_paused)
            Dvar_SetInt(const_cast<dvar_s*>(sv_paused), 0);
        if (serverRootInitialized_ && com_sv_running
            && com_sv_running->current.enabled)
            SV_Shutdown("EXE_DISCONNECTED");
        else if (clientRootInitialized_ && clientUIActives[0].isRunning)
            CL_Disconnect(0);
    });
    const bool cleanStop = SourceBoundarySucceeded("stop source session")
        && clientStopped && audioStopped;
#if KISAK_VITA_CPU_RUNTIME
    VitaCpuAudio_Drain();
#endif
    VitaLoopback_Reset();
    VitaSourceLoopback_Reset();
    VitaSourcePlatform_ResetSessionState();
    tickAccumulatorMs_ = 0;
    clientAccumulatorMs_ = 0;
    playerPresentation_ = {};
    matchPresentation_ = {};
    grenadePresentation_ = {};
    previousWeaponState_ = WEAPON_READY;
    previousWeaponShotCount_ = 0;
    initialJoinElapsedMs_ = 0;
    initialJoinStage_ = 0;
    if (cleanStop) {
        status_ = {};
        status_.state = VitaSourceSessionState::Initialized;
    }
#if KISAK_VITA_CPU_RUNTIME
    VitaCpuProfile_Counter("source-session-stopped",cleanStop);
    VitaCpuProfile_Flush(true);
#endif
}

void VitaSourceSession::Shutdown() {
    Stop();
    VitaScriptServicesClearError();
    InvokeSourceRoot([] { VitaScriptServicesReleaseWeaponInfo(); });

    // Keep teardown roots in separate non-returning error boundaries.  A
    // source ERR_DROP from client disconnect must not skip SL_Shutdown and
    // leave the next menu/map generation with stale interned-string state.
    if (clientRootInitialized_ && clientUIActives[0].isRunning) {
        VitaScriptServicesClearError();
        InvokeSourceRoot([] { CL_Disconnect(0); });
    }
    if (sourceSoundInitialized_) {
        VitaScriptServicesClearError();
        InvokeSourceRoot([] { SND_Shutdown(); });
        sourceSoundInitialized_ = false;
    }
    if (sourceDataMounted_) {
        VitaScriptServicesClearError();
        InvokeSourceRoot([] { Phys_Shutdown(); });
        VitaScriptServicesClearError();
        InvokeSourceRoot([] { Ragdoll_Unregister(); });
    }
    if (sourceDataMounted_)
        VitaScriptServicesClearRawFile();
    // Match Com_Close ordering: DObj/XAnim may still hold interned script
    // strings, and the VM owns variables that must be released before the
    // string table itself is destroyed.
    if (sourceDObjInitialized_) {
        VitaScriptServicesClearError();
        InvokeSourceRoot([] { Com_ShutdownDObj(); });
        VitaScriptServicesClearError();
        InvokeSourceRoot([] { DObjShutdown(); });
    }
    if (sourceXAnimInitialized_) {
        VitaScriptServicesClearError();
        InvokeSourceRoot([] { XAnimShutdown(); });
    }
    if (registry_ && sourceStringsInitialized_) {
        VitaScriptServicesClearError();
        InvokeSourceRoot([this] { registry_->ReleaseScriptStrings(); });
    }
    if (sourceScriptInitialized_) {
        VitaScriptServicesClearError();
        InvokeSourceRoot([] { Scr_Shutdown(); });
    }
    if (sourceStringsInitialized_) {
        // Dvar names and string values hold source SL references.
        if (sourceDvarsInitialized_) {
            VitaScriptServicesClearError();
            InvokeSourceRoot([] { Dvar_Shutdown(); });
        }
        VitaScriptServicesClearError();
        InvokeSourceRoot([] { SL_Shutdown(); });
    }
    // GPU resources belong to the outer map transaction. Source registration
    // caches only map-owned pointers and must release them before that arena.
    cls.rendererStarted = 0;
    cls.whiteMaterial = cls.consoleMaterial = nullptr;
    cls.consoleFont = nullptr;
    cls.vidConfig = {};
    con.initialized = 0;
    sharedUiInfo.assets = {};
    VitaScriptServicesReleaseHunk();
    VitaSourcePlatform_Clear();
    VitaLoopback_Reset();
    VitaSourceLoopback_Reset();
    registry_ = nullptr;
    tickAccumulatorMs_ = 0;
    clientAccumulatorMs_ = 0;
    serverRootInitialized_ = false;
    clientRootInitialized_ = false;
    sourceStringsInitialized_ = false;
    sourceDvarsInitialized_ = false;
    sourceScriptInitialized_ = false;
    sourceXAnimInitialized_ = false;
    sourceDObjInitialized_ = false;
    sourceDataMounted_ = false;
    sourceModelAssetsMounted_ = false;
    sourceFontsMounted_ = false;
    sourceAnimationsMounted_ = false;
    sourceEffectsMounted_ = false;
    sourceWeaponsMounted_ = false;
    sourceMapRootsMounted_ = false;
    mountedRootMap_[0] = '\0';
    playerPresentation_ = {};
    matchPresentation_ = {};
    grenadePresentation_ = {};
    previousWeaponState_ = WEAPON_READY;
    previousWeaponShotCount_ = 0;
    status_ = {};
}

const char* VitaSourceSessionStateName(VitaSourceSessionState state) {
    switch (state) {
    case VitaSourceSessionState::Cold: return "cold";
    case VitaSourceSessionState::Initialized: return "initialized";
    case VitaSourceSessionState::Loading: return "loading";
    case VitaSourceSessionState::Connecting: return "connecting";
    case VitaSourceSessionState::Active: return "active";
    case VitaSourceSessionState::Paused: return "paused";
    case VitaSourceSessionState::Failed: return "failed";
    }
    return "unknown";
}

} // namespace kisak::vita
