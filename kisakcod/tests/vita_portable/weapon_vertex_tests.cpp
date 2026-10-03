#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <limits>
#include <random>
#include <vita/vita_viewmodel_vertices.h>
#include <vita/vita_viewmodel_packet.h>
#include <vita/vita_render_pass_probe.h>
using namespace kisak::vita;
int main() {
    VitaViewModelVertexConverter converter;
    VitaPackedSkinVertex input{};GfxVitaViewmodelVertex actual{};
    VitaViewModelProjection projection{};
    auto noLight=[](const float*,float*){return false;};
    assert(!converter.Convert(input,actual,projection,noLight));
    converter.Prepare();converter.Prepare();assert(converter.Ready());
    static_assert(sizeof(converter)<66000);
    for(unsigned scale=0;scale<256;++scale)for(unsigned byte=0;byte<256;++byte) {
        VitaPackedAttribute attr{};attr.array[3]=uint8_t(scale);
        attr.array[0]=attr.array[1]=attr.array[2]=uint8_t(byte);
        float decoded[3];skinmath::Vec3UnpackUnitVec(attr,decoded);
        for(float value:decoded)assert(converter.Pack(scale,byte)==PackVertexSnorm(value));
    }
    for(unsigned byte=0;byte<256;++byte)assert(PackVertexUnorm8(sourceVertexColors[byte])==byte);
    std::mt19937 random(99173);
    const auto number=[&](){return (int(random()%200001)-100000)/127.0f;};
    for(unsigned test=0;test<100000;++test) {
        input.color.packed=random();input.normal.packed=random();input.tangent.packed=random();input.texCoord.packed=random();
        for(unsigned c=0;c<3;++c) {
            input.xyz[c]=number();projection.viewOffset[c]=number();projection.cameraOrigin[c]=number();
            for(auto& v:projection.cameraAxis[c])v=number()/1000.0f;
        }
        projection.tanHalfFovX=.67f;projection.tanHalfFovY=.53f;
        float response[3]{number()/100.0f,number()/100.0f,number()/100.0f};
        if(test%11==0)response[0]=std::numeric_limits<float>::quiet_NaN();
        if(test%13==0)response[1]=std::numeric_limits<float>::infinity();
        bool resolved=test%7!=0;
        GfxDrawVertex expanded{};assert(ConvertSourceVertex(input,expanded,projection));
        if(resolved)for(unsigned c=0;c<3;++c)expanded.color[c]*=response[c];
        GfxVitaViewmodelVertex expected{};
        std::memcpy(expected.position,expanded.position,sizeof(expected.position));
        std::memcpy(expected.worldPosition,expanded.worldPosition,sizeof(expected.worldPosition));
        std::memcpy(expected.texCoord,expanded.texCoord,sizeof(expected.texCoord));
        for(unsigned c=0;c<3;++c) {
            expected.normal[c]=PackVertexSnorm(expanded.normal[c]);
            expected.tangent[c]=PackVertexSnorm(expanded.tangent[c]);
        }
        for(unsigned c=0;c<4;++c)expected.color[c]=PackVertexUnorm8(expanded.color[c]);
        float originalNormal[3];skinmath::Vec3UnpackUnitVec(input.normal,originalNormal);
        std::memset(&actual,0xa5,sizeof(actual));
        assert(converter.Convert(input,actual,projection,[&](const float* normal,float* out){
            assert(std::memcmp(normal,originalNormal,sizeof(originalNormal))==0);
            std::memcpy(out,response,sizeof(response));return resolved;
        }));
        assert(std::memcmp(&actual,&expected,sizeof(actual))==0);
    }
    projection.tanHalfFovX=0;
    bool called=false;
    assert(!converter.Convert(input,actual,projection,[&](const float*,float*){called=true;return true;}));
    assert(!called);
    for(unsigned phase=0;phase<5;++phase) {
        const auto requested=kVitaWeaponVertexComparison|phase;VitaRenderPassProbe probe;probe.Begin(requested,0);
        assert(VitaWeaponVertexCandidate(requested)==(phase==2));
        assert(VitaWeaponPipelineEnabled(requested)==(phase>=1 && phase<=3));
        assert(probe.weaponVertex==VitaWeaponPipelineEnabled(requested));
        assert(probe.weaponPipeline==probe.weaponVertex && !probe.Fenced());
    }
    for(unsigned flag:{0u,kVitaWeaponPipelineComparison,kVitaFastBaselineDrawProbe,kVitaHudComparison})
        for(unsigned phase=1;phase<=3;++phase)assert(!VitaWeaponVertexCandidate(flag|phase));
}
