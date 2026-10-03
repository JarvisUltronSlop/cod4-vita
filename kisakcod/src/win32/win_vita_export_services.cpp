// GPL-3.0. Offline DB conversion has no Steam networking or ticket service.
// This translation unit is selected only for KISAK_VITA_ASSET_EXPORTER.
#include <universal/q_shared.h>
#include "win_steam.h"
#include "win_vita_export.h"
#include <qcommon/qcommon.h>

bool g_steamInitialized = false;

void Steam_Init()
{
    if (!Win_IsHeadlessVitaExport())
        Com_Error(ERR_FATAL, "This binary is an offline Vita asset exporter. Run prepare-owned-content.ps1.");
}
void Steam_Shutdown() {}
bool Steam_UpdateClientAuthTicket(netadr_t) { return false; }
bool Steam_GetRawClientTicket(unsigned char **buffer, uint32 *size)
{
    if (buffer) *buffer = nullptr;
    if (size) *size = 0;
    return false;
}
void Steam_CancelClientTicket() {}
uint64_t Steam_GetClientSteamID64() { return 0; }
bool Steam_CheckClientTicket(const void *, uint32, uint64_t) { return false; }
void Steam_CheckClients() {}
void Steam_OnClientDropped(uint64_t) {}
void Steam_SV_AddTestCommands() {}
