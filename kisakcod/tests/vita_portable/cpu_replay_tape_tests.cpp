#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_cpu_replay_tape.h>
#include <cassert>
#include <fstream>
#include <iterator>
#include <vector>
using kisak::vita::VitaCpuReplayTape;
int main(int argc,char** argv) {
    assert(argc==2);
    std::ifstream input(argv[1],std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
    VitaCpuReplayTape tape;uint32_t elapsed=0;
    VitaCpuReplayTape::Input value;
    assert(!tape.Next(value,elapsed) && !tape.Complete());
    assert(tape.Load(bytes.data(),bytes.size()));
    assert(tape.Count()==1350 && tape.Clock()==1000 && tape.Seed()==0x434f4434);
    uint32_t total=0,fire=0,reload=0,crouch=0,cycle=0,fragEdge=0,look=0;
    for(uint32_t step=1;step<=tape.Count();++step) {
        assert(tape.Next(value,elapsed));
        assert(value.timeMs==step*1000/30 && elapsed>=33 && elapsed<=34);
        total+=elapsed;
        assert(tape.Clock()==1000+total && tape.Index()==step);
        fire+=(value.buttons&1)!=0;reload+=(value.buttons&16)!=0;crouch+=(value.buttons&512)!=0;
        cycle+=(value.actions&1)!=0;fragEdge+=(value.actions&2)!=0;look+=value.yawMilliDegrees!=0;
        if(value.timeMs<15000)assert(!value.buttons && !value.actions && !value.forward && !value.right);
    }
    assert(total==45000 && tape.Complete() && !tape.Next(value,elapsed));
    assert(fire>300 && reload==60 && crouch==120 && look==120 && cycle==1 && fragEdge==1);
    const auto store=[](uint8_t* p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));};
    const auto invalid=[&](std::vector<uint8_t> changed,bool crc=false) {
        if(crc)store(changed.data()+28,VitaCpuReplayTape::Crc(changed.data()+32,changed.size()-32));
        assert(!tape.Load(changed.data(),changed.size()));
        assert(!tape.Next(value,elapsed) && !tape.Complete() && tape.Clock()==0);
    };
    assert(!tape.Load(nullptr,0));
    for(size_t size:{size_t(0),size_t(31),bytes.size()-1})assert(!tape.Load(bytes.data(),size));
    auto changed=bytes;changed.push_back(0);invalid(changed);
    for(unsigned offset:{0u,4u,8u,12u,16u,24u,28u}) {
        changed=bytes;store(changed.data()+offset,0xffffffffu);invalid(changed);
    }
    changed=bytes;changed[44]^=1;invalid(changed); // damaged movement byte
    changed=bytes;store(changed.data()+32,0);invalid(changed,true);
    changed=bytes;store(changed.data()+48,33);invalid(changed,true); // duplicate timestamp
    changed=bytes;store(changed.data()+48,2000);invalid(changed,true); // oversized source step
    changed=bytes;changed[47]=0x80;invalid(changed,true); // unknown input action
    assert(tape.Load(bytes.data(),bytes.size()) && tape.Next(value,elapsed) && tape.Index()==1);
}
