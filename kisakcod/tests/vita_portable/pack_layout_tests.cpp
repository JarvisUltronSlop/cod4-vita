#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_pack_layout.h>
#include <vita/vita_map_assets.h>
#include <cassert>
#include <vector>

using namespace kisak::vita;
template<class T> void Put(std::vector<uint8_t>& bytes,const T& value) {
    const auto* source=reinterpret_cast<const uint8_t*>(&value);
    bytes.insert(bytes.end(),source,source+sizeof(value));
}
int main() {
    KvapHeader h{};std::memcpy(h.magic,"KVAP",4);h.major=1;h.headerSize=64;h.endian=1;
    h.tocOffset=64;h.tocCount=1;
    KvapChunk c{};c.type=1;c.offset=128;c.rawSize=c.storedSize=4;c.alignment=16;
    std::vector<uint8_t> pack;Put(pack,h);Put(pack,c);Put(pack,uint32_t(123));
    const uint32_t crc=KvapCrc32(pack.data(),128);
    std::vector<uint8_t> index{'V','P','K','L'};
    Put(index,uint32_t(1));Put(index,uint32_t(48));Put(index,uint32_t(2));
    for(uint32_t id:{0u,0xffffffffu}) {
        Put(index,id);Put(index,uint64_t(pack.size()));Put(index,crc);
        char name[32]{};std::strcpy(name,id==0?"mp_shipment.kvap":"common.kvap");
        for(char value:name)index.push_back(uint8_t(value));
    }
    assert(VitaPackLayoutValidate(index.data(),index.size(),0,132,crc)==VitaPackLayoutError::Ok);
    assert(VitaPackLayoutValidate(index.data(),index.size(),0xffffffffu,132,crc)==VitaPackLayoutError::Ok);
    assert(VitaPackLayoutValidate(index.data(),index.size(),1,132,crc)==VitaPackLayoutError::MissingPack);
    assert(VitaPackLayoutValidate(index.data(),index.size(),0,131,crc)==VitaPackLayoutError::MismatchedPack);
    assert(VitaPackLayoutValidate(index.data(),index.size(),0,132,crc^1)==VitaPackLayoutError::MismatchedPack);
    assert(VitaPackLayoutValidate(index.data(),index.size()-1,0,132,crc)==VitaPackLayoutError::CorruptIndex);
    assert(VitaPackLayoutValidate(nullptr,0,0,132,crc)==VitaPackLayoutError::CorruptIndex);
    auto duplicate=index;std::memcpy(duplicate.data()+64,index.data()+16,4);
    assert(VitaPackLayoutValidate(duplicate.data(),duplicate.size(),0,132,crc)==VitaPackLayoutError::CorruptIndex);
    auto badName=index;std::memset(badName.data()+32,65,32);
    assert(VitaPackLayoutValidate(badName.data(),badName.size(),0,132,crc)==VitaPackLayoutError::CorruptIndex);
    FILE* file=std::tmpfile();assert(file);
    assert(std::fwrite(pack.data(),1,pack.size(),file)==pack.size());std::fflush(file);
    assert(VitaPackLayoutProbeFile(index.data(),index.size(),0,file)==VitaPackLayoutError::Ok);
    std::fseek(file,80,SEEK_SET);std::fputc(4,file);std::fflush(file);
    assert(VitaPackLayoutProbeFile(index.data(),index.size(),0,file)==VitaPackLayoutError::MismatchedPack);
    std::fclose(file);
    assert(!VitaMapUsesTexturePaging(0));
    for(uint32_t id=1;id<kVitaMpMapCount;++id)
        assert(VitaMapUsesTexturePaging(id)==VitaMapIsEnabled(id));
    assert(!VitaMapUsesTexturePaging(99));
}
