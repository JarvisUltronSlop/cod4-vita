#pragma once
#include <psp2/kernel/threadmgr/thread.h>
SceUID sceKernelCreateSema(const char*,int,int,int,void*);
int sceKernelSignalSema(SceUID,int);
int sceKernelWaitSema(SceUID,int,SceUInt*);
int sceKernelDeleteSema(SceUID);
