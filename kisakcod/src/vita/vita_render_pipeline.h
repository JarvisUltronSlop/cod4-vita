#pragma once
#include "vita_render_pass_probe.h"

namespace kisak::vita {
// Rendering choices are separate from diagnostic phase/timing/input controls.
// Ordinary gameplay uses the same established baseline as the world GPU test.
struct VitaRenderPipeline {
    bool ordinary=false,weaponPipeline=false,weaponVertex=false;
    bool weaponSpecular=false,worldAlpha=false,worldAlphaActive=false;
    bool prepareWeapon=false,packWeapon=false;
    static VitaRenderPipeline Select(bool gameplay,bool cpuDiagnostic,uint32_t tag) {
        if(!gameplay)tag=0;
        VitaRenderPassProbe probe;probe.Begin(tag,0);
        const bool normal=gameplay && !cpuDiagnostic && tag==0;
        return {normal,normal || probe.weaponPipeline,normal || probe.weaponVertex,
            normal || probe.weaponSpecular,normal || probe.worldAlpha,normal || probe.WorldAlphaActive(),
            normal || VitaWeaponPipelineEnabled(tag),normal || VitaWeaponVertexEnabled(tag)};
    }
};
}
