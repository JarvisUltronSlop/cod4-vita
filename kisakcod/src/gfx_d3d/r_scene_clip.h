#pragma once

// Shared source contract from R_SetViewParmsForScene/R_GetDefaultNearClip.
// Cgame deliberately emits zero to request r_znear, not a zero-depth frustum.
constexpr float R_DEFAULT_NEAR_CLIP = 4.0f;
inline float R_ResolveNearClip(float requested, float registeredDefault) {
    if (requested <= 0.0f)
        return registeredDefault > 0.01f ? registeredDefault : 0.01f;
    return requested;
}
