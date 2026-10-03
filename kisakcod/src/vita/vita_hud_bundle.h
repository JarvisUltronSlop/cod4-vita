#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vita/vita_asset_db.h>

namespace kisak::vita {

// Pointer-free VITH role bindings. Pixel data remains owned by VitaImageBundle;
// this table only connects a gameplay/UI role to an image name in that bundle.
struct VitaHudBinding {
    std::string role;
    std::string image;
};

class VitaHudBundle {
public:
    bool Load(const VitaAssetDatabase& database, uint64_t assetId = 0);
    void Clear();

    bool Loaded() const { return loaded_; }
    uint32_t Count() const { return static_cast<uint32_t>(bindings_.size()); }
    const VitaHudBinding* Find(const char* role) const;
    const char* ImageFor(const char* role) const;

private:
    std::vector<VitaHudBinding> bindings_;
    bool loaded_ = false;
};

} // namespace kisak::vita
