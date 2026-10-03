#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <vita/vita_cpu_file.h>
#include <vita/vita_cpu_read_probe.h>
#include <vita/vita_cpu_profile.h>
#include <atomic>
#include <cstring>
#include <psp2/kernel/threadmgr/thread.h>
#include <fstream>
#include <filesystem>
#include <vector>
#include <random>
#include <array>
using namespace kisak::vita;
std::atomic<unsigned> readSamples{0};
namespace kisak::vita {
void VitaCpuProfile_Record(VitaCpuEvent event) {
    assert(VitaCpuCurrentRole()==VitaCpuRole::Background);
    assert(event.kind==uint32_t(VitaCpuEventKind::Loading));
    assert(std::strcmp(event.name,"package-read-sample-unavailable")==0);
    assert(event.value0==0 && event.value1<=65536 && event.startUs>0);
    ++readSamples;
}
}
int main() {
    unsigned samples=0;for(uint64_t i=0;i<1024;++i)samples+=VitaCpuSampleRead(i);assert(samples==32);
    assert(VitaCpuSampleRead(0) && VitaCpuSampleRead(32) && !VitaCpuSampleRead(31));
    uint64_t delta=999;assert(VitaCpuReadClockDelta(true,100,true,125,delta) && delta==25);
    assert(!VitaCpuReadClockDelta(false,100,true,125,delta) && delta==0);
    assert(!VitaCpuReadClockDelta(true,100,false,125,delta) && delta==0);
    assert(!VitaCpuReadClockDelta(true,125,true,100,delta) && delta==0);
    assert(VitaCpuReadClockDelta(true,125,true,125,delta) && delta==0);

    CpuTestFailNextThreadStart();assert(!VitaCpuFiles_Start());assert(CpuTestLiveThreads()==0);
    const auto path=std::filesystem::absolute("cpu-file-fixture.bin");
    std::vector<uint8_t> expected(3*65536+37);
    for(size_t i=0;i<expected.size();++i)expected[i]=uint8_t((i*17)^(i>>12));
    {std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(expected.data()),expected.size());}
    {
        VitaCpuFile file;assert(!file.Open("nonexistent-cpu-fixture.bin"));
        assert(file.Open(path.string().c_str()));assert(file.Size()==expected.size());
        std::vector<uint8_t> read(expected.size());assert(file.Read(0,read.data(),read.size()));assert(read==expected);
        std::mt19937 random(42);
        for(unsigned i=0;i<1200;++i) {
            const auto offset=random()%expected.size();const size_t count=std::min<size_t>(random()%8192,expected.size()-offset);
            assert(file.Read(offset,read.data(),count));assert(std::equal(read.begin(),read.begin()+count,expected.begin()+offset));
        }
        assert(!file.Read(expected.size()-1,read.data(),2));assert(!file.Read(0,nullptr,1));
        assert(file.Read(expected.size(),nullptr,0));
        // Closing immediately after a read drains a possibly pending prefetch.
        for(unsigned generation=1;generation<=100;++generation) {
            VitaCpuFiles_SetGeneration(generation);
            assert(file.Read(0,read.data(),1));file.Close();assert(!file.IsOpen());
            assert(file.Open(path.string().c_str()));assert(file.Read(65530,read.data(),30));
            assert(std::equal(read.begin(),read.begin()+30,expected.begin()+65530));
        }
        std::array<VitaCpuFile,16> held;
        for(unsigned i=0;i<15;++i)assert(held[i].Open(path.string().c_str()));
        assert(!held[15].Open(path.string().c_str())); // bounded handle exhaustion
    }
    VitaCpuFiles_Drain();VitaCpuFiles_Stop();assert(CpuTestLiveThreads()==0);
    assert(readSamples>0);
    std::filesystem::remove(path);
}
