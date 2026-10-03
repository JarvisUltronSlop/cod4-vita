#pragma once
// The existing world-copy vertex shader emits clip Z=0. Collapse the viewport
// depth range to the far value so the same draw also initializes independent
// weapon depth. Restore the caller's range before any weapon is submitted.
// Callbacks keep the state transaction testable without a desktop GL driver.
template<class ReadRange,class SetRange,class SetDepth,class Draw>
void GfxVitaCompositionDepth(bool fused,ReadRange readRange,SetRange setRange,
                             SetDepth setDepth,Draw draw) {
    if (!fused) {draw();return;}
    float range[2];readRange(range);
    setDepth(true); // depth test ALWAYS, depth writes enabled
    setRange(range[1],range[1]);
    draw();
    setRange(range[0],range[1]);
    setDepth(false); // ordinary LEQUAL, writes enabled
}

#include <cmath>
struct GfxVitaCompositionState {
    float range[2]{};
    bool depth=false,write=false,always=false;
    unsigned colorMask=0;
    bool stencil=false,blend=false,alpha=false,scissor=false;
    unsigned Flags() const {return colorMask|(depth?16u:0u)|(write?32u:0u)|(always?64u:0u)
        |(stencil?128u:0u)|(blend?256u:0u)|(alpha?512u:0u)|(scissor?1024u:0u);}
};
// Failure bits keep individual missing evidence visible. The incoming range is
// normally [1/64,1], not [0,1]; compare restoration with that actual input.
inline unsigned GfxVitaCompositionCheck(bool fused,const float* before,
    const GfxVitaCompositionState& state,const float* after) {
    unsigned failures=0;
    if(!std::isfinite(before[0]) || !std::isfinite(before[1]) || before[0]<0
        || before[0]>before[1] || before[1]>1)failures|=1;
    if(state.range[0]!=(fused?before[1]:before[0]) || state.range[1]!=before[1])failures|=2;
    if(state.depth!=fused || state.write!=fused || (fused && !state.always))failures|=4;
    if(state.colorMask!=15)failures|=8;
    if(state.stencil || state.blend || state.alpha || state.scissor)failures|=16;
    if(after[0]!=before[0] || after[1]!=before[1])failures|=32;
    return failures;
}
