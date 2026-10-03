#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <vita/vita_source_vertex.h>
#include <vita/vita_prepared_model_light.h>

namespace kisak::vita {
// Value-only snapshot ABI. No source DObj, pose, asset or frontend pointers.
union VitaPackedAttribute { uint32_t packed; uint8_t array[4]; };
struct VitaPackedSkinVertex {
    float xyz[3], binormalSign;
    VitaPackedAttribute color, texCoord, normal, tangent;
};
static_assert(sizeof(VitaPackedSkinVertex)==32);
struct VitaViewModelProjection {
    float viewOffset[3]{}, cameraOrigin[3]{}, cameraAxis[3][3]{};
    float tanHalfFovX=1, tanHalfFovY=1;
    void operator()(const float* relative,float* projected,float* world) const {
        float delta[3];
        for(int axis=0;axis<3;++axis){
            world[axis]=relative[axis]+viewOffset[axis];
            delta[axis]=world[axis]-cameraOrigin[axis];
        }
        projected[0]=-skinmath::Vec3Dot(delta,cameraAxis[1])/tanHalfFovX;
        projected[1]=skinmath::Vec3Dot(delta,cameraAxis[2])/tanHalfFovY;
        projected[2]=skinmath::Vec3Dot(delta,cameraAxis[0]);
    }
};
struct VitaViewModelLightSample {
    bool valid=false,hasDirectionalColors=false;
    float primaryLightWeight=0,color[3]{};
    std::array<uint8_t,168> directionalColors{};
};
struct VitaViewModelWorldLight {
    bool valid=false;
    float sunDirection[3]{},sunLightColor[3]{};
};
struct VitaViewModelInputs {
    VitaViewModelProjection projection;
    VitaViewModelLightSample sample;
    VitaViewModelWorldLight world;
    uint32_t entity=0;
};
template<class Sample,class World>
void CopyViewModelLight(const Sample& sample,const World& world,VitaViewModelInputs& dest){
    dest.sample.valid=sample.valid;dest.sample.hasDirectionalColors=sample.hasDirectionalColors;
    dest.sample.primaryLightWeight=sample.primaryLightWeight;
    std::memcpy(dest.sample.color,sample.color,sizeof(dest.sample.color));
    dest.sample.directionalColors=sample.directionalColors;
    dest.world.valid=world.valid;
    std::memcpy(dest.world.sunDirection,world.sunDirection,sizeof(dest.world.sunDirection));
    std::memcpy(dest.world.sunLightColor,world.sunLightColor,sizeof(dest.world.sunLightColor));
}
}
