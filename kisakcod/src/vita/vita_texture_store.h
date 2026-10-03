#pragma once
#include <cstddef>
#include <cstdint>
namespace kisak::vita {
inline constexpr uint32_t kVitaTexturePageGpuBytes=16u*1024u*1024u;
// Renderer/loading owner submits one synchronous request to a core-2 worker.
// No source callbacks or graphics APIs run on the worker. Backing extents are
// recycled only after their final texture lease is released. All public calls
// belong to the renderer/loading owner; Stop requires that owner to be quiescent.
bool VitaTextureStore_Start(const char* path);
uint32_t VitaTextureStore_Write(const void* bytes,uint32_t size);
bool VitaTextureStore_Read(uint32_t token,void* bytes,uint32_t size);
void VitaTextureStore_Release(uint32_t token);
void VitaTextureStore_Stop();
uint64_t VitaTextureStore_LiveBytes();
}
