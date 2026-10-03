#!/usr/bin/env python3
"""Regression check for authored per-weapon movement/ADS timing."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SERVICES = ROOT / "src" / "vita" / "vita_pmove_services.cpp"
SERVICES_HEADER = ROOT / "src" / "vita" / "vita_pmove_services.h"
SERVER = ROOT / "src" / "vita" / "vita_shipment_dm.cpp"
SERVER_HEADER = ROOT / "src" / "vita" / "vita_shipment_dm.h"


def main() -> int:
    header = SERVICES_HEADER.read_text(encoding="utf-8")
    source = SERVICES.read_text(encoding="utf-8")
    server = SERVER.read_text(encoding="utf-8")
    server_header = SERVER_HEADER.read_text(encoding="utf-8")
    assert "float sprintDurationScale = 1.0f;" in header
    assert "int32_t adsTransInTime = 220;" in header
    assert "int32_t adsTransOutTime = 220;" in header
    assert "BG_GetMaxSprintTime" in source
    assert "g_weaponConfig.sprintDurationScale" in source
    assert "g_weaponConfig.adsTransInTime" in source
    assert "g_weaponConfig.adsTransOutTime" in source
    # The portable bootstrap does not run the desktop dvar registration
    # pass.  Keep every movement/animation dvar touched by the shared
    # KisakCOD Pmove path pointed at its authored COD4 default record.
    assert "void EnsureVitaMovementDvars()" in source
    assert "player_sprintTime = &g_floatSprintTime;" in source
    assert "jump_height = &g_floatJumpHeight;" in source
    assert "player_view_pitch_up = &g_floatPitchUp;" in source
    assert "player_view_pitch_down = &g_floatPitchDown;" in source
    assert "bg_bobAmplitudeSprinting = &g_vecBobAmplitudeSprinting;" in source
    assert "bg_bobAmplitudeProne = &g_vecBobAmplitudeProne;" in source
    assert "movementConfig.sprintDurationScale = movementWeapon->sprintDurationScale;" in server
    assert "movementConfig.adsTransInTime = movementWeapon->adsTransInTime;" in server
    assert "movementConfig.adsTransOutTime = movementWeapon->adsTransOutTime;" in server
    # The bootstrap must leave the source reload state machine authoritative;
    # a compact fallback is allowed only when the reduced probe target does
    # not enter one of the source reload states.
    assert "bool IsSourceReloadState(int32_t state)" in server
    assert "const bool sourceReload = IsSourceReloadState(sourceWeaponState);" in server
    assert "slot->state.reloadStartTime = serverTime;" in server
    assert "slot->state.reloadEndTime = serverTime + std::max(1, duration);" in server
    assert "sourceMelee || sourceOffhand || sourceSprint || sourceReload" in server
    # Frag throws use the real source offhand transition when available and
    # retain a bounded probe fallback instead of silently dropping the action.
    assert "const auto SpawnGrenade = [&](bool sourceConsumed)" in server
    assert "SpawnGrenade(true)" in server
    assert "SpawnGrenade(false)" in server
    assert "slot->nativeState.ammoclip[4] = slot->state.grenadeCount;" in server
    assert "uint32_t action = 0;" in server_header
    assert "int32_t grenadeCount = 2;" in server_header
    assert "float shotPoint[3]" in server
    assert "ResolveHitscan(*slot, weapon, serverTime, shotPoint, shotDirection)" in server
    assert "std::memcpy(fireEvent.point, shotPoint" in server
    assert "std::memcpy(fireEvent.direction, shotDirection" in server
    assert "shooter.state.weaponSlot, shotPoint, forward" in server
    assert "shooter.state.weaponSlot, start, forward" not in server
    assert "int32_t weaponClipAmmo[4] = {};" in server_header
    assert "int32_t weaponReserveAmmo[4] = {};" in server_header
    assert "weaponClipAmmo[previousWeapon] = slot->state.clipAmmo" in server
    assert "slot->state.clipAmmo = slot->weaponClipAmmo[slot->state.weaponSlot]" in server
    assert "slot->state.reserveAmmo = slot->weaponReserveAmmo[slot->state.weaponSlot]" in server
    assert "slot->nativeState.ammoclip[weaponSlot] = slot->weaponClipAmmo[weaponSlot]" in server
    assert "slot->nativeState.ammo[weaponSlot] = slot->weaponReserveAmmo[weaponSlot]" in server
    print("vita weapon timing tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
