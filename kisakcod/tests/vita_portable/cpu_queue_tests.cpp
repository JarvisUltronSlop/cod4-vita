#include <vita/vita_cpu_queue.h>
#include <vita/vita_cpu_index.h>
#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <thread>

using namespace kisak::vita;
int main() {
    VitaCpuPointerIndex<2048> index;
    for(uint16_t i=1;i<=1024;++i){auto* cell=index.Find(i*16,i%7*64);assert(cell && !cell->id);*cell={uintptr_t(i*16),uintptr_t(i%7*64),i};}
    for(uint16_t i=1;i<=1024;++i){auto* cell=index.Find(i*16,i%7*64);assert(cell && cell->id==i);}
    index.Reset();assert(index.Find(16,64)->id==0);
    VitaCpuDeadline clock;
    clock.Reset(123,30);
    uint64_t deadline=0;
    for(unsigned i=0;i<108000;++i)deadline=clock.Advance();
    assert(deadline==3600000123ull);
    assert(!clock.Due(deadline));
    clock.Reset(7,20);
    assert(clock.Next()==50007);
    assert(clock.Due(1000007)); // late deadlines remain pending, not discarded
    unsigned ticks=0;
    while(clock.Due(1000007)){clock.Advance();++ticks;}
    assert(ticks==20);

    VitaCpuQueue<uint32_t,8> queue;
    for(uint32_t i=0;i<8;++i)assert(queue.Push(i));
    assert(!queue.Push(99));
    for(uint32_t i=0,value=0;i<8;++i){assert(queue.Pop(&value));assert(value==i);}
    uint32_t ignored=0;assert(!queue.Pop(&ignored));
    std::thread producer([&]{for(uint32_t i=0;i<200000;++i)while(!queue.Push(i))std::this_thread::yield();});
    for(uint32_t i=0,value=0;i<200000;++i){while(!queue.Pop(&value))std::this_thread::yield();assert(value==i);}
    producer.join();

    struct Snapshot {uint32_t sequence=0;std::array<uint32_t,256> body{};};
    std::array<Snapshot,3> slots{};
    VitaCpuTripleOwnership ownership;
    const uint32_t readerSlot=ownership.ReadSlot();
    for(uint32_t i=1;i<100;++i){slots[ownership.WriteSlot()].sequence=i;ownership.Publish();}
    assert(slots[readerSlot].sequence==0); // stalled reader was never overwritten
    assert(ownership.Acquire());assert(slots[ownership.ReadSlot()].sequence==99);
    assert(!ownership.Acquire());
    ownership.Reset();slots={};
    constexpr uint32_t count=50000;
    std::thread snapshotProducer([&]{
        for(uint32_t i=1;i<=count;++i){auto& s=slots[ownership.WriteSlot()];s.sequence=i;s.body.fill(i);ownership.Publish();}
    });
    uint32_t last=0;
    while(last<count){
        if(!ownership.Acquire()){std::this_thread::yield();continue;}
        const auto& s=slots[ownership.ReadSlot()];assert(s.sequence>last);last=s.sequence;
        std::this_thread::yield(); // make overwrite races visible
        for(auto v:s.body)assert(v==last);
    }
    snapshotProducer.join();
}
