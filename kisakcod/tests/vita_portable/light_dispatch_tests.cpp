#ifdef NDEBUG
#undef NDEBUG
#endif
#include <gfx_vita/gfx_vita_light_dispatch.h>
#include <gfx_vita/gfx_vita_pose_light_bounds.h>
#include <gfx/gfx_backend.h>
#include <cassert>
#include <limits>
#include <initializer_list>
using Light=GfxPrimaryLightDescriptor;
struct Lighting {
    unsigned primaryLightCount=2, dynamicLightCount=0, sunPrimaryLightIndex=1;
    Light primaryLights[16]{}, dynamicLights[4]{};
};
int main() {
    Lighting lighting;
    lighting.primaryLights[1].color[0]=1;
    assert(!GfxVitaNeedsLocalLightLoops(lighting)); // Black reserved slot + sun.
    for (unsigned index=0;index<16;++index) {
        lighting.primaryLightCount=16;
        for (unsigned channel=0;channel<3;++channel) {
            for (float value : {1.f, -1.f, std::numeric_limits<float>::min(),
                                std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::infinity()}) {
                lighting.primaryLights[index].color[channel]=value;
                assert(GfxVitaNeedsLocalLightLoops(lighting)==(index!=lighting.sunPrimaryLightIndex));
            }
            lighting.primaryLights[index].color[channel]=0;
        }
    }
    lighting.primaryLightCount=0;
    lighting.dynamicLightCount=4;
    for (unsigned i=0;i<4;++i) {
        lighting.dynamicLights[i].color[2]=0.001f;
        assert(GfxVitaNeedsLocalLightLoops(lighting));
        lighting.dynamicLights[i].color[2]=0;
        assert(!GfxVitaNeedsLocalLightLoops(lighting));
    }
    lighting.dynamicLightCount=5; assert(GfxVitaNeedsLocalLightLoops(lighting));
    lighting.dynamicLightCount=0; lighting.primaryLightCount=17;
    assert(GfxVitaNeedsLocalLightLoops(lighting));
    lighting=Lighting{};
    lighting.sunPrimaryLightIndex=16; assert(GfxVitaNeedsLocalLightLoops(lighting));
    lighting=Lighting{};
    lighting.primaryLights[0].radius=std::numeric_limits<float>::quiet_NaN();
    assert(GfxVitaNeedsLocalLightLoops(lighting));
    lighting=Lighting{};
    lighting.primaryLights[0].origin[0]=std::numeric_limits<float>::infinity();
    assert(GfxVitaNeedsLocalLightLoops(lighting));
    lighting=Lighting{};
    auto& local=lighting.primaryLights[0];local.type=2;local.color[0]=1;local.radius=10;
    GfxDrawLightBounds bounds{{20,-1,-1},{30,1,1},true};
    assert(!GfxVitaBoundsNeedLocalLights(lighting,bounds));
    // Intersection, tangency and a conservative rounding guard retain lights.
    for(float x:{0.f,9.f,10.f,10.5f,11.f}) {
        bounds.minimum[0]=x;assert(GfxVitaBoundsNeedLocalLights(lighting,bounds));
    }
    bounds.minimum[0]=20;
    for(unsigned type:{0u,1u,4u,255u}) {local.type=type;assert(GfxVitaBoundsNeedLocalLights(lighting,bounds));}
    for(unsigned type:{2u,3u,5u,6u}) {local.type=type;assert(!GfxVitaBoundsNeedLocalLights(lighting,bounds));}
    local.origin[0]=25;assert(GfxVitaBoundsNeedLocalLights(lighting,bounds));local.origin[0]=0;
    // Any overlapping FX light blocks the sun-only specialization as well.
    lighting.dynamicLightCount=1;lighting.dynamicLights[0]=local;
    lighting.dynamicLights[0].origin[0]=25;assert(GfxVitaBoundsNeedLocalLights(lighting,bounds));
    lighting.dynamicLights[0].origin[0]=0;assert(!GfxVitaBoundsNeedLocalLights(lighting,bounds));
    lighting.dynamicLightCount=5;assert(GfxVitaBoundsNeedLocalLights(lighting,bounds));lighting.dynamicLightCount=0;
    lighting.primaryLightCount=17;assert(GfxVitaBoundsNeedLocalLights(lighting,bounds));lighting.primaryLightCount=2;
    lighting.sunPrimaryLightIndex=2;assert(GfxVitaBoundsNeedLocalLights(lighting,bounds));lighting.sunPrimaryLightIndex=1;
    bounds.valid=false;assert(GfxVitaBoundsNeedLocalLights(lighting,bounds));bounds.valid=true;
    bounds.minimum[0]=31;assert(GfxVitaBoundsNeedLocalLights(lighting,bounds));bounds.minimum[0]=20;
    const float nan=std::numeric_limits<float>::quiet_NaN();
    for(float* p:{&bounds.minimum[0],&bounds.maximum[2],&local.color[1],&local.direction[0],
                  &local.origin[2],&local.radius,&local.cosHalfFovOuter,&local.shadowVisibility}) {
        const float old=*p;*p=nan;assert(GfxVitaBoundsNeedLocalLights(lighting,bounds));*p=old;
    }
    local.radius=-1;assert(GfxVitaBoundsNeedLocalLights(lighting,bounds));local.radius=10;
    bool loops=true;
    auto pending=bounds;
    {GfxVitaDrawLocalLightScope scope(loops,pending,lighting,true);assert(!loops && !pending.valid);}
    assert(loops);
    {GfxVitaDrawLocalLightScope scope(loops,pending,lighting,true);assert(loops);}
    pending=bounds;
    {GfxVitaDrawLocalLightScope scope(loops,pending,lighting,false);assert(loops && !pending.valid);}
    // Compare every rejected cell with the shader's radial support at corners
    // and interior samples. No rejected position can have positive falloff.
    local.radius=10;local.type=3;
    for(int x=-30;x<=30;x+=3)for(int y=-30;y<=30;y+=3)for(int z=-30;z<=30;z+=3) {
        bounds={{float(x),float(y),float(z)},{float(x+2),float(y+2),float(z+2)},true};
        if(GfxVitaBoundsNeedLocalLights(lighting,bounds))continue;
        for(int i=0;i<3;++i)for(int j=0;j<3;++j)for(int k=0;k<3;++k) {
            const float distance=std::sqrt(float((x+i)*(x+i)+(y+j)*(y+j)+(z+k)*(z+k)));
            assert(distance>=local.radius);
        }
    }
    GfxVitaPoseLightBounds<2> poses;
    GfxDrawVertex vertices[3]{};
    vertices[0].worldPosition[0]=20;vertices[1].worldPosition[0]=30;vertices[2].worldPosition[0]=25;
    assert(poses.Store(7,vertices,3));
    auto copied=poses.Find(7,0,3);assert(copied.valid && copied.minimum[0]==20 && copied.maximum[0]==30);
    assert(!GfxVitaBoundsNeedLocalLights(lighting,copied));
    vertices[0].worldPosition[0]=0; // Bounds own values, never the upload pointer.
    assert(poses.Find(7,0,3).minimum[0]==20);
    assert(!poses.Find(7,0,4).valid && !poses.Find(7,UINT32_MAX,1).valid && !poses.Find(7,0,0).valid);
    assert(poses.Store(7,vertices,3));copied=poses.Find(7,1,2);
    assert(copied.valid && GfxVitaBoundsNeedLocalLights(lighting,copied));
    // A prefix update cannot prove bounds for old tail bytes in another ring slot.
    assert(poses.Store(7,vertices,1));assert(poses.Find(7,0,1).valid && !poses.Find(7,1,1).valid);
    assert(poses.Store(8,vertices,3));assert(!poses.Store(9,vertices,3));
    assert(!poses.Find(9,0,3).valid && poses.Find(7,0,1).valid && poses.Find(8,0,3).valid);
    poses.Forget(7);assert(!poses.Find(7,0,1).valid);assert(poses.Store(9,vertices,3));
    vertices[1].worldPosition[2]=nan;assert(!poses.Store(8,vertices,3));assert(!poses.Find(8,0,3).valid);
    assert(!poses.Store(9,vertices,0));assert(!poses.Find(9,0,1).valid);
    vertices[1].worldPosition[2]=0;assert(poses.Store(7,vertices,3));
    poses.Clear();assert(!poses.Find(7,0,3).valid);
}
