#pragma once

// Eligibility is taken from the source material of this actual lit draw.
// Transparency and unknown source state always retain the original program.
constexpr bool GfxVitaWeaponOpaqueEligible(bool dynamic, bool viewmodel,
    bool world, bool instanced, bool sourceState, bool alphaTest, bool blend,
    bool color, bool baked, bool hud, bool sky, unsigned benchmark) {
    return dynamic && viewmodel && !world && !instanced && sourceState
        && !alphaTest && !blend && color && baked && !hud && !sky && benchmark==0;
}
