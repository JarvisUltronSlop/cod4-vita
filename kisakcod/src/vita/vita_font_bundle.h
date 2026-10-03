#pragma once
#if KISAK_VITA_CPU_RUNTIME
#include <vita/vita_cpu_catalog_key.h>
#endif

#include <cstdint>
#include <string>
#include <vector>

#include <vita/vita_asset_db.h>

namespace kisak::vita {

struct VitaFontGlyph {
    uint16_t letter = 0;
    int8_t x0 = 0;
    int8_t y0 = 0;
    uint8_t dx = 0;
    uint8_t pixelWidth = 0;
    uint8_t pixelHeight = 0;
    float s0 = 0.0f;
    float t0 = 0.0f;
    float s1 = 0.0f;
    float t1 = 0.0f;
};

struct VitaFontRecord {
    std::string name;
    std::string imageName;
    std::string materialName;
    std::string glowMaterialName;
    int32_t pixelHeight = 0;
    std::vector<VitaFontGlyph> glyphs;
};

// Pointer-free VITF v1/v2 reader for the original font metrics and glyph UVs.
// V2 also preserves the exact source material and optional glow material.
// The associated image is resolved separately through VitaImageBundle.
class VitaFontBundle {
public:
    bool Load(const VitaAssetDatabase& database);
    void Clear();

    bool Loaded() const { return loaded_; }
    uint32_t Count() const { return static_cast<uint32_t>(fonts_.size()); }
    uint32_t Version() const { return version_; }
    const VitaFontRecord* Find(const char* name) const;
    const VitaFontRecord* Record(uint32_t index) const;

private:
#if KISAK_VITA_CPU_RUNTIME
    VitaCpuCatalogKey catalogKey_;
#endif
    std::vector<VitaFontRecord> fonts_;
    bool loaded_ = false;
    uint32_t version_ = 0;
};

} // namespace kisak::vita
