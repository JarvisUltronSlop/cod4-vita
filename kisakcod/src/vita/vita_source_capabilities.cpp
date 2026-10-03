#include <client_mp/client_mp.h>
#include <qcommon/com_playerprofile.h>
#include <devgui/devgui.h>
#include <gfx_d3d/r_utils.h>
#include <win32/win_input.h>
#include <win32/win_local.h>
#include <win32/win_storage.h>
#include <vita/vita_source_platform.h>

// Platform services explicitly excluded from the offline slice. Keep these
// separate from gameplay and renderer adapters so ownership audits can name
// every unavailable capability. No remote player, microphone, persistent
// progression, desktop profile, browser, or desktop DevGui exists here.
namespace {
void Unavailable(unsigned bit, const char* name) {
    static uint32_t reported = 0;
    if (!(reported & (1u << bit))) {
        reported |= 1u << bit;
        Com_PrintWarning(16, "Vita capability unavailable: %s\n", name);
    }
}
}

const dvar_t* com_playerProfile = nullptr;
void kisak::vita::VitaSourcePlatform_RegisterCapabilities() {
    com_playerProfile = Dvar_RegisterString("com_playerProfile", "Vita", DVAR_ROM,
                                           "Offline local player (no persistent profiles)");
}
bool CL_IsPlayerTalking(int, int) { return false; }
bool CL_IsPlayerMuted(int, uint32_t) { return false; }
void CL_MutePlayer(int, uint32_t) { Unavailable(0, "voice chat"); }
void CL_ResetPlayerMuting(uint32_t) {} // no remote mute records exist
double Voice_GetVoiceLevel() { return 0.0; }
int CL_ServerStatus(char*, char* output, int capacity) {
    if (output && capacity > 0) output[0] = '\0';
    return 0; // no LAN/online discovery results
}
char Com_NewPlayerProfile(const char*) { Unavailable(1, "profile creation"); return 0; }
char Com_DeletePlayerProfile(const char*) { Unavailable(2, "profile deletion"); return 0; }
void Com_ChangePlayerProfile(int, char*) { Unavailable(3, "profile selection"); }
void Com_SetRecommended(int, int) { Unavailable(4, "desktop hardware autodetection"); }
int LiveStorage_GetStat(int, int) { return 0; } // source's not-fetched response
void LiveStorage_SetStat(int, int, uint32_t) { Unavailable(5, "persistent progression"); }
void LiveStorage_ValidateCaCStat(int, int, int) {
    Com_Error(ERR_DROP, "Create-a-Class is unavailable in the Vita vertical slice");
}
bool DevGui_IsActive() { return false; }
void DevGui_Toggle() { Unavailable(6, "desktop DevGui"); }
void ProfLoad_DrawOverlay(rectDef_s*) { Unavailable(7, "desktop load-profiler overlay"); }
void IN_SetCursorPos(tagPOINT) {} // controller focus has no operating-system cursor
void Sys_OpenURL(const char*, int) { Unavailable(8, "online browser launch"); }

int R_PickMaterial(int, const float*, const float*, char* materialName,
                   char* techniqueName, char* surfaceName, uint32_t capacity) {
    if (capacity) {
        if (materialName) materialName[0] = '\0';
        if (techniqueName) techniqueName[0] = '\0';
        if (surfaceName) surfaceName[0] = '\0';
    }
    Unavailable(9, "desktop material picker");
    return 0;
}
