#include <vita/vita_script_vm_runtime.h>

#include <psp2/kernel/clib.h>

#include <algorithm>
#include <cstring>
#include <cstdio>

#include <client_mp/client_mp.h>
#include <game_mp/g_main_mp.h>
#include <game_mp/g_public_mp.h>
#include <game_mp/g_utils_mp.h>
#include <server_mp/server_mp.h>
#include <script/scr_main.h>
#include <script/scr_stringlist.h>
#include <script/scr_variable.h>
#include <script/scr_vm.h>

#include <vita/vita_shipment_entities.h>
#include <vita/vita_script_services.h>

namespace kisak::vita {
namespace {

gclient_s g_vitaClient{};
uint32_t g_vitaPersId = 0;
bool g_ready = false;
bool g_gameTypeStarted = false;
int32_t g_lastServerTime = -1;
int32_t g_lastAdvanceServerTime = -1;
uint32_t g_materializedEntityCount = 1;

void TraceNativeStage(const char* stage) {
#if defined(KISAK_VITA_EXPERIMENTAL_NATIVE_ENTITY_RUNTIME)
    if (stage) {
        sceClibPrintf("[vita-script] entity-stage=%s\n", stage);
        VitaScriptServicesTrace("entity_stage", stage);
    }
#else
    (void)stage;
#endif
}

#if defined(KISAK_VITA_EXPERIMENTAL_NATIVE_ENTITY_RUNTIME)
// The full desktop SV_InitGame path allocates every configstring before
// G_InitGame starts executing authored scripts.  The Vita probe deliberately
// bypasses that broad server startup, so builtins such as getnorthyaw and
// precachestatusicon would otherwise pass a null string id into
// SV_GetConfigstring.  Seed the same empty-string table once, then provide
// Shipment's authored north-yaw default.  This is probe-only state: the
// production package still uses the bounded local-DM bridge and never enters
// this native callback path.
void InitializeNativeServerConfigstrings() {
    if (!sv.emptyConfigString)
        sv.emptyConfigString = SL_GetString_("", 0, MT_TYPE_CONFIG_STRING);
    for (uint32_t index = 0; index < MAX_CONFIGSTRINGS; ++index) {
        if (!sv.configstrings[index])
            sv.configstrings[index] = SL_GetString_("", 0, MT_TYPE_CONFIG_STRING);
    }
    constexpr uint32_t kNorthYawConfigstring = 0x336u;
    if (std::strcmp(SL_ConvertToString(sv.configstrings[kNorthYawConfigstring]), "0") != 0) {
        // Keep the table's ownership rules aligned with SV_SetConfigstring:
        // replace the old reference before adopting the authored value.
        SL_RemoveRefToString(sv.configstrings[kNorthYawConfigstring]);
        sv.configstrings[kNorthYawConfigstring] =
            SL_GetString_("0", 0, MT_TYPE_CONFIG_STRING);
    }
    VitaScriptServicesTrace("native_configstrings", "empty_table=2442 northyaw=0");
}
#endif

int ResolveScriptHandle(const char* filename, const char* label) {
    return static_cast<int>(VitaScriptVm_NativeFunctionHandle(filename, label));
}

bool SetLevelStringField(const char* fieldName, const char* value) {
    TraceNativeStage("set_level_field_begin");
    if (!fieldName || !value || !scrVarPub.levelId)
        return false;
    const uint32_t field = Scr_GetVariableFieldIndex(
        scrVarPub.levelId, SL_GetCanonicalString(fieldName));
    TraceNativeStage("set_level_field_index");
    if (!field)
        return false;
    VariableValue scriptValue{};
    scriptValue.type = VAR_STRING;
    // SetVariableValue takes ownership of the string-list reference, just as
    // the VM's OP_SetLevelFieldVariableField path does.
    scriptValue.u.stringValue = SL_GetString(value, 0);
    TraceNativeStage("set_level_field_value");
    SetVariableValue(field, &scriptValue);
    TraceNativeStage("set_level_field_set");
    return true;
}

bool SetObjectStringField(uint32_t objectId, const char* fieldName,
                          const char* value) {
    if (!objectId || !fieldName || !value)
        return false;
    const uint32_t field = Scr_GetVariableFieldIndex(
        objectId, SL_GetCanonicalString(fieldName));
    if (!field)
        return false;
    VariableValue scriptValue{};
    scriptValue.type = VAR_STRING;
    scriptValue.u.stringValue = SL_GetString(value, 0);
    SetVariableValue(field, &scriptValue);
    return true;
}

// Keep the callback payload identical to g_scr_main_mp.cpp.  The desktop
// path passes WeaponDef::szInternalName and the canonical MOD/hit-location
// string IDs, not the short asset aliases used by the Vita weapon selector.
const char* WeaponInternalName(uint32_t weapon) {
    switch (weapon) {
    case 2:
        return "mp5_mp";
    case 3:
        return "usp_mp";
    case 1:
    default:
        return "m4_mp";
    }
}

uint16_t WeaponMeansOfDeath(uint32_t weapon) {
    return weapon == 3 ? scr_const.mod_pistol_bullet : scr_const.mod_rifle_bullet;
}

void ReleasePersistentObject() {
    if (!g_vitaPersId)
        return;
    ClearObject(g_vitaPersId);
    if (scrVarDebugPub)
        --scrVarDebugPub->extRefCount[g_vitaPersId];
    RemoveRefToEmptyObject(g_vitaPersId);
    g_vitaPersId = 0;
}

void ReleaseEntityStrings() {
    const uint32_t count = std::min<uint32_t>(
        std::max<uint32_t>(g_materializedEntityCount, 1u), ENTITYNUM_WORLD);
    for (uint32_t index = 0; index < count; ++index) {
        if (g_entities[index].r.inuse)
            Scr_FreeEntityConstStrings(&g_entities[index]);
    }
    if (g_entities[ENTITYNUM_WORLD].r.inuse)
        Scr_FreeEntityConstStrings(&g_entities[ENTITYNUM_WORLD]);
}

void AbandonEntityTable() {
    ReleaseEntityStrings();
    ReleasePersistentObject();
    std::memset(&g_vitaClient, 0, sizeof(g_vitaClient));
    std::memset(g_entities, 0, sizeof(g_entities));
    level = level_locals_t{};
    g_materializedEntityCount = 1;
}

void ClearEntityTable(const ShipmentMapEntities* mapEntities) {
    // Keep the desktop MP entity table as the source of truth.  The Vita
    // slice only marks the local player and the world entity in use; all
    // other slots remain deterministic zeroed storage.
    std::memset(g_entities, 0, sizeof(g_entities));
    std::memset(&g_vitaClient, 0, sizeof(g_vitaClient));
    g_gameTypeStarted = false;
    level = level_locals_t{};
    level.gentities = g_entities;
    level.clients = &g_vitaClient;
    level.maxclients = 1;
    level.num_entities = 1;
    level.numConnectedClients = 1;
    level.time = 0;
    level.previousTime = 0;
    level.frametime = 0;

    g_vitaClient.ps.clientNum = 0;
    g_vitaClient.ps.pm_type = PM_NORMAL;

    gentity_s& player = g_entities[0];
    player.s.number = 0;
    player.s.eType = ET_PLAYER;
    player.s.clientNum = 0;
    player.r.inuse = 1;
    player.r.linked = 1;
    player.client = &g_vitaClient;
    Scr_SetString(&player.classname, scr_const.player);
    player.health = 100;
    player.maxHealth = 100;

    gentity_s& world = g_entities[ENTITYNUM_WORLD];
    world.s.number = ENTITYNUM_WORLD;
    world.r.inuse = 1;
    world.r.linked = 1;
    Scr_SetString(&world.classname, scr_const.worldspawn);

    // Materialize the pointer-free map entity sidecar before executing the
    // native level-main/startup path.  This makes retail getEnt/getEntArray
    // calls (notably DM spawn logic) observe the exported Shipment entities.
    g_materializedEntityCount = 1;
    if (mapEntities && mapEntities->Loaded()) {
        const auto& records = mapEntities->Records();
        const uint32_t capacity = ENTITYNUM_WORLD - 1;
        const uint32_t recordCount = std::min<uint32_t>(
            static_cast<uint32_t>(records.size()), capacity);
        for (uint32_t recordIndex = 0; recordIndex < recordCount; ++recordIndex) {
            const ShipmentMapEntity& record = records[recordIndex];
            if (record.classname.empty())
                continue;
            const uint32_t entityIndex = g_materializedEntityCount++;
            gentity_s& entity = g_entities[entityIndex];
            entity.s.number = static_cast<int>(entityIndex);
            entity.s.eType = ET_GENERAL;
            entity.r.inuse = 1;
            entity.r.linked = 1;
            // Keep string-list ownership identical to the native
            // G_ParseEntityField/G_SetConstString path.  The helper drops any
            // prior field reference before adopting the newly interned id.
            G_SetConstString(&entity.classname,
                             const_cast<char*>(record.classname.c_str()));
            if (!record.target.empty())
                G_SetConstString(&entity.target,
                                 const_cast<char*>(record.target.c_str()));
            if (!record.targetname.empty())
                G_SetConstString(&entity.targetname,
                                 const_cast<char*>(record.targetname.c_str()));
            if (!record.model.empty())
                G_SetConstString(&entity.model,
                                 const_cast<char*>(record.model.c_str()));
            entity.spawnflags = record.spawnflags;
            entity.damage = record.damage;
            if (record.hasCount)
                entity.count = record.count;
            if (record.hasHealth) {
                entity.health = record.health;
                entity.maxHealth = record.health;
            }
            if (record.hasOrigin) {
                std::memcpy(entity.r.currentOrigin, record.origin,
                            sizeof(entity.r.currentOrigin));
                std::memcpy(entity.s.lerp.pos.trBase, record.origin,
                            sizeof(entity.s.lerp.pos.trBase));
                entity.s.lerp.pos.trType = TR_STATIONARY;
            }
            if (record.hasAngles) {
                std::memcpy(entity.r.currentAngles, record.angles,
                            sizeof(entity.r.currentAngles));
                std::memcpy(entity.s.lerp.apos.trBase, record.angles,
                            sizeof(entity.s.lerp.apos.trBase));
                entity.s.lerp.apos.trType = TR_STATIONARY;
            }
        }
        level.num_entities = static_cast<int32_t>(g_materializedEntityCount);
    }

    // ClientConnect receives a persistent script object from the server
    // before it dispatches player callbacks.  Retain the same object-backed
    // pers field for this local client; otherwise self.pers["team"] resolves
    // through a null scriptPersId and the retail DM spawn callback cannot
    // select a team spawn list.
    g_vitaPersId = AllocObject();
    if (scrVarDebugPub)
        ++scrVarDebugPub->extRefCount[g_vitaPersId];
    g_vitaClient.sess.scriptPersId = static_cast<uint16_t>(g_vitaPersId);
    g_vitaClient.sess.connected = CON_CONNECTED;
    g_vitaClient.sess.sessionState = SESS_STATE_PLAYING;
    g_vitaClient.sess.maxHealth = 100;
    g_vitaClient.sess.cs.clientIndex = 0;
    g_vitaClient.sess.cs.team = TEAM_ALLIES;
    g_vitaClient.sess.localClient = 1;
}

} // namespace

bool VitaScriptVm_InitializeEntityRuntime(const ShipmentMapEntities* mapEntities) {
    if (!VitaScriptVm_Loaded())
        return false;
    if (g_ready)
        return true;

    TraceNativeStage("begin");
    ClearEntityTable(mapEntities);
    TraceNativeStage("after_clear_entity_table");
    std::memset(&g_scr_data, 0, sizeof(g_scr_data));
    TraceNativeStage("after_clear_scr_data");

    // Mirror the handle table populated by GScr_LoadScripts/GScr_LoadGameType
    // on desktop MP.  The Vita event bridge still dispatches through the
    // resolved wrapper handles, but native game code can now observe the same
    // gametype/script data instead of an uninitialized global.
    // Scr_LoadScript returns the compiled script object.  The desktop
    // GScr_LoadScriptAndLabel path stores the separate function handle for
    // ``main``; use that same handle here before Scr_ExecThread is called.
    g_scr_data.gametype.main = ResolveScriptHandle(
        "maps/mp/gametypes/dm", "main");
    g_scr_data.levelscript = ResolveScriptHandle(
        "maps/mp/mp_shipment", "main");
    g_scr_data.gametypescript = g_scr_data.gametype.main;
    g_scr_data.gametype.startupgametype = ResolveScriptHandle(
        "maps/mp/gametypes/_callbacksetup", "CodeCallback_StartGameType");
    g_scr_data.gametype.playerconnect = ResolveScriptHandle(
        "maps/mp/gametypes/_callbacksetup", "CodeCallback_PlayerConnect");
    g_scr_data.gametype.playerdisconnect = ResolveScriptHandle(
        "maps/mp/gametypes/_callbacksetup", "CodeCallback_PlayerDisconnect");
    g_scr_data.gametype.playerdamage = ResolveScriptHandle(
        "maps/mp/gametypes/_callbacksetup", "CodeCallback_PlayerDamage");
    g_scr_data.gametype.playerkilled = ResolveScriptHandle(
        "maps/mp/gametypes/_callbacksetup", "CodeCallback_PlayerKilled");
    g_scr_data.gametype.playerlaststand = ResolveScriptHandle(
        "maps/mp/gametypes/_callbacksetup", "CodeCallback_PlayerLastStand");
    g_scr_data.delete_ = ResolveScriptHandle("codescripts/delete", "main");
    g_scr_data.initstructs = ResolveScriptHandle("codescripts/struct", "initstructs");
    g_scr_data.createstruct = ResolveScriptHandle("codescripts/struct", "createstruct");
    TraceNativeStage("after_resolve_handles");
    char handleDetail[256]{};
    std::snprintf(handleDetail, sizeof(handleDetail),
                  "level=%d main=%d startup=%d connect=%d disconnect=%d damage=%d "
                  "killed=%d laststand=%d delete=%d initstructs=%d createstructs=%d",
                  g_scr_data.levelscript, g_scr_data.gametype.main,
                  g_scr_data.gametype.startupgametype,
                  g_scr_data.gametype.playerconnect,
                  g_scr_data.gametype.playerdisconnect,
                  g_scr_data.gametype.playerdamage,
                  g_scr_data.gametype.playerkilled,
                  g_scr_data.gametype.playerlaststand,
                  g_scr_data.delete_, g_scr_data.initstructs, g_scr_data.createstruct);
    VitaScriptServicesTrace("entity_handles", handleDetail);
    if (!g_scr_data.levelscript || !g_scr_data.gametype.main
        || !g_scr_data.gametype.startupgametype
        || !g_scr_data.gametype.playerconnect
        || !g_scr_data.gametype.playerdisconnect
        || !g_scr_data.gametype.playerdamage
        || !g_scr_data.gametype.playerkilled
        || !g_scr_data.gametype.playerlaststand || !g_scr_data.delete_
        || !g_scr_data.initstructs || !g_scr_data.createstruct) {
        sceClibPrintf("[vita-script] native game script table incomplete\\n");
        AbandonEntityTable();
        std::memset(&g_scr_data, 0, sizeof(g_scr_data));
        return false;
    }

#if !defined(KISAK_VITA_EXPERIMENTAL_NATIVE_ENTITY_RUNTIME)
    // The cached table proves that the compiled code positions survive the
    // load transaction, but Vita3K currently exits inside the first native
    // dm/level callback (before a renderer frame) when this boundary is
    // enabled. Keep the production slice fail-closed until that unsupported
    // builtin/VM ABI path is isolated; the local DM bridge remains the
    // authoritative gameplay path in the meantime. A deliberate experimental
    // build can opt in with KISAK_VITA_EXPERIMENTAL_NATIVE_ENTITY_RUNTIME.
    VitaScriptServicesTrace("entity_runtime_deferred",
                            "native callback ABI pending qualification");
    AbandonEntityTable();
    std::memset(&g_scr_data, 0, sizeof(g_scr_data));
    return false;
#endif

    // These are the same class maps/fields installed by GScr_PostLoadScripts
    // in the desktop MP bootstrap.  They must exist before an entity object is
    // materialized for Scr_ExecEntThreadNum.
    for (uint32_t classNum = 0; classNum < CLASS_NUM_COUNT; ++classNum)
        Scr_SetClassMap(classNum);
    TraceNativeStage("after_class_maps");
    GScr_AddFieldsForEntity();
    TraceNativeStage("after_entity_fields");
    // Scr_AddEntityNum is a VM-parameter helper; use the lower-level entity
    // lookup here so materialization does not leave 242 stale arguments on
    // the VM stack before dm.main()/level-main executes.
    for (uint32_t entityIndex = 0; entityIndex < g_materializedEntityCount; ++entityIndex)
        Scr_GetEntityId(entityIndex, CLASS_NUM_ENTITY);
    Scr_GetEntityId(ENTITYNUM_WORLD, CLASS_NUM_ENTITY);
#if defined(KISAK_VITA_EXPERIMENTAL_NATIVE_ENTITY_RUNTIME)
    const uint32_t playerEntityId = FindEntityId(0, CLASS_NUM_ENTITY);
    const uint32_t worldEntityId = FindEntityId(ENTITYNUM_WORLD, CLASS_NUM_ENTITY);
    sceClibPrintf("[vita-script] entity-ids level=%u pers=%u player=%u player_type=%u world=%u world_type=%u\\n",
                  static_cast<unsigned>(scrVarPub.levelId),
                  static_cast<unsigned>(g_vitaPersId),
                  static_cast<unsigned>(playerEntityId),
                  static_cast<unsigned>(GetObjectType(playerEntityId)),
                  static_cast<unsigned>(worldEntityId),
                  static_cast<unsigned>(GetObjectType(worldEntityId)));
#endif
    TraceNativeStage("after_entity_ids");
    if (!SetObjectStringField(g_vitaPersId, "team", "allies")) {
        sceClibPrintf("[vita-script] could not seed player pers.team\n");
        AbandonEntityTable();
        std::memset(&g_scr_data, 0, sizeof(g_scr_data));
        return false;
    }

    // Match the native G_InitGame ordering.  The game object must exist
    // before dm.main() evaluates game[...], and level.gameType is the
    // argument consumed by the retail dvar-registration helpers.  Executing
    // dm.main() once installs _callbacksetup's dynamic damage/killed hooks.
    // When the packaged map sidecar is present, follow desktop G_InitGame and
    // execute level-main followed by Scr_StartupGameType as well.  A null
    // sidecar is the renderer-free probe path and remains player/world-only.
    // G_InitGame keeps this flag asserted across the entire script startup
    // transaction.  The retail precache builtins (precacheShader,
    // precacheModel, loadFx, and friends) use it to distinguish the initial
    // level load from post-startup callbacks; omitting it makes the authored
    // dm/level scripts fail immediately even though the VM/compiler state is
    // otherwise valid.
    level.initializing = 1;
    level.startTime = level.time;
    Scr_SetLoading(1);
    TraceNativeStage("before_game_variable");
    Scr_AllocGameVariable();
    TraceNativeStage("after_game_variable");
#if defined(KISAK_VITA_EXPERIMENTAL_NATIVE_ENTITY_RUNTIME)
    InitializeNativeServerConfigstrings();
    TraceNativeStage("after_native_configstrings");
    // Desktop G_InitGame calls G_LoadStructs immediately after allocating the
    // game object and before loading/starting the gametype.  The Vita native
    // probe does not yet link the full spawn parser, but the first half of
    // G_LoadStructs is the important script-side contract: executing
    // codescripts/struct::initstructs seeds the struct constructors used by
    // dm.main and the map script.  Keep this in the experimental path until
    // the complete spawn-var parser is brought over.
    TraceNativeStage("before_initstructs");
    const uint16_t initStructThread = Scr_ExecThread(
        static_cast<uint32_t>(g_scr_data.initstructs), 0);
    TraceNativeStage("after_initstructs");
    if (!initStructThread) {
        sceClibPrintf("[vita-script] codescripts/struct::initstructs returned no thread\n");
        Scr_SetLoading(0);
        AbandonEntityTable();
        std::memset(&g_scr_data, 0, sizeof(g_scr_data));
        return false;
    }
    Scr_FreeThread(initStructThread);
#endif
    if (!SetLevelStringField("gameType", "dm")) {
        sceClibPrintf("[vita-script] could not seed level.gameType\n");
        Scr_SetLoading(0);
        AbandonEntityTable();
        std::memset(&g_scr_data, 0, sizeof(g_scr_data));
        return false;
    }
    TraceNativeStage("before_dm_main");
    const uint16_t gameTypeThread = Scr_ExecThread(
        static_cast<uint32_t>(g_scr_data.gametype.main), 0);
    TraceNativeStage("after_dm_main");
    if (!gameTypeThread) {
        sceClibPrintf("[vita-script] dm.main returned no thread\n");
        Scr_SetLoading(0);
        AbandonEntityTable();
        std::memset(&g_scr_data, 0, sizeof(g_scr_data));
        return false;
    }
    TraceNativeStage("before_free_dm_main");
#if defined(KISAK_VITA_EXPERIMENTAL_NATIVE_ENTITY_RUNTIME)
    // Vita3K currently faults while dropping the completed dm.main external
    // reference (the VM has already marked the root thread dead after its
    // child-thread startup work).  Keep the probe reference until the whole
    // native startup transaction has completed; production remains fail-closed
    // and the desktop path still frees this thread immediately.
    TraceNativeStage("defer_free_dm_main");
#else
    Scr_FreeThread(gameTypeThread);
#endif
    TraceNativeStage("after_free_dm_main");
    if (mapEntities && mapEntities->Loaded() && g_materializedEntityCount > 1) {
        TraceNativeStage("before_level_main");
        const uint16_t levelThread = Scr_ExecThread(
            static_cast<uint32_t>(g_scr_data.levelscript), 0);
        TraceNativeStage("after_level_main");
        if (!levelThread) {
            sceClibPrintf("[vita-script] mp_shipment.main returned no thread\\n");
            Scr_SetLoading(0);
            AbandonEntityTable();
            std::memset(&g_scr_data, 0, sizeof(g_scr_data));
            return false;
        }
        Scr_FreeThread(levelThread);
        TraceNativeStage("before_startup_gametype");
        const uint16_t startupThread = Scr_ExecThread(
            static_cast<uint32_t>(g_scr_data.gametype.startupgametype), 0);
        TraceNativeStage("after_startup_gametype");
        if (!startupThread) {
            sceClibPrintf("[vita-script] Scr_StartupGameType returned no thread\\n");
            Scr_SetLoading(0);
            AbandonEntityTable();
            std::memset(&g_scr_data, 0, sizeof(g_scr_data));
            return false;
        }
        Scr_FreeThread(startupThread);
        // Match the desktop ClientConnect/ClientBegin boundary after the
        // level has installed its callbacks.  The callback initializes the
        // persistent score/stat fields consumed by later damage/killed hooks;
        // begin is then delivered through the normal entity notification.
        g_vitaClient.sess.connected = CON_CONNECTING;
        const uint16_t connectThread = Scr_ExecEntThreadNum(
            0, CLASS_NUM_ENTITY,
            static_cast<int>(g_scr_data.gametype.playerconnect), 0);
        if (!connectThread) {
            sceClibPrintf("[vita-script] CodeCallback_PlayerConnect returned no thread\\n");
            Scr_SetLoading(0);
            AbandonEntityTable();
            std::memset(&g_scr_data, 0, sizeof(g_scr_data));
            return false;
        }
        Scr_FreeThread(connectThread);
        g_vitaClient.sess.connected = CON_CONNECTED;
        Scr_Notify(&g_entities[0], scr_const.begin, 0);
        g_gameTypeStarted = true;
    }
    Scr_SetLoading(0);
    level.initializing = 0;

    g_lastServerTime = -1;
    g_lastAdvanceServerTime = -1;
    g_ready = true;
    sceClibPrintf("[vita-script] entity runtime ready entities=%u world=%u startup=%u\n",
                  static_cast<unsigned>(g_materializedEntityCount),
                  static_cast<unsigned>(ENTITYNUM_WORLD),
                  static_cast<unsigned>(g_gameTypeStarted));
    return true;
}

void VitaScriptVm_ShutdownEntityRuntime() {
    if (!g_ready)
        return;
    // The script system owns the entity-variable references.  Remove the
    // native table contents before Scr_ShutdownSystem tears those references
    // down, then let the next load rebuild the class maps from scratch.
    g_ready = false;
    g_gameTypeStarted = false;
    g_lastServerTime = -1;
    g_lastAdvanceServerTime = -1;
    ReleasePersistentObject();
    ReleaseEntityStrings();
    std::memset(&g_vitaClient, 0, sizeof(g_vitaClient));
    std::memset(g_entities, 0, sizeof(g_entities));
    std::memset(&g_scr_data, 0, sizeof(g_scr_data));
    level = level_locals_t{};
    g_materializedEntityCount = 1;
}

bool VitaScriptVm_EntityRuntimeReady() { return g_ready; }

void VitaScriptVm_SyncEntityState(const float origin[3], const float angles[3],
                                  int32_t health, bool alive, uint32_t weapon,
                                  int32_t serverTime) {
    if (!g_ready || !origin || !angles)
        return;
    g_vitaClient.ps.commandTime = serverTime;
    std::memcpy(g_vitaClient.ps.origin, origin, sizeof(g_vitaClient.ps.origin));
    std::memcpy(g_vitaClient.ps.viewangles, angles,
                sizeof(g_vitaClient.ps.viewangles));
    g_vitaClient.ps.weapon = weapon;
    g_vitaClient.ps.stats[0] = std::max(0, health);
    g_vitaClient.ps.stats[2] = std::max(0, health);
    g_entities[0].health = std::max(0, health);
    g_entities[0].s.lerp.pos.trTime = serverTime;
    std::memcpy(g_entities[0].s.lerp.pos.trBase, origin,
                sizeof(g_entities[0].s.lerp.pos.trBase));
    std::memcpy(g_entities[0].r.currentOrigin, origin,
                sizeof(g_entities[0].r.currentOrigin));
    std::memcpy(g_entities[0].r.currentAngles, angles,
                sizeof(g_entities[0].r.currentAngles));
    g_entities[0].s.weapon = static_cast<int>(weapon);
    if (alive)
        g_entities[0].s.lerp.eFlags &= ~EF_DEAD;
    else
        g_entities[0].s.lerp.eFlags |= EF_DEAD;
}

bool VitaScriptVm_AdvanceTime(int32_t serverTime) {
    if (!g_ready || serverTime < g_lastAdvanceServerTime)
        return false;
    if (serverTime == g_lastAdvanceServerTime)
        return true;

    level.previousTime = level.time;
    level.time = serverTime;
    level.frametime = std::max(0, serverTime - level.previousTime);
    // This is the same native VM clock pump used by G_RunFrame.  It resumes
    // wait/scheduled threads and frees completed entity references before the
    // next callback dispatch; it must run at most once per server tick.
    Scr_IncTime();
    g_lastAdvanceServerTime = serverTime;
    return true;
}

bool VitaScriptVm_DispatchCallback(uint8_t eventType, uint32_t clientNum,
                                   int32_t serverTime, uint32_t functionHandle,
                                   uint32_t attackerClientNum, int32_t damage,
                                   uint32_t weapon, const float point[3],
                                   const float direction[3]) {
    if (!g_ready || clientNum != 0 || functionHandle == 0)
        return false;
    if (functionHandle != VitaScriptVm_CallbackHandle(eventType))
        return false;
    if (serverTime < g_lastServerTime)
        return false;
    g_lastServerTime = serverTime;
    level.previousTime = level.time;
    level.time = serverTime;

    // Build the same argument order as Scr_PlayerDamage/Scr_PlayerKilled in
    // g_scr_main_mp.cpp.  The Vita server supplies a bounded point/direction,
    // damage amount, weapon slot, and attacker entity; absent optional data
    // resolves to the deterministic world/zero defaults below.
    if (eventType == 4 || eventType == 5) {
        const gentity_s* attacker = attackerClientNum == 0
            ? &g_entities[0] : &g_entities[ENTITYNUM_WORLD];
        const gentity_s* inflictor = attacker;
        const float fallbackPoint[3] = {
            g_entities[clientNum].r.currentOrigin[0],
            g_entities[clientNum].r.currentOrigin[1],
            g_entities[clientNum].r.currentOrigin[2],
        };
        const float fallbackDirection[3] = {0.0f, 0.0f, 1.0f};
        const float* callbackPoint = point ? point : fallbackPoint;
        const float* callbackDirection = direction ? direction : fallbackDirection;
        const char* weaponName = WeaponInternalName(weapon);
        if (eventType == 4) {
            Scr_AddInt(0); // timeOffset
            Scr_AddConstString(scr_const.none); // hit location
            GScr_AddVector(callbackDirection);
            GScr_AddVector(callbackPoint);
            Scr_AddString(weaponName);
            Scr_AddConstString(WeaponMeansOfDeath(weapon));
            Scr_AddInt(0); // dflags
            Scr_AddInt(std::max(0, damage));
            GScr_AddEntity(const_cast<gentity_s*>(attacker));
            GScr_AddEntity(const_cast<gentity_s*>(inflictor));
        } else {
            Scr_AddInt(0); // deathAnimDuration
            Scr_AddInt(0); // psTimeOffset
            Scr_AddConstString(scr_const.none); // hit location
            GScr_AddVector(callbackDirection);
            Scr_AddString(weaponName);
            Scr_AddConstString(WeaponMeansOfDeath(weapon));
            Scr_AddInt(std::max(0, damage));
            GScr_AddEntity(const_cast<gentity_s*>(attacker));
            GScr_AddEntity(const_cast<gentity_s*>(inflictor));
        }
    } else if (eventType != 0 && eventType != 3) {
        return false;
    }
    const uint16_t thread = Scr_ExecEntThreadNum(
        clientNum, CLASS_NUM_ENTITY, static_cast<int>(functionHandle),
        eventType == 4 ? 10u : (eventType == 5 ? 9u : 0u));
    if (thread == 0)
        return false;
    Scr_FreeThread(thread);
    return true;
}

} // namespace kisak::vita
