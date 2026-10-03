#pragma once
#include <cstdint>

// Gameplay preference cannot override an explicit diagnostic. Keep the shader
// source separate so changing menu/dispatch policy does not invalidate its cache.
constexpr bool GfxVitaUseSimpleWorld(bool gameplay, bool requested,
    bool diagnostic, uint32_t probe, bool scaling, uint32_t phase,
    uint32_t ready, bool world, bool eligible) {
    const bool selected = scaling ? phase == 4
        : gameplay && requested && !diagnostic && probe == 0;
    return selected && ready == 3 && world && eligible;
}
