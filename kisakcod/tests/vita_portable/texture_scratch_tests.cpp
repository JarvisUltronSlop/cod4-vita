#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_texture_scratch.h>
#include <cassert>
#include <cstring>
#include <initializer_list>
using kisak::vita::VitaTextureScratch;
static bool busy=false,deny=false;
static unsigned resizes=0,releases=0,waits=0;
static void* Resize(void* old,size_t bytes) {
    assert(!busy);++resizes;
    if(deny)return nullptr;
    return std::realloc(old,bytes);
}
static void Release(void* data){assert(!busy);++releases;std::free(data);}
static bool Finish(){++waits;busy=false;return true;}
int main() {
    VitaTextureScratch scratch(Resize,Release);
    for(unsigned cycle=0;cycle<10;++cycle) {
        const unsigned before=resizes;
        assert(!scratch.Reserve(0,Finish));
        assert(!scratch.Reserve(VitaTextureScratch::kLimit+1,Finish));
        assert(resizes==before && !scratch.Data() && !scratch.Capacity());
        busy=true;assert(scratch.Reserve(65537,Finish));
        assert(!busy && scratch.Capacity()==131072);
        std::memset(scratch.Data(),0x6a,65537);
        auto* old=scratch.Data();const auto count=resizes,waitCount=waits;
        assert(scratch.Reserve(87040,Finish) && scratch.Data()==old);
        assert(resizes==count && waits==waitCount);
        // Failed synchronization and allocation both preserve the old buffer.
        busy=true;assert(!scratch.Reserve(200000,[]{return false;}));
        assert(scratch.Data()==old && busy && resizes==count);
        deny=true;assert(!scratch.Reserve(200000,Finish));deny=false;
        assert(scratch.Data()==old && scratch.Capacity()==131072 && !busy);
        for(unsigned i=0;i<65537;++i)assert(scratch.Data()[i]==0x6a);
        // Maximum observed payload from the run80 record stream. Every smaller
        // texture can be paged using the preallocated buffer without growth.
        busy=true;assert(scratch.Reserve(1398128,Finish));
        assert(scratch.Capacity()==1441792);
        const auto stagingCalls=resizes;
        for(auto n:{1,174776,349552,699064,1398128})assert(scratch.Reserve(n,Finish));
        assert(resizes==stagingCalls);
        assert(scratch.Reserve(VitaTextureScratch::kLimit,Finish));
        assert(scratch.Capacity()==VitaTextureScratch::kLimit);
        // An in-flight reader/transfer prevents release. Retry only when drained.
        busy=true;const auto released=releases;
        assert(!scratch.Clear([]{return false;}) && releases==released && scratch.Data());
        assert(scratch.Clear(Finish) && !scratch.Data() && !scratch.Capacity());
        assert(releases==released+1);
        assert(scratch.Clear(Finish) && releases==released+1);
    }
}
