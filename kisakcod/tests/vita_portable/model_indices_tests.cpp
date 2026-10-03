#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_model_indices.h>
#include <cassert>
#include <cstdlib>
#include <vector>
using namespace kisak::vita;
int main() {
    for(unsigned cycle=0;cycle<10;++cycle) {
        VitaModelRecord model;model.vertices.resize(8);
        model.indices={0,1,2,2,3,0,4,5,6};
        const auto original=model.indices;
        VitaModelSurface surface;surface.vertexCount=4;surface.indexCount=6;
        unsigned allocations=0;std::vector<void*> owned;
        const auto allocate=[&](size_t bytes,size_t alignment)->void* {
            assert(alignment==4);++allocations;
            auto* p=std::malloc(bytes);owned.push_back(p);return p;
        };
        bool borrowed=false;
        auto* shared=VitaSourceModelIndices(model,surface,true,allocate,&borrowed);
        assert(shared==model.indices.data() && borrowed && !allocations);
        auto* copied=VitaSourceModelIndices(model,surface,false,allocate,&borrowed);
        assert(copied && copied!=shared && !borrowed && allocations==1);
        for(unsigned i=0;i<6;++i)assert(shared[i]==copied[i]);
        surface.vertexStart=4;surface.vertexCount=4;surface.indexStart=6;surface.indexCount=3;
        auto* rebased=VitaSourceModelIndices(model,surface,true,allocate,&borrowed);
        assert(rebased && !borrowed && rebased[0]==0 && rebased[2]==2);
        assert(!VitaSourceModelIndices(model,surface,true,[](size_t,size_t)->void*{return nullptr;}));
        surface.vertexStart=0;surface.vertexCount=4;surface.indexStart=1;surface.indexCount=3;
        auto* unaligned=VitaSourceModelIndices(model,surface,true,allocate,&borrowed);
        assert(unaligned && !borrowed && (reinterpret_cast<uintptr_t>(unaligned)&3u)==0);
        surface.indexCount=2;auto before=allocations;
        assert(!VitaSourceModelIndices(model,surface,true,allocate) && allocations==before);
        surface.indexCount=3;surface.indexStart=UINT32_MAX;
        assert(!VitaSourceModelIndices(model,surface,true,allocate) && allocations==before);
        surface.indexStart=6;surface.vertexStart=0;
        assert(!VitaSourceModelIndices(model,surface,true,allocate) && allocations==before);
        assert(model.indices==original);
        // Consumers release their handles before the owning map vectors.
        shared=nullptr;for(void* p:owned)std::free(p);owned.clear();model.indices.clear();
    }
}
