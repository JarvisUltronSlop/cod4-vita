#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace kisak::vita {
struct TerrainPartitionLayout {
    static constexpr size_t RootTriangles = 255, LeafTriangles = 16;
    static size_t Roots(size_t triangles) { return (triangles + RootTriangles - 1) / RootTriangles; }
    static size_t Leaves(size_t triangles) {
        return (triangles / RootTriangles) * 16 + ((triangles % RootTriangles) + LeafTriangles - 1) / LeafTriangles;
    }
    static size_t Trees(size_t triangles) { return Roots(triangles) + Leaves(triangles); }
};

// Preserve each original 255-triangle root and its bounds/material boundary.
// Children cover consecutive source triangle ranges, in original order.
// Only the original native CM cull/intersection routines consume this layout.
template<class Vertices, class Triangles, class Partition, class Tree>
void BuildTerrainPartitions(const Vertices& vertices, const Triangles& triangles,
                            Partition* partitions, Tree* trees) {
    const size_t roots = TerrainPartitionLayout::Roots(triangles.size());
    const auto bounds = [&](Tree& tree, size_t first, size_t count) {
        float minimum[3] = {std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()};
        float maximum[3] = {-std::numeric_limits<float>::max(),-std::numeric_limits<float>::max(),-std::numeric_limits<float>::max()};
        for (size_t i=first; i<first+count; ++i) for (uint32_t corner=0; corner<3; ++corner) {
            const auto& point = vertices[triangles[i][corner]];
            for (uint32_t axis=0; axis<3; ++axis) {
                minimum[axis]=std::min(minimum[axis],point[axis]);
                maximum[axis]=std::max(maximum[axis],point[axis]);
            }
        }
        for (uint32_t axis=0; axis<3; ++axis) {
            tree.origin[axis]=(minimum[axis]+maximum[axis])*0.5f;
            tree.halfSize[axis]=(maximum[axis]-minimum[axis])*0.5f+0.125f;
        }
    };
    size_t leaf=0;
    for (size_t root=0; root<roots; ++root) {
        const size_t first=root*255, count=std::min<size_t>(255,triangles.size()-first);
        auto& parent=trees[root]; parent={}; parent.materialIndex=0;
        bounds(parent,first,count);
        parent.childCount=static_cast<uint16_t>((count+15)/16);
        parent.u.firstChildIndex=static_cast<int>(roots+leaf);
        for (size_t offset=0; offset<count; offset+=16,++leaf) {
            const size_t size=std::min<size_t>(16,count-offset);
            auto& part=partitions[leaf]; part={};
            part.firstTri=static_cast<int>(first+offset); part.triCount=static_cast<uint8_t>(size);
            auto& child=trees[roots+leaf]; child={}; child.materialIndex=0;
            child.u.partitionIndex=static_cast<int>(leaf);
            bounds(child,first+offset,size);
        }
    }
}
}
