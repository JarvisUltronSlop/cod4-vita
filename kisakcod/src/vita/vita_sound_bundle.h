#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <vita/vita_asset_db.h>
#include <vita/vita_sound_alias_record.h>
#if KISAK_VITA_CPU_RUNTIME
#include <vita/vita_cpu_file.h>
#endif

namespace kisak::vita {

struct VitaSoundRecord {
    std::string name;
    uint32_t rate = 0;
    uint32_t bits = 0;
    uint32_t channels = 0;
    uint32_t samples = 0;
    uint32_t format = 0;
    std::vector<uint8_t> bytes;
    const KvapChunk* sourceChunk = nullptr;
    uint64_t sourceOffset = 0;
    uint32_t sourceBytes = 0;
};

// Reader for the optional pointer-free VITS v1 PCM stream.  The host exporter
// only admits already-decoded 16-bit samples, so the Vita side never sees an
// MSS pointer or a desktop decoder object.
class VitaSoundBundle {
public:
    VitaSoundBundle() = default;
    ~VitaSoundBundle() { Clear(); }
    VitaSoundBundle(const VitaSoundBundle&) = delete;
    VitaSoundBundle& operator=(const VitaSoundBundle&) = delete;
    // Own the backing handles until every prepared sample and alias is gone.
    bool LoadPackage(const char* path, const char* commonPath = nullptr);
    // The database and its read context must outlive this borrowed mount.
    bool Load(const VitaAssetDatabase& database);
    void Clear();

    bool Loaded() const { return loaded_; }
    uint32_t Count() const { return static_cast<uint32_t>(sounds_.size()); }
    uint64_t PcmBytes() const;
    const VitaSoundRecord* Find(const char* name) const;
    const VitaSoundRecord* Record(uint32_t index) const;
    // Main-thread asset preparation only. Audio playback never performs a
    // package read, and cannot retain an asset after its map is released.
    const VitaSoundRecord* PreparePcm(const char* name) const;
    bool ReadAlias(const char* name, std::vector<VitaSoundAliasRecord>* output) const;
    bool HasAliasCatalog() const { return !aliases_.empty(); }
    uint32_t AliasCount() const { return static_cast<uint32_t>(aliases_.size()); }
    const char* AliasName(uint32_t index) const { return index < aliases_.size() ? aliases_[index].name.c_str() : nullptr; }
    void FreezePreparation() const { preparationFrozen_ = true; }

private:
    bool LoadRecords(const VitaAssetDatabase& database);
    struct AliasRange { std::string name; const KvapChunk* chunk; uint64_t offset; uint32_t count; };
    std::vector<AliasRange> aliases_;
    mutable std::vector<VitaSoundRecord> sounds_;
    const VitaAssetDatabase* database_ = nullptr;
    VitaAssetDatabase ownedDatabase_;
#if KISAK_VITA_CPU_RUNTIME
    VitaCpuFile primaryWorker_,commonWorker_;
#endif
    std::FILE* primaryFile_ = nullptr;
    std::FILE* commonFile_ = nullptr;
    mutable bool preparationFrozen_ = false;
    bool loaded_ = false;
};

} // namespace kisak::vita
