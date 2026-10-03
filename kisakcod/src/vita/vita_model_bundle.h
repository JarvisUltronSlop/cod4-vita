#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vita/vita_asset_db.h>

namespace kisak::vita {

#pragma pack(push, 1)
struct VitaModelVertex {
    float xyz[3];
    float texCoord[2];
    uint32_t normal;
    uint32_t tangent;
    uint32_t color;
};

// VITX v3 keeps the desktop XModel skeleton pointer-free while preserving both
// sides of the KisakCOD skinning boundary: the first pose is the raw local
// XModel transform consumed by CalcSkelNonRootBones, and the second is the
// already-composed baseMat used by R_SkinXModel for inverse-bind skinning.
// Parents are stored separately as one byte per bone so the Vita reader can
// reproduce the DObj hierarchy without host pointers. VITX v5 also carries a
// fixed, bounded textual name for every bone so the Vita asset registry can
// intern genuine scr_string_t identities when it constructs an XModel.
struct VitaModelBone {
    float quat[4];
    float trans[3];
    float transWeight;
    float bindQuat[4];
    float bindTrans[3];
    float bindTransWeight;
};

struct VitaModelVertexWeight {
    uint16_t bone[4];
    uint16_t weight[4];
};
#pragma pack(pop)

static_assert(sizeof(VitaModelVertex) == 32);
static_assert(sizeof(VitaModelBone) == 64);
static_assert(sizeof(VitaModelVertexWeight) == 16);

struct VitaModelSurface {
    std::string materialName;
    uint32_t vertexStart = 0;
    uint32_t vertexCount = 0;
    uint32_t indexStart = 0;
    uint32_t indexCount = 0;
};

// VLOD is the GPU-oriented companion to the source-facing VITX record.  VITX
// deliberately remains the pointer-free LOD0/skeleton contract consumed by
// the real KisakCOD runtime; this record carries only authored lower-detail
// geometry used by the persistent Vita render archive.
struct VitaModelRenderLod {
    uint32_t sourceLod = 0;
    float distance = 0.0f;
    std::vector<VitaModelSurface> surfaces;
    std::vector<VitaModelVertex> vertices;
    std::vector<uint16_t> indices;
};

struct VitaModelRecord {
    std::string name;
    float mins[3] = {0.0f, 0.0f, 0.0f};
    float maxs[3] = {0.0f, 0.0f, 0.0f};
    std::vector<VitaModelSurface> surfaces;
    std::vector<VitaModelVertex> vertices;
    std::vector<uint8_t> parents;
    std::vector<std::string> boneNames;
    std::vector<VitaModelBone> bones;
    bool localSkeleton = false;
    // VITX v3 stores authored tag indices one-based in the skin-header
    // reserved field: tag_weapon in the low half and tag_camera in the high
    // half. VITX v4 appends the source tag_flash index, and v5 adds the
    // textual bone-name table required to rebuild XModel::boneNames. KisakCOD attaches
    // gunXModel[0] to tag_weapon, derives the first-person camera from
    // tag_camera, and resolves muzzle FX from tag_flash when present.
    int16_t weaponAttachBone = -1;
    int16_t cameraTagBone = -1;
    int16_t flashTagBone = -1;
    std::vector<VitaModelVertexWeight> weights;
    std::vector<uint16_t> indices;
    uint32_t lodCount = 1;
    uint32_t lodRampType = 0;
    float lodDistances[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<VitaModelRenderLod> renderLods;

    bool HasSkinning() const {
        return !bones.empty() && weights.size() == vertices.size();
    }

    const VitaModelRenderLod* FindRenderLod(uint32_t sourceLod) const;
};

class VitaModelBundle {
public:
    bool Load(const VitaAssetDatabase& database, uint64_t assetId = 0,
              uint64_t renderLodAssetId = 0);
    void Clear();
    bool Loaded() const { return loaded_; }
    uint32_t WireVersion() const { return wireVersion_; }
    bool SourceComplete() const {
        if (!loaded_ || wireVersion_ < 5)
            return false;
        for (const VitaModelRecord& model : models_) {
            if (model.boneNames.size() != model.bones.size())
                return false;
        }
        return true;
    }
    uint32_t Count() const { return static_cast<uint32_t>(models_.size()); }
    bool RenderLodsLoaded() const { return renderLodsLoaded_; }
    uint32_t RenderLodCount() const;
    // The native map archive owns an immutable GPU copy after Loading. Drop
    // the render-only CPU vectors at that boundary; source-facing VITX LOD0,
    // skeletons, viewmodels, and the typed registry remain untouched.
    void ReleaseRenderLods();
    const VitaModelRecord* Find(const char* name) const;
    const VitaModelRecord* FindContains(const char* name) const;
    const std::vector<VitaModelRecord>& Records() const { return models_; }

    // Keep only the source XModels needed by the selected map and the active
    // weapon closure.  The package stores the complete map model chunk, but
    // retaining every unused record wastes guest heap alongside the map BSP
    // and image payloads on Vita3K.
    void PruneToNames(const std::vector<std::string>& names);

private:
    std::vector<VitaModelRecord> models_;
    uint32_t wireVersion_ = 0;
    bool loaded_ = false;
    bool renderLodsLoaded_ = false;
};

} // namespace kisak::vita
