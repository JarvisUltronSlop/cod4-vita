#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vita/vita_asset_db.h>

namespace kisak::vita {

struct VitaMaterialRecord {
    std::string name;
    std::string colorMap;
    // KisakCOD's MaterialTextureDef semantic slots are retained by name so
    // Vita can bind the same authored material layers as the desktop path.
    // Empty slots are valid: a number of stock UI/utility materials expose
    // only a color map or a code-resource texture.
    std::string normalMap;
    std::string specularMap;
    std::string techniqueSet;
    uint32_t surfaceTypeBits = 0;
    uint8_t gameFlags = 0;
    uint8_t sortKey = 0;
    uint8_t stateFlags = 0;
    uint8_t textureCount = 0;
    uint8_t colorSamplerState = 0;
    uint8_t normalSamplerState = 0;
    uint8_t specularSamplerState = 0;
    // KisakCOD's Material_SetMaterialDrawRegion result. Region 3 is the
    // shadow-only route and must not enter the normal camera color pass.
    uint8_t cameraRegion = 0;
    // OR of KisakCOD MaterialPass::customSamplerFlags bits for the authored
    // lit techniques. Bit 0x02 is the primary lightmap and 0x04 is the
    // secondary lightmap. Keep the source bit values intact so the Vita
    // renderer can make the same pass-selection decision without native
    // Material/Technique pointers.
    uint8_t lightmapSamplerFlags = 0;
    // VITL v4: first source lit-pass GfxStateBits pair. These values are
    // retained verbatim so surface alpha-test/blend/depth/cull behavior can
    // be decoded on Vita instead of inferred from material names.
    uint32_t litStateBits0 = 0;
    uint32_t litStateBits1 = 0;
    uint8_t litStateValid = 0;
    // VITL v5: exact first TECHNIQUE_UNLIT pass, independent of lit state.
    uint32_t unlitStateBits0 = 0;
    uint32_t unlitStateBits1 = 0;
    uint8_t unlitStateValid = 0;
    uint8_t colorSemantic = 2; // TS_COLOR_MAP; TS_2D is retained as zero.

    bool HasNormalMap() const { return !normalMap.empty(); }
    bool HasSpecularMap() const { return !specularMap.empty(); }
};

// Pointer-free material-to-image lookup exported from the desktop database.
class VitaMaterialBundle {
public:
    bool Load(const VitaAssetDatabase& database, uint64_t assetId = 0);
    void Clear();

    bool Loaded() const { return loaded_; }
    uint32_t Count() const { return static_cast<uint32_t>(materials_.size()); }
    const VitaMaterialRecord* Find(const char* name) const;
    const VitaMaterialRecord* Record(uint32_t index) const;

private:
    std::vector<VitaMaterialRecord> materials_;
    bool loaded_ = false;
};

} // namespace kisak::vita
