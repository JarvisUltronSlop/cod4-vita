#pragma once

#include <cstddef>
#include <cstdint>

// Pmove only observes these three fields.  Keep their original 32-bit
// fastfile offsets while the full pointer-bearing WeaponDef is moved behind
// the Vita package converter.  This header is included only by the staged
// ARM gameplay probe; the desktop build continues to use xanim/xanim.h.
struct WeaponDef
{
    uint8_t _prefix[320];
    int32_t fireType;
    uint8_t _prefixAfterFireType[1048 - 324];
    float moveSpeedScale;
    float adsMoveSpeedScale;
    float sprintDurationScale;
    uint8_t _between[1464 - 1060];
    int32_t freezeMovementWhenFiring;
};

static_assert(offsetof(WeaponDef, moveSpeedScale) == 1048);
static_assert(offsetof(WeaponDef, adsMoveSpeedScale) == 1052);
static_assert(offsetof(WeaponDef, sprintDurationScale) == 1056);
static_assert(offsetof(WeaponDef, freezeMovementWhenFiring) == 1464);
static_assert(offsetof(WeaponDef, fireType) == 320);
