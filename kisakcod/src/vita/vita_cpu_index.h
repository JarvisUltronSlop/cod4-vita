#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace kisak::vita {
// Index only: callers retain original append order and original numeric IDs.
// Empty cells are zero; capacity bounds both lookup work and memory.
template<uint32_t Capacity> class VitaCpuPointerIndex {
    static_assert(Capacity>1 && (Capacity&(Capacity-1))==0);
public:
    struct Cell {uintptr_t object=0,pose=0;uint16_t id=0;};
    Cell* Find(uintptr_t object,uintptr_t pose) {
        uint64_t hash=uint64_t(object)*0x9e3779b185ebca87ull ^ uint64_t(pose)*0xc2b2ae3d27d4eb4full;
        hash^=hash>>33;
        for(uint32_t probe=0;probe<Capacity;++probe) {
            auto& cell=cells_[(uint32_t(hash)+probe)&(Capacity-1)];
            if(!cell.id || (cell.object==object && cell.pose==pose))return &cell;
        }
        return nullptr;
    }
    void Reset(){cells_={};}
private:
    std::array<Cell,Capacity> cells_{};
};
}
