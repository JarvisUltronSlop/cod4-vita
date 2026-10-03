#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <vector>
#include <thread>
#include <gfx_vita/gfx_vita_lightmap_layout.h>
#include <vita/vita_render_pass_probe.h>
// Independent decoder: repeatedly select quadrants, then advance square
// blocks along the long axis. Does not use the production Morton function.
static void Decode(size_t i,unsigned w,unsigned h,unsigned& x,unsigned& y) {
    const unsigned side=w<h?w:h;const size_t block=i/(side*side);i%=side*side;
    x=unsigned(block%(w/side))*side;y=unsigned(block/(w/side))*side;
    for(unsigned step=side/2;step;step/=2) {
        const size_t quadrant=i/(step*step);i%=step*step;
        x+=unsigned(quadrant/2)*step;y+=unsigned(quadrant%2)*step;
    }
}
static void BatchFailureTests() {
    const uint32_t ids[]={11,22,33};
    // Inject exhaustion at every allocation. Earlier copies are reclaimed,
    // original leases stay usable, and a later retry can succeed.
    for(unsigned fail=0;fail<3;++fail) {
        std::vector<uint32_t> live;unsigned calls=0,rollbacks=0;
        assert(!GfxVitaPrepareLightmapBatch(ids,3,[&](uint32_t id){
            if(calls++==fail)return false;live.push_back(id);return true;
        },[&]{live.clear();++rollbacks;}));
        assert(live.empty() && calls==fail+1 && rollbacks==1);
        assert(GfxVitaPrepareLightmapBatch(ids,3,[&](uint32_t id){live.push_back(id);return true;},[&]{assert(false);}));
        assert(live==std::vector<uint32_t>({11,22,33}));
    }
    unsigned calls=0,rollbacks=0;
    const auto prepare=[&](uint32_t){++calls;return true;};const auto rollback=[&]{++rollbacks;};
    const uint32_t duplicate[]={1,2,1},zero[]={1,0,3},full[]={1,2,3,4,5,6,7,8};
    assert(!GfxVitaPrepareLightmapBatch(nullptr,1,prepare,rollback));
    assert(!GfxVitaPrepareLightmapBatch(ids,0,prepare,rollback));
    assert(!GfxVitaPrepareLightmapBatch(ids,9,prepare,rollback));
    assert(!GfxVitaPrepareLightmapBatch(duplicate,3,prepare,rollback));
    assert(!GfxVitaPrepareLightmapBatch(zero,3,prepare,rollback));
    assert(calls==0 && rollbacks==5);
    assert(GfxVitaPrepareLightmapBatch(full,8,prepare,rollback) && calls==8 && rollbacks==5);
}
int main() {
    BatchFailureTests();
    GfxVitaLightmapTransitionClock clock;
    assert(clock.PausedUs(100)==0);clock.Begin(100);
    assert(clock.PausedUs(90)==0 && clock.PausedUs(150)==50);
    clock.End(180);assert(clock.PausedUs(1000)==80);
    clock.Begin(200);assert(clock.PausedUs(210)==90);clock.End(230);assert(clock.PausedUs(240)==110);
    clock.Reset();assert(clock.PausedUs(1000)==0);
    std::atomic<bool> done{false};
    std::thread writer([&]{for(unsigned i=0;i<10000;++i){clock.Reset();clock.Begin(100);clock.End(150);}done=true;});
    while(!done){const auto paused=clock.PausedUs(200);assert(paused==0 || paused==50 || paused==100);}
    writer.join();

    assert(GfxVitaSamplerUnitValid(false,4) && !GfxVitaSamplerUnitValid(true,4));
    assert(!GfxVitaSamplerUnitValid(false,5) && GfxVitaSamplerUnitValid(true,3));
    for(const auto dims:{std::pair{8u,8u},std::pair{16u,8u},std::pair{8u,16u},std::pair{512u,1024u},std::pair{1024u,1024u}})for(unsigned bpp:{1u,4u}) {
        const auto w=dims.first,h=dims.second,stride=w*bpp+16;
        std::vector<uint8_t> source(stride*h,0xa7),destination(w*h*bpp+32,0xcc);
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w*bpp;++x)source[size_t(y)*stride+x]=uint8_t(x*13+y*37+(x>>8)+(y>>8));
        const auto saved=source;
        assert(GfxVitaSwizzleLightmap(destination.data()+16,w*h*bpp,source.data(),source.size(),w,h,bpp,stride));
        assert(source==saved);
        for(unsigned i=0;i<16;++i)assert(destination[i]==0xcc && destination[destination.size()-1-i]==0xcc);
        for(size_t i=0;i<size_t(w)*h;++i) {
            unsigned x,y;Decode(i,w,h,x,y);assert(x<w && y<h);
            assert(!std::memcmp(destination.data()+16+i*bpp,source.data()+size_t(y)*stride+x*bpp,bpp));
        }
        std::vector<uint8_t> packed(size_t(w)*h*bpp+32,0xda),workspace(kGfxVitaLightmapWorkspaceBytes+32,0xb7);
        for(unsigned y=0;y<h;++y)std::memcpy(packed.data()+16+size_t(y)*w*bpp,source.data()+size_t(y)*stride,w*bpp);
        const auto original=packed;const auto digest=GfxVitaLightmapHash(packed.data()+16,w,h,bpp,false);
        for(unsigned repeat=0;repeat<2;++repeat) {
            assert(GfxVitaReorderLightmap(packed.data()+16,size_t(w)*h*bpp,w,h,bpp,true,workspace.data()+16,kGfxVitaLightmapWorkspaceBytes));
            assert(!std::memcmp(packed.data()+16,destination.data()+16,size_t(w)*h*bpp));
            assert(GfxVitaLightmapHash(packed.data()+16,w,h,bpp,true)==digest);
            assert(GfxVitaReorderLightmap(packed.data()+16,size_t(w)*h*bpp,w,h,bpp,false,workspace.data()+16,kGfxVitaLightmapWorkspaceBytes));
            assert(packed==original && GfxVitaLightmapHash(packed.data()+16,w,h,bpp,false)==digest);
        }
        for(unsigned i=0;i<16;++i)assert(workspace[i]==0xb7 && workspace[workspace.size()-1-i]==0xb7);
        for(bool direction:{false,true}) {
            assert(!GfxVitaReorderLightmap(packed.data()+16,1,w,h,bpp,direction,workspace.data(),workspace.size()));
            assert(!GfxVitaReorderLightmap(packed.data()+16,size_t(w)*h*bpp,w,h,bpp,direction,workspace.data(),1));
            assert(!GfxVitaReorderLightmap(packed.data()+16,size_t(w)*h*bpp,w,h,bpp,direction,packed.data()+17,kGfxVitaLightmapWorkspaceBytes));
            assert(packed==original);
        }
        assert(GfxVitaVerifyLightmap(destination.data()+16,source.data(),w,h,bpp,stride));
        destination[17]^=1;assert(!GfxVitaVerifyLightmap(destination.data()+16,source.data(),w,h,bpp,stride));
        assert(!GfxVitaSwizzleLightmap(destination.data(),1,source.data(),source.size(),w,h,bpp,stride));
        assert(!GfxVitaSwizzleLightmap(destination.data(),destination.size(),source.data(),1,w,h,bpp,stride));
        assert(!GfxVitaSwizzleLightmap(source.data(),source.size(),source.data(),source.size(),w,h,bpp,stride));
    }
    uint8_t dummy=1;
    assert(!GfxVitaReorderLightmap(&dummy,16u*1024*1024,4096,4096,1,true,&dummy,kGfxVitaLightmapWorkspaceBytes));
    assert(dummy==1);
    assert(!GfxVitaLightmapDimensions(12,16,4) && !GfxVitaLightmapDimensions(8192,8,1) && !GfxVitaLightmapDimensions(8,8,2));
    assert(!GfxVitaSwizzleLightmap(nullptr,100,nullptr,100,8,8,1,8));
    using namespace kisak::vita;
    for(unsigned phase=1;phase<=3;++phase) {
        const uint32_t flag=kVitaLightmapLayoutComparison|phase;VitaRenderPassProbe p;p.Begin(flag,0);
        assert(p.phase==phase && p.lightmapLayout && !p.worldLifetime && p.worldAlpha && !p.Fenced());
        assert(VitaWorldAlphaEnabled(flag) && VitaWeaponVertexEnabled(flag) && VitaWeaponSpecularEnabled(flag));
        assert(!p.comparison && !p.workload && !p.drawProbe && !p.material);
    }
    assert(!VitaRenderProbeValid(kVitaLightmapLayoutComparison) && !VitaRenderProbeValid(kVitaLightmapLayoutComparison|4));
    // These high bits now name the exact component tag; phase6 is invalid.
    assert(!VitaRenderProbeValid(kVitaLightmapLayoutComparison|kVitaWorldLifetimeComparison|6));
}
