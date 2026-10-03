#include <vita/vita_cpu_shader_cache.h>
#if KISAK_VITA_CPU_RUNTIME
#include <vita/vita_cpu_profile.h>
#include <vita/vita_sha256.h>
#include "vita_cpu_shader_key.h"
#include <vitaGL.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <cstdio>
#include <cstring>
#include <atomic>
namespace {std::atomic<unsigned> shaderCompileFailures{0};std::atomic<bool> worldPrecisionReady{false};std::atomic<bool>worldScalingReady{false};}
namespace kisak::vita {bool VitaCpuShaderCache_WorldScalingReady(){return worldScalingReady.load(std::memory_order_acquire);}
void VitaCpuShaderCache_SetWorldScalingReady(bool ready){worldScalingReady.store(ready,std::memory_order_release);}
bool VitaCpuShaderCache_WorldPrecisionReady(){return worldPrecisionReady.load(std::memory_order_acquire);}
void VitaCpuShaderCache_SetWorldPrecisionReady(bool ready){worldPrecisionReady.store(ready,std::memory_order_release);}
unsigned VitaCpuShaderCache_CompileFailures(){return shaderCompileFailures.load(std::memory_order_relaxed);}}
extern "C" void vglCod4ShaderMessage(unsigned type, uint64_t identity, unsigned reason, int line, const char* message) {
    if(reason>=10 && reason<=13)shaderCompileFailures.fetch_add(1,std::memory_order_relaxed);
    char subject[112];std::snprintf(subject,sizeof(subject),"type=%u source=%016llx reason=%u line=%d",type,(unsigned long long)identity,reason,line);
    kisak::vita::VitaCpuProfile_Trace("shader-compiler-message",subject,message);
    kisak::vita::VitaCpuProfile_Counter("shader-compiler-message",reason,identity);
}

extern "C" void vglCod4CacheEnable(GLboolean enabled);
extern "C" void vglCod4CacheEvent(unsigned event,unsigned elapsed,unsigned bytes) {
    static const char* labels[]={"shader-cache-disabled","shader-cache-hit","shader-cache-miss",
        "shader-cache-rejected","shader-cache-write","shader-cache-write-failed","shader-compile",
        "shader-cache-padding-accepted","shader-cache-program-length-rejected","shader-cache-program-check-rejected","shader-compile-failed"};
    if(event>=sizeof(labels)/sizeof(labels[0]))return;
    kisak::vita::VitaCpuEvent record{};
    record.startUs=kisak::vita::VitaCpuNow()-elapsed;record.elapsedUs=record.exclusiveUs=elapsed;
    record.kind=uint32_t(kisak::vita::VitaCpuEventKind::Loading);record.value0=bytes;record.value1=event;
    std::snprintf(record.name,sizeof(record.name),"%s",labels[event]);kisak::vita::VitaCpuProfile_Record(record);
}
namespace kisak::vita {
void VitaCpuShaderCache_Initialize() {
    VITA_CPU_LOADING("shader-cache-compatibility");
    VitaSha256 hash;hash.Update(kVitaCpuShaderCompatibility,std::strlen(kVitaCpuShaderCompatibility));
    // This is the exact default module path used by the pinned VitaShaRK.
    // Never reuse compiled programs when the compiler module cannot be identified.
    FILE* compiler=std::fopen("ur0:/data/libshacccg.suprx","rb");
    bool valid=false;
    if(compiler) {
        char bytes[16384];size_t count,total=0;
        while((count=std::fread(bytes,1,sizeof(bytes),compiler))!=0 && total<16*1024*1024) {
            hash.Update(bytes,count);total+=count;
        }
        valid=total>0 && total<16*1024*1024 && !std::ferror(compiler) && std::feof(compiler);
        std::fclose(compiler);
    }
    char key[65]{};const auto digest=hash.Finish();
    for(unsigned i=0;i<32;++i)std::snprintf(key+2*i,3,"%02x",unsigned(digest[i]));
    char path[160];std::snprintf(path,sizeof(path),"ux0:data/CODVIT001/shader-cache/%s",key);
    sceIoMkdir("ux0:data/CODVIT001/shader-cache",0777);
    sceIoMkdir(path,0777);vglSetShaderCachePath(path);
    vglCod4CacheEnable(valid?GL_TRUE:GL_FALSE);
    VitaCpuProfile_Counter(valid?"shader-cache-compatible":"shader-compiler-identity-unavailable",1);
    char namespaceRecord[96];std::snprintf(namespaceRecord,sizeof(namespaceRecord),"shader-cache-namespace-%s",key);
    VitaCpuProfile_Counter(namespaceRecord,valid);
    // Collector work shares core 2 below the pinned OpenAL output priority.
    vglSetupGarbageCollector(0x10000110,SCE_KERNEL_CPU_MASK_USER_2);
}
}
#endif
