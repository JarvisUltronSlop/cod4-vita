#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <array>
#include <vector>
#include <unordered_map>
#include <new>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#include <vita/vita_source_registry.h>
#include <vita/vita_ordered_brush_groups.h>

namespace {
std::unordered_map<void*,size_t> live;
size_t largest=0;
size_t maximumExtent=1024*1024;
bool deny=false;
void* Fragmented(size_t bytes) {
    // Model a heap with ample total free space but no one-MiB-plus extent.
    if(deny || bytes>maximumExtent)return nullptr;
    void* p=std::malloc(bytes);if(p){live[p]=bytes;largest=std::max(largest,bytes);}return p;
}
void Release(void* p){assert(live.erase(p)==1);std::free(p);}
struct Root {uint32_t state=0;} cm,comWorld;
unsigned stringReleases=0;
const void* stringNode=nullptr;
void SL_RemoveRefToString(uint16_t) {
    bool owned=false;
    for(auto [p,size]:live) {
        auto a=reinterpret_cast<uintptr_t>(stringNode),b=reinterpret_cast<uintptr_t>(p);
        owned|=a>=b && a-b<size;
    }
    assert(owned);++stringReleases;
}
}
namespace kisak::vita {
void VitaCpuProfile_Counter(const char*,uint64_t,uint64_t) {}
}
#include "registry_storage_methods.inc"

namespace kisak::vita {
struct VitaRegistryTestAccess {
    static void Inject(VitaAssetRegistry& r) {
        r.storage_.~VitaRegistryStorage();new(&r.storage_)VitaRegistryStorage(Fragmented,Release);
        r.mapRootStorage_.~VitaRegistryStorage();new(&r.mapRootStorage_)VitaRegistryStorage(Fragmented,Release);
    }
    static size_t Mark(VitaAssetRegistry& r){return r.cursor_;}
    static void Rollback(VitaAssetRegistry& r,size_t mark){r.cursor_=mark;}
    static void RootMode(VitaAssetRegistry& r,bool on){r.allocatingMapRoots_=on;if(on)r.mapRootCapacity_=512*1024;}
    static void String(VitaAssetRegistry& r) {
        auto* ref=static_cast<VitaAssetRegistry::ScriptStringRef*>(r.Allocate(sizeof(VitaAssetRegistry::ScriptStringRef),alignof(VitaAssetRegistry::ScriptStringRef)));
        assert(ref);ref->value=7;ref->previous=r.scriptStringRefs_;r.scriptStringRefs_=ref;stringNode=ref;
    }
    static void Mounted(VitaAssetRegistry& r){r.mapRootsMounted_=true;cm.state=comWorld.state=42;}
};
}
int main() {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
    _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
#endif
    using namespace kisak::vita;
    assert(!Fragmented(14*1024*1024));
    for(unsigned cycle=0;cycle<10;++cycle) {
        VitaAssetRegistry registry;VitaRegistryTestAccess::Inject(registry);
        deny=true;assert(!registry.Initialize(14*1024*1024));assert(live.empty());deny=false;
        assert(registry.Initialize(14*1024*1024));
        const auto initial=registry.Stats();
        struct Record {unsigned char* p;size_t n;unsigned char value;};std::vector<Record> records;
        for(unsigned i=0;i<1000;++i) {
            const size_t bytes=i%50==0?512*1024:31;
            const size_t alignment=size_t(1)<<(i%7);
            const size_t expected=((VitaRegistryTestAccess::Mark(registry)+alignment-1)&~(alignment-1))+bytes;
            auto* p=static_cast<unsigned char*>(registry.Allocate(bytes,alignment));
            assert(p && reinterpret_cast<uintptr_t>(p)%alignment==0);
            for(size_t j=0;j<bytes;++j)assert(p[j]==0);
            assert(registry.Stats().residentBytes==expected);
            std::memset(p,int(i%251+1),bytes);records.push_back({p,bytes,static_cast<unsigned char>(i%251+1)});
        }
        const auto before=registry.Stats();
        deny=true;assert(!registry.Allocate(512*1024,16));deny=false;
        assert(registry.Stats().residentBytes==before.residentBytes && registry.Stats().backingBytes==before.backingBytes);
        assert(!registry.Allocate(14*1024*1024,16));assert(registry.AllocationLimitReached());
        assert(!registry.Allocate(10,3));
        const auto mark=VitaRegistryTestAccess::Mark(registry);
        assert(registry.Allocate(100,16));assert(registry.Allocate(100,16));
        assert(!registry.AllocationLimitReached());
        VitaRegistryTestAccess::Rollback(registry,mark);
        auto* reused=static_cast<unsigned char*>(registry.Allocate(100,16));assert(reused);
        for(unsigned i=0;i<100;++i)assert(!reused[i]);
        assert(registry.Stats().backingBlocks<=before.backingBlocks+1);
        for(auto r:records)for(size_t j=0;j<r.n;++j)assert(r.p[j]==r.value);
        VitaRegistryTestAccess::RootMode(registry,true);
        assert(registry.Allocate(128*1024,64));VitaRegistryTestAccess::RootMode(registry,false);
        VitaRegistryTestAccess::String(registry);VitaRegistryTestAccess::Mounted(registry);
        const unsigned releases=stringReleases;
        registry.Reset(VitaAssetLifetime::Map);
        assert(stringReleases==releases+1 && !cm.state && !comWorld.state);
        assert(registry.Stats().residentBytes==initial.residentBytes);
        assert(registry.Stats().backingBytes==initial.backingBytes && registry.Stats().backingBlocks==1);
        assert(registry.Allocate(256,16));registry.Destroy();assert(live.empty());
    }
    // Exact logical-boundary allocations, atomic failure and invalid rewind.
    VitaRegistryStorage s(Fragmented,Release);size_t cursor=0;
    assert(s.Allocate(1,4096,1,cursor));assert(cursor==1);assert(!s.Allocate(1,1,1,cursor));
    assert(!s.Rewind(2));assert(s.Mark()==1);s.Clear();assert(live.empty());
    // Model-sized arrays must not strand the current small-record page.
    // The sixth capture failed a 43,008-byte array at the backing allowance.
    for(unsigned cycle=0;cycle<10;++cycle) {
        VitaAssetRegistry registry;VitaRegistryTestAccess::Inject(registry);
        assert(registry.Initialize(14*1024*1024));
        std::vector<unsigned char*> small,large;
        size_t middle=0;
        for(unsigned i=0;i<300;++i) {
            if(i==150)middle=VitaRegistryTestAccess::Mark(registry);
            auto* a=static_cast<unsigned char*>(registry.Allocate(32,16));
            auto* b=static_cast<unsigned char*>(registry.Allocate(43008,16));
            assert(a && b);
            assert(a[0]==0 && b[0]==0 && b[43007]==0);
            std::memset(a,21,32);std::memset(b,42,43008);
            small.push_back(a);large.push_back(b);
        }
        const auto stats=registry.Stats();
        assert(stats.backingBytes<=stats.residentBytes+256*1024);
        VitaRegistryTestAccess::Rollback(registry,middle);
        assert(registry.Allocate(128,64));
        for(unsigned i=0;i<150;++i) {
            for(unsigned j=0;j<32;++j)assert(small[i][j]==21);
            for(unsigned j=0;j<43008;++j)assert(large[i][j]==42);
        }
        registry.Reset(VitaAssetLifetime::Map);assert(registry.Stats().backingBlocks==1);
        registry.Destroy();assert(live.empty());
    }
    // The fifth physical capture reached the former 32,768 per-object cap.
    // Exercise actual registry allocation and reset with far more small records.
    for(unsigned cycle=0;cycle<10;++cycle) {
        VitaAssetRegistry registry;VitaRegistryTestAccess::Inject(registry);
        assert(registry.Initialize(14*1024*1024));
        std::vector<unsigned char*> pointers;
        size_t middle=0;
        for(unsigned i=0;i<100000;++i) {
            if(i==50000)middle=VitaRegistryTestAccess::Mark(registry);
            const size_t bytes=1+i%63,alignment=size_t(1)<<(i%5);
            auto* p=static_cast<unsigned char*>(registry.Allocate(bytes,alignment));
            assert(p && reinterpret_cast<uintptr_t>(p)%alignment==0);
            for(size_t j=0;j<bytes;++j)assert(p[j]==0);
            std::memset(p,int(i%251+1),bytes);pointers.push_back(p);
        }
        const auto stats=registry.Stats();
        assert(stats.backingBlocks<100);
        assert(stats.backingBytes<=stats.residentBytes+VitaRegistryStorage::kMaximumOverhead);
        for(unsigned i=0;i<pointers.size();++i)
            for(size_t j=0;j<1+i%63;++j)assert(pointers[i][j]==i%251+1);
        VitaRegistryTestAccess::Rollback(registry,middle);
        auto* zero=static_cast<unsigned char*>(registry.Allocate(128*1024,4096));assert(zero);
        for(unsigned i=0;i<128*1024;++i)assert(zero[i]==0);
        for(unsigned i=0;i<50000;++i)
            for(size_t j=0;j<1+i%63;++j)assert(pointers[i][j]==i%251+1);
        registry.Reset(VitaAssetLifetime::Map);assert(registry.Stats().backingBlocks==1);
        registry.Destroy();assert(live.empty());
    }
    // Near-threshold objects must fit the logical budget without page tails
    // consuming the backing allowance. Larger arrays use exact pages.
    for(const auto [bytes,count]:std::array<std::pair<size_t,unsigned>,2>{{{4095,3000},{16383,800}}}) {
        cursor=0;
        for(unsigned i=0;i<count;++i)assert(s.Allocate(bytes,1,14*1024*1024,cursor));
        assert(s.Reserved()<=cursor+VitaRegistryStorage::kMaximumOverhead);
        s.Clear();assert(live.empty());
    }
    // Exercise page creation and release with repeated revisits to old small
    // pages, alignment changes and arbitrary valid construction checkpoints.
    struct Saved {unsigned char* p;size_t bytes,end;unsigned char value;};
    std::vector<Saved> saved;uint32_t random=42;cursor=0;
    for(unsigned i=0;i<2000;++i) {
        random=random*1664525u+1013904223u;
        if(i%17==0 && !saved.empty()) {
            const size_t keep=random%saved.size();
            cursor=keep?saved[keep-1].end:0;
            assert(s.Rewind(cursor));saved.resize(keep);
        }
        const size_t bytes=std::array<size_t,6>{31,4095,16383,43008,65536,1}[random%6];
        const size_t alignment=size_t(1)<<((random>>8)%13);
        auto* p=static_cast<unsigned char*>(s.Allocate(bytes,alignment,14*1024*1024,cursor));
        assert(p && reinterpret_cast<uintptr_t>(p)%alignment==0);
        for(size_t j=0;j<bytes;++j)assert(p[j]==0);
        const auto value=static_cast<unsigned char>(i%251+1);
        std::memset(p,value,bytes);saved.push_back({p,bytes,cursor,value});
        for(const auto& record:saved) {
            assert(record.p[0]==record.value && record.p[record.bytes/2]==record.value && record.p[record.bytes-1]==record.value);
        }
    }
    s.Clear();assert(live.empty());
    // Strong alignment, exact capacity and metadata/unused-space bounds.
    cursor=0;
    while(auto* p=s.Allocate(1,4096,96*1024*1024,cursor)) {
        assert(reinterpret_cast<uintptr_t>(p)%4096==0);
        assert(s.Reserved()<=cursor+VitaRegistryStorage::kMaximumOverhead);
    }
    assert(s.LastFailure()==VitaRegistryStorage::Failure::BackingBudget);
    assert(s.Reserved()<=cursor+VitaRegistryStorage::kMaximumOverhead);
    s.Clear();assert(live.empty());
    // A heap unable to supply a normal page can still supply an exact object.
    maximumExtent=1024;cursor=0;
    assert(s.Allocate(16,16,14*1024*1024,cursor));
    assert(s.MaximumRequest()<=maximumExtent);
    assert(!s.Allocate(2048,16,14*1024*1024,cursor));
    assert(s.LastFailure()==VitaRegistryStorage::Failure::Heap && cursor==16);
    maximumExtent=1024*1024;
    s.Clear();assert(live.empty());
    assert(largest<=1024*1024);
}
