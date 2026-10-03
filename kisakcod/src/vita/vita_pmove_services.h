#pragma once

#include <bgame/bg_local.h>
#include <qcommon/msg_mp.h>

namespace kisak::vita {
class ShipmentCollision;
}

// The Vita package carries a compact scalar weapon record rather than the
// desktop pointer-bearing WeaponDef.  Feed the values that Pmove actually
// consults through this narrow bridge before each authoritative frame.
struct VitaPmoveWeaponConfig {
    uint32_t weapon = 1;
    uint32_t fireType = 0;
    int32_t startAmmo = 90;
    int32_t maxAmmo = 999;
    int32_t clipSize = 30;
    int32_t damage = 40;
    int32_t playerDamage = 40;
    int32_t fireDelay = 100;
    int32_t fireTime = 100;
    int32_t rechamberTime = 0;
    int32_t reloadTime = 1400;
    int32_t reloadEmptyTime = 1800;
    float moveSpeedScale = 1.0f;
    float adsMoveSpeedScale = 1.0f;
    float sprintDurationScale = 1.0f;
    int32_t adsTransInTime = 220;
    int32_t adsTransOutTime = 220;
    int32_t freezeMovementWhenFiring = 0;
};

// The shared multiplayer movement routine is kept intact.  These two narrow
// entry points provide the Vita-owned collision context and a server-style
// pmove invocation without exposing the rest of the desktop game loop.
extern "C" void VitaPmove_SetCollision(
    const kisak::vita::ShipmentCollision* collision);

extern "C" void VitaPmove_SetWeaponConfig(
    const VitaPmoveWeaponConfig* config);

extern "C" bool VitaPmove_Run(playerState_s* state,
                               const usercmd_s* command,
                               const usercmd_s* oldCommand);
