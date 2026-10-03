#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vita/vita_asset_db.h>

namespace kisak::vita {

struct VitaAnimationSample {
    float quat[4]{};
    float trans[3]{};
    float transWeight = 0.0f;
};

struct VitaAnimationTrack {
    std::string modelName;
    uint32_t boneCount = 0;
    uint32_t frameCount = 0;
    std::vector<VitaAnimationSample> samples;

    const VitaAnimationSample* Sample(uint32_t frame, uint32_t bone) const;
};

struct VitaAnimationNotify {
    std::string name;
    float time = 0.0f;
};

struct VitaAnimationDelta {
    bool present = false;
    bool hasTrans = false;
    bool hasQuat = false;
    bool transSmall = false;
    uint16_t transSize = 0;
    uint16_t quatSize = 0;
    float transBase[3]{};
    float transRange[3]{};
    int16_t quatFrame0[2]{};
    std::vector<uint8_t> transFrames;
    std::vector<uint8_t> transIndices;
    std::vector<uint8_t> quatFrames;
    std::vector<uint8_t> quatIndices;
};

struct VitaAnimationClip {
    std::string name;
    uint16_t numFrames = 0;
    bool loop = false;
    bool delta = false;
    bool isDefault = false;
    bool sourceCompressed = false;
    uint8_t boneCount[10] = {};
    uint8_t notifyCount = 0;
    uint8_t assetType = 0;
    uint32_t dataByteCount = 0;
    uint32_t dataShortCount = 0;
    uint32_t dataIntCount = 0;
    uint32_t randomDataByteCount = 0;
    uint32_t randomDataIntCount = 0;
    uint32_t randomDataShortCount = 0;
    uint32_t indexCount = 0;
    float framerate = 0.0f;
    float frequency = 0.0f;
    std::vector<std::string> boneNames;
    std::vector<uint8_t> dataByte;
    std::vector<int16_t> dataShort;
    std::vector<int32_t> dataInt;
    std::vector<uint8_t> randomDataByte;
    std::vector<int16_t> randomDataShort;
    std::vector<int32_t> randomDataInt;
    std::vector<uint8_t> indices;
    VitaAnimationDelta deltaPart;
    std::vector<VitaAnimationTrack> tracks;
    std::vector<VitaAnimationNotify> notifies;

    float DurationSeconds() const;
    const VitaAnimationTrack* TrackForModel(const char* modelName) const;
    bool SourceComplete() const;
};

class VitaAnimationBundle {
public:
    bool Load(const VitaAssetDatabase& database);
    void Clear();
    bool Loaded() const { return loaded_; }
    uint32_t WireVersion() const { return wireVersion_; }
    size_t StagingCapacityBytes() const { return stagingCapacityBytes_; }
    const char* LoadError() const { return loadError_; }
    bool SourceComplete() const;
    uint32_t Count() const { return static_cast<uint32_t>(clips_.size()); }
    uint32_t NotifyCount() const;
    const VitaAnimationClip* Find(const char* name) const;
    const VitaAnimationTrack* FindTrack(const char* animationName,
                                        const char* modelName) const;
    // Return authored notetracks crossed by a normalized playback interval.
    // The caller owns the small fixed output buffer; no heap work is needed
    // while the Vita frame loop dispatches viewmodel events.
    uint32_t CollectNotifies(const VitaAnimationClip* clip, float oldPhase,
                             float newPhase, bool wrapped,
                             const VitaAnimationNotify** output,
                             uint32_t outputCapacity) const;
    const std::vector<VitaAnimationClip>& Records() const { return clips_; }

private:
    std::vector<VitaAnimationClip> clips_;
    bool loaded_ = false;
    uint32_t wireVersion_ = 0;
    size_t stagingCapacityBytes_ = 0;
    const char* loadError_ = "";
};

} // namespace kisak::vita
