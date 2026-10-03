// Exercise the actual bootstrap package gate and its loading order on retained
// packs. The scene parser below validates wire bytes; no GPU/source VM is run.
#include <vita/vita_content_manifest.h>
#include <vita/vita_pack_layout.h>
#include <vita/vita_map_assets.h>
#include <vita/vita_shipment_collision.h>
#include <vita/vita_shipment_entities.h>
#include <vita/vita_shipment_weapons.h>
#include <vita/vita_model_bundle.h>
#include <vita/vita_animation_bundle.h>
#include <vita/vita_material_bundle.h>
#include <vita/vita_rawfile_bundle.h>
#include <vita/vita_image_bundle.h>
#include <vita/vita_effect_bundle.h>
#include <vita/vita_environment.h>
#include <vita/vita_impact_fx_bundle.h>
#include <vita/vita_hud_bundle.h>
#include <vita/vita_font_bundle.h>
#include <vita/vita_localize_bundle.h>
#include <vita/vita_stringtable_bundle.h>
#include <vita/vita_menu_bundle.h>
#include <vita/vita_sound_bundle.h>
#include <vita/vita_primary_lights.h>
#include <vita/vita_cpu_file.h>
#include <vita/vita_cpu_profile.h>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <vector>

using namespace kisak::vita;
namespace kisak::vita {
void VitaCpuProfile_Record(VitaCpuEvent) {}
void VitaScriptServicesTrace(const char* stage,const char* subject,const char* detail="") {
    std::printf("%s: %s %s\n",stage,subject,detail);
}
}
#define sceClibPrintf(...) std::printf(__VA_ARGS__)
std::vector<uint8_t> layoutIndex;
VitaPackLayoutError InstalledPackLayout(const char* path,uint32_t mapIndex) {
    if(layoutIndex.empty())return VitaPackLayoutError::Ok;
    FILE* input=std::fopen(path,"rb");
    const auto result=VitaPackLayoutProbeFile(layoutIndex.data(),layoutIndex.size(),mapIndex,input);
    if(input)std::fclose(input);
    return result;
}
struct ShipmentPreview {
    bool loaded=false;
    struct { bool valid=false; } lightGrid;
    std::vector<uint8_t> vertices;
    VitaEnvironmentState environment;
    std::vector<VitaPrimaryLightRecord> primaryLights;
    uint32_t sunPrimaryLightIndex=0;
};
struct FileContext {
    FILE* file;
    VitaCpuFile worker;
    bool workerRequired=false;
    explicit FileContext(FILE* input,const char* path=nullptr):file(input) {
        if(path){workerRequired=true;worker.Open(path);}
    }
};
bool ReadFile(void* raw,uint64_t offset,void* destination,size_t size) {
    auto* file=static_cast<FileContext*>(raw);
    if(file->workerRequired)return file->worker.Read(offset,destination,size);
    return !std::fseek(file->file,long(offset),SEEK_SET)
        && std::fread(destination,1,size,file->file)==size;
}
bool ReadWorld(const VitaAssetDatabase& db,uint64_t id,uint32_t magic) {
    const auto* chunk=db.Find(VitaAssetType::World,id);
    if(!chunk)return false;
    std::vector<uint8_t> bytes(chunk->rawSize);
    uint32_t found=0;
    if(db.Read(chunk,bytes.data(),bytes.size())!=KvapError::Ok || bytes.size()<4)return false;
    std::memcpy(&found,bytes.data(),4);return found==magic;
}
bool LoadWorldPreview(const VitaAssetDatabase& db,ShipmentPreview* preview,uint64_t id) {
    preview->loaded=ReadWorld(db,id,0x57544956);return preview->loaded;
}
bool LoadShipmentLightGrid(const VitaAssetDatabase& db,ShipmentPreview* preview,uint64_t id) {
    preview->lightGrid.valid=ReadWorld(db,id,0x47544956);return preview->lightGrid.valid;
}
#include "source_primary_lights.inc"
#include "source_package_probe.inc"

int main(int argc,char** argv) {
    if(argc!=2)return 2;
    VitaCpuRegisterThread(VitaCpuRole::Source);
    const std::filesystem::path root=argv[1];
    if(FILE* index=std::fopen((root/"cpu-pack-layout.bin").string().c_str(),"rb")) {
        layoutIndex.resize(16+17*48+1);
        layoutIndex.resize(std::fread(layoutIndex.data(),1,layoutIndex.size(),index));
        std::fclose(index);
    }
    const auto common=(root/"common.kvap").string();
    bool success=true;
    for(unsigned cycle=0;cycle<3 && success;++cycle)for(const unsigned map:{0u,1u,2u}) {
        const auto path=(root/(std::string("mp_")+kVitaMpMaps[map].name+".kvap")).string();
        if(!std::filesystem::exists(path))continue;
        ShipmentPreview preview;ShipmentCollision collision;ShipmentMapEntities entities;
        ShipmentWeaponTable weapons;VitaRawFileBundle raw;VitaEffectBundle effects;
        VitaImpactFxBundle impact;VitaLocalizeBundle localized;VitaStringTableBundle strings;
        VitaMenuBundle menus;VitaSoundBundle sounds;VitaHudBundle hud;VitaImageBundle images;
        VitaMaterialBundle materials;VitaFontBundle fonts;VitaModelBundle models;VitaAnimationBundle animations;
        bool mounted=false;char reason[192]{};
        success=ProbePackage(path.c_str(),&preview,&collision,&entities,&weapons,&raw,&effects,
            &impact,&localized,&strings,&menus,&sounds,&hud,&images,&materials,&fonts,&models,&animations,
            &mounted,map,common.c_str(),reason,sizeof(reason));
        std::printf("\n{\"cycle\":%u,\"map\":\"%s\",\"playable\":%s,\"mounted\":%s,\"reason\":\"%s\"}\n",
            cycle,kVitaMpMaps[map].name,success?"true":"false",mounted?"true":"false",reason);
        std::fflush(stdout);
        if(!success)break;
    }
    VitaCpuFiles_Stop();return success?0:2;
}
