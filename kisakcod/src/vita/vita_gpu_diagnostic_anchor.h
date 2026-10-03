#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
namespace kisak::vita {
inline uint32_t VitaGpuFloatBits(float value) {
    uint32_t bits;std::memcpy(&bits,&value,sizeof(bits));return bits;
}
inline bool VitaGpuAnchorValid(const float* origin,const float* angles) {
    if(!origin || !angles)return false;
    for(unsigned i=0;i<3;++i)
        if(!std::isfinite(origin[i]) || !std::isfinite(angles[i]) || std::fabs(origin[i])>131072.0f)return false;
    return true;
}
inline bool VitaGpuAnchorReady(const float* anchor,const float* angles,
    const float* server,const float* client,const float* serverAngles,const float* clientAngles,
    const float* serverVelocity,const float* clientVelocity) {
    if(!VitaGpuAnchorValid(anchor,angles) || !VitaGpuAnchorValid(server,serverAngles)
        || !VitaGpuAnchorValid(client,clientAngles) || !serverVelocity || !clientVelocity)return false;
    for(unsigned i=0;i<3;++i) {
        if(std::fabs(server[i]-client[i])>0.25f || std::fabs(server[i]-anchor[i])>(i==2?64.0f:0.25f))return false;
        if(!std::isfinite(serverVelocity[i]) || !std::isfinite(clientVelocity[i])
            || std::fabs(serverVelocity[i])>0.25f || std::fabs(clientVelocity[i])>0.25f)return false;
        if(std::fabs(std::remainder(serverAngles[i]-angles[i],360.0f))>0.1f
            || std::fabs(std::remainder(clientAngles[i]-angles[i],360.0f))>0.1f)return false;
    }
    return true;
}
}
