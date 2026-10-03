#pragma once
#include <vita/vita_render_contract.h>

struct DObj_s;
struct cpose_t;
struct refdef_s;
struct GfxPackedVertex;
struct GfxParticleCloud;
struct GfxLight;
struct GfxCmdDrawText2D;

namespace kisak::vita {
enum class VitaSourceUiKind : uint8_t { Quad, Text, SaveScreen, ShockBlur, ShockFlash };
struct VitaSourceUiDraw {
    VitaSourceUiKind kind = VitaSourceUiKind::Quad;
    uint16_t resource = 0;
    float corners[4][2]{};
    float uv[4] = {0, 0, 1, 1};
    float texcoords[4][2]{{0,0},{1,0},{1,1},{0,1}};
    float color[4] = {1, 1, 1, 1};
    float glowColor[4]{};
    int effectTimes[4]{};
    uint32_t screenIndex = 0;
    // Original R_AddCmdDrawText/ConsoleText writes its complete source
    // command here, including flags and pulse/glow parameters. The private
    // adapter owns these frame-local references; the public packet uses IDs.
    alignas(16) uint8_t sourceTextCommand[480]{};
};
struct VitaSourceFxDraw {
    uint16_t material = 0;
    uint16_t argumentOffset = 0;
    uint16_t argumentCount = 0;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
};
struct VitaSourceFxView {
    const GfxPackedVertex* vertices = nullptr;
    const uint16_t* indices = nullptr;
    const float (*arguments)[4] = nullptr;
    const VitaSourceFxDraw* draws = nullptr;
    const GfxParticleCloud* clouds = nullptr;
    const uint16_t* cloudMaterials = nullptr;
    const GfxLight* lights = nullptr;
    uint32_t vertexCount = 0, indexCount = 0;
};
bool VitaSourceRender_Effects(uint32_t frameGeneration, VitaSourceFxView* output);

// Frame-local source references live inside this main-thread adapter. The
// public packet contains generation-checked handles, never owning pointers.
void VitaSourceRender_BeginMap(uint32_t generation);
const VitaRenderPacket& VitaSourceRender_Packet();
const refdef_s* VitaSourceRender_View();
const VitaSourceUiDraw* VitaSourceRender_Ui(uint32_t generation, uint32_t index);
const GfxCmdDrawText2D* VitaSourceRender_Text(uint32_t generation, uint32_t index);
const void* VitaSourceRender_Resource(uint32_t generation, uint16_t handle);
bool VitaSourceRender_DObj(uint32_t generation, uint16_t handle,
                          const DObj_s** object, const cpose_t** pose);
} // namespace kisak::vita
