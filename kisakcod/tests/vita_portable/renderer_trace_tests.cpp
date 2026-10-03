#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_cpu_profile.h>
#include <vita/vita_cpu_queue.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace kisak::vita {
std::atomic<bool> active{true},failed{false};
std::atomic<uint64_t> dropped{0},textQueueDropped{0};
std::atomic<uint32_t> textQueuePeak{0};
thread_local VitaCpuRole role=VitaCpuRole::Source;
thread_local uint32_t generation=7,sequence=23;
void Peak(std::atomic<uint32_t>& peak,uint32_t value) {
    auto old=peak.load();while(old<value && !peak.compare_exchange_weak(old,value)){}
}
struct Output {unsigned sink;std::string text;};
std::vector<Output> output;
unsigned Sink(unsigned sink){return sink;}
void Write(unsigned sink,const char* text){output.push_back({sink,text});}
uint64_t VitaCpuNow(){return 123456;}
void VitaCpuProfile_Counter(const char*,uint64_t,uint64_t){}
#include "renderer_trace_functions.inc"

void drain() {
    unsigned source=perfRecords.Count(),render=renderTextRecords.Count();bool flush[6]{};
    while(source || render) {
        DrainTextRecords(perfRecords,source,flush);
        DrainTextRecords(renderTextRecords,render,flush);
    }
    assert(!failed);
}
void tests() {
    // Render errors reach Script with escaped JSON and the original context.
    role=VitaCpuRole::Render;
    VitaCpuProfile_Trace("shader-compiler-message","source=123","bad \"type\"\nline\\end");
    assert(renderTextRecords.Count()==1 && perfRecords.Count()==0 && dropped==0);
    drain();assert(output.size()==1 && output[0].sink==3);
    assert(output[0].text.find("bad \\\"type\\\"\\u000aline\\\\end")!=std::string::npos);
    assert(output[0].text.find("\"generation\":7,\"sequence\":23")!=std::string::npos);
    // Only the renderer's Script sink is permitted; other workers cannot enter.
    assert(!VitaCpuProfile_AppendLog(VitaCpuLog::Replay,"x",1));
    role=VitaCpuRole::Background;
    assert(!VitaCpuProfile_AppendLog(VitaCpuLog::Script,"x",1));
    assert(dropped==2 && !renderTextRecords.Count());
    role=VitaCpuRole::Render;
    // Delayed consumer: every slot is usable, overflow is explicit, no overwrite.
    for(unsigned i=0;i<64;++i) {
        const auto text=std::to_string(i);
        assert(VitaCpuProfile_AppendLog(VitaCpuLog::Script,text.c_str(),uint32_t(text.size()),i==63));
    }
    assert(!VitaCpuProfile_AppendLog(VitaCpuLog::Script,"overflow",8));
    assert(dropped==3 && textQueueDropped==1 && textQueuePeak==64);
    role=VitaCpuRole::Source;
    assert(VitaCpuProfile_AppendLog(VitaCpuLog::Replay,"source",6));
    unsigned render=renderTextRecords.Count();bool flush[6]{};
    output.clear();assert(DrainTextRecords(renderTextRecords,render,flush));
    assert(render==32 && !flush[3] && perfRecords.Count()==1);
    assert(DrainTextRecords(renderTextRecords,render,flush));assert(render==0 && flush[3]);
    for(unsigned i=0;i<64;++i)assert(output[i].text==std::to_string(i));
    // Producer stopped: final cohort drains both queues without requiring more input.
    active=false;drain();assert(output.back().sink==5 && output.back().text=="source");
    assert(!VitaCpuProfile_AppendLog(VitaCpuLog::Script,"stopped",7));
    active=true;output.clear();
    // Independent source and render producers with a delayed/concurrent recorder.
    constexpr unsigned count=2000;
    auto producer=[](VitaCpuRole owner,const char* prefix) {
        role=owner;
        for(unsigned i=0;i<count;++i) {
            const auto text=std::string(prefix)+std::to_string(i);
            while((owner==VitaCpuRole::Render?renderTextRecords.Count():perfRecords.Count())>=
                  (owner==VitaCpuRole::Render?64u:256u))std::this_thread::yield();
            assert(VitaCpuProfile_AppendLog(VitaCpuLog::Script,text.c_str(),uint32_t(text.size())));
        }
    };
    const auto before=dropped.load();
    std::thread source(producer,VitaCpuRole::Source,"s"),renderer(producer,VitaCpuRole::Render,"r");
    while(output.size()<2*count){drain();std::this_thread::yield();}
    source.join();renderer.join();drain();assert(dropped==before);
    unsigned s=0,r=0;
    for(const auto& item:output) {
        assert(item.sink==3);
        if(item.text[0]=='s')assert(item.text=="s"+std::to_string(s++));
        else assert(item.text=="r"+std::to_string(r++));
    }
    assert(s==count && r==count && !perfRecords.Count() && !renderTextRecords.Count());
}
}
int main() {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
#endif
    kisak::vita::tests();
}
