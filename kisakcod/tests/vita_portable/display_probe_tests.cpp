#ifdef NDEBUG
#undef NDEBUG
#endif
#define KISAK_VITA_CPU_RUNTIME 1
#define VITA_DISPLAY_PROBE_TEST 1
#include <cassert>
#include <cstdint>
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
struct SceGxmContext {}; struct SceGxmNotification {};struct SceGxmSyncObject {int value;};
struct SceDisplayFrameBuf {};enum SceDisplaySetBufSync { NextFrame };
using SceGxmDisplayQueueCallback=void(const void*);
struct SceGxmInitializeParams {unsigned flags,displayQueueMaxPendingCount;SceGxmDisplayQueueCallback* displayQueueCallback;unsigned displayQueueCallbackDataSize;unsigned parameterBufferSize;};
static std::atomic<uint64_t> now{100};static thread_local int tid=1;static unsigned finishes=0,ends=0,originalCalls=0;
static bool deferred=false,waitVblank=false;static int addError=0,setError=0;
static SceGxmDisplayQueueCallback* installed=nullptr;
static std::vector<const void*> pending;
uint64_t sceKernelGetProcessTimeWide(){return now++;}
int sceKernelGetThreadId(){return tid;}
int sceKernelGetThreadCpuAffinityMask(int id){assert(id==2);return 0x70000;}
int sceKernelGetThreadCurrentPriority(){assert(tid==2);return 64;}
void sceGxmFinish(SceGxmContext*){++finishes;now+=100000;}
static void deliver(const void* data){tid=2;installed(data);tid=1;}
int sceGxmDisplayQueueFinish(){for(auto p:pending)deliver(p);pending.clear();return 0;}
#include <vita/vita_display_probe.cpp>
static bool concurrent=false;
static kisak::vita::VitaCpuQueue<const void*,2> simulatedSdkQueue;
struct Event {std::string name;uint64_t a,b;kisak::vita::VitaCpuContext context;};
static std::vector<Event> events;static kisak::vita::VitaCpuContext cpuContext{7,1};
namespace kisak::vita {
VitaCpuContext VitaCpuProfile_Context(){assert(tid==1);return cpuContext;}
void VitaCpuProfile_SetSequence(uint32_t g,uint32_t s){assert(tid==1);cpuContext={g,s};}
void VitaCpuProfile_Counter(const char* n,uint64_t a,uint64_t b){assert(tid==1);events.push_back({n,a,b,cpuContext});}
}
extern "C" int __real_sceGxmVshInitialize(const SceGxmInitializeParams* p){assert(p->flags==123 && p->displayQueueMaxPendingCount==2 && p->displayQueueCallbackDataSize==4 && p->parameterBufferSize==262144);installed=p->displayQueueCallback;return 0;}
extern "C" int __real_sceGxmEndScene(SceGxmContext*,const SceGxmNotification*,const SceGxmNotification*){++ends;return 0;}
extern "C" int __real_sceGxmDisplayQueueAddEntry(SceGxmSyncObject* a,SceGxmSyncObject* b,const void* p){assert(a->value==1 && b->value==2);if(!addError){if(concurrent){while(!simulatedSdkQueue.Push(p))std::this_thread::yield();}else if(deferred)pending.push_back(p);else deliver(p);}now+=50;return addError;}
extern "C" int __real_sceDisplaySetFrameBuf(const SceDisplayFrameBuf*,SceDisplaySetBufSync){now+=7;return setError;}
extern "C" int __real_sceDisplayWaitVblankStartMulti(unsigned int count){assert(count==1);now+=16000;return 0;}
static const void* expectedData=reinterpret_cast<void*>(1234);
static void original(const void* p){assert(p==expectedData);++originalCalls;SceDisplayFrameBuf fb;__wrap_sceDisplaySetFrameBuf(&fb,NextFrame);if(waitVblank)__wrap_sceDisplayWaitVblankStartMulti(1);}
static Event last(const char* name){for(auto i=events.rbegin();i!=events.rend();++i)if(i->name==name)return *i;assert(false);return {};}
int main(){
#ifdef _MSC_VER
 _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
 using namespace kisak::vita;
 SceGxmInitializeParams init{123,2,original,4,262144};assert(__wrap_sceGxmVshInitialize(&init)==0);assert(init.displayQueueCallback==original);
 SceGxmSyncObject oldBuffer{1},newBuffer{2};SceGxmContext context;
 auto submit=[&]{__wrap_sceGxmEndScene(&context,nullptr,nullptr);return __wrap_sceGxmDisplayQueueAddEntry(&oldBuffer,&newBuffer,expectedData);};
 submit();VitaDisplayProbe_Drain();assert(events.empty() && originalCalls==1 && finishes==0);
 for(unsigned p=1;p<=4;++p){
  VitaDisplayProbe_SetPhase(p);cpuContext.sequence=p+10;const auto beforeEnds=ends;submit();
  const auto id=last("display-submit-id").a;assert(last("display-submit-id").b==p);
  assert(last("display-submit-finish-us").b==(p==2?100001u:0u));assert(ends==beforeEnds+1);
  assert(last("display-probe-health").a==1 && last("display-probe-health").b==0);
  cpuContext.sequence=99;VitaDisplayProbe_Drain();assert(cpuContext.sequence==99);
  assert(last("display-cb-enter").a==id && last("display-cb-enter").context.sequence==p+10);
  assert(last("display-cb-setbuf-us").b==8 && last("display-cb-vblank-us").b==0);
  assert(last("display-cb-thread").a==2 && last("display-cb-thread").b==0x70000);
 }
 assert(finishes==1);
 VitaDisplayProbe_SetPhase(1);deferred=true;waitVblank=true;
 for(unsigned i=0;i<3;++i){cpuContext.sequence=50+i;submit();}
 assert(pending.size()==3);VitaDisplayProbe_Finish();assert(pending.empty());
 assert(last("display-cb-vblank-us").b==16001 && last("display-cb-calls").b==1);
 assert(last("display-cb-enter").context.sequence==52 && last("display-probe-drained").b==0);
 deferred=false;waitVblank=false;
 concurrent=true;const auto beforeConcurrent=originalCalls;
 std::thread display([]{for(unsigned i=0;i<2000;++i){const void* p=nullptr;while(!simulatedSdkQueue.Pop(&p))std::this_thread::yield();deliver(p);}});
 for(unsigned i=0;i<2000;++i){cpuContext.sequence=1000+i;submit();VitaDisplayProbe_Drain();}
 display.join();VitaDisplayProbe_Drain();concurrent=false;
 assert(originalCalls==beforeConcurrent+2000 && last("display-probe-health").b==0);
 assert(last("display-cb-enter").context.sequence==2999);
 setError=-1;submit();VitaDisplayProbe_Drain();assert(last("display-cb-status").b==1);setError=0;
 // Delayed consumer fills only bounded diagnostic storage; callbacks continue.
 const auto beforeCalls=originalCalls;for(unsigned i=0;i<130;++i)submit();
 assert(originalCalls==beforeCalls+130 && last("display-probe-health").b==2);
 VitaDisplayProbe_Drain();addError=-9;assert(submit()==-9);assert(last("display-submit-status").b==uint32_t(-9));
 assert(last("display-probe-health").b==3);
}
