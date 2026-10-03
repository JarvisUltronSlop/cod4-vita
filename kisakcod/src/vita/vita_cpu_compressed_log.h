#pragma once
#include "../../deps/zlib/zlib.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace kisak::vita {
// Independent checksummed zlib members, at most 32 KiB uncompressed each.
// A flush finishes the current member: even an abruptly closed application
// leaves all previously flushed members independently decodable. No heap use.
class VitaCpuCompressedLog {
    static constexpr size_t kInputBytes=32768,kArenaBytes=320*1024;
    z_stream stream_{};
    alignas(16) unsigned char arena_[kArenaBytes]{};
    unsigned char input_[kInputBytes]{},output_[kInputBytes+1024]{};
    size_t arenaUsed_=0,pending_=0;
    bool ready_=false;
    static voidpf Allocate(voidpf opaque,uInt items,uInt size) {
        auto& self=*static_cast<VitaCpuCompressedLog*>(opaque);
        if(size && items>kArenaBytes/size)return nullptr;
        const size_t bytes=size_t(items)*size,at=(self.arenaUsed_+15)&~size_t(15);
        if(at>kArenaBytes || bytes>kArenaBytes-at)return nullptr;
        void* result=self.arena_+at;std::memset(result,0,bytes);self.arenaUsed_=at+bytes;return result;
    }
    static void Release(voidpf,voidpf) {}
public:
    bool Initialize() {
        if(ready_)deflateEnd(&stream_);
        ready_=false;stream_={};arenaUsed_=pending_=0;
        stream_.zalloc=Allocate;stream_.zfree=Release;stream_.opaque=this;
        ready_=deflateInit(&stream_,1)==Z_OK;return ready_;
    }
    size_t ArenaUsed() const {return arenaUsed_;}
    template<class Sink> bool Flush(Sink sink) {
        if(!ready_)return false;
        if(!pending_)return true;
        stream_.next_in=input_;stream_.avail_in=uInt(pending_);
        stream_.next_out=output_;stream_.avail_out=sizeof(output_);
        const int result=deflate(&stream_,Z_FINISH);
        if(result!=Z_STREAM_END || stream_.avail_in || !sink(output_,sizeof(output_)-stream_.avail_out))return false;
        pending_=0;return deflateReset(&stream_)==Z_OK;
    }
    template<class Sink> bool Append(const char* data,size_t bytes,Sink sink) {
        if(!ready_)return false;
        while(bytes) {
            const size_t count=std::min(bytes,kInputBytes-pending_);
            std::memcpy(input_+pending_,data,count);pending_+=count;data+=count;bytes-=count;
            if(pending_==kInputBytes && !Flush(sink))return false;
        }
        return true;
    }
};
}
