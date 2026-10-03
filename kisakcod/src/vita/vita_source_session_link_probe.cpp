#include <client_mp/client_mp.h>
#include <cgame_mp/cg_local_mp.h>
#include <server_mp/server_mp.h>
#include <vita/vita_source_session.h>

// Link-only worklist for the source-owned session lifecycle.  Frame closure
// is checked separately so failures here identify map/client initialization
// services instead of obscuring regressions in SV_Frame or CL_Frame.
int main() {
    volatile auto serverInit = &SV_Init;
    volatile auto serverSpawn = &SV_SpawnServer;
    volatile auto clientInit = &CL_Init;
    volatile auto cgameInit = &CL_InitCGame;
    volatile auto cgameFrame = &CG_DrawActiveFrame;
    volatile auto sessionInit = &kisak::vita::VitaSourceSession::Initialize;
    volatile auto sessionMountData =
        &kisak::vita::VitaSourceSession::MountSourceData;
    volatile auto sessionMountModels =
        &kisak::vita::VitaSourceSession::MountSourceModelAssets;
    volatile auto sessionMountAnimations =
        &kisak::vita::VitaSourceSession::MountSourceAnimations;
    volatile auto sessionMountEffects =
        &kisak::vita::VitaSourceSession::MountSourceEffects;
    volatile auto sessionMount =
        &kisak::vita::VitaSourceSession::MountSourceWeapons;
    volatile auto sessionMountMap =
        &kisak::vita::VitaSourceSession::MountMapRoots;
    volatile auto sessionStart = &kisak::vita::VitaSourceSession::StartMap;
    volatile auto sessionFrame = &kisak::vita::VitaSourceSession::Frame;
    volatile auto sessionStop = &kisak::vita::VitaSourceSession::Stop;
    return serverInit && serverSpawn && clientInit && cgameInit && cgameFrame
        && sessionInit && sessionMountData && sessionMountModels && sessionMountAnimations
        && sessionMountEffects
        && sessionMount && sessionMountMap
        && sessionStart && sessionFrame
        && sessionStop ? 0 : 2;
}
