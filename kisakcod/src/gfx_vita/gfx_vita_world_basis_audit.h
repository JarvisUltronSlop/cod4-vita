#pragma once
#include "gfx_vita_world_basis.h"

// Load-time evidence only. This does not change the shader certificate or
// reorder/split the immutable draw. Keep it outside the shader cache inputs.
enum class GfxVitaBasisReason : uint32_t {
    Constant, NormalVaries, TangentVaries, BothVary, Nonfinite, Degenerate,
    InvalidIndex, Count
};
struct GfxVitaWorldBasisAudit {
    std::array<uint32_t,static_cast<size_t>(GfxVitaBasisReason::Count)> reasons{};
    uint32_t triangles=0, runs=0, constantRuns=0, longestConstantRun=0;
    bool inputValid=false;
};
template<class VertexAt> inline GfxVitaBasisReason GfxVitaAuditBasisTriangle(
    const uint16_t* indices,uint32_t vertexCount,VertexAt vertex) {
    for(unsigned j=0;j<3;++j)
        if(indices[j]>=vertexCount)return GfxVitaBasisReason::InvalidIndex;
    std::array<GfxVitaWorldBasisKey,3> keys;
    for(unsigned j=0;j<3;++j) {
        const auto& v=vertex(indices[j]);
        for(unsigned k=0;k<3;++k)
            if(!std::isfinite(v.normal[k]) || !std::isfinite(v.tangent[k]))
                return GfxVitaBasisReason::Nonfinite;
        keys[j]=GfxVitaWorldBasis(v);
    }
    for(const auto& key:keys)
        if(!key.valid)return GfxVitaBasisReason::Degenerate;
    bool normal=false,tangent=false;
    for(unsigned j=1;j<3;++j)for(unsigned k=0;k<3;++k) {
        normal|=keys[j].bytes[k]!=keys[0].bytes[k];
        tangent|=keys[j].bytes[k+3]!=keys[0].bytes[k+3];
    }
    return normal ? (tangent?GfxVitaBasisReason::BothVary:GfxVitaBasisReason::NormalVaries)
                  : (tangent?GfxVitaBasisReason::TangentVaries:GfxVitaBasisReason::Constant);
}
template<class VertexAt> inline GfxVitaWorldBasisAudit GfxVitaAuditWorldBasis(
    const uint16_t* indices,uint32_t indexCount,uint32_t vertexCount,VertexAt vertex) {
    GfxVitaWorldBasisAudit out;
    if(!indices || indexCount<3 || indexCount%3 || !vertexCount)return out;
    out.inputValid=true;
    bool previous=false;uint32_t streak=0;
    for(uint32_t i=0;i<indexCount;i+=3) {
        const auto reason=GfxVitaAuditBasisTriangle(indices+i,vertexCount,vertex);
        ++out.reasons[static_cast<size_t>(reason)];++out.triangles;
        if(reason==GfxVitaBasisReason::InvalidIndex)out.inputValid=false;
        const bool constant=reason==GfxVitaBasisReason::Constant;
        if(i==0 || constant!=previous){++out.runs;if(constant)++out.constantRuns;}
        streak=constant?streak+1:0;
        out.longestConstantRun=std::max(out.longestConstantRun,streak);
        previous=constant;
    }
    return out;
}
