#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace kisak::vita {
// Source constructors need contiguous objects, not one contiguous allocation
// for the whole registry. Logical marks retain the original arena accounting.
// Small objects share fixed pages acquired during loading. Larger objects get
// a contiguous page of their own without displacing the active small page.
// Runs describe consecutive allocations on a page, so rollback metadata is
// needed only when switching pages/alignment, not for every small string.
class VitaRegistryStorage {
public:
    using Allocator=void* (*)(size_t);
    using Deallocator=void (*)(void*);
    static constexpr size_t kPageBytes=64u*1024u;
    static constexpr size_t kMaximumOverhead=2u*1024u*1024u;
    static constexpr size_t kMaximumAlignment=4096;
    enum class Failure : unsigned { None, InvalidRequest, LogicalCapacity, InvalidMark, BackingBudget, Heap };
    VitaRegistryStorage(Allocator allocate=&std::malloc,Deallocator release=&std::free)
        : allocate_(allocate),release_(release) {}
    ~VitaRegistryStorage(){Clear();}
    VitaRegistryStorage(const VitaRegistryStorage&)=delete;
    VitaRegistryStorage& operator=(const VitaRegistryStorage&)=delete;

    void* Allocate(size_t bytes,size_t alignment,size_t capacity,size_t& cursor) {
        failure_=Failure::None;
        if(!bytes || !alignment || alignment>kMaximumAlignment || (alignment&(alignment-1)))return Fail(Failure::InvalidRequest);
        if(cursor>capacity || cursor>std::numeric_limits<size_t>::max()-(alignment-1))return Fail(Failure::LogicalCapacity);
        const size_t aligned=(cursor+alignment-1)&~(alignment-1);
        if(aligned>capacity || bytes>capacity-aligned)return Fail(Failure::LogicalCapacity);
        if(!Rewind(cursor))return Fail(Failure::InvalidMark);
        const size_t end=aligned+bytes;
        // Keep a shared page's unusable tail small even for awkward array sizes.
        const bool small=bytes<kPageBytes/16;
        void* data=nullptr;
        if(small && small_) {
            if(last_ && last_->page==small_) {
                const size_t offset=last_->dataOffset+aligned-last_->start;
                if(offset<=small_->capacity && bytes<=small_->capacity-offset) {
                    auto* candidate=small_->data+offset;
                    if((reinterpret_cast<uintptr_t>(candidate)&(alignment-1))==0) {
                        small_->used=offset+bytes;
                        data=candidate;
                    }
                }
            }
            if(!data)data=BeginRun(small_,cursor,aligned,end,alignment);
        }
        if(!data) {
            const size_t pageOverhead=sizeof(Page)+alignof(Run)-1;
            const size_t runOverhead=sizeof(Run)+std::max<size_t>(64,alignment)-1;
            if(end>std::numeric_limits<size_t>::max()-kMaximumOverhead)return Fail(Failure::BackingBudget);
            const size_t limit=end+kMaximumOverhead;
            if(reserved_>limit || pageOverhead>limit-reserved_ ||
               runOverhead>limit-reserved_-pageOverhead || end-cursor>limit-reserved_-pageOverhead-runOverhead)
                return Fail(Failure::BackingBudget);
            const size_t required=runOverhead+end-cursor;
            const size_t preferred=small?std::max(kPageBytes,required):required;
            const size_t payload=std::min(preferred,limit-reserved_-pageOverhead);
            size_t rawBytes=pageOverhead+payload;
            void* raw=allocate_(rawBytes);
            if(!raw && payload>required) {
                rawBytes=pageOverhead+required;
                raw=allocate_(rawBytes);
            }
            if(!raw)return Fail(Failure::Heap);
            auto* page=static_cast<Page*>(raw);
            page->previous=pages_;page->previousSmall=small_;
            page->rawBytes=rawBytes;page->capacity=rawBytes-pageOverhead;page->used=0;
            const auto base=reinterpret_cast<uintptr_t>(raw)+sizeof(Page);
            page->data=reinterpret_cast<uint8_t*>((base+alignof(Run)-1)&~uintptr_t(alignof(Run)-1));
            // A fresh page reserves the worst-case run alignment above.
            data=BeginRun(page,cursor,aligned,end,alignment);
            if(!data) {release_(page);return Fail(Failure::BackingBudget);}
            pages_=page;if(small)small_=page;
            ++count_;reserved_+=rawBytes;
            peakReserved_=std::max(peakReserved_,reserved_);maximumRequest_=std::max(maximumRequest_,rawBytes);
        }
        std::memset(data,0,bytes);
        last_->end=end;cursor=end;
        return data;
    }
    bool Rewind(size_t mark) {
        if(mark>Mark())return false;
        // Rewind runs in allocation order, which may revisit a small page on
        // both sides of a large allocation. Retained objects never move.
        while(last_ && mark<=last_->start) {
            Run* old=last_;Page* page=old->page;
            last_=old->previous;page->used=old->previousUsed;
            if(!page->used) {
                pages_=page->previous;
                if(small_==page)small_=page->previousSmall;
                reserved_-=page->rawBytes;--count_;release_(page);
            }
        }
        if(last_) {
            last_->end=mark;
            last_->page->used=last_->dataOffset+mark-last_->start;
        }
        return true;
    }
    void Clear(){Rewind(0);peakReserved_=maximumRequest_=0;failure_=Failure::None;}
    size_t Mark() const{return last_?last_->end:0;}
    size_t Reserved() const{return reserved_;}
    size_t PeakReserved() const{return peakReserved_;}
    size_t MaximumRequest() const{return maximumRequest_;}
    size_t Blocks() const{return count_;}
    Failure LastFailure() const{return failure_;}
private:
    void* Fail(Failure failure){failure_=failure;return nullptr;}
    struct Page {Page* previous;Page* previousSmall;size_t capacity,used,rawBytes;uint8_t* data;};
    struct Run {Run* previous;Page* page;size_t start,end,dataOffset,previousUsed;};
    void* BeginRun(Page* page,size_t cursor,size_t aligned,size_t end,size_t alignment) {
        const size_t offset=(page->used+alignof(Run)-1)&~(alignof(Run)-1);
        if(offset>page->capacity || sizeof(Run)>page->capacity-offset)return nullptr;
        const auto base=reinterpret_cast<uintptr_t>(page->data+offset+sizeof(Run));
        const size_t mappingAlignment=std::max<size_t>(64,alignment);
        const size_t padding=(cursor-base)&(mappingAlignment-1);
        const size_t dataOffset=offset+sizeof(Run)+padding;
        if(dataOffset>page->capacity || end-cursor>page->capacity-dataOffset)return nullptr;
        auto* run=reinterpret_cast<Run*>(page->data+offset);
        run->previous=last_;run->page=page;run->start=cursor;run->end=end;
        run->dataOffset=dataOffset;run->previousUsed=page->used;
        last_=run;page->used=dataOffset+end-cursor;
        return page->data+dataOffset+aligned-cursor;
    }
    Run* last_=nullptr;
    Page* pages_=nullptr;
    Page* small_=nullptr;
    size_t count_=0,reserved_=0,peakReserved_=0,maximumRequest_=0;
    Failure failure_=Failure::None;
    Allocator allocate_;Deallocator release_;
};
}
