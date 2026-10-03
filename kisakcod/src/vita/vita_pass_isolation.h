#pragma once
#include "vita_render_pass_probe.h"
#include <array>
namespace kisak::vita {
// CPU/source preparation remains intact; count actual draw submission decisions.
// Clears and fullscreen composition must continue to initialize every target.
struct VitaPassIsolation {
    uint32_t phase=0,errors=0;
    std::array<uint32_t,7> prepared{},suppressed{};
    void Begin(uint32_t requested) {*this={};if(VitaPassIsolationMode(requested))phase=requested&0xffu;}
    bool Draw(unsigned pass,bool clear) {
        if(!phase)return true;
        if(pass>=prepared.size()){++errors;return true;}
        ++prepared[pass];
        const bool remove=!clear && ((phase==2 && pass==unsigned(VitaRenderPass::World))
            || (phase==4 && pass==unsigned(VitaRenderPass::PropsEffects))
            || (phase==6 && pass==unsigned(VitaRenderPass::Weapon)));
        suppressed[pass]+=remove;return !remove;
    }
    bool CopyPost() const {return phase==8;}
};
}
