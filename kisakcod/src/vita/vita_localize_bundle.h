#pragma once
#if KISAK_VITA_CPU_RUNTIME
#include <vita/vita_cpu_catalog_key.h>
#endif

#include <cstdint>
#include <string>
#include <vector>

#include <vita/vita_asset_db.h>

namespace kisak::vita {

struct VitaLocalizedString {
    std::string name;
    std::string value;
};

// Reader for the pointer-free VITZ localized key/value stream emitted by the
// Windows oracle.  The UI can resolve @MENU_* tokens without importing the
// desktop LocalizeEntry graph or a platform localization API.
class VitaLocalizeBundle {
public:
    bool Load(const VitaAssetDatabase& database);
    void Clear();

    bool Loaded() const { return loaded_; }
    uint32_t Count() const { return static_cast<uint32_t>(records_.size()); }
    const VitaLocalizedString* Find(const char* name) const;
    const VitaLocalizedString* Record(uint32_t index) const;

private:
#if KISAK_VITA_CPU_RUNTIME
    VitaCpuCatalogKey catalogKey_;
#endif
    std::vector<VitaLocalizedString> records_;
    bool loaded_ = false;
};

} // namespace kisak::vita
