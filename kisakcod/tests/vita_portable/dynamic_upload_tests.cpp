#ifdef NDEBUG
#undef NDEBUG
#endif
#define KISAK_VITA_CPU_RUNTIME 1
#include <vita/vita_dynamic_vertex_scratch.h>
#include <vita/vita_vertex_pack.h>
#include <vita/vita_cpu_profile.h>
#include <gfx/gfx_backend.h>
#include <gfx_vita/gfx_vita_viewmodel.h>
#include <gfx_vita/gfx_vita_pose_buffers.h>
#include <gfx_vita/gfx_vita_pose_light_bounds.h>
#include <map>
#include <cassert>
#include <cstring>
#include <vector>
#include <random>
#include <cstdio>

using DynamicViewModelVertex=GfxVitaViewmodelVertex;
using GLuint=uint32_t;using GLenum=uint32_t;using GLsizeiptr=ptrdiff_t;
constexpr GLenum GL_ARRAY_BUFFER=1,GL_DYNAMIC_DRAW=2,GL_NO_ERROR=0;
bool g_software=false,g_initialized=true;
struct Resources {std::map<uint32_t,uint32_t> sizes;} g_nativeBuffers;
GfxVitaPoseBuffers g_poseBuffers;
GfxVitaPoseLightBounds<> g_poseLightBounds;
uint64_t g_poseSwapSerial=0;
unsigned g_poseRingUploads=0,g_poseRingFallbacks=0,g_poseRingRotations=0;
uint64_t sceKernelGetProcessTimeWide(){static uint64_t now=0;return ++now;}
uint64_t g_nativeStaticBufferBytes=0;
bool failAllocation=false,glFail=false,trackFail=false;
unsigned deleted=0,uploads=0;
uint32_t bound=0,created=0;
GLenum error=0;
struct Scratch: kisak::vita::DynamicVertexScratch<DynamicViewModelVertex> {
    bool Reserve(size_t count){return DynamicVertexScratch::Reserve(count,+[](void* p,size_t n)->void* {
        return failAllocation?nullptr:std::realloc(p,n);
    });}
} g_dynamicViewModelUpload;
std::vector<uint8_t> submitted;
void EndNativeStaticPipeline(){}
void sceClibPrintf(const char*,...){}
namespace kisak::vita {void VitaCpuProfile_Counter(const char*,uint64_t,uint64_t){}}
void glGenBuffers(int,GLuint* id){*id=++created;}
void glBindBuffer(GLenum,GLuint id){bound=id;}
void glBufferData(GLenum,GLsizeiptr,const void*,GLenum){assert(bound);}
void glDeleteBuffers(int,const GLuint*){++deleted;}
GLenum glGetError(){auto e=error;error=0;return e;}
bool TrackNativeResource(Resources* r,GLuint id,uint32_t size,uint64_t* total){
    if(trackFail)return false;r->sizes[id]=size;*total+=size;return true;
}
void ForgetNativeResource(Resources* r,GLuint id,uint64_t* total){*total-=r->sizes[id];r->sizes.erase(id);}
uint32_t NativeResourceSize(const Resources& r,GLuint id){auto i=r.sizes.find(id);return i==r.sizes.end()?0:i->second;}
void glBufferSubData(GLenum,int,GLsizeiptr size,const void* data){
    assert(bound);++uploads;submitted.assign(static_cast<const uint8_t*>(data),static_cast<const uint8_t*>(data)+size);
    if(glFail)error=7;
}
#include "source_dynamic_upload.inc"

int main(){
    assert(!CreateDynamicVertexBuffer(0).value);
    failAllocation=true;assert(!CreateDynamicVertexBuffer(64).value);
    assert(deleted==1 && g_nativeStaticBufferBytes==0 && g_dynamicViewModelUpload.size()==0);
    failAllocation=false;auto buffer=CreateDynamicVertexBuffer(1024);assert(buffer.value);
    auto* stable=g_dynamicViewModelUpload.data();
    std::mt19937 rng(0xc0d421);
    std::vector<GfxDrawVertex> vertices(1024);
    uint64_t comparisons=0;
    for(unsigned pass=0;pass<256;++pass){
        const uint32_t count=pass%2?3:1024;
        for(auto& v:vertices){
            uint32_t fields[sizeof(v)/4];
            for(auto& bits:fields)bits=rng();
            std::memcpy(&v,fields,sizeof(v));
        }
        assert(UpdateDynamicVertexBuffer(buffer,vertices.data(),count));
        assert(g_dynamicViewModelUpload.data()==stable && g_dynamicViewModelUpload.size()==1024);
        assert(submitted.size()==1024*sizeof(DynamicViewModelVertex));
        for(unsigned i=0;i<count;++i){
            DynamicViewModelVertex expected{};const auto& v=vertices[i];
            std::memcpy(expected.position,v.position,sizeof(expected.position));
            std::memcpy(expected.texCoord,v.texCoord,sizeof(expected.texCoord));
            std::memcpy(expected.worldPosition,v.worldPosition,sizeof(expected.worldPosition));
            for(unsigned j=0;j<3;++j){
                auto old=[](float x){return int8_t(std::lround(std::clamp(std::isfinite(x)?x:0.f,-1.f,1.f)*127.f));};
                expected.normal[j]=old(v.normal[j]);expected.tangent[j]=old(v.tangent[j]);
            }
            for(unsigned j=0;j<4;++j)expected.color[j]=uint8_t(std::lround(std::clamp(std::isfinite(v.color[j])?v.color[j]:0.f,0.f,1.f)*255.f));
            assert(std::memcmp(submitted.data()+i*sizeof(expected),&expected,sizeof(expected))==0);++comparisons;
        }
    }
    for(unsigned i=0;i<vertices.size();++i)for(unsigned axis=0;axis<3;++axis)
        vertices[i].worldPosition[axis]=float(i+axis);
    assert(UpdateDynamicVertexBuffer(buffer,vertices.data(),1024));
    auto bounds=g_poseLightBounds.Find(buffer.value,0,1024);
    assert(bounds.valid && bounds.minimum[0]==0 && bounds.maximum[2]==1025);
    assert(UpdateDynamicVertexBuffer(buffer,vertices.data(),3));
    assert(g_poseLightBounds.Find(buffer.value,0,3).valid && !g_poseLightBounds.Find(buffer.value,3,1).valid);
    const auto oldUploads=uploads;
    assert(!UpdateDynamicVertexBuffer({},vertices.data(),1));
    assert(!UpdateDynamicVertexBuffer(buffer,nullptr,1));
    assert(!g_poseLightBounds.Find(buffer.value,0,1).valid);
    assert(!UpdateDynamicVertexBuffer(buffer,vertices.data(),1025));
    assert(!UpdateDynamicVertexBuffer(buffer,vertices.data(),UINT32_MAX));
    assert(uploads==oldUploads);
    error=5;assert(UpdateDynamicVertexBuffer(buffer,vertices.data(),1)); // stale error drained
    assert(g_poseLightBounds.Find(buffer.value,0,1).valid);
    glFail=true;assert(!UpdateDynamicVertexBuffer(buffer,vertices.data(),1));glFail=false;
    assert(!g_poseLightBounds.Find(buffer.value,0,1).valid);
    failAllocation=true;assert(!CreateDynamicVertexBuffer(2048).value);
    assert(g_dynamicViewModelUpload.data()==stable && g_dynamicViewModelUpload.size()==1024);
    assert(!g_dynamicViewModelUpload.Reserve(SIZE_MAX));
    assert(UpdateDynamicVertexBuffer(buffer,vertices.data(),1));
    DestroyBuffer(buffer);assert(g_nativeBuffers.sizes.empty() && g_nativeStaticBufferBytes==0);
    assert(!g_poseLightBounds.Find(buffer.value,0,1).valid);
    assert(g_poseBuffers.ExtraBytes()==0 && g_poseBuffers.ShadowBytes()==0);
    std::printf("%llu complete 44-byte native uploads match original packing; scratch reuse/failure and GL error handling passed\n",static_cast<unsigned long long>(comparisons));
}
