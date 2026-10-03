#pragma once
#include <cstddef>
#include <cstdint>
using SceUID=int;using SceSize=std::size_t;using SceUInt=unsigned;
constexpr int SCE_KERNEL_CPU_MASK_USER_0=0x10000;
constexpr int SCE_KERNEL_CPU_MASK_USER_1=0x20000;
constexpr int SCE_KERNEL_CPU_MASK_USER_2=0x40000;
SceUID sceKernelCreateThread(const char*,int (*entry)(SceSize,void*),int,int,int,int,void*);
int sceKernelStartThread(SceUID,SceSize,void*);
int sceKernelWaitThreadEnd(SceUID,int*,void*);
int sceKernelDeleteThread(SceUID);
int sceKernelDelayThread(unsigned);
int sceKernelGetThreadId();
int CpuTestLiveThreads();
void CpuTestFailNextThreadStart();
