#pragma once
#include <gfx_vita/gfx_vita_viewmodel.h>
#include <vita/vita_source_vertex.h>
#include <vita/vita_vertex_pack.h>

namespace kisak::vita {
// Renderer-owned, initialized during resource loading. The source encoding's
// scale byte and component byte fully determine the final signed GPU byte.
// Retain the original float decode/multiply/rounding when building the table.
class VitaViewModelVertexConverter {
    std::array<int8_t, 65536> packedUnit{};
    bool ready=false;
public:
    void Prepare() {
        if(ready)return;
        for(unsigned scale=0;scale<256;++scale) {
            const float decodeScale=(static_cast<float>(scale)- -192.0f)/32385.0f;
            for(unsigned component=0;component<256;++component)
                packedUnit[(scale<<8)|component]=PackVertexSnorm(
                    (static_cast<float>(component)-127.0f)*decodeScale);
        }
        ready=true;
    }
    bool Ready() const {return ready;}
    int8_t Pack(unsigned scale,unsigned component) const {
        return packedUnit[(scale<<8)|component];
    }
    template<class PackedVertex,class Project,class ResolveLight>
    bool Convert(const PackedVertex& input,GfxVitaViewmodelVertex& dest,
                 Project project,ResolveLight resolve) const {
        if(!ready)return false;
        project(input.xyz,dest.position,dest.worldPosition);
        for(float component:dest.position)if(!std::isfinite(component))return false;
        skinmath::Vec2UnpackTexCoords(input.texCoord,dest.texCoord);
        float normal[3],response[3]{1,1,1};
        skinmath::Vec3UnpackUnitVec(input.normal,normal);
        const bool lit=resolve(normal,response);
        for(unsigned c=0;c<3;++c) {
            dest.normal[c]=Pack(input.normal.array[3],input.normal.array[c]);
            dest.tangent[c]=Pack(input.tangent.array[3],input.tangent.array[c]);
            dest.color[c]=lit?PackVertexUnorm8(sourceVertexColors[input.color.array[c]]*response[c])
                             :input.color.array[c];
        }
        dest.normal[3]=dest.tangent[3]=0;
        dest.color[3]=input.color.array[3];
        return true;
    }
};
}
