#pragma once

// A diagnostic scissor must never escape the prop group, including a rejected
// snapshot. Native calls are supplied by the renderer; no allocations or GL
// handles cross the source/render ownership boundary.
struct GfxVitaPropScissor {
    bool active=false, enabled=false;
    int box[4]{};
    template<class Read,class Set> bool Begin(Read&& read,Set&& set) {
        if(active)return false;
        read(enabled,box);active=true;
        const int tiny[4]={0,0,1,1};set(true,tiny);return true;
    }
    template<class Set> void Restore(Set&& set) {
        if(!active)return;
        set(enabled,box);active=false;
    }
};
