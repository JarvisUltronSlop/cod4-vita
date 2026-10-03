#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>

using SceUID=int;
constexpr bool GL_FALSE=false,GL_TRUE=true;
enum {VGL_MEM_VRAM,VGL_MEM_RAM,VGL_MEM_PHYCONT,VGL_MEM_BUDGET,VGL_MEM_EXTERNAL,VGL_MEM_ALL};
enum {SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
    SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW,SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_CDIALOG_RW,
    SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW,
    SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_CDIALOG_NC_RW,SCE_GXM_MEMORY_ATTRIB_RW};
#define VGL_ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
bool has_cached_mem=false,vgl_has_cdlg_support=false;
size_t mempool_size[VGL_MEM_ALL]{};
void* mempool_addr[VGL_MEM_ALL]{},*mempool_end[VGL_MEM_ALL]{},*mempool_mspace[VGL_MEM_ALL]{};
int vgl_initialization_error=0,allocCalls=0,mapCalls=0,baseCalls=0,failAlloc=0,failMap=0,failBase=0;
constexpr int allocationError=-101,mappingError=-102,baseError=-103;
int sceKernelAllocMemBlock(const char*,int,size_t,void*){return ++allocCalls==failAlloc?allocationError:allocCalls;}
int sceKernelGetMemBlockBase(int id,void** output){++baseCalls;*output=reinterpret_cast<void*>(uintptr_t(id)*0x10000000);return baseCalls==failBase?baseError:0;}
int sceGxmMapMemory(void*,size_t,int){return ++mapCalls==failMap?mappingError:0;}
void* sceClibMspaceCreate(void* p,size_t){return p;}
struct SceKernelMemBlockInfo {size_t size;void* mappedBase;size_t mappedSize;};
int sceKernelGetMemBlockInfoByAddr(void*,SceKernelMemBlockInfo* i){i->mappedBase=reinterpret_cast<void*>(0x50000000);i->mappedSize=128*1024*1024;return 0;}
struct SceGxmContext {};
SceGxmContext context;
SceGxmContext* gxm_context=nullptr;
struct SceGxmContextParams {
    void *hostMem,*vdmRingBufferMem,*vertexRingBufferMem,*fragmentRingBufferMem,*fragmentUsseRingBufferMem;
    unsigned hostMemSize,vdmRingBufferMemSize,vertexRingBufferMemSize,fragmentRingBufferMemSize,
        fragmentUsseRingBufferMemSize,fragmentUsseRingBufferOffset;
};
void *vdm_ring_buffer_addr[1],*vertex_ring_buffer_addr[1],*fragment_ring_buffer_addr[1],
    *fragment_usse_ring_buffer_addr[1],*context_host_mem[1];
unsigned gxm_vdm_buf_size=4096,gxm_vertex_buf_size=1048576,gxm_fragment_buf_size=4096,gxm_usse_buf_size=4096;
constexpr int VGL_CONTEXT_MAIN=0,SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE=4096;
int ringCalls=0,failRing=0,createCalls=0,createResult=0,uniformCalls=0,initResult=0;
void* Ring(){return ++ringCalls==failRing?nullptr:&context;}
void* gpu_alloc_mapped_aligned_for_gpu(size_t,size_t){return Ring();}
void* gpu_fragment_usse_alloc_mapped(size_t,unsigned* offset){*offset=0;return Ring();}
void* vglMalloc(size_t){return Ring();}
void vgl_memset(void* p,int v,size_t n){std::memset(p,v,n);}
int sceGxmCreateContext(SceGxmContextParams*,SceGxmContext** c){++createCalls;if(!createResult)*c=&context;return createResult;}
void vglSetupUniformCircularPool(){++uniformCalls;}
void init_gxm(){vgl_initialization_error=initResult;}
int unsafe_allocator_counter=0,gcCalls=0,innerCalls=0,gc_mutex[2]{};
constexpr int FRAME_PURGE_FREQ=3;
void glFinish(){assert(gxm_context);++gcCalls;}
void sceGxmFinish(SceGxmContext* c){assert(c);++gcCalls;}
void sceKernelWaitSema(int,int,void*){++gcCalls;}
void sceKernelSignalSema(int,int){++gcCalls;}
void sceKernelDelayThread(int){}
void* gpu_alloc_mapped_aligned_for_cpu_inner(size_t,size_t){++innerCalls;return &context;}
void* gpu_alloc_mapped_aligned_for_gpu_inner(size_t,size_t){++innerCalls;return &context;}
constexpr size_t ram_pool_size=60*1024*1024,cdram_pool_size=16*1024*1024,phycont_pool_size=0,cdlg_pool_size=0;
#include "cpu_gxm_init.inc"

void Reset() {
    gxm_context=nullptr;vgl_initialization_error=allocCalls=mapCalls=baseCalls=ringCalls=createCalls=uniformCalls=0;
    failAlloc=failMap=failBase=failRing=createResult=initResult=gcCalls=innerCalls=unsafe_allocator_counter=0;
}
int main() {
    for(int cached=0;cached<2;++cached) {
        has_cached_mem=cached;
        for(int n=1;n<=2;++n) {
            Reset();failAlloc=n;assert(!Startup());assert(vgl_initialization_error==allocationError);assert(!createCalls && !ringCalls);
            Reset();failMap=n;assert(!Startup());assert(vgl_initialization_error==mappingError);assert(!createCalls && !ringCalls);
            Reset();failBase=n;assert(!Startup());assert(vgl_initialization_error==baseError);assert(!createCalls && !ringCalls);
        }
        for(int n=1;n<=5;++n){Reset();failRing=n;assert(!Startup());assert(!createCalls && !uniformCalls);}
        Reset();createResult=-104;assert(!Startup());assert(vgl_initialization_error==-104 && !uniformCalls);
        Reset();initResult=-105;assert(!Startup());assert(!allocCalls && !createCalls);
        Reset();assert(Startup());assert(createCalls==1 && uniformCalls==1);
        assert(mempool_size[VGL_MEM_RAM]+mempool_size[VGL_MEM_VRAM]==76*1024*1024);
    }
    Reset();assert(!gpu_alloc_mapped_aligned_unsafe_for_cpu(4096,4096));
    assert(!gpu_alloc_mapped_aligned_unsafe_for_gpu(4096,4096));assert(!gcCalls && !innerCalls);
    gxm_context=&context;assert(gpu_alloc_mapped_aligned_unsafe_for_cpu(4096,4096));assert(gcCalls && innerCalls);
}
