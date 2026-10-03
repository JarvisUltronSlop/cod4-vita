#pragma once
#include <algorithm>
#include <cstdint>

namespace kisak::vita {
// Stream source skinning in bounded blocks directly into the conversion pass.
// The caller supplies the original skin function and owns scratch/output. No
// retained pointers, allocation, reordering, bone math or packing changes.
template<class Surface, class VertexInfo, class RigidList, class Skin, class Consume>
bool SkinSurfaceBlocks(const Surface& source, uint32_t blockCapacity,
                       Skin skin, Consume consume) {
    if (!blockCapacity || blockCapacity > 32767u
        || (source.vertCount && !source.verts0)) return false;
    uint32_t count = 0;
    if (source.deformed) {
        for (unsigned group = 0; group < 4; ++group) {
            if (source.vertInfo.vertCount[group] < 0) return false;
            count += source.vertInfo.vertCount[group];
        }
        if (count != source.vertCount || (count && !source.vertInfo.vertsBlend)) return false;
    } else {
        if (source.vertListCount && !source.vertList) return false;
        for (uint32_t i = 0; i < source.vertListCount; ++i)
            count += source.vertList[i].vertCount;
        if (count != source.vertCount) return false;
    }
    uint32_t vertex = 0, blend = 0;
    const uint32_t groups = source.deformed ? 4u : source.vertListCount;
    for (uint32_t group = 0; group < groups; ++group) {
        uint32_t remaining = source.deformed ? source.vertInfo.vertCount[group]
                                            : source.vertList[group].vertCount;
        while (remaining) {
            const uint32_t amount = std::min(remaining, blockCapacity);
            Surface block = source;
            block.verts0 = source.verts0 + vertex;
            block.vertCount = static_cast<decltype(block.vertCount)>(amount);
            RigidList rigid{};
            if (source.deformed) {
                block.vertInfo = VertexInfo{};
                block.vertInfo.vertCount[group] = static_cast<decltype(block.vertCount)>(amount);
                block.vertInfo.vertsBlend = source.vertInfo.vertsBlend + blend;
            } else {
                rigid = source.vertList[group];
                rigid.vertCount = static_cast<decltype(rigid.vertCount)>(amount);
                block.vertList = &rigid;
                block.vertListCount = 1;
            }
            skin(block);
            if (!consume(vertex, amount)) return false;
            vertex += amount;
            if (source.deformed) blend += amount * (group * 2u + 1u);
            remaining -= amount;
        }
    }
    return true;
}
}
