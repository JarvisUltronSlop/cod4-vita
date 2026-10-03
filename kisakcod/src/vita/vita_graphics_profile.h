#pragma once
#include <cstdint>
namespace kisak::vita {
#if (defined(__vita__) && KISAK_VITA_CPU_RUNTIME) || defined(VITA_GRAPHICS_PROFILE_TEST)
void VitaGraphicsProfile_Begin(uint32_t phase,bool counters=true);
void VitaGraphicsProfile_End();
void VitaGraphicsProfile_Finish();
void VitaGraphicsProfile_Pass(uint32_t nextPass,uint32_t fbo,uint32_t width,uint32_t height);
void VitaGraphicsProfile_Scene(uint64_t start,uint64_t end,int result);
bool VitaGraphicsProfile_Active();
uint32_t VitaGraphicsProfile_Status(); // 0 idle, 1 baseline, 2 collecting, 3 unavailable
#else
inline void VitaGraphicsProfile_Begin(uint32_t,bool=true){}
inline void VitaGraphicsProfile_End(){}
inline void VitaGraphicsProfile_Finish(){}
inline void VitaGraphicsProfile_Pass(uint32_t,uint32_t,uint32_t,uint32_t){}
inline void VitaGraphicsProfile_Scene(uint64_t,uint64_t,int){}
inline bool VitaGraphicsProfile_Active(){return false;}
inline uint32_t VitaGraphicsProfile_Status(){return 0;}
#endif
}
