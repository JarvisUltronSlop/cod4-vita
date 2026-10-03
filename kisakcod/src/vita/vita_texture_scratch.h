#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdlib>

namespace kisak::vita {
// Renderer-owned reusable upload storage. Reserve while registering backed
// textures, before publication; ordinary page reads never allocate. Growth and
// release require completed transfers and synchronous file-worker replies.
class VitaTextureScratch {
public:
    static constexpr size_t kLimit=8u*1024u*1024u, kGranularity=64u*1024u;
    using Resize=void* (*)(void*,size_t);
    using Release=void (*)(void*);
    VitaTextureScratch(Resize resize=&std::realloc,Release release=&std::free)
        :resize_(resize),release_(release) {}
    VitaTextureScratch(const VitaTextureScratch&)=delete;
    VitaTextureScratch& operator=(const VitaTextureScratch&)=delete;
    // No destructor: the renderer explicitly synchronizes and clears before
    // graphics shutdown. Static destruction cannot synchronize a dead context.
    template<class Quiesce> bool Reserve(size_t bytes,Quiesce quiesce) {
        if (!bytes || bytes>kLimit) return false;
        if (bytes<=capacity_) return true;
        const size_t wanted=(bytes+kGranularity-1)&~(kGranularity-1);
        if (!quiesce()) return false;
        void* replacement=resize_(data_,wanted);
        if (!replacement) return false; // Old buffer remains valid on failure.
        data_=static_cast<uint8_t*>(replacement);capacity_=wanted;return true;
    }
    template<class Quiesce> bool Clear(Quiesce quiesce) {
        if (data_ && !quiesce()) return false;
        if (data_) release_(data_);
        data_=nullptr;capacity_=0;return true;
    }
    uint8_t* Data() const {return data_;}
    size_t Capacity() const {return capacity_;}
private:
    Resize resize_;Release release_;
    uint8_t* data_=nullptr;size_t capacity_=0;
};
}
