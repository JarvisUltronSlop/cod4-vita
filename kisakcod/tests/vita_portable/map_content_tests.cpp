#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_content_manifest.h>
#include <vita/vita_map_assets.h>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#ifdef _MSC_VER
#include <crtdbg.h>
#include <cstdlib>
#endif
using namespace kisak::vita;
using Bytes=std::vector<uint8_t>;
template<class T> void Put(Bytes& bytes,const T& value) {
    auto p=reinterpret_cast<const uint8_t*>(&value);bytes.insert(bytes.end(),p,p+sizeof(T));
}
bool Read(void* context,uint64_t offset,void* out,size_t size) {
    const auto& bytes=*static_cast<Bytes*>(context);
    if(offset>bytes.size() || size>bytes.size()-offset)return false;
    std::memcpy(out,bytes.data()+offset,size);return true;
}
Bytes Package(uint64_t id,const std::string& text,KvapChunkType type=KvapChunkType::Manifest) {
    KvapHeader h{};std::memcpy(h.magic,"KVAP",4);h.major=1;h.endian=1;
    h.headerSize=sizeof(h);h.tocOffset=sizeof(h);h.tocCount=1;h.packageId=id;h.sourceHash[0]=42;
    KvapChunk c{};c.type=static_cast<uint32_t>(type);c.assetId=1;c.alignment=16;
    c.offset=sizeof(h)+sizeof(c);c.rawSize=c.storedSize=text.size();c.crc32=KvapCrc32(text.data(),text.size());
    Bytes bytes;Put(bytes,h);Put(bytes,c);bytes.insert(bytes.end(),text.begin(),text.end());return bytes;
}
std::string Manifest(const char* role,uint32_t index=0) {
    return std::string("{\"schemaVersion\":1,\"packageRole\":\"")+role+
        "\",\"certified\":true,\"playable\":true,\"payloadPolicy\":\"runtime-ready-v1\","
        "\"predictedCpuResidentBytes\":1024,\"predictedGpuResidentBytes\":1024,"+
        "\"mapIndex\":"+std::to_string(index)+",\"mapId\":\""+kVitaMpMaps[index].name+"\",\"commonPackageId\":1}";
}
int main() {
#ifdef _MSC_VER
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    auto commonBytes=Package(1,Manifest("common"));VitaAssetDatabase commonDb;
    assert(commonDb.Mount(Read,&commonBytes,commonBytes.size())==KvapError::Ok);
    VitaContentManifest common{};
    assert(VitaContentManifest_Load(commonDb,VitaContentRole::Common,&common)==VitaContentError::Ok);
    for(uint32_t index=0;index<kVitaMpMapCount;++index) {
        auto bytes=Package(2+index,Manifest("map",index));VitaAssetDatabase db;
        assert(db.Mount(Read,&bytes,bytes.size())==KvapError::Ok);
        VitaContentManifest map{};
        const auto error=VitaContentManifest_Load(db,VitaContentRole::Map,&map);
        if(!VitaMapIsEnabled(index)) {assert(error==VitaContentError::MapRejected);continue;}
        assert(error==VitaContentError::Ok && map.mapIndex==index);
        assert(VitaContentManifest_ValidateDependency(common,map)==VitaContentError::Ok);
        auto bad=map;bad.commonPackageId=99;
        assert(VitaContentManifest_ValidateDependency(common,bad)==VitaContentError::DependencyMismatch);
        bad=map;bad.sourceInstallHash[0]^=1;
        assert(VitaContentManifest_ValidateDependency(common,bad)==VitaContentError::DependencyMismatch);
        bad=map;bad.predictedGpuResidentBytes=kVitaGpuReserveBytes;
        assert(VitaContentManifest_ValidateDependency(common,bad)==VitaContentError::BudgetExceeded);
        bytes.back()^=1;
        assert(VitaContentManifest_Load(db,VitaContentRole::Map,&map)==VitaContentError::ManifestCorrupt);
        db.Unmount();assert(!db.Mounted() && db.ChunkCount()==0);
        bytes.back()^=1;assert(db.Mount(Read,&bytes,bytes.size())==KvapError::Ok);
        assert(VitaContentManifest_Load(db,VitaContentRole::Map,&map)==VitaContentError::Ok);
    }
    for (unsigned contract=0;contract<3;++contract) {
        auto text=Manifest("map");text.replace(text.find("\"schemaVersion\":1"),17,"\"schemaVersion\":2");
        if(contract)text.insert(1,contract==1?"\"textureResidency\":\"paged-world-v1\",":"\"textureResidency\":\"unknown\",");
        auto bytes=Package(42,text);VitaAssetDatabase db;VitaContentManifest map{};
        assert(db.Mount(Read,&bytes,bytes.size())==KvapError::Ok);
        assert(VitaContentManifest_Load(db,VitaContentRole::Map,&map)==
            (contract==1?VitaContentError::Ok:VitaContentError::SchemaMismatch));
        if(contract==1)assert(VitaContentManifest_ValidateDependency(common,map)==VitaContentError::DependencyMismatch);
    }
    for (unsigned rejected=0;rejected<3;++rejected) {
        auto text=Manifest("map");
        if (rejected==0) text.replace(text.find("\"playable\":true"),15,"\"playable\":false");
        if (rejected==1) text.replace(text.find("runtime-ready-v1"),16,"reference-only");
        if (rejected==2) text.erase(text.find("\"playable\":true,"),16);
        auto bytes=Package(42,text);VitaAssetDatabase db;VitaContentManifest map{};
        assert(db.Mount(Read,&bytes,bytes.size())==KvapError::Ok);
        assert(VitaContentManifest_Load(db,VitaContentRole::Map,&map)==VitaContentError::NotRuntimeReady);
    }
    // Map-local bytes take precedence, including shared asset IDs. Destroying
    // the map database drops both registries before the next map is mounted.
    auto primary=Package(2,"map",KvapChunkType::Strings);
    auto fallback=Package(1,"old",KvapChunkType::Strings);VitaAssetDatabase overlay;
    assert(overlay.Mount(Read,&primary,primary.size())==KvapError::Ok);
    assert(overlay.MountOverlay(Read,&fallback,fallback.size())==KvapError::Ok);
    const auto* chunk=overlay.Find(VitaAssetType::Strings,1);char payload[3]{};
    assert(chunk && overlay.HeaderFor(chunk)->packageId==2);
    assert(overlay.Read(chunk,payload,sizeof(payload))==KvapError::Ok && !std::memcmp(payload,"map",3));
    VitaContentManifest absent{};
    assert(VitaContentManifest_Load(overlay,VitaContentRole::Map,&absent)==VitaContentError::ManifestMissing);
    overlay.Unmount();assert(!overlay.Find(VitaAssetType::Strings,1));
    assert(overlay.Mount(Read,&primary,4)!=KvapError::Ok && !overlay.Mounted());
    assert(overlay.Mount(Read,&commonBytes,commonBytes.size())==KvapError::Ok);
}
