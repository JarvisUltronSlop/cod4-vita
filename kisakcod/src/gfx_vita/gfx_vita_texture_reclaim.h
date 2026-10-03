#pragma once
#include <cstdint>

// Renderer owner only, AFTER completion of every GPU draw/transfer referencing
// this image. Only pageable, non-FBO, non-paletted 2D images may enter here.
// Detach before deletion so VitaGL cannot also enqueue the storage for GC.
// Deleting the GL name still clears its bindings and releases the descriptor.
template<class Api>
bool GfxVitaReclaimCompletedTexture(uint32_t texture, Api& api) {
    if (!texture) return false;
    const auto previous=api.Bound();
    api.Bind(texture);
    void* pixels=api.Pixels();
    if (!pixels) { api.Bind(previous); return false; }
    api.Detach();
    api.Delete(texture);
    api.Free(pixels);
    api.Bind(previous==texture ? 0u : previous);
    return true;
}
