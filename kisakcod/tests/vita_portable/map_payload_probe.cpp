// Host readback of the actual delivered packs. This is not a source VM or
// physical-GPU qualification; it detects incompatible typed payloads early.
#include <vita/vita_content_manifest.h>
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
#include <vita/vita_model_indices.h>
#include <vita/vita_environment.h>
#include <vita/vita_impact_fx_bundle.h>
#include <vita/vita_hud_bundle.h>
#include <vita/vita_font_bundle.h>
#include <vita/vita_localize_bundle.h>
#include <vita/vita_stringtable_bundle.h>
#include <vita/vita_menu_bundle.h>
#include <vita/vita_sound_bundle.h>
#include <vita/vita_primary_lights.h>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <cstring>
#if KISAK_VITA_MAP_WORKER_READ
#include <vita/vita_cpu_file.h>
#include <vita/vita_cpu_profile.h>
namespace kisak::vita { void VitaCpuProfile_Record(VitaCpuEvent) {} }
#endif
using namespace kisak::vita;
struct ShipmentPreview {
    std::vector<VitaPrimaryLightRecord> primaryLights;
    uint32_t sunPrimaryLightIndex=0;
};
#define sceClibPrintf(...) std::printf(__VA_ARGS__)
#include "source_primary_lights.inc"
static void Require(bool ok,const char* stage) {
    if(!ok){std::fprintf(stderr,"Map payload failure: %s\n",stage);std::exit(2);}
}
struct File {
    FILE* handle=nullptr;
    uint64_t readCalls=0,readBytes=0;
#if KISAK_VITA_MAP_WORKER_READ
    VitaCpuFile worker;
#endif
    explicit File(FILE* file,const char* path):handle(file) {
#if KISAK_VITA_MAP_WORKER_READ
        Require(worker.Open(path),"worker file open");
#endif
    }
    ~File(){if(handle)std::fclose(handle);}
};
static bool Read(void* context,uint64_t offset,void* out,size_t bytes) {
    auto& input=*static_cast<File*>(context);
    ++input.readCalls;input.readBytes+=bytes;
#if KISAK_VITA_MAP_WORKER_READ
    return input.worker.Read(offset,out,bytes);
#else
    auto* file=input.handle;
#ifdef _WIN32
    if(_fseeki64(file,offset,SEEK_SET))return false;
#else
    if(fseeko(file,offset,SEEK_SET))return false;
#endif
    return std::fread(out,1,bytes,file)==bytes;
#endif
}
int main(int argc,char** argv) {
#if KISAK_VITA_MAP_WORKER_READ
    VitaCpuRegisterThread(VitaCpuRole::Source);
#endif
    Require(argc==2,"supply content directory");const std::filesystem::path root=argv[1];
    for(unsigned cycle=0;cycle<3;++cycle)for(uint32_t index=0;index<kVitaMpMapCount;++index) {
        if(!VitaMapIsEnabled(index))continue;
        const auto path=root/(std::string("mp_")+kVitaMpMaps[index].name+".kvap");
        if(!std::filesystem::exists(path))continue;
        const auto start=std::chrono::steady_clock::now();
        const auto commonPath=(root/"common.kvap").string();
        File map{std::fopen(path.string().c_str(),"rb"),path.string().c_str()},
            common{std::fopen(commonPath.c_str(),"rb"),commonPath.c_str()};
        Require(map.handle && common.handle,"open");VitaAssetDatabase db;
        Require(db.Mount(Read,&map,std::filesystem::file_size(path))==KvapError::Ok,"mount map");
        Require(db.MountOverlay(Read,&common,std::filesystem::file_size(root/"common.kvap"))==KvapError::Ok,"mount common");
        Require(db.IsPlayableShipment(),"runtime launch compatibility gate");
        VitaEnvironmentState environment;
        Require(LoadVitaEnvironment(db,VitaMapEnvironmentAssetId(index),&environment),"environment");
        ShipmentPreview preview;
        Require(LoadShipmentPrimaryLights(db,&preview,VitaMapPrimaryLightsAssetId(index)),"primary lights");
        VitaEffectBundle selectedEffects;
        Require(selectedEffects.Load(db,VitaMapEffectsAssetId(index)),"selected map effects");
        VitaImpactFxBundle impactFx;Require(impactFx.Load(db),"impact FX");
        VitaHudBundle hud;Require(hud.Load(db,VitaMapHudAssetId(index)),"HUD roles");
        VitaImageBundle mapImages;Require(mapImages.LoadMetadata(db,VitaMapImagesAssetId(index)),"map image metadata");
        for(const char* role:{"weapon_m4","weapon_mp5","weapon_usp","low_health_overlay","compass",
                "compass_ticker","compass_ticker_background","muzzle_flash","ammo_rifle","ammo_pistol",
                "scorebar_allies","scorebar_opfor"}) {
            const auto* image=hud.ImageFor(role);
            Require(image && mapImages.Find(image),role);
        }
        VitaFontBundle fonts;Require(fonts.Load(db),"frontend fonts");
        VitaLocalizeBundle localized;Require(localized.Load(db),"localized strings");
        VitaStringTableBundle strings;Require(strings.Load(db),"string tables");
        VitaMenuBundle menus;Require(menus.Load(db),"menu records");
        VitaSoundBundle sounds;Require(sounds.LoadPackage(path.string().c_str(),(root/"common.kvap").string().c_str()),"sound closure");
        VitaContentManifest manifest{};
        Require(VitaContentManifest_Load(db,VitaContentRole::Map,&manifest)==VitaContentError::Ok && manifest.mapIndex==index,"manifest");
        ShipmentCollision collision;ShipmentMapEntities entities;ShipmentWeaponTable weapons;
        VitaModelBundle models;VitaAnimationBundle animations;VitaMaterialBundle materials;VitaRawFileBundle scripts;
        Require(collision.Load(db,VitaMapCollisionAssetId(index)) && collision.SourceComplete(),"collision");
        Require(entities.Load(db,VitaMapEntitiesAssetId(index)) && !entities.DeathmatchSpawns().empty(),"FFA spawns");
        Require(weapons.Load(db) && weapons.SourceComplete(),"weapons");
        Require(models.Load(db,VitaMapModelsAssetId(index),VitaMapModelLodsAssetId(index)) && models.SourceComplete() && models.RenderLodsLoaded(),"models/LODs");
        Require(animations.Load(db) && animations.SourceComplete(),"animations");
        Require(materials.Load(db,VitaMapMaterialsAssetId(index)),"materials");
        Require(scripts.Load(db),"scripts");
        const auto script=std::string("maps/mp/mp_")+kVitaMpMaps[index].name+".gsc";
        Require(scripts.Find(script.c_str()),"selected map script");
        uint64_t sharedIndices=0,copiedIndices=0;
        for(const auto& model:models.Records()) for(const auto& surface:model.surfaces) {
            bool borrowed=false;void* owned=nullptr;
            auto* result=VitaSourceModelIndices(model,surface,true,[&](size_t bytes,size_t)->void* {
                owned=std::malloc(bytes);copiedIndices+=bytes;return owned;
            },&borrowed);
            Require(result!=nullptr,"model source indices");
            for(uint32_t i=0;i<surface.indexCount;++i)
                Require(result[i]==model.indices[surface.indexStart+i]-surface.vertexStart,"source local triangle parity");
            if(borrowed)sharedIndices+=surface.indexCount*2;
            std::free(owned);
        }
        VitaEffectBundle fx;Require(fx.Load(db),"effects");
        uint64_t fxElements=0,fxVelocity=0,fxVisual=0,fxTrail=0;
        for(uint32_t i=0;i<fx.Count();++i)for(const auto& e:fx.Record(i)->elements){
            ++fxElements;fxVelocity+=e.velocitySamples.size()*96;fxVisual+=e.visualSamples.size()*48;
            fxTrail+=e.trailVertices.size()*20+e.trailIndices.size()*2;
        }
        std::printf("{\"map\":\"%s\",\"cycle\":%u,\"sharedModelIndexBytes\":%llu,\"copiedModelIndexBytes\":%llu,\"fxElements\":%llu,\"fxVelocityBytes\":%llu,\"fxVisualBytes\":%llu,\"fxTrailBytes\":%llu}\n",kVitaMpMaps[index].name,cycle,(unsigned long long)sharedIndices,(unsigned long long)copiedIndices,(unsigned long long)fxElements,(unsigned long long)fxVelocity,(unsigned long long)fxVisual,(unsigned long long)fxTrail);
        uint32_t images=0;
        std::vector<std::string> indexedNames;
        uint64_t initialPayloadBytes=0;
        for(uint32_t i=0;i<db.ChunkCount();++i) {
            const auto* chunk=db.Chunk(i);
            if(chunk->type!=uint32_t(KvapChunkType::Image))continue;
            VitaImageBundle metadata;Require(metadata.LoadMetadata(db,chunk->assetId),"image metadata");
            images+=metadata.Count();
            bool colorChunk=chunk->assetId==VitaMapImagesAssetId(index);
            for(uint32_t segment=0;segment<64;++segment)
                colorChunk|=chunk->assetId==VitaMapColorImagesAssetId(index,segment);
            if(colorChunk)for(uint32_t n=0;n<metadata.Count();++n)
                indexedNames.push_back(metadata.Record(n)->name);
            const auto family=chunk->assetId & 0xffffffffff000000ull;
            if(family==0x4d49020000000000ull || family==0x4d49030000000000ull) {
                VitaImageIndex semanticIndex(db);
                Require(semanticIndex.Build(chunk->assetId),"world semantic index/full CRC");
                uint64_t peak=0,total=0;const auto calls=map.readCalls;
                for(uint32_t n=0;n<metadata.Count();++n) {
                    VitaImageRecord image;
                    const auto* name=metadata.Record(n)->name.c_str();
                    Require(semanticIndex.Contains(name) && semanticIndex.Read(name,&image),"world semantic exact payload/CRC");
                    peak=std::max<uint64_t>(peak,image.bytes.size());total+=image.bytes.size();
                }
                Require(map.readCalls-calls==metadata.Count(),"one read per world semantic image");
                std::printf("{\"map\":\"%s\",\"cycle\":%u,\"semanticAssetId\":\"%llx\",\"records\":%u,\"payloadBytes\":%llu,\"peakPayload\":%llu}\n",
                    kVitaMpMaps[index].name,cycle,(unsigned long long)chunk->assetId,metadata.Count(),
                    (unsigned long long)total,(unsigned long long)peak);
            }
            if(chunk->assetId==VitaMapImagesAssetId(index)) {
                std::vector<std::string> initialNames;
                for(uint32_t n=0;n<metadata.Count();++n) {
                    const auto* image=metadata.Record(n);const auto& name=image->name;
                    if((image->flags&4) || name.rfind("*lightmap",0)==0
                        || name.rfind("compass",0)==0 || name.rfind("loadscreen",0)==0
                        || name.rfind("gamefonts",0)==0 || name.rfind("hud_",0)==0
                        || name.rfind("menu_",0)==0 || name.rfind("dtimer",0)==0
                        || name.rfind("background_",0)==0 || name.rfind("minimap",0)==0
                        || name=="overlay_low_health" || name=="explosion_flash"
                        || name=="ammo_counter_bullet" || name=="grenadeicon"
                        || name=="dpad_arrow" || name.rfind("score_bar",0)==0)
                        initialNames.push_back(name);
                }
                const auto count=metadata.Count();
                Require(metadata.LoadSelected(db,chunk->assetId,initialNames,0,16*1024*1024,true),"bounded native initial images");
                Require(metadata.Count()==count,"metadata retained outside staging cap");
                for(const auto& name:initialNames)Require(!metadata.Find(name.c_str())->bytes.empty(),"protected/UI payload retained");
                initialPayloadBytes=metadata.ReleaseAllPayloads();
                Require(initialPayloadBytes<=16*1024*1024,"native payload cap");
            }
        }
        VitaImageIndex colorIndex(db);
        map.readCalls=map.readBytes=0;
        Require(colorIndex.BuildMapColors(index),"checked color index");
        const auto indexCalls=map.readCalls,indexBytes=map.readBytes;
        map.readCalls=map.readBytes=0;
        uint64_t maxPayload=0;
        for(const auto& name:indexedNames) {
            VitaImageRecord image;
            Require(colorIndex.Read(name.c_str(),&image),"direct indexed color payload/CRC");
            maxPayload=std::max<uint64_t>(maxPayload,image.bytes.size());
        }
        Require(map.readCalls==indexedNames.size(),"one direct read per color lookup");
        std::printf("{\"map\":\"%s\",\"cycle\":%u,\"colorIndexRecords\":%u,\"indexReadCalls\":%llu,\"indexReadBytes\":%llu,\"payloadReadCalls\":%llu,\"payloadReadBytes\":%llu,\"peakSinglePayload\":%llu}\n",
            kVitaMpMaps[index].name,cycle,colorIndex.Count(),(unsigned long long)indexCalls,
            (unsigned long long)indexBytes,(unsigned long long)map.readCalls,
            (unsigned long long)map.readBytes,(unsigned long long)maxPayload);
        // Finite, bounded query outcomes at every exported FFA start; exact
        // source Pmove/RNG/event parity is a separate gameplay acceptance gate.
        uint32_t collisionHits=0;
        for(const auto& spawn:entities.DeathmatchSpawns()) {
            float end[3]={spawn.origin[0],spawn.origin[1],spawn.origin[2]-128};ShipmentCollisionTrace trace;
            const bool hit=collision.TraceLine(spawn.origin,end,1,&trace);
            collisionHits+=hit;
            Require(hit || (!trace.startSolid && trace.fraction==1),"collision hit/result agreement");
            Require(std::isfinite(trace.fraction) && trace.fraction>=0 && trace.fraction<=1,"finite collision result");
        }
        const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
        std::printf("{\"map\":\"%s\",\"cycle\":%u,\"hostReadbackMs\":%lld,\"spawns\":%zu,\"collisionHits\":%u,\"brushes\":%u,\"models\":%u,\"animations\":%u,\"images\":%u,\"initialPayloadBytes\":%llu,\"sourceVmExercised\":false}\n",
            kVitaMpMaps[index].name,cycle,(long long)ms,entities.DeathmatchSpawns().size(),collisionHits,collision.BrushCount(),models.Count(),animations.Count(),images,(unsigned long long)initialPayloadBytes);
        std::fflush(stdout);
    }
#if KISAK_VITA_MAP_WORKER_READ
    Require(VitaCpuProfile_Read().dropped==0,"worker read invalidation");
    VitaCpuFiles_Stop();
#endif
}
