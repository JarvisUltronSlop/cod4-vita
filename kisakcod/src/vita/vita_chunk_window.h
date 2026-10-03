#pragma once
#include <vita/vita_asset_db.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace kisak::vita {
// One bounded window, no payload-sized temporary. Every first-read window
// contributes to the original whole-chunk CRC in file order. Backward header
// reads may refill a window but cannot count its bytes twice. Publication is
// allowed only after Finish verifies all payload bytes and the stored checksum.
class VitaChunkWindow {
    const VitaAssetDatabase& database_;
    const KvapChunk* chunk_;
    mutable std::array<uint8_t,65536> window_;
    mutable size_t start_=SIZE_MAX,length_=0,checked_=0;
    mutable uint32_t crc_=0xffffffffu;
    mutable bool good_=true;
    bool Fill(size_t start) const {
        const size_t length=std::min(window_.size(),size()-start);
        const auto error=database_.ReadRange(chunk_,start,window_.data(),length);
        if(error!=KvapError::Ok){KvapReportReadFailure(chunk_,error);good_=false;return false;}
        start_=start;length_=length;
        if(start==checked_){crc_=KvapCrc32Update(crc_,window_.data(),length);checked_+=length;}
        return true;
    }
public:
    VitaChunkWindow(const VitaAssetDatabase& database,const KvapChunk* chunk)
        :database_(database),chunk_(chunk) {
        good_=chunk && chunk->rawSize<=std::numeric_limits<size_t>::max()
            && chunk->rawSize==chunk->storedSize
            && chunk->codec==static_cast<uint32_t>(KvapCodec::None);
    }
    size_t size() const {return chunk_?static_cast<size_t>(chunk_->rawSize):0;}
    bool Read(size_t offset,void* output,size_t bytes) const {
        if(!good_ || (!output && bytes) || offset>size() || bytes>size()-offset){good_=false;return false;}
        auto* dest=static_cast<uint8_t*>(output);
        while(bytes){
            const size_t block=offset/window_.size()*window_.size();
            while(checked_<block)if(!Fill(checked_))return false;
            if(start_!=block && !Fill(block))return false;
            const size_t count=std::min(bytes,length_-(offset-start_));
            std::memcpy(dest,window_.data()+offset-start_,count);
            dest+=count;offset+=count;bytes-=count;
        }
        return true;
    }
    bool Finish() const {
        if(!good_)return false;
        while(checked_<size())if(!Fill(checked_))return false;
        if(~crc_!=chunk_->crc32){KvapReportReadFailure(chunk_,KvapError::ChecksumMismatch,~crc_);return false;}
        return true;
    }
};
}
