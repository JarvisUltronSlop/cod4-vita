#pragma once
#include <vita/vita_model_bundle.h>
#include <cstdint>

namespace kisak::vita {
// Source XSurface consumes local uint16 indices read-only. Only an unchanged,
// suitably aligned zero-base slice can borrow the map bundle's immutable data.
// Nonzero bases retain the original per-index conversion and owned allocation.
// Caller must destroy the registry before clearing/reloading the model bundle.
template<class Allocate>
uint16_t* VitaSourceModelIndices(const VitaModelRecord& model,
    const VitaModelSurface& surface, bool borrowMapPayload, Allocate allocate,
    bool* borrowed=nullptr) {
    if (borrowed) *borrowed=false;
    if (!surface.vertexCount || !surface.indexCount || surface.indexCount%3
        || surface.vertexCount>UINT16_MAX || surface.indexCount/3>UINT16_MAX
        || uint64_t(surface.vertexStart)+surface.vertexCount>model.vertices.size()
        || uint64_t(surface.indexStart)+surface.indexCount>model.indices.size()) return nullptr;
    const auto* input=model.indices.data()+surface.indexStart;
    for (uint32_t i=0;i<surface.indexCount;++i)
        if (input[i]<surface.vertexStart || uint64_t(input[i])>=uint64_t(surface.vertexStart)+surface.vertexCount)
            return nullptr;
    if (borrowMapPayload && surface.vertexStart==0
        && (reinterpret_cast<uintptr_t>(input)&3u)==0) {
        if (borrowed) *borrowed=true;
        // XSurface's legacy interface is mutable, but runtime consumers only
        // read this array. The desktop loader's fixups have already happened.
        return const_cast<uint16_t*>(input);
    }
    auto* output=static_cast<uint16_t*>(allocate(surface.indexCount*sizeof(uint16_t),4));
    if (!output) return nullptr;
    for (uint32_t i=0;i<surface.indexCount;++i)
        output[i]=static_cast<uint16_t>(input[i]-surface.vertexStart);
    return output;
}
}
