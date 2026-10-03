#pragma once
#include "gfx_vita_material_family.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

// Match the signed-byte packing of immutable StaticWorldVertex exactly.
inline int8_t GfxVitaWorldBasisSnorm(float value) {
    return static_cast<int8_t>(std::lround(std::clamp(std::isfinite(value)?value:0.f,-1.f,1.f)*127.f));
}
struct GfxVitaWorldBasisKey {
    std::array<int8_t,6> bytes{};bool valid=false;
};
template<class Vertex> inline GfxVitaWorldBasisKey GfxVitaWorldBasis(const Vertex& v) {
    GfxVitaWorldBasisKey key;
    for(unsigned i=0;i<3;++i) {
        if(!std::isfinite(v.normal[i]) || !std::isfinite(v.tangent[i]))return key;
        key.bytes[i]=GfxVitaWorldBasisSnorm(v.normal[i]);key.bytes[i+3]=GfxVitaWorldBasisSnorm(v.tangent[i]);
    }
    const auto& k=key.bytes;float n2=0,c2=0;
    for(unsigned i=0;i<3;++i) {
        n2+=float(k[i])*k[i];unsigned j=(i+1)%3,l=(i+2)%3;
        const int cross=int(k[j])*k[l+3]-int(k[l])*k[j+3];c2+=float(cross)*cross;
    }
    // Tangent projection length squared = |N x T|^2/|N|^2.
    // Exclude the original shader's degenerate fallback boundary with margin.
    key.valid=n2>0 && c2>n2*127.f*127.f*0.01f;
    return key;
}
template<class VertexAt> inline bool GfxVitaConstantWorldBasis(
    const uint16_t* indices,uint32_t indexCount,uint32_t vertexCount,VertexAt vertex) {
    if(!indices || indexCount<3 || indexCount%3 || !vertexCount)return false;
    for(uint32_t i=0;i<indexCount;i+=3) {
        GfxVitaWorldBasisKey first;
        for(unsigned j=0;j<3;++j) {
            const uint32_t index=indices[i+j];if(index>=vertexCount)return false;
            const auto key=GfxVitaWorldBasis(vertex(index));if(!key.valid)return false;
            if(j==0)first=key;else if(first.bytes!=key.bytes)return false;
        }
    }
    return true;
}
// The copied source command certifies only the following static draw.
struct GfxVitaWorldBasisState {
    bool pending=false,active=false;
    void Reset(){pending=active=false;}
};
struct GfxVitaWorldBasisScope {
    GfxVitaWorldBasisState& state;
    GfxVitaWorldBasisScope(GfxVitaWorldBasisState& s,bool instanced):state(s) {state.active=state.pending && !instanced;state.pending=false;}
    ~GfxVitaWorldBasisScope(){state.active=false;}
};
inline bool GfxVitaBasisReplace(std::string& s,const char* before,const char* after) {
    const std::string from=before;const auto at=s.find(from);
    if(at==std::string::npos || s.find(from,at+from.size())!=std::string::npos)return false;
    s.replace(at,from.size(),after);return true;
}
inline bool GfxVitaBuildWorldBasis(const char* originalVertex,const char* originalFragment,
    bool transparent,std::string& vertex,std::string& fragment) {
    vertex.clear();fragment.clear();if(!originalVertex || !originalFragment)return false;
    std::string v=originalVertex,f;
    if(!(transparent?GfxVitaBuildWorldAlpha(originalFragment,f):GfxVitaBuildMaterialFamily(originalFragment,1,f)))return false;
    if(!GfxVitaBasisReplace(v,"varying vec3 v_tangent;","varying vec3 v_tangent;\nvarying vec3 v_bitangent;"))return false;
    if(!GfxVitaBasisReplace(v,"    v_normal = worldNormal;\n    v_tangent = worldTangent;",
        "    float normalLength = dot(worldNormal, worldNormal);\n"
        "    vec3 normal = normalLength > 0.000001 ? normalize(worldNormal) : vec3(0.0, 0.0, 1.0);\n"
        "    vec3 tangentBasis = worldTangent - normal * dot(normal, worldTangent);\n"
        "    float tangentLength = dot(tangentBasis, tangentBasis);\n"
        "    vec3 tangent = tangentLength > 0.000001 ? normalize(tangentBasis) : vec3(1.0, 0.0, 0.0);\n"
        "    v_normal = normal;\n    v_tangent = tangent;\n    v_bitangent = normalize(cross(normal, tangent));"))return false;
    if(!GfxVitaBasisReplace(f,"varying vec3 v_tangent;","varying vec3 v_tangent;\nvarying vec3 v_bitangent;"))return false;
    if(!GfxVitaBasisReplace(f,"    float normalLength = dot(v_normal, v_normal);\n    vec3 normal = normalLength > 0.000001\n        ? normalize(v_normal) : vec3(0.0, 0.0, 1.0);","    vec3 normal = v_normal;"))return false;
    if(!GfxVitaBasisReplace(f,"        vec3 tangentBasis = v_tangent - normal * dot(normal, v_tangent);\n        float tangentLength = dot(tangentBasis, tangentBasis);\n        vec3 tangent = tangentLength > 0.000001\n            ? normalize(tangentBasis) : vec3(1.0, 0.0, 0.0);\n        vec3 bitangent = normalize(cross(normal, tangent));",
        "        vec3 tangent = v_tangent;\n        vec3 bitangent = v_bitangent;"))return false;
    vertex.swap(v);fragment.swap(f);return true;
}
