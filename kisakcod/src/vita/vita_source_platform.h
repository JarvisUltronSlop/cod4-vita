#pragma once

#include <cstddef>
#include <cstdint>

// Narrow host-facing services used by the recovered KisakCOD MP runtime.
// Source engine code continues to call its original DB/R/SND entry points;
// their Vita implementations below this seam translate typed, pointer-free
// KVAP records into arena-owned source structures and renderer/audio handles.
// No desktop pointer is accepted by this contract.

namespace kisak::vita {

enum class VitaSourceSoundAction : uint8_t {
    Play = 0,
    PlayLoop,
    StopAmbient,
    StopAll,
};

struct VitaSourcePlatformHooks {
    // Return a source-facing object constructed in the current map/front-end
    // arena. `assetType` uses XAssetType's numeric value; nullptr means the
    // validated pack does not contain the requested asset.
    void* (*findAsset)(uint32_t assetType, const char* name,
                       void* context) = nullptr;

    // Submit a source alias to the fixed Vita voice pool.  The callback
    // resolves the alias to already-decoded pack-owned PCM and returns a
    // positive playback id when accepted.
    int (*sound)(VitaSourceSoundAction action, const char* aliasName,
                 const float* origin, int localClientNum,
                 void* context) = nullptr;

    // Loading is driven by the outer responsive state machine.  These pulses
    // never draw or swap; they only publish the current source stage.
    void (*loadingPulse)(const char* stage, void* context) = nullptr;
    float (*loadingFraction)(void* context) = nullptr;

    // Source fog configstrings remain authoritative.  The native renderer
    // receives their decoded state through this callback.
    void (*fogChanged)(bool enabled, float start, float density,
                       uint8_t red, uint8_t green, uint8_t blue,
                       int startTime, int transitionTime,
                       void* context) = nullptr;

    // Bounded warning sink for renderer/effect resource limits.
    void (*warning)(uint32_t warningType, void* context) = nullptr;

    const char* referencedFastfileNames = "";
    const char* referencedFastfileChecksums = "";
    uint32_t worldChecksum = 0;
    void* context = nullptr;
};

void VitaSourcePlatform_Bind(const VitaSourcePlatformHooks& hooks);
void VitaSourcePlatform_Clear();
bool VitaSourcePlatform_IsBound();

void* VitaSourcePlatform_FindAsset(uint32_t assetType, const char* name);
int VitaSourcePlatform_Sound(VitaSourceSoundAction action,
                             const char* aliasName, const float* origin,
                             int localClientNum = 0);
void VitaSourcePlatform_LoadingPulse(const char* stage);
void VitaSourcePlatform_SetFog(bool enabled, float start, float density,
                               uint8_t red, uint8_t green, uint8_t blue,
                               int startTime, int transitionTime);
// Toggle/transition the current server fog without discarding the parameters
// most recently supplied through R_SetFogFromServer.
void VitaSourcePlatform_SwitchFog(bool enabled, int startTime,
                                  int transitionTime);
uint32_t VitaSourcePlatform_WorldChecksum();
const char* VitaSourcePlatform_FastfileNames();
const char* VitaSourcePlatform_FastfileChecksums();
// Resets bounded platform bookkeeping between fully torn-down source map
// sessions.  Arena/resource destruction remains the caller's responsibility.
void VitaSourcePlatform_ResetSessionState();
void VitaSourcePlatform_RegisterDvars();
void VitaSourcePlatform_RegisterCapabilities();

} // namespace kisak::vita
