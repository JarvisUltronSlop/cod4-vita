#pragma once
#include <cstdint>
#include <gfx/gfx_backend.h>
#if KISAK_VITA_CPU_RUNTIME
#include <vita/vita_viewmodel_packet.h>
#endif

namespace kisak::vita {
struct VitaModelRecord;
struct VitaSourceModelResult {
    uint64_t hiddenSurfaces = 0;
    float muzzle[2]{};
    bool muzzleValid = false;
#if KISAK_VITA_CPU_RUNTIME
    VitaViewModelProjection projection;
#endif
};

// Read the frame-local cgame DObj and run the original scalar XSurface
// skinning. Output positions retain camera Z for native homogeneous clipping.
bool VitaSourceModel_Skin(uint32_t frameGeneration, uint16_t handle,
    uint32_t modelIndex, const VitaModelRecord& mesh, GfxDrawVertex* vertices,
    uint32_t capacity, VitaSourceModelResult* result);
void VitaSourceModel_Reset();
#if KISAK_VITA_CPU_RUNTIME
bool VitaSourceModel_SkinPacked(uint32_t frameGeneration, uint16_t handle,
    uint32_t modelIndex, const VitaModelRecord& mesh, VitaPackedSkinVertex* vertices,
    uint32_t capacity, VitaSourceModelResult* result);
#endif
} // namespace kisak::vita
