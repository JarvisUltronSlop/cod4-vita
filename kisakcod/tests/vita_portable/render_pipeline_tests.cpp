#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_render_pipeline.h>
#include <gfx_vita/gfx_vita_pose_buffers.h>
#include <cassert>
#include <cstdlib>
#include <array>
#include <map>
#include <vector>
using namespace kisak::vita;
int main() {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
#endif
    const auto ordinary=VitaRenderPipeline::Select(true,false,0);
    const auto test=VitaRenderPipeline::Select(true,false,kVitaWorldScaling|4);
    assert(ordinary.ordinary && !test.ordinary);
    assert(ordinary.weaponPipeline==test.weaponPipeline && ordinary.weaponVertex==test.weaponVertex);
    assert(ordinary.worldAlpha==test.worldAlpha && ordinary.worldAlphaActive==test.worldAlphaActive);
    assert(ordinary.weaponSpecular==test.weaponSpecular && ordinary.prepareWeapon==test.prepareWeapon && ordinary.packWeapon==test.packWeapon);
    assert(ordinary.prepareWeapon && ordinary.packWeapon && ordinary.worldAlphaActive);
    for(auto p:{VitaRenderPipeline::Select(false,false,0),VitaRenderPipeline::Select(false,false,kVitaWorldScaling|4),VitaRenderPipeline::Select(true,true,0)})
        assert(!p.ordinary && !p.prepareWeapon && !p.packWeapon);
    for(uint32_t tag:{0u,1u,2u,3u,kVitaWeaponPipelineComparison|1,kVitaWeaponPipelineComparison|2,kVitaWeaponVertexComparison|1,kVitaWorldScaling|4,kVitaPassIsolation|2}) {
        if(!tag)continue;
        auto p=VitaRenderPipeline::Select(true,false,tag);VitaRenderPassProbe old;old.Begin(tag,0);
        assert(!p.ordinary && p.weaponPipeline==old.weaponPipeline && p.worldAlpha==old.worldAlpha);
        assert(p.prepareWeapon==VitaWeaponPipelineEnabled(tag) && p.packWeapon==VitaWeaponVertexEnabled(tag));
    }
    GfxVitaPoseBuffers pool;uint32_t next=100;std::map<uint32_t,unsigned> live;
    auto create=[&](uint32_t size){live[++next]=size;return next;};
    auto release=[&](uint32_t id){assert(live.erase(id)==1);};
    assert(pool.Reserve(1,32,create,release));assert(live.size()==5 && pool.ExtraBytes()==160 && pool.ShadowBytes()==32);
    assert(!pool.Reserve(1,32,create,release));
    std::array<unsigned char,32> data{};std::map<uint32_t,uint64_t> lastDraw;
    unsigned allocations=next;
    for(uint64_t frame=0;frame<2000;++frame) {
        data[0]=static_cast<unsigned char>(frame);data[31]=77;
        const auto u=pool.Prepare(1,data.data(),data.size(),frame);
        assert(u.handle && !u.fallback && u.bytes==32 && std::memcmp(u.data,data.data(),32)==0);
        if(lastDraw.count(u.handle))assert(frame-lastDraw[u.handle]>4);
        assert(pool.Draw(1,frame)==u.handle);lastDraw[u.handle]=frame;
    }
    assert(next==allocations); // No allocation callback after Reserve.
    data[0]=123;
    auto partial=pool.Prepare(1,data.data(),1,2000);
    assert(static_cast<const unsigned char*>(partial.data)[0]==123 && static_cast<const unsigned char*>(partial.data)[31]==77);
    assert(!pool.Prepare(1,data.data(),33,2000).handle);
    // Delayed consumers/no swaps: all six slots become busy; fallback remains
    // a normal SubData upload for the driver to protect, not a direct map.
    bool fallback=false;
    for(unsigned i=0;i<10;++i) {auto u=pool.Prepare(1,data.data(),32,2000);fallback|=u.fallback;pool.Draw(1,2000);}
    assert(fallback);
    assert(!pool.Prepare(1,data.data(),32,3000).fallback); // pause/resume age
    pool.ReleaseExtra(1,release);assert(live.empty() && !pool.ExtraBytes() && !pool.ShadowBytes());
    auto legacy=pool.Prepare(1,data.data(),1,3000);assert(legacy.handle==1 && legacy.data==data.data() && legacy.fallback);
    // Fail at each physical allocation, checking partial rollback.
    for(unsigned failAt=0;failAt<5;++failAt) {
        unsigned count=0;
        assert(!pool.Reserve(2,32,[&](uint32_t size){return count++==failAt?0u:create(size);},release));
        assert(live.empty() && !pool.ExtraBytes());
    }
    assert(!pool.Reserve(3,UINT32_MAX,create,release));
    for(unsigned i=1;i<=16;++i)assert(pool.Reserve(i,32,create,release));
    assert(!pool.Reserve(17,32,create,release));pool.Clear(release);assert(live.empty());
}
