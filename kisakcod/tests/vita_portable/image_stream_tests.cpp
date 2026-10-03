#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_image_bundle.h>
#include <vita/vita_map_assets.h>
#include <cassert>
#include <cstring>
#include <new>
#include <cstdlib>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
static bool failPayloadAllocation=false;
static bool boundPayloads=false;
static void* boundedPayload=nullptr;
static bool IsBoundedPayload(size_t bytes) {
    // MSVC adds space for its 32-byte vector alignment and original pointer
    // when allocating buffers larger than 4096 bytes (plus a Debug sentinel).
    return bytes==70003 || bytes==70003+31+sizeof(void*)
        || bytes==70003+31+2*sizeof(void*);
}
void* operator new(size_t bytes) {
    if(failPayloadAllocation && bytes==32) {failPayloadAllocation=false;throw std::bad_alloc();}
    if(boundPayloads && IsBoundedPayload(bytes) && boundedPayload)throw std::bad_alloc();
    if(void* memory=std::malloc(bytes?bytes:1)) {
        if(boundPayloads && IsBoundedPayload(bytes))boundedPayload=memory;
        return memory;
    }
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept {
    if(memory==boundedPayload)boundedPayload=nullptr;
    std::free(memory);
}
void operator delete(void* memory,size_t) noexcept {::operator delete(memory);}
using namespace kisak::vita;
using Bytes=std::vector<uint8_t>;
static uint64_t readCalls=0,readBytes=0;
static bool failRead=false;
template<class T> void Put(Bytes& out,T value) {
    const auto* p=reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(),p,p+sizeof(value));
}
bool Read(void* context,uint64_t offset,void* dst,size_t bytes) {
    ++readCalls;readBytes+=bytes;
    if (failRead) return false;
    const auto& input=*static_cast<const Bytes*>(context);
    if(offset>input.size() || bytes>input.size()-offset) return false;
    std::memcpy(dst,input.data()+offset,bytes); return true;
}
Bytes Package(uint64_t asset=1,unsigned records=3,unsigned payloadSize=32,bool duplicate=false) {
    Bytes payload;
    Put(payload,uint32_t(0x49544956)); Put(payload,uint32_t(1)); Put(payload,uint32_t(records));
    Put(payload,uint64_t(records)*payloadSize);
    for(unsigned i=0;i<records;++i) {
        const std::string name="image"+std::to_string(duplicate?0:i);
        Put(payload,uint32_t(name.size())); Put(payload,uint32_t(payloadSize));
        Put(payload,uint32_t(8)); Put(payload,uint32_t(8)); Put(payload,uint32_t(1));
        Put(payload,uint32_t(1)); Put(payload,uint32_t(0x31545844)); Put(payload,uint32_t(0));
        payload.insert(payload.end(),name.begin(),name.end());
        payload.insert(payload.end(),payloadSize,static_cast<uint8_t>(i+1));
    }
    KvapHeader h{}; std::memcpy(h.magic,"KVAP",4); h.major=1; h.endian=1;
    h.headerSize=sizeof(h); h.tocOffset=sizeof(h); h.tocCount=1; h.packageId=1;
    KvapChunk c{}; c.type=static_cast<uint32_t>(KvapChunkType::Image); c.assetId=asset;
    c.offset=sizeof(h)+sizeof(c); c.alignment=16; c.rawSize=c.storedSize=payload.size();
    c.crc32=KvapCrc32(payload.data(),payload.size());
    Bytes pack; Put(pack,h); Put(pack,c); pack.insert(pack.end(),payload.begin(),payload.end());
    return pack;
}
int main() {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    auto pack=Package(); VitaAssetDatabase db;
    assert(db.Mount(Read,&pack,pack.size())==KvapError::Ok);
    VitaImageBundle images;
    // The native envelope retains all aliases while holding at most one
    // payload here. The cap applies even to an unfiltered selection.
    assert(images.LoadSelected(db,1,{},0,32,true));
    assert(images.Count()==3 && images.ReleaseAllPayloads()==32);
    assert(images.Find("image2") && images.Find("image2")->bytes.empty());
    assert(images.LoadSelected(db,1,{"image2"},0,32,true));
    assert(images.Count()==3 && images.Find("image2")->bytes.size()==32);
    auto corrupt=pack;corrupt.back()^=1;VitaAssetDatabase corruptDb;
    assert(corruptDb.Mount(Read,&corrupt,corrupt.size())==KvapError::Ok);
    assert(!images.LoadSelected(corruptDb,1,{"image0"},0,32,true));
    assert(!images.Loaded() && images.Count()==0);
    failPayloadAllocation=true;
    assert(!images.LoadSelected(db,1,{},0,32,true));
    assert(!failPayloadAllocation && !images.Loaded() && images.Count()==0);
    assert(images.LoadNamed(db,1,"image0"));
    failPayloadAllocation=true;
    assert(!images.AppendNamed(db,1,"image1"));
    assert(!failPayloadAllocation && images.Count()==1 && images.Find("image0")->bytes.size()==32);
    failPayloadAllocation=true;
    assert(!images.LoadNamed(db,1,"image1") && !images.Loaded());
    assert(!failPayloadAllocation);
    // A single checked scan, then exactly one payload read per lookup. Exercise
    // window-boundary records and compare bytes against the legacy reader.
    auto wide=Package(1,80,70003);VitaAssetDatabase wideDb;
    assert(wideDb.Mount(Read,&wide,wide.size())==KvapError::Ok);
    VitaImageIndex index(wideDb);
    readCalls=readBytes=0;
    assert(index.Build(1) && index.Count()==80);
    assert(readBytes==wide.size()-sizeof(KvapHeader)-sizeof(KvapChunk));
    assert(readCalls==(readBytes+65535)/65536);
    // Reproduce a segment that cannot coexist on the remaining heap. The old
    // bulk reader fails cleanly; the indexed path stages all 80 records while
    // the allocator permits only one payload at a time, including on reload.
    boundPayloads=true;
    assert(!images.LoadSelected(wideDb,1,{}));
    assert(!images.Loaded() && !boundedPayload);
    for(unsigned cycle=0;cycle<3;++cycle) {
        assert(index.Build(1));
        for(unsigned i=0;i<80;++i) {
            const auto name="image"+std::to_string(i);
            assert(index.Contains(name.c_str()));
            VitaImageRecord image;
            assert(index.Read(name.c_str(),&image));
            assert(image.bytes.size()==70003 && image.bytes.front()==i+1);
        }
        assert(!boundedPayload);
    }
    boundPayloads=false;
    readCalls=0;
    assert(!index.Contains(nullptr) && !index.Contains("") && !index.Contains("absent"));
    assert(readCalls==0);
    for (unsigned i=0;i<80;++i) {
        const auto name="image"+std::to_string(i);
        readCalls=readBytes=0;
        VitaImageRecord actual;
        assert(index.Read(name.c_str(),&actual));
        assert(readCalls==1 && readBytes==70003);
        VitaImageBundle legacy;assert(legacy.LoadNamed(wideDb,1,name.c_str()));
        assert(actual.bytes==legacy.Find(name.c_str())->bytes);
    }
    readCalls=0;VitaImageRecord record;
    assert(!index.Read("absent",&record) && readCalls==0);
    wide.back()^=1;
    assert(!index.Read("image79",&record)); // post-index payload corruption
    assert(!index.Build(1) && index.Count()==0); // full chunk CRC
    wide.back()^=1;
    failRead=true;assert(!index.Build(1) && index.Count()==0);failRead=false;
    assert(index.Build(1));wideDb.Unmount();
    assert(!index.Read("image0",&record));
    auto duplicate=Package(1,3,32,true);VitaAssetDatabase duplicateDb;
    assert(duplicateDb.Mount(Read,&duplicate,duplicate.size())==KvapError::Ok);
    VitaImageIndex duplicateIndex(duplicateDb);assert(!duplicateIndex.Build(1));
    VitaImageIndex smallIndex(db);assert(smallIndex.Build(1));
    assert(images.LoadIndexed(smallIndex,"image0"));
    failPayloadAllocation=true;
    assert(!images.LoadIndexed(smallIndex,"image1",true) && !failPayloadAllocation);
    assert(images.Count()==1 && images.Find("image0")->bytes.size()==32);
    assert(!images.LoadIndexed(smallIndex,"absent",true));
    for(unsigned cycle=0;cycle<10;++cycle) {
        assert(images.LoadSelected(db,1,{"image0","image1","image2"},0,32));
        assert(images.Count()==1 && images.Find("image0") && !images.Find("image2"));
        images.ReleasePayload("image0");
        for(const char* name:{"image1","image2"}) {
            assert(images.AppendNamed(db,1,name));
            const auto* image=images.Find(name); assert(image && image->bytes.size()==32);
            // At most one compressed payload coexists with retained metadata.
            assert(images.ReleaseAllPayloads()==32);
            assert(images.Find(name)->bytes.empty());
        }
        assert(images.Count()==3 && !images.AppendNamed(db,1,"image1"));
        assert(!images.AppendNamed(db,1,"absent") && images.Count()==3);
        VitaImageBundle scratch; assert(scratch.LoadNamed(db,1,"image0"));
        assert(scratch.Find("image0")->bytes[0]==1);
        assert(images.Find("image0")->bytes.empty());
        images.Clear(); assert(images.Count()==0 && !images.Loaded());
    }
    // Large-map colours omitted from the startup VITI remain available in
    // the map-local tail. Reading one never retains the whole segment.
    for(uint32_t map=0;map<kVitaMpMapCount;++map) {
        if(!VitaMapIsEnabled(map))continue;
        auto tail=Package(VitaMapColorImagesAssetId(map,0));VitaAssetDatabase segmented;
        assert(segmented.Mount(Read,&tail,tail.size())==KvapError::Ok);
        assert(images.LoadNamedMapColor(segmented,map,"image1"));
        assert(images.Count()==1 && images.Find("image1")->bytes.size()==32);
        assert(images.LoadNamedMapColor(segmented,map,"image2",true));
        assert(images.Count()==2 && images.ReleaseAllPayloads()==64);
        assert(!images.LoadNamedMapColor(segmented,map,"absent",true) && images.Count()==2);
        assert(!images.LoadNamedMapColor(segmented,13,"image0",true));
        VitaImageIndex tailIndex(segmented);
        assert(tailIndex.BuildMapColors(map));
        assert(images.LoadIndexed(tailIndex,"image2") && images.Count()==1);
        assert(images.Find("image2")->bytes.size()==32);
        assert(!tailIndex.BuildMapColors(13) && !tailIndex.Count());
        segmented.Unmount();images.Clear();assert(images.Count()==0);
    }
}

