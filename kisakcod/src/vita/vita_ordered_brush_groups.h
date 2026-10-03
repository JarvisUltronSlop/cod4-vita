#pragma once
#include <vita/vita_trace_brush_filter.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>

namespace kisak::vita {
// Source-owned immutable broadphase for the world leaf only. Groups retain
// original consecutive membership order. No sorting, fraction clipping or
// replacement collision math. A rejected union proves each member rejects.
template<size_t Capacity=2048, bool Refine=true> class OrderedBrushGroups {
    struct Group {float mins[3]{},maxs[3]{};bool valid=false;};
    std::array<Group,Capacity> groups_{};
    const void* brushes_=nullptr;
    const uint16_t* members_=nullptr;
    size_t count_=0;
    size_t childOffset_=0;
public:
    static constexpr size_t Width=16;
    static constexpr size_t ChildWidth=4;
    size_t ProbeWidth() const {return childOffset_ ? ChildWidth : Width;}
    size_t ProbeWidth(const void* brushes,const uint16_t* members,size_t count) const {
        return brushes==brushes_ && members==members_ && count==count_ ? ProbeWidth() : Width;
    }
    void Reset(){brushes_=nullptr;members_=nullptr;count_=0;childOffset_=0;}
    template<class Brush> bool Build(const Brush* brushes,const uint16_t* members,size_t count) {
        Reset();
        if(!brushes || !members || !count || count>Capacity*Width)return false;
        const auto buildGroup=[&](Group& group,size_t first,size_t width) {
            group.valid=true;
            const auto& initial=brushes[members[first]];
            for(size_t a=0;a<3;++a){group.mins[a]=initial.mins[a];group.maxs[a]=initial.maxs[a];}
            for(size_t i=first;i<std::min(first+width,count);++i) {
                const auto& brush=brushes[members[i]];
                for(size_t a=0;a<3;++a) {
                    if(!std::isfinite(brush.mins[a]) || !std::isfinite(brush.maxs[a]) || brush.mins[a]>brush.maxs[a])group.valid=false;
                    group.mins[a]=std::min(group.mins[a],brush.mins[a]);
                    group.maxs[a]=std::max(group.maxs[a],brush.maxs[a]);
                }
            }
        };
        const size_t parents=(count+Width-1)/Width;
        for(size_t first=0;first<count;first+=Width)
            buildGroup(groups_[first/Width],first,Width);
        // Use spare entries in the already-reserved world bounds array. Large
        // leaves that cannot fit both levels retain the original 16-wide path.
        // Every child is a consecutive subset; no brush or equal-hit ordering
        // changes. Invalid bounds disable rejection for the affected group.
        const size_t children=(count+ChildWidth-1)/ChildWidth;
        if(Refine && children<=Capacity-parents) {
            childOffset_=parents;
            for(size_t first=0;first<count;first+=ChildWidth)
                buildGroup(groups_[childOffset_+first/ChildWidth],first,ChildWidth);
        }
        brushes_=brushes;members_=members;count_=count;return true;
    }
    size_t Skip(const void* brushes,const uint16_t* members,size_t count,size_t first,
                const TraceBrushFilter& filter,const float* radius) const {
        if(brushes!=brushes_ || members!=members_ || count!=count_ || first>=count_)return 0;
        if(first%Width==0) {
            const auto& group=groups_[first/Width];
            if(group.valid && filter.Reject(group.mins,group.maxs,radius))return std::min(Width,count_-first);
        }
        if(childOffset_ && first%ChildWidth==0) {
            const auto& child=groups_[childOffset_+first/ChildWidth];
            if(child.valid && filter.Reject(child.mins,child.maxs,radius))return std::min(ChildWidth,count_-first);
        }
        return 0;
    }
};
// Built only after successful registry publication; cleared before that
// registry frees source assets. Other leaves and temporary box hulls bypass it.
inline OrderedBrushGroups<> worldBrushGroups;
}
