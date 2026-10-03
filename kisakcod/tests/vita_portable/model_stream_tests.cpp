#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_model_bundle.h>
#include <vita/vita_chunk_window.h>
#include "reference_model_bundle.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <filesystem>
#include <limits>
#undef assert
#define assert(condition) do { if(!(condition)) { std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#condition);std::exit(3); } } while(false)
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

static size_t allocationLimit=SIZE_MAX,largestAllocation=0;
static bool failNextAllocation=false;
void* operator new(size_t bytes){
    largestAllocation=std::max(largestAllocation,bytes);
    // Skip MSVC Debug iterator-proxy allocations in noexcept constructors;
    // fail an actual decoded-storage request, as the Vita allocator does.
    if(failNextAllocation && bytes>=256){failNextAllocation=false;throw std::bad_alloc();}
    if(bytes>allocationLimit)throw std::bad_alloc();
    if(auto* p=std::malloc(bytes?bytes:1))return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete(void* p,size_t) noexcept{std::free(p);}
struct File {
    FILE* file=nullptr;
    size_t maximumRead=0,calls=0,failCall=SIZE_MAX;
    uint64_t corruptOffset=UINT64_MAX;
};
bool ReadFile(void* context,uint64_t offset,void* output,size_t count){
    auto& f=*static_cast<File*>(context);
    f.maximumRead=std::max(f.maximumRead,count);
    if(++f.calls==f.failCall)return false;
#ifdef _WIN32
    if(_fseeki64(f.file,offset,SEEK_SET))return false;
#else
    if(fseeko(f.file,offset,SEEK_SET))return false;
#endif
    if(std::fread(output,1,count,f.file)!=count)return false;
    if(f.corruptOffset>=offset && f.corruptOffset-offset<count)
        static_cast<uint8_t*>(output)[f.corruptOffset-offset]^=1;
    return true;
}
template<class A,class B> void SameVector(const A& a,const B& b){
    assert(a.size()==b.size());
    static_assert(sizeof(typename A::value_type)==sizeof(typename B::value_type));
    if(!a.empty())assert(!std::memcmp(a.data(),b.data(),a.size()*sizeof(a[0])));
}
template<class A,class B> void SameSurfaces(const A& a,const B& b){
    assert(a.size()==b.size());
    for(size_t i=0;i<a.size();++i){
        assert(a[i].materialName==b[i].materialName);
        assert(a[i].vertexStart==b[i].vertexStart && a[i].vertexCount==b[i].vertexCount);
        assert(a[i].indexStart==b[i].indexStart && a[i].indexCount==b[i].indexCount);
    }
}
template<class A,class B> void SameBundle(const A& a,const B& b){
    assert(a.Loaded()==b.Loaded() && a.WireVersion()==b.WireVersion());
    assert(a.SourceComplete()==b.SourceComplete() && a.Count()==b.Count());
    assert(a.RenderLodsLoaded()==b.RenderLodsLoaded());
    for(size_t i=0;i<a.Records().size();++i){
        const auto& x=a.Records()[i];const auto& y=b.Records()[i];
        assert(x.name==y.name && x.localSkeleton==y.localSkeleton);
        assert(!std::memcmp(x.mins,y.mins,sizeof(x.mins)) && !std::memcmp(x.maxs,y.maxs,sizeof(x.maxs)));
        assert(x.weaponAttachBone==y.weaponAttachBone && x.cameraTagBone==y.cameraTagBone && x.flashTagBone==y.flashTagBone);
        SameSurfaces(x.surfaces,y.surfaces);SameVector(x.vertices,y.vertices);SameVector(x.weights,y.weights);
        SameVector(x.indices,y.indices);SameVector(x.parents,y.parents);SameVector(x.bones,y.bones);
        assert(x.boneNames==y.boneNames && x.lodCount==y.lodCount && x.lodRampType==y.lodRampType);
        assert(!std::memcmp(x.lodDistances,y.lodDistances,sizeof(x.lodDistances)));
        assert(x.renderLods.size()==y.renderLods.size());
        for(size_t lod=0;lod<x.renderLods.size();++lod){
            const auto& p=x.renderLods[lod];const auto& q=y.renderLods[lod];
            assert(p.sourceLod==q.sourceLod && !std::memcmp(&p.distance,&q.distance,sizeof(p.distance)));
            SameSurfaces(p.surfaces,q.surfaces);SameVector(p.vertices,q.vertices);SameVector(p.indices,q.indices);
        }
    }
}
int main(int argc,char** argv){
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
    _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
#endif
    using namespace kisak::vita;
    assert(argc==3);
    File map{std::fopen(argv[1],"rb")},common{std::fopen(argv[2],"rb")};assert(map.file && common.file);
    VitaAssetDatabase database;
    assert(database.Mount(ReadFile,&map,std::filesystem::file_size(argv[1]))==KvapError::Ok);
    assert(database.MountOverlay(ReadFile,&common,std::filesystem::file_size(argv[2]))==KvapError::Ok);
    kisak::reference::VitaModelBundle reference;
    assert(reference.Load(database));
    VitaModelBundle candidate;
    largestAllocation=0;map.maximumRead=common.maximumRead=0;
    allocationLimit=8*1024*1024;
    assert(candidate.Load(database));SameBundle(candidate,reference);
    const auto boundedLargest=largestAllocation;
    assert(map.maximumRead<=65536 && common.maximumRead<=65536);
    std::printf("Exact decoded model/LOD records: %u; largest allocation under 8 MiB cap: %zu; max read: %zu\n",
        candidate.Count(),boundedLargest,std::max(map.maximumRead,common.maximumRead));
    bool oldFailed=false;
    try{kisak::reference::VitaModelBundle old;old.Load(database);}catch(const std::bad_alloc&){oldFailed=true;}
    assert(oldFailed); // reproduce the whole-chunk allocation problem
    std::puts("Old loader allocation failure reproduced");std::fflush(stdout);
    failNextAllocation=true;assert(!candidate.Load(database));assert(!candidate.Loaded() && candidate.Count()==0);
    assert(!failNextAllocation);
    std::puts("Allocation failure returned cleanly");std::fflush(stdout);
    assert(candidate.Load(database));SameBundle(candidate,reference);
    // Exercise the window itself at headers straddling each boundary and at
    // backward reads. Full CRC must still match after each window is revisited.
    const KvapChunk* chunk=nullptr;
    for(uint32_t i=0;i<database.ChunkCount();++i){
        auto* c=database.Chunk(i);
        if(c->type==uint32_t(VitaAssetType::Model) && c->rawSize>8*1024*1024){chunk=c;break;}
    }
    assert(chunk);
    {
        VitaChunkWindow window(database,chunk);uint8_t expected[80],actual[80];
        for(size_t offset=65520;offset+80<chunk->rawSize;offset+=65536){
            assert(database.ReadRange(chunk,offset,expected,sizeof(expected))==KvapError::Ok);
            assert(window.Read(offset,actual,sizeof(actual)));assert(!std::memcmp(expected,actual,sizeof(actual)));
            assert(window.Read(offset-8,actual,8));
        }
        assert(window.Finish());
        assert(!window.Read(SIZE_MAX,actual,1));assert(!window.Finish());
    }
    std::puts("Window boundaries and backward reads passed");std::fflush(stdout);
    // Read errors and CRC corruption must not publish partially decoded assets.
    // Find the owning file by whether the chunk pointer comes from the primary.
    File* owner=database.HeaderFor(chunk)==database.Header()?&map:&common;
    owner->corruptOffset=chunk->offset+chunk->rawSize-1;
    assert(!candidate.Load(database,chunk->assetId));assert(!candidate.Loaded());
    std::puts("CRC corruption rejected");std::fflush(stdout);
    owner->corruptOffset=UINT64_MAX;
    owner->failCall=owner->calls+3;
    assert(!candidate.Load(database,chunk->assetId));assert(!candidate.Loaded());
    owner->failCall=SIZE_MAX;
    for(unsigned cycle=0;cycle<3;++cycle){assert(candidate.Load(database));SameBundle(candidate,reference);candidate.Clear();}
    std::fclose(map.file);std::fclose(common.file);
}
