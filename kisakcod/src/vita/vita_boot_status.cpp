#include <vita/vita_boot_status.h>
#include <vita/vita_diagnostic_glyph.h>
#include <vita/vita_cpu_profile.h>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace kisak::vita {
namespace {
std::atomic<VitaBootStage> stage{VitaBootStage::Starting};
std::atomic<bool> stop{false};
SceUID memory=-1,thread=-1;
uint32_t* pixels=nullptr;
uint64_t startUs=0;
constexpr const char* labels[]={"STARTING", "VALIDATING CONTENT CATALOG", "LOADING COMMON FRONTEND", "INITIALIZING RENDERER AND SHADERS", "OPENING MAIN MENU", "STARTUP FAILED"};
void Text(const char* text,uint32_t y,uint32_t color) {
    uint32_t x=64;
    for(;*text && x+18<960;++text,x+=18) {
        for(unsigned row=0;row<7;++row) {
            const char* bits=VitaDiagnosticGlyphRow(*text,row);
            for(unsigned col=0;col<5;++col)if(bits[col]=='1')
                for(unsigned dy=0;dy<3;++dy)for(unsigned dx=0;dx<3;++dx)
                    pixels[(y+row*3+dy)*1024+x+col*3+dx]=color;
        }
    }
}
int Draw(SceSize,void*) {
    VitaCpuRegisterThread(VitaCpuRole::Background,SCE_KERNEL_CPU_MASK_USER_2);
    while(!stop.load(std::memory_order_acquire)) {
        std::fill_n(pixels,1024*544,0xff15100bu);
        Text("CALL OF DUTY 4",150,0xffefefefu);
        Text(labels[unsigned(stage.load())],220,0xffecbd6du);
        char elapsed[64];std::snprintf(elapsed,sizeof(elapsed),"ELAPSED %u SECONDS",unsigned((VitaCpuNow()-startUs)/1000000));
        Text(elapsed,270,0xffc7c7c7u);
        SceDisplayFrameBuf frame{};frame.size=sizeof(frame);frame.base=pixels;frame.pitch=1024;
        frame.pixelformat=SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;frame.width=960;frame.height=544;
        // Only the boot owner publishes this framebuffer. It stops doing so
        // before VitaGL's first swap; the storage stays alive until that swap.
        if(stage.load()!=VitaBootStage::Ready)sceDisplaySetFrameBuf(&frame,SCE_DISPLAY_SETBUF_NEXTFRAME);
        sceKernelDelayThread(250000);
    }
    return 0;
}
}
bool VitaBootStatus_Start() {
    startUs=VitaCpuNow();stop.store(false);stage.store(VitaBootStage::Starting);
    memory=sceKernelAllocMemBlock("cod4-boot-status",SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,0x240000,nullptr);
    if(memory<0)return false;
    if(sceKernelGetMemBlockBase(memory,reinterpret_cast<void**>(&pixels))<0){sceKernelFreeMemBlock(memory);memory=-1;return false;}
    thread=sceKernelCreateThread("cod4-boot-status",Draw,0x10000110,32768,0,SCE_KERNEL_CPU_MASK_USER_2,nullptr);
    if(thread<0 || sceKernelStartThread(thread,0,nullptr)<0){
        if(thread>=0)sceKernelDeleteThread(thread);
        sceKernelFreeMemBlock(memory);thread=memory=-1;pixels=nullptr;return false;
    }
    return true;
}
void VitaBootStatus_Stage(VitaBootStage next){
    VitaCpuProfile_Startup(labels[unsigned(next)],0);
    stage.store(next,std::memory_order_release);
    if(next==VitaBootStage::Ready || next==VitaBootStage::Failed) {
        // Join before the first GL swap, but retain the displayed allocation
        // until Stop confirms the replacement framebuffer has been presented.
        stop.store(true,std::memory_order_release);
        if(thread>=0){sceKernelWaitThreadEnd(thread,nullptr,nullptr);sceKernelDeleteThread(thread);thread=-1;}
    }
    VitaCpuProfile_Counter("boot-stage",uint32_t(next),VitaCpuNow()-startUs);
}
void VitaBootStatus_Stop(){
    stop.store(true,std::memory_order_release);
    if(thread>=0){sceKernelWaitThreadEnd(thread,nullptr,nullptr);sceKernelDeleteThread(thread);thread=-1;}
    if(memory>=0){sceDisplayWaitVblankStart();sceKernelFreeMemBlock(memory);memory=-1;pixels=nullptr;}
}
}
