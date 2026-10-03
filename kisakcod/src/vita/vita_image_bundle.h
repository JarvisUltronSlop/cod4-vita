#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vita/vita_asset_db.h>

namespace kisak::vita {

struct VitaImageRecord {
    std::string name;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 0;
    uint32_t levelCount = 0;
    uint32_t format = 0;
    uint32_t flags = 0;
    std::vector<uint8_t> bytes;
};

// Loading-owner scoped: the mounted database must outlive the index. Retains
// bounded metadata/offsets, never a pack payload or a renderer reference.
class VitaImageIndex {
public:
    explicit VitaImageIndex(const VitaAssetDatabase& database) : database_(database) {}
    bool Build(uint64_t assetId);
    bool BuildMapColors(uint32_t mapIndex);
    bool Contains(const char* name) const;
    bool Read(const char* name, VitaImageRecord* output) const;
    uint32_t Count() const { return static_cast<uint32_t>(entries_.size()); }
private:
    struct Entry {
        VitaImageRecord image;
        uint64_t assetId=0, offset=0;
        uint32_t size=0, crc=0, chunkCrc=0;
    };
    bool AppendChunk(uint64_t assetId);
    const VitaAssetDatabase& database_;
    std::vector<Entry> entries_;
    size_t nameBytes_=0;
};

// Reader for the optional pointer-free VITI texture stream. Payload bytes are
// retained in Vita-owned memory and remain in the source image format so the
// gfx_vita backend can choose a compressed upload or a local decoder.
class VitaImageBundle {
public:
    bool Load(const VitaAssetDatabase& database, uint64_t assetId = 0,
              uint64_t additionalAssetId = 0);
    // Load only the named records from one VITI chunk. The reader still
    // walks and bounds-checks every source record, but does not retain
    // unrelated model/world payloads in the scalar guest heap. Complete
    // unfiltered loads continue to validate the full chunk CRC. retainMetadata
    // keeps every alias and validates the full CRC while enforcing the payload
    // cap; omitted bytes can then be loaded by exact name during native staging.
    bool LoadSelected(const VitaAssetDatabase& database, uint64_t assetId,
                      const std::vector<std::string>& names,
                      uint64_t additionalAssetId = 0,
                      uint64_t maxPayloadBytes = UINT64_MAX,
                      bool retainMetadata = false);
    // Stream one authored image record without constructing the temporary
    // 256-record vector used by LoadSelected. Native large-map staging calls
    // this for one semantic alias at a time after the world heap peak.
    bool LoadNamed(const VitaAssetDatabase& database, uint64_t assetId,
                   const char* name);
    bool LoadIndexed(const VitaImageIndex& index, const char* name, bool append=false);
    // Append one named record without evicting earlier source payloads. The
    // accelerated path uses this as a bounded upload keep-alive: VitaGL/GXM
    // is allowed to finish consuming every source range before the family is
    // released, while the scalar path continues to use LoadNamed/LoadSelected.
    bool AppendNamed(const VitaAssetDatabase& database, uint64_t assetId,
                     const char* name);
    // Search the map's base image chunk, then its bounded colour segments.
    // Retain only the exact named payload; append preserves earlier records.
    bool LoadNamedMapColor(const VitaAssetDatabase& database, uint32_t mapIndex,
                           const char* name, bool append = false);
    // Validate and retain the metadata for every record in the selected
    // image chunk without retaining any compressed payload bytes. Large-map
    // startup uses this as the package-probe state, then reloads the exact
    // source names needed by the renderer through LoadSelected.
    bool LoadMetadata(const VitaAssetDatabase& database, uint64_t assetId = 0,
                      uint64_t additionalAssetId = 0);
    bool Append(const VitaAssetDatabase& database, uint64_t assetId);
    bool AppendSelected(const VitaAssetDatabase& database, uint64_t assetId,
                        const std::vector<std::string>& names,
                        uint64_t maxPayloadBytes = UINT64_MAX);
    void Clear();

    bool Loaded() const { return loaded_; }
    uint32_t Count() const { return static_cast<uint32_t>(images_.size()); }
    const VitaImageRecord* Find(const char* name) const;
    const VitaImageRecord* Record(uint32_t index) const;

    // The scalar Vita3K diagnostic expands selected compressed images into
    // backend-owned RGBA.  Once a payload has been uploaded, release its
    // source bytes so the resident source-image envelope does not compete
    // with the decoded world/model textures.  Metadata remains available for
    // subsequent material lookups and the accelerated path never depends on
    // this streaming helper.
    void ReleasePayload(const char* name);

    // Drop all retained source payload bytes after every required texture has
    // been uploaded.  The image metadata remains available for diagnostics
    // and material lookup, while scalar Vita3K runs avoid keeping compressed
    // payloads alive alongside decoded renderer textures.
    uint64_t ReleaseAllPayloads();

private:
    std::vector<VitaImageRecord> images_;
    bool loaded_ = false;
};

} // namespace kisak::vita
