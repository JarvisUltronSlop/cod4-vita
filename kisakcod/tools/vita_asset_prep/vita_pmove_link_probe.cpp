#include <bgame/bg_public.h>
#include <bgame/bg_local.h>
#include <vita/vita_pmove_services.h>

// Link-only experiment: prove how much of the real shared Pmove path can be
// reached from the Vita engine archive before wiring it into the session.
int main() {
    playerState_s player{};
    player.pm_type = PM_NORMAL;
    player.commandTime = 0;
    player.groundEntityNum = ENTITYNUM_NONE;
    player.viewHeightTarget = 60;
    player.viewHeightCurrent = 60.0f;
    player.speed = 190;
    player.gravity = 800;
    usercmd_s command{};
    command.serverTime = 16;
    command.weapon = 1;
    VitaPmove_Run(&player, &command, nullptr);
    return 0;
}
