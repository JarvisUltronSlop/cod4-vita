#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_render_settings.h>
#include <gfx_vita/gfx_vita_target_transaction.h>
#include <cassert>
#include <limits>
#include <algorithm>
#include <vector>
using namespace kisak::vita;
int main() {
    // Regression: a prop-pass reset must not use the 960x544 display inside
    // a smaller FBO. World/prop coordinates must remain coincident after upscale.
    for(auto width:{960u,720u,480u}) {
        const GfxVitaViewportSize world{width,width*544/960}, native{960,544};
        const auto ground=GfxVitaPassViewport(true,true,false,false,false,world,native,native);
        const auto props=GfxVitaPassViewport(true,true,false,false,false,world,native,native);
        assert(ground.width==width && ground.height==world.height);
        assert(props.width==ground.width && props.height==ground.height);
        const auto hands=GfxVitaPassViewport(true,true,true,false,false,world,native,native);
        const auto hud=GfxVitaPassViewport(true,true,true,true,false,world,native,native);
        assert(hands.width==960 && hands.height==544 && hud.width==960 && hud.height==544);
        const auto menu=GfxVitaPassViewport(true,false,false,false,false,world,native,native);
        const auto control=GfxVitaPassViewport(true,true,false,false,true,world,native,native);
        assert(menu.width==960 && control.width==960);
        // Existing resolution benchmarks use a reduced composition target.
        const auto legacy=GfxVitaPassViewport(true,false,false,false,false,{0,0},world,native);
        assert(legacy.width==width && legacy.height==world.height);
        const auto legacyHud=GfxVitaPassViewport(true,false,false,true,false,{0,0},world,native);
        assert(legacyHud.width==960 && legacyHud.height==544);
    }
    VitaRenderSettings settings;
    assert(settings.Width()==720 && settings.Height()==408);
    settings.resolution=VitaWorldResolution::Native; assert(settings.Width()==960 && settings.Height()==544);
    settings.resolution=VitaWorldResolution::Performance; assert(settings.Width()==480 && settings.Height()==272);
    for(const char* name:{"foliage_tree","foliage_shrub","grass","pallet","foliage_grass"})
        assert(VitaReducedGrassModel(name)==(std::strcmp(name,"foliage_grass")==0));
    assert(!VitaReducedGrassModel(nullptr));
    std::vector<uint32_t> placements;
    for(uint32_t i=0;i<10000;++i) placements.push_back(i);
    const auto optional=[](uint32_t i){return VitaOptionalGrassPlacement(i);};
    const auto before=std::count_if(placements.begin(),placements.end(),optional);
    assert(before>2300 && before<2700);
    auto copy=placements;
    auto tail=std::stable_partition(placements.begin(),placements.end(),[&](auto i){return !optional(i);});
    assert(std::all_of(tail,placements.end(),optional));
    std::reverse(copy.begin(),copy.end());
    assert(std::count_if(copy.begin(),copy.end(),optional)==before);
    // Every surface/LOD of the same source placement chooses the same cohort.
    assert(!VitaOmitDistantGrass(false,1024)); assert(VitaOmitDistantGrass(false,1025));
    assert(VitaOmitDistantGrass(true,896)); assert(!VitaOmitDistantGrass(true,895));
    assert(!VitaOmitDistantGrass(false,950)); assert(VitaOmitDistantGrass(true,950));
    assert(!VitaOmitDistantGrass(true,std::numeric_limits<float>::quiet_NaN()));
    // Exercise the actual target transaction with failure at each resource
    // acquisition and during deferred warmup; no partial generation may leak.
    int handles=0, destroys=0, warms=0;
    auto destroy=[&]{handles=0;++destroys;};
    for(int fail=0;fail<5;++fail) {
        bool ok=GfxVitaTargetTransaction(720,408,destroy,[&]{
            for(int i=0;i<4;++i) {++handles;if(i==fail)return false;} return true;
        },[&]{++warms;return fail!=4;});
        assert(!ok && handles==0);
    }
    assert(warms==1);
    for(int cycle=0;cycle<10;++cycle) for(auto w:{960u,720u,480u}) {
        assert(GfxVitaTargetTransaction(w,w*544/960,destroy,[&]{handles=w==960?0:4;return true;},[]{return true;}));
        assert(handles==(w==960?0:4));
    }
    const int previous=destroys;
    assert(!GfxVitaTargetTransaction(719,408,destroy,[]{return true;},[]{return true;}));
    assert(destroys==previous); // Invalid request preserves the live generation.
    destroy(); assert(handles==0);
}
