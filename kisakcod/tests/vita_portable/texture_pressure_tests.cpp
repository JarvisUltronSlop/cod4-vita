#ifdef NDEBUG
#undef NDEBUG
#endif
#include <gfx_vita/gfx_vita_texture_pressure.h>
#include <vita/vita_texture_pool.h>
#include <cassert>
#include <array>

int main() {
    using Result=GfxVitaPageUpload;
    constexpr uint64_t mib=1024*1024;
    uint64_t budget=16*mib, resident=14*mib;
    unsigned attempts=0, evictions=0;
    auto evict=[&] { if(resident<=2*mib)return false;resident-=mib;++evictions;return true; };
    auto used=[&] {return resident;};
    assert(GfxVitaUploadWithPagePressure(2*mib,budget,[&] {
        ++attempts;return resident<=8*mib?Result::Complete:Result::OutOfMemory;
    },evict,used));
    assert(attempts==2 && evictions==7 && budget==9*mib && resident==7*mib);
    // The stricter budget persists and successful uploads do not evict more.
    attempts=evictions=0;
    assert(GfxVitaUploadWithPagePressure(mib,budget,[&] {++attempts;return Result::Complete;},evict,used));
    assert(attempts==1 && !evictions && budget==9*mib);
    // All remaining pages are pinned: no retry without real reclamation.
    resident=2*mib;attempts=0;
    assert(!GfxVitaUploadWithPagePressure(mib,budget,[&] {++attempts;return Result::OutOfMemory;},evict,used));
    assert(attempts==1 && resident==2*mib);
    // Permanent allocation failure is capped at three attempts; pins survive.
    resident=14*mib;budget=16*mib;attempts=evictions=0;
    assert(!GfxVitaUploadWithPagePressure(2*mib,budget,[&] {++attempts;return Result::OutOfMemory;},evict,used));
    assert(attempts==3 && resident==2*mib && evictions==12 && budget==4*mib);
    // Corrupt input/transfer errors must not be retried or evict useful data.
    attempts=evictions=0;
    assert(!GfxVitaUploadWithPagePressure(mib,budget,[&] {++attempts;return Result::Failed;},evict,used));
    assert(attempts==1 && !evictions);
    assert(!GfxVitaUploadWithPagePressure(mib,budget,[]{return Result::OutOfMemory;},[]{return true;},used));

    // Fragmented 16-unit allocator: eight free units, initially no free run of
    // two. Use real shared-image leases, excluding a pinned alias from eviction.
    kisak::vita::VitaTexturePool<8,16> pool;
    std::array<bool,16> occupied{};
    std::array<uint32_t,8> leases{};
    for(unsigned i=0;i<8;++i) {
        kisak::vita::VitaTextureIdentity key{};key.width=i+1;key.bytes=1;key.storageClass=1;
        leases[i]=pool.Insert(key,2*i+1,0,i+1);assert(leases[i]);occupied[2*i]=true;
    }
    const auto pinned=pool.Acquire(*pool.Identity(leases[0]));assert(pinned);
    const auto incoming=[] {return 2u;};
    unsigned partialOwned=0, cleaned=0;
    budget=16;attempts=0;
    assert(GfxVitaUploadWithPagePressure(incoming(),budget,[&] {
        ++attempts;assert(!partialOwned); // Previous partial attempt was retired.
        for(unsigned i=0;i+1<16;++i)if(!occupied[i]&&!occupied[i+1])return Result::Complete;
        ++partialOwned;--partialOwned;++cleaned;return Result::OutOfMemory;
    },[&] {
        uint32_t victim=0;
        pool.VisitResident([&](uint32_t h){if(!victim&&!pool.SameImage(h,pinned))victim=h;});
        if(!victim)return false;
        const auto id=pool.Evict(victim);assert(id);occupied[id-1]=false;return true;
    },[&] {return pool.ResidentBytes();}));
    assert(attempts==2 && cleaned==1 && pool.Resolve(pinned)==1 && pool.Resolve(leases[0])==1);
    assert(pool.ResidentBytes()==4 && pool.LeaseCount()==9 && budget==6);
    assert(pool.Backing(leases[1]) && !pool.Resolve(leases[1]));
    assert(pool.SetResident(leases[1],3)); // Eviction preserves immutable identity/backing.
    assert(pool.Resolve(leases[1])==3);
}
