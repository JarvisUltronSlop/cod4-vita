#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_cpu_profile.h>
#include <vita/vita_cpu_queue.h>
#include <vita/vita_cpu_recording_policy.h>
#include <vita/vita_cpu_compressed_log.h>
#include <vita/vita_cpu_source_service.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
using SceSize=size_t;
using SceUID=int;
constexpr int SCE_KERNEL_CPU_MASK_USER_2=0x40000;
struct SceKernelThreadInfo {
    size_t size; char name[32]; uint64_t runClocks;
    int currentCpuAffinityMask,lastExecutedCpuId,currentPriority;
    unsigned waitType;
};
int sceKernelGetThreadInfo(int,SceKernelThreadInfo*) { return -1; }
int sceKernelGetThreadStackFreeSize(int) { return -1; }
int sceKernelDelayThread(unsigned us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); return 0; }
namespace {
std::atomic<bool> holdWrite{false},writeEntered{false},holdFlush{false},flushEntered{false},failWrite{false};
std::atomic<unsigned> serviced{0};
std::vector<std::string> lines;
std::string pendingLines[6];
template<class Predicate> void Await(Predicate ready) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!ready()) { assert(std::chrono::steady_clock::now()<deadline); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}
size_t TestWrite(const void* data,size_t size,size_t count,FILE* file) {
    if(holdWrite.load()) { writeEntered=true; Await([]{return !holdWrite.load();}); }
    if(failWrite.exchange(false))return 0;
    // Decode each actual writer emission with the same retained zlib; member
    // boundaries may split JSON records, so preserve tails per output file.
    z_stream stream{};assert(inflateInit(&stream)==Z_OK);
    stream.next_in=(Bytef*)data;stream.avail_in=uInt(size*count);
    unsigned char output[32768];stream.next_out=output;stream.avail_out=sizeof(output);
    assert(inflate(&stream,Z_FINISH)==Z_STREAM_END && stream.avail_in==0);
    auto& pending=pendingLines[uintptr_t(file)-1];
    pending.append(reinterpret_cast<char*>(output),sizeof(output)-stream.avail_out);
    assert(inflateEnd(&stream)==Z_OK);
    for(size_t at;(at=pending.find('\n'))!=std::string::npos;) {lines.push_back(pending.substr(0,at+1));pending.erase(0,at+1);}
    return count;
}
int TestFlush(FILE*) {
    if(holdFlush.load()) { flushEntered=true; Await([]{return !holdFlush.load();}); }
    return 0;
}
int TestClose(FILE*) { return 0; }
}
#include "cpu_recorder_writer.inc"
namespace kisak::vita {
uint64_t VitaCpuNow() {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
void VitaCpuRegisterThread(VitaCpuRole value,int) { role=value; }
VitaCpuRole VitaCpuCurrentRole() { return role; }
}
using namespace kisak::vita;

void Reset() {
    sourceEvents.Reset();perfRecords.Reset();
    for(auto& c:channels)c.queue.Reset();
    sourceQueueDropped=eventQueueDropped=textQueueDropped=0;
    sourceQueuePeak=eventQueuePeak=textQueuePeak=0;
    recorded=dropped=written=0;flushRequest=flushed=0;
    failed=false;stopping=false;active=true;
    scopeTotalCount=0;channel=0;role=VitaCpuRole::Source;
    perf=reinterpret_cast<FILE*>(uintptr_t(1));loading=reinterpret_cast<FILE*>(uintptr_t(2));
    frameRecords=reinterpret_cast<FILE*>(uintptr_t(3));
    for(unsigned i=0;i<3;++i)auxiliary[i]=reinterpret_cast<FILE*>(uintptr_t(i+4));
    flushSchedule={};writeMaxUs=flushMaxUs=flushCount=compressedBytes=0;
    for(auto& log:compressedLogs)assert(log.Initialize());
    for(auto& text:pendingLines)text.clear();
    lines.clear();holdWrite=false;holdFlush=false;writeEntered=false;flushEntered=false;failWrite=false;
    VitaCpuSource_SetService(nullptr);
}
void Emit(uint32_t seq) {
    VitaCpuEvent e{};std::strcpy(e.name,"fixture");e.kind=uint32_t(VitaCpuEventKind::Counter);
    e.startUs=123456;e.generation=7;e.sequence=seq;e.role=0;e.value0=seq;Enqueue(e);
}
void Stop(std::thread& writer) { active=false;stopping=true;writer.join(); }

int main() {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    VitaCpuFlushSchedule schedule;
    for(unsigned i=0;i<6;++i)schedule.Mark(i);
    for(unsigned i=0;i<6;++i) {
        assert(schedule.Periodic(1000000ull*i)==int(i));
        schedule.Clean(i);assert(schedule.Periodic(1000000ull*i)==-1);
    }
    assert(schedule.Periodic(6000000)==-1);
    Reset();holdWrite=true;
    PerfRecord marker{};std::strcpy(marker.text,"{}\n");marker.bytes=3;
    assert(perfRecords.Push(marker));
    std::thread writer([]{Writer(0,nullptr);});
    Await([]{return writeEntered.load();});
    // Storage is fully stalled; source publication must not wait or overwrite.
    for(uint32_t i=1;i<=6000;++i)Emit(i);
    assert(sourceEvents.Count()==6000 && dropped.load()==0 && recorded.load()==6000);
    holdWrite=false;
    VitaCpuProfile_Flush(true);
    assert(sourceEvents.Count()==0 && flushed.load()==flushRequest.load());
    // A new explicit flush must not acknowledge until the dirty sink is flushed.
    // Existing playback service continues while that flush blocks.
    VitaCpuSource_SetService([]{++serviced;});serviced=0;
    holdFlush=true;Emit(6001);
    std::thread release([]{
        Await([]{return flushEntered.load();});
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        assert(flushed.load()<flushRequest.load());holdFlush=false;
    });
    VitaCpuProfile_Flush(true);release.join();assert(serviced.load()>=3);
    VitaCpuSource_SetService(nullptr);
    Stop(writer);
    unsigned seen=0;
    for(const auto& line:lines)if(line.find("\"name\":\"fixture\"")!=std::string::npos) {
        ++seen;
        assert(line.find("\"generation\":7,\"sequence\":"+std::to_string(seen)+",")!=std::string::npos);
        assert(line.find("\"startUs\":123456,")!=std::string::npos);
    }
    assert(seen==6001 && dropped.load()==0);
    assert(compressedBytes<written.load()/3);
    for(const auto& text:pendingLines)assert(text.empty());
    assert(sourceQueuePeak.load()>=6000 && flushMaxUs>=90000);
    Reset();
    for(uint32_t i=1;i<=kCpuSourceEventCapacity+1;++i)Emit(i);
    assert(dropped.load()==1 && sourceQueueDropped.load()==1 && recorded.load()==kCpuSourceEventCapacity);
    std::thread saturated([]{Writer(0,nullptr);});VitaCpuProfile_Flush(true);Stop(saturated);
    assert(sourceEvents.Count()==0);
    Reset();
    // A burst of durability flags used to force one physical flush per record.
    // They now share the batch flush, which still follows every flagged record.
    for(unsigned i=0;i<200;++i) {
        PerfRecord record{};record.flush=true;record.bytes=3;std::strcpy(record.text,"{}\n");
        assert(perfRecords.Push(record));
    }
    for(uint32_t i=1;i<=6000;++i)Emit(i);
    std::thread burst([]{Writer(0,nullptr);});VitaCpuProfile_Flush(true);Stop(burst);
    assert(!dropped.load() && !failed.load() && flushCount<12);
    assert(std::count(lines.begin(),lines.end(),"{}\n")==200);
    Reset();failWrite=true;Emit(1);
    std::thread failedWriter([]{Writer(0,nullptr);});
    VitaCpuProfile_Flush(true);Stop(failedWriter);
    assert(failed.load());
    std::printf("Actual recorder: 6001 ordered events through stalled storage; explicit flush acknowledgement and source audio service; 8192 bounded saturation/overflow; I/O failure drain and rotating dirty-file flush passed. Source queue bytes=%zu\n",sizeof(sourceEvents));
}
