#pragma once
#include <cstdint>
struct GfxBackendApi;
struct GfxTextureHandle;
namespace kisak::vita {
class VitaMaterialBundle;
class VitaImageBundle;
struct VitaRenderPacket;
struct VitaSourceUiBackendReport {
    uint32_t batches = 0;
    uint32_t commands = 0, quads = 0, missingMaterials = 0, unsupportedCommands = 0;
};
// GPU leases belong to one map and are released before its material bundle.
// Stage is a loading-only operation; drawing performs no file I/O or uploads.
bool VitaSourceUiBackend_Stage(const GfxBackendApi*, const VitaMaterialBundle&,
    const VitaImageBundle&, const VitaImageBundle* commonImages, const char* mapName,
    GfxTextureHandle mapCompassTexture,
    void (*progress)(uint32_t, uint32_t, void*) = nullptr, void* progressContext = nullptr);
void VitaSourceUiBackend_Reset(const GfxBackendApi*);
bool VitaSourceUiBackend_Ready();
VitaSourceUiBackendReport VitaSourceUiBackend_Draw(const GfxBackendApi*, const VitaRenderPacket&);
}
