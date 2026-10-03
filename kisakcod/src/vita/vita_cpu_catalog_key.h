#pragma once
#include <vita/vita_asset_db.h>
#include <array>
#include <cstring>
namespace kisak::vita {
// Exact, ordered metadata from an already validated immutable package catalog.
// Primary/overlay order is part of the identity, so map overrides win as before.
class VitaCpuCatalogKey {
public:
    void Capture(const VitaAssetDatabase& database,VitaAssetType type) {
        count_=0;valid_=true;
        for(uint32_t i=0;i<database.ChunkCount();++i) {
            const auto* chunk=database.Chunk(i);
            if(!chunk || chunk->type!=uint32_t(type))continue;
            const auto* header=database.HeaderFor(chunk);
            if(!header || count_==records_.size()){valid_=false;return;}
            auto& r=records_[count_++];r.chunk=*chunk;
            r.package=header->packageId;std::memcpy(r.source,header->sourceHash,32);
        }
        valid_=count_!=0;
    }
    bool operator==(const VitaCpuCatalogKey& other) const {
        if(!valid_ || !other.valid_ || count_!=other.count_)return false;
        for(unsigned i=0;i<count_;++i) {
            const auto& a=records_[i];const auto& b=other.records_[i];
            if(a.package!=b.package || std::memcmp(a.source,b.source,32)
                || std::memcmp(&a.chunk,&b.chunk,sizeof(KvapChunk)))return false;
        }
        return true;
    }
private:
    struct Record {KvapChunk chunk{};uint64_t package=0;uint8_t source[32]{};};
    std::array<Record,16> records_{};
    unsigned count_=0;bool valid_=false;
};
}
