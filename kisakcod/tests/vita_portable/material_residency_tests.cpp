#ifdef NDEBUG
#undef NDEBUG
#endif
#define KISAK_VITA_CPU_RUNTIME 1
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include <vita/vita_cpu_source_service.h>

// Compile the real lookup/rebind loops. Only asset containers, thread role and
// time are supplied by this fixture; audio services the existing source hook.
uint64_t nowUs=1,lastService=1,maxGap=0;
unsigned services=0;
kisak::vita::VitaCpuRole role=kisak::vita::VitaCpuRole::Source;
namespace kisak::vita {
uint64_t VitaCpuNow(){return nowUs;}
VitaCpuRole VitaCpuCurrentRole(){return role;}
struct Material {std::string name,colorMap,normalMap,specularMap;};
struct VitaMaterialBundle {
    std::vector<Material> records;
    bool loaded=true;
    bool Loaded()const{return loaded;}
    const Material* Find(const char* key)const {
        nowUs+=1000; // Artificial expensive lookup, not a performance estimate.
        for(const auto& r:records)if(r.name==key)return &r;
        return nullptr;
    }
};
struct ModelSurface {std::string materialName;};
struct Model {std::string name;std::vector<ModelSurface> surfaces;};
struct VitaModelBundle {
    std::vector<Model> records;
    bool loaded=true;
    bool Loaded()const{return loaded;}
    const auto& Records()const{return records;}
};
}
struct GfxTextureHandle {uint32_t value=0;};
struct ShipmentSurface {char materialName[64]{};GfxTextureHandle normalTexture,specularTexture;};
struct NativeStaticModelBatch {ShipmentSurface surface;};
struct NativeStaticModelChunk {std::vector<NativeStaticModelBatch> batches;};
struct ShipmentPreview {
    std::vector<NativeStaticModelChunk> nativeStaticModelChunks;
    std::vector<std::pair<std::string,GfxTextureHandle>> modelTextureHandles,modelNormalTextureHandles,modelSpecularTextureHandles;
    const char* sourceHandModelName="hands";
};
bool IsRequiredNativeViewModel(const kisak::vita::Model& model,const char* hands){return model.name==hands;}
#include "source_material_residency.inc"

int main() {
    using namespace kisak::vita;
    VitaMaterialBundle materials;
    materials.records={{"full","color","normal","specular"},
        {"missing","color","missingNormal","missingSpecular"},
        {"utility","color","$identitynormalmap","color"},
        {"absentColor","absent","normal","specular"}};
    VitaModelBundle models;
    models.records={{"hands",{{"full"},{"missing"},{"utility"},{"absentColor"},{"unknown"},{""}}},
                    {"optional",{{"missing"}}}};
    ShipmentPreview preview;
    preview.modelTextureHandles={{"color",{17}}};
    preview.modelNormalTextureHandles={{"normal",{0}},{"normal",{21}},{"normal",{22}}};
    preview.modelSpecularTextureHandles={{"specular",{31}}};
    NativeStaticModelBatch batch{};
    std::copy_n("full",5,batch.surface.materialName);
    preview.nativeStaticModelChunks.push_back({{batch}});
    auto result=RebindNativeModelSemantics(&preview,&models,&materials);
    assert(result.staticBatches==1 && result.staticNormalBound==1 && result.staticSpecularBound==1);
    assert(result.normalRequired==3 && result.normalMissing==1);
    assert(result.specularRequired==4 && result.specularMissing==1);
    assert(result.optionalNormalMissing==1 && result.optionalSpecularMissing==1);
    const auto& surface=preview.nativeStaticModelChunks[0].batches[0].surface;
    assert(surface.normalTexture.value==21 && surface.specularTexture.value==31);
    // Large audit: source playback keeps getting serviced in both static and
    // dynamic loops, including iterations that exit early for missing assets.
    preview.nativeStaticModelChunks[0].batches.assign(1000,batch);
    models.records[0].surfaces.assign(1000,{"full"});
    models.records[1].surfaces.assign(1000,{"unknown"});
    nowUs=lastService=1;maxGap=services=0;
    VitaCpuSource_SetService(+[]{
        assert(role==VitaCpuRole::Source);
        maxGap=std::max(maxGap,nowUs-lastService);lastService=nowUs;++services;
        VitaCpuSource_Service(); // no recursive audio/control entry
    });
    result=RebindNativeModelSemantics(&preview,&models,&materials);
    assert(services>100 && maxGap<=26000);
    assert(result.normalRequired==2000 && result.normalMissing==0);
    // A render/background caller cannot inherit the source servicing hook.
    const auto priorServices=services;role=VitaCpuRole::Background;
    RebindNativeModelSemantics(&preview,&models,&materials);assert(services==priorServices);
    role=VitaCpuRole::Source;VitaCpuSource_SetService(nullptr);
    RebindNativeModelSemantics(&preview,&models,&materials);assert(services==priorServices);
    models.loaded=false;assert(RebindNativeModelSemantics(&preview,&models,&materials).staticBatches==0);
    assert(RebindNativeModelSemantics(nullptr,&models,&materials).normalRequired==0);
}
