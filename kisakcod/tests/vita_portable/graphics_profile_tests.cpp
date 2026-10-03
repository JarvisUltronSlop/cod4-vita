#ifdef NDEBUG
#undef NDEBUG
#endif
#define VITA_GRAPHICS_PROFILE_TEST 1
#define KISAK_VITA_CPU_RUNTIME 1
#include <vita/vita_cpu_profile.h>
#include <vita/vita_graphics_metrics.h>
#include <vita/vita_render_pass_probe.h>
#include <cassert>
#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>
struct SceRazorGpuLiveResultInfo {void* result_data;uint32_t entry_count,overflow_count,buffer_size;uint64_t start_time,end_time;uint32_t metric_group;int reserved;};
constexpr int SCE_SYSMODULE_RAZOR_HUD=10;
static int moduleResult=-1,startResult=0,stopResult=0,bufferResult=0;static uint32_t selected=0,starts=0,stops=0,loads=0;static uint64_t now=0;static void* previous=nullptr;static bool driver=false;static bool badPointer=false,overflow=false;
int sceSysmoduleIsLoaded(int){return loads && moduleResult==0?0:-1;}
int sceSysmoduleLoadModule(int){++loads;return moduleResult;}
int sceRazorGpuLiveSetMetricsGroup(uint32_t g){assert(!driver);selected=g;return 0;}
int sceRazorGpuLiveSetBuffer(void* b,uint32_t bytes,SceRazorGpuLiveResultInfo* out){assert(bytes==65536);*out={previous,0,0,bytes,0,1,selected,0};if(badPointer){out->entry_count=1;out->result_data=reinterpret_cast<void*>(1);}if(overflow)out->overflow_count=1;previous=b;return bufferResult;}
int sceRazorGpuLiveStart(){++starts;driver=startResult==0;return startResult;}
int sceRazorGpuLiveStop(){++stops;if(!stopResult)driver=false;return stopResult;}
int sceKernelGetProcessId(){return 7;}
namespace kisak::vita {
static VitaCpuContext context{1,1};struct Event{std::string name;uint64_t a,b;};static std::vector<Event> events;
uint64_t VitaCpuNow(){return ++now;}
VitaCpuContext VitaCpuProfile_Context(){return context;}
void VitaCpuProfile_Counter(const char* n,uint64_t a,uint64_t b){events.push_back({n,a,b});}
}
#include "../../src/vita/vita_graphics_profile.cpp"
template<class T> void put(std::vector<uint8_t>& x,size_t offset,T v){std::memcpy(x.data()+offset,&v,sizeof(v));}
int main(){
#ifdef _WIN32
_set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
using namespace kisak::vita;
for(unsigned phase=0;phase<=7;++phase){VitaRenderPassProbe p;p.Begin(kVitaGraphicsProfile|phase,1);const bool valid=phase>0 && phase<=6;assert(p.graphicsProfile==valid && p.worldAlpha==valid && p.weaponPipeline==valid);assert(!p.worldComponents && !p.Fenced() && !p.drawProbe && !p.lightmapLayout && !p.worldLifetime);assert(VitaWorldAlphaEnabled(kVitaGraphicsProfile|phase)==valid);assert(VitaWeaponVertexEnabled(kVitaGraphicsProfile|phase)==valid);}
assert(VitaGraphicsProfilePhase(0)==1 && VitaGraphicsProfilePhase(19999)==1 && VitaGraphicsProfilePhase(20000)==2 && VitaGraphicsProfilePhase(119999)==6 && VitaGraphicsProfilePhase(120000)==0);
std::vector<uint8_t> bytes(72);put<uint16_t>(bytes,0,72);put<uint16_t>(bytes,2,0);put<uint64_t>(bytes,8,10);put<uint64_t>(bytes,16,20);bytes[24]=2;put<float>(bytes,32,20);put<float>(bytes,36,30);put<float>(bytes,40,40);put<uint32_t>(bytes,64,7);
unsigned seen=0;auto emit=[&](const VitaGraphicsMetric& m){++seen;assert(m.process==7 && m.end-m.start==10);};assert(VitaGraphicsMetricsParse(bytes.data(),72,1,emit) && seen==1);
for(auto size:{0u,7u,71u}){seen=0;assert(!VitaGraphicsMetricsParse(bytes.data(),size,1,emit) && seen==0);}
auto bad=bytes;put<uint16_t>(bad,0,128);assert(!VitaGraphicsMetricsParse(bad.data(),72,1,emit));bad=bytes;put<float>(bad,36,101);assert(!VitaGraphicsMetricsParse(bad.data(),72,1,emit));bad=bytes;put<uint64_t>(bad,16,9);assert(!VitaGraphicsMetricsParse(bad.data(),72,1,emit));
bad=bytes;bad.insert(bad.end(),bytes.begin(),bytes.end());put<uint16_t>(bad,72,0);seen=0;assert(!VitaGraphicsMetricsParse(bad.data(),144,2,emit) && !seen);
assert(!VitaGraphicsMetricsParse(nullptr,72,1,emit));assert(VitaGraphicsMetricsParse(nullptr,0,0,emit));
for(unsigned p=1;p<=9;++p) {
    VitaGraphicsProfile_Begin(p,false);assert(VitaGraphicsProfile_Status()==1 && loads==0 && starts==0);
    VitaGraphicsProfile_Scene(now,now+1,0);VitaGraphicsProfile_End();
}
VitaGraphicsProfile_Begin(0,false);assert(!VitaGraphicsProfile_Active());
VitaGraphicsProfile_Begin(1);assert(VitaGraphicsProfile_Status()==1 && loads==0);VitaGraphicsProfile_End();VitaGraphicsProfile_Begin(2);assert(VitaGraphicsProfile_Status()==3 && loads==1 && starts==0);for(int i=0;i<5;++i){VitaGraphicsProfile_Begin(2);VitaGraphicsProfile_End();}assert(loads==1);VitaGraphicsProfile_Finish();assert(!VitaGraphicsProfile_Active());
context.generation=2;moduleResult=0;VitaGraphicsProfile_Begin(1);for(unsigned p=2;p<=5;++p){VitaGraphicsProfile_Begin(p);assert(VitaGraphicsProfile_Status()==2 && driver);VitaGraphicsProfile_End();}VitaGraphicsProfile_Begin(6);assert(!driver && starts==4 && stops==4 && VitaGraphicsProfile_Status()==1);VitaGraphicsProfile_Finish();
context.generation=3;VitaGraphicsProfile_Begin(1);VitaGraphicsProfile_Begin(2);bufferResult=-9;VitaGraphicsProfile_End();assert(!driver && VitaGraphicsProfile_Status()==3);bufferResult=0;VitaGraphicsProfile_Finish();
context.generation=4;VitaGraphicsProfile_Begin(1);startResult=-1;VitaGraphicsProfile_Begin(2);assert(VitaGraphicsProfile_Status()==3 && !driver);startResult=0;VitaGraphicsProfile_Finish();
context.generation=40;VitaGraphicsProfile_Begin(1);VitaGraphicsProfile_Begin(2);badPointer=true;VitaGraphicsProfile_End();assert(VitaGraphicsProfile_Status()==3 && !driver);badPointer=false;VitaGraphicsProfile_Finish();
context.generation=41;VitaGraphicsProfile_Begin(1);VitaGraphicsProfile_Begin(2);overflow=true;VitaGraphicsProfile_End();assert(VitaGraphicsProfile_Status()==3 && !driver);overflow=false;VitaGraphicsProfile_Finish();
context.generation=5;VitaGraphicsProfile_Begin(1);VitaGraphicsProfile_Begin(2);stopResult=-1;VitaGraphicsProfile_Finish();assert(VitaGraphicsProfile_Status()==3);auto before=starts;context.generation=6;VitaGraphicsProfile_Begin(1);VitaGraphicsProfile_Begin(2);assert(starts==before && VitaGraphicsProfile_Status()==3);
}
