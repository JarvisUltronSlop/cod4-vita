#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <vita/vita_asset_db.h>

namespace kisak::vita {

struct VitaRawFileRecord {
    std::string name;
    std::vector<uint8_t> bytes;
};

// Reader for the Windows oracle's pointer-free VITR v1 stream.  It is kept
// independent of the real script VM: the VM can later request the same raw
// source by name without learning about KVAP offsets or x86 pointers.
class VitaRawFileBundle {
public:
    bool Load(const VitaAssetDatabase& database);
    void Clear();

    bool Loaded() const { return loaded_; }
    uint32_t Count() const { return static_cast<uint32_t>(records_.size()); }
    const VitaRawFileRecord* Find(const char* name) const;
    const VitaRawFileRecord* Record(uint32_t index) const;

private:
    std::vector<VitaRawFileRecord> records_;
    bool loaded_ = false;
};

} // namespace kisak::vita
