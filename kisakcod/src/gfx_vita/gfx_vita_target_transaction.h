#pragma once
#include <cstdint>

inline bool GfxVitaValidSceneSize(uint32_t w,uint32_t h) {
    return (w==960 && h==544)||(w==720 && h==408)||(w==480 && h==272);
}
// A target generation becomes usable only after allocation AND deferred GPU
// setup succeed. Failure owns every partial resource until rollback completes.
template<class Destroy,class Allocate,class Warm>
bool GfxVitaTargetTransaction(uint32_t w,uint32_t h,Destroy destroy,Allocate allocate,Warm warm) {
    if(!GfxVitaValidSceneSize(w,h)) return false;
    destroy();
    if(!allocate() || !warm()) { destroy(); return false; }
    return true;
}

struct GfxVitaViewportSize { uint32_t width, height; };
// Every pass uses the currently selected target, independently of whether the
// controlled benchmark is active. A reduced world and its props share one
// viewport; native viewmodel composition and the final HUD use their own.
inline GfxVitaViewportSize GfxVitaPassViewport(bool sceneReady, bool gameplay,
    bool nativeViewmodel, bool nativeHud, bool clearControl,
    GfxVitaViewportSize world, GfxVitaViewportSize composition,
    GfxVitaViewportSize display) {
    if (nativeHud || clearControl || !sceneReady) return display;
    if (gameplay && !nativeViewmodel && world.width && world.height) return world;
    return composition;
}
