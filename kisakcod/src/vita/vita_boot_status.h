#pragma once
#include <cstdint>
namespace kisak::vita {
enum class VitaBootStage : uint32_t { Starting, Catalog, Frontend, Renderer, Ready, Failed };
bool VitaBootStatus_Start();
void VitaBootStatus_Stage(VitaBootStage stage);
void VitaBootStatus_Stop();
}
