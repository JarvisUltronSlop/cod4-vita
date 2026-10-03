#pragma once
#include <cstdlib>
#include <limits>
#include <type_traits>

namespace kisak::vita {
// Renderer-owned POD scratch. Each upload overwrites every used byte before
// GL consumes it. Retain the largest staging allocation; no value initialization
// or shrinking on smaller updates, and allocation failure preserves old data.
template<class T> class DynamicVertexScratch {
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivial_v<T>);
    T* data_=nullptr;
    size_t count_=0;
public:
    ~DynamicVertexScratch(){std::free(data_);}
    DynamicVertexScratch()=default;
    DynamicVertexScratch(const DynamicVertexScratch&)=delete;
    DynamicVertexScratch& operator=(const DynamicVertexScratch&)=delete;
    bool Reserve(size_t count,void*(*resize)(void*,size_t)=std::realloc) {
        if(count<=count_)return true;
        if(count>std::numeric_limits<size_t>::max()/sizeof(T))return false;
        void* allocation=resize(data_,count*sizeof(T));
        if(!allocation)return false;
        data_=static_cast<T*>(allocation);count_=count;return true;
    }
    size_t size()const{return count_;}
    T* data(){return data_;}
    T& operator[](size_t i){return data_[i];}
};
}
