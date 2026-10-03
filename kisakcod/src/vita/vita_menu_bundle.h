#pragma once
#if KISAK_VITA_CPU_RUNTIME
#include <vita/vita_cpu_catalog_key.h>
#endif

#include <cstdint>
#include <string>
#include <vector>

#include <vita/vita_asset_db.h>

namespace kisak::vita {

struct VitaMenuItem {
    std::string name;
    std::string text;
    std::string action;
    std::string onAccept;
    int32_t type = 0;
    int32_t alignment = 0;
    int32_t textStyle = 0;
    uint32_t flags = 0;
    float rect[4]{};
    float textRect[4]{};
    float textScale = 0.0f;
};

struct VitaMenuRecord {
    std::string name;
    std::string font;
    uint32_t fullScreen = 0;
    int32_t style = 0;
    float rect[4]{};
    float foreColor[4]{};
    float backColor[4]{};
    float focusColor[4]{};
    float disableColor[4]{};
    std::vector<VitaMenuItem> items;
};

// Reader for the Windows oracle's pointer-free VITM v1 menu stream.  It is
// intentionally independent of ui_shared.h so a Vita build never receives
// an x86 menuDef_t or an engine-owned string pointer.
class VitaMenuBundle {
public:
    bool Load(const VitaAssetDatabase& database);
    void Clear();

    bool Loaded() const { return loaded_; }
    uint32_t Count() const { return static_cast<uint32_t>(menus_.size()); }
    const VitaMenuRecord* Find(const char* name) const;
    const VitaMenuItem* FindItem(const char* menuName, const char* itemName) const;
    const VitaMenuRecord* Record(uint32_t index) const;

private:
#if KISAK_VITA_CPU_RUNTIME
    VitaCpuCatalogKey catalogKey_;
#endif
    std::vector<VitaMenuRecord> menus_;
    bool loaded_ = false;
};

} // namespace kisak::vita
