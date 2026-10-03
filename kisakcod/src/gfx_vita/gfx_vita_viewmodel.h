#pragma once
#include <cstddef>
#include <cstdint>

// Immutable upload ABI. Positions are original source-skinned camera
// coordinates with the source FOV applied to X/Y; Z is retained for clipping.
struct GfxVitaViewmodelVertex {
    float position[3];
    float texCoord[2];
    float worldPosition[3];
    int8_t normal[4];
    int8_t tangent[4];
    uint8_t color[4];
};
static_assert(sizeof(GfxVitaViewmodelVertex) == 44);

#if KISAK_VITA_CLEAN_VIEWMODEL
// Independent bring-up pipeline. It uses source diffuse + source model-light
// vertex colors. Normal/specular qualification follows visible geometry;
// this diagnostic pipeline cannot enter the normal Release candidate.
bool GfxVitaViewmodel_Init();
void GfxVitaViewmodel_Shutdown();
bool GfxVitaViewmodel_Draw(uint32_t vertices, uint32_t indices,
    uint32_t firstVertex, uint32_t firstIndex, uint32_t indexCount,
    uint32_t nativeColorTexture);
#endif
