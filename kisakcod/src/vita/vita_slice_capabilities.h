#pragma once

namespace kisak::vita {

// Real source definitions required by the fixed loadout and the original
// map/DM scripts. The short-fuse grenade and car explosion are script-owned
// dependencies, not additional selectable weapons.
inline constexpr const char* kVitaSourceWeaponNames[] = {
    "none", "defaultweapon_mp", "mp5_mp", "usp_mp",
    "frag_grenade_mp", "flash_grenade_mp",
    "frag_grenade_short_mp", "destructible_car",
};

} // namespace kisak::vita
