#pragma once

#include <cstdint>

namespace kisak::vita {

class VitaRawFileBundle;
class ShipmentMapEntities;

// Mounts the complete package-owned rawfile closure and runs the real
// KisakCOD compiler/VM load lifecycle for the Shipment deathmatch script.
// The caller owns the bundle and must keep it alive until Shutdown.
bool VitaScriptVm_LoadShipment(const VitaRawFileBundle& rawfiles);
void VitaScriptVm_Shutdown();
bool VitaScriptVm_Loaded();
uint32_t VitaScriptVm_Handle();
uint32_t VitaScriptVm_CallbackHandle(uint8_t eventType);
bool VitaScriptVm_HasCallback(uint8_t eventType);
// Function handles are looked up while the script-load object is alive and
// cached before Scr_EndLoadScripts clears that compiler-side table. Native
// entity bootstrap code uses this accessor after the VM is ready.
uint32_t VitaScriptVm_NativeFunctionHandle(const char* filename,
                                           const char* label);


// Installs the one-client MP entity/class-field boundary used by the Vita
// local server.  This is deliberately separate from script compilation: the
// native VM must not receive an entity callback until g_entities, level, the
// class maps, and the player client storage all exist.
// With mapEntities supplied, materialize the exported Shipment entity sidecar
// and execute the native level-main/startup path.  A null sidecar intentionally
// retains the renderer-free player/world-only probe path.
bool VitaScriptVm_InitializeEntityRuntime(
    const ShipmentMapEntities* mapEntities = nullptr);
void VitaScriptVm_ShutdownEntityRuntime();
bool VitaScriptVm_EntityRuntimeReady();
void VitaScriptVm_SyncEntityState(const float origin[3], const float angles[3],
                                  int32_t health, bool alive, uint32_t weapon,
                                  int32_t serverTime);
// Advances the native script clock once for a local-server frame.  Repeated
// calls for the same server time are idempotent so render/input retries cannot
// run wait/resume threads twice.
bool VitaScriptVm_AdvanceTime(int32_t serverTime);

// Dispatches a callback resolved during VitaScriptVm_LoadShipment.  Spawn and
// respawn use zero parameters; damage/killed receive the bounded retail
// argument order assembled from the authoritative Shipment event.
bool VitaScriptVm_DispatchCallback(uint8_t eventType, uint32_t clientNum,
                                   int32_t serverTime, uint32_t functionHandle,
                                   uint32_t attackerClientNum = UINT32_MAX,
                                   int32_t damage = 0, uint32_t weapon = 0,
                                   const float point[3] = nullptr,
                                   const float direction[3] = nullptr);

} // namespace kisak::vita
