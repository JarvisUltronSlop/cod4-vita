#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_viewmodel_light_cache.h>
#include <vita/vita_render_pass_probe.h>
#include <gfx_vita/gfx_vita_material_family.h>
#include <gfx_vita/gfx_vita_weapon_opaque.h>
#include <cassert>
#include <limits>
using namespace kisak::vita;
static VitaViewModelLightCache cache;
int main() {
    VitaViewModelInputs input{};input.sample.valid=input.world.valid=true;
    input.sample.hasDirectionalColors=true;input.sample.primaryLightWeight=.7f;
    input.world.sunDirection[2]=1;input.world.sunLightColor[0]=.4f;
    for(unsigned i=0;i<168;++i)input.sample.directionalColors[i]=uint8_t(i*17);
    float normal[3]{.2f,.3f,.7f},out[3]{};
    unsigned calls=0;
    auto evaluate=[&](const float* n,float* o){++calls;return PreparedModelLight(input.sample,input.world).Evaluate(n,o);};
    assert(cache.Begin(1,4,input));assert(cache.Resolve(normal,out,evaluate));assert(calls==1);
    assert(!cache.Begin(1,4,input));assert(cache.Resolve(normal,out,evaluate));assert(calls==1 && cache.Hits()==1);
    input.projection.cameraOrigin[0]=9; // irrelevant to response; normal is independently keyed
    assert(!cache.Begin(1,4,input));assert(cache.Resolve(normal,out,evaluate));assert(calls==1);
    // Every consumed light field invalidates, independent of struct padding.
    for(unsigned field=0;field<180;++field) {
        auto changed=input;
        if(field<168)changed.sample.directionalColors[field]^=1;
        else if(field<171)changed.sample.color[field-168]=1;
        else if(field<174)changed.world.sunDirection[field-171]+=.01f;
        else if(field<177)changed.world.sunLightColor[field-174]+=.01f;
        else if(field==177)changed.sample.primaryLightWeight=.8f;
        else if(field==178)changed.sample.valid=false;
        else changed.sample.hasDirectionalColors=false;
        cache.Begin(1,4,input);assert(cache.Begin(1,4,changed));
    }
    cache.Begin(1,4,input);auto changed=input;changed.world.valid=false;assert(cache.Begin(1,4,changed));
    cache.Begin(1,4,input);assert(cache.Begin(2,4,input));assert(cache.Begin(2,5,input));
    cache.Invalidate();assert(cache.Begin(2,5,input));
    // Populate well beyond capacity, repeat normals, and change sample values.
    // Every successful result must exactly match the unchanged evaluator.
    for(unsigned frame=0;frame<4;++frame) {
        if(frame==2)input.sample.directionalColors[27]^=19;
        cache.Begin(2,5,input);PreparedModelLight prepared(input.sample,input.world);
        for(unsigned i=0;i<20000;++i) {
            float n[3]{float(i%113)-56,float((i/113)%97)-48,float(i%37)-18};
            float expected[3]{},actual[3]{};
            const auto eval=[&](const float* a,float* b){return prepared.Evaluate(a,b);};
            const bool result=eval(n,expected);assert(cache.Resolve(n,actual,eval)==result);
            if(result)assert(std::memcmp(actual,expected,sizeof(actual))==0);
        }
    }
    // Failed/nonfinite evaluations are never installed as successful cache hits.
    input.sample.primaryLightWeight=std::numeric_limits<float>::quiet_NaN();
    cache.Begin(2,5,input);assert(!cache.Resolve(normal,out,evaluate));assert(!cache.Resolve(normal,out,evaluate));assert(cache.Hits()==0 && cache.Misses()==2);
    for(unsigned phase=0;phase<5;++phase) {
        const auto requested=kVitaWeaponPipelineComparison|phase;VitaRenderPassProbe p;p.Begin(requested,0);
        assert(VitaWeaponPipelineCandidate(requested)==(phase==2));
        assert(p.weaponPipeline==(phase>=1 && phase<=3));assert(!p.Fenced() && !p.drawProbe && !p.weaponOpaque);
    }
    for(unsigned mode:{0u,kVitaHudComparison,kVitaFastBaselineDrawProbe,kVitaEarlyAlphaComparison})assert(!VitaWeaponPipelineCandidate(mode|2));
    // The reused shader is only the full normal+specular baked family, with
    // exactly zero valid local lights. Missing maps/local lighting fall back.
    for(unsigned mask=0;mask<256;++mask) {
        const bool eligible=GfxVitaWeaponOpaqueEligible(true,true,false,false,true,false,false,mask&1,true,false,false,0);
        const auto family=GfxVitaMaterialFamily(eligible,true,mask&2,mask&1,mask&4,mask&8,mask&16,mask&32,mask&64,mask&128);
        assert((family==2)==(mask==141));
    }
}
