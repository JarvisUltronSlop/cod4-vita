#pragma once

#include <cstdint>

namespace kisak::vita {

constexpr uint32_t kVitaAnimationMagic = 0x51544956; // "VITQ"
constexpr uint32_t kVitaAnimationVersionSource = 3;
constexpr uint32_t kVitaAnimationBoneNameBytes = 64;
constexpr uint32_t kVitaAnimationNotifyNameBytes = 128;

#pragma pack(push, 1)

// VITQ v3 owns the complete compressed XAnimParts input without retaining a
// desktop pointer. The native payload is laid out as bone names, the seven
// compressed arrays, the frame-index array, notetracks, then the optional
// pointer-free delta graph. The sampled payload retains the v2 model tracks
// used by the scalar oracle and the current bounded viewmodel renderer.
struct VitaAnimationRecordV3 {
    char name[128];
    uint16_t numframes;
    uint8_t bLoop;
    uint8_t bDelta;
    uint8_t boneCount[10];
    uint8_t notifyCount;
    uint8_t assetType;
    uint8_t isDefault;
    uint8_t reserved8;
    uint16_t dataByteCount;
    uint16_t dataShortCount;
    uint16_t dataIntCount;
    uint16_t randomDataByteCount;
    uint16_t randomDataIntCount;
    uint16_t reserved16;
    uint32_t randomDataShortCount;
    uint32_t indexCount;
    uint32_t boneNameCount;
    uint32_t nativePayloadBytes;
    uint32_t trackCount;
    uint32_t sampledPayloadBytes;
    uint32_t deltaPayloadBytes;
    float framerate;
    float frequency;
};

struct VitaAnimationBoneNameV3 {
    char value[kVitaAnimationBoneNameBytes];
};

struct VitaAnimationNotifyV3 {
    char name[kVitaAnimationNotifyNameBytes];
    float time;
};

// A source XAnimDeltaPart contains two independently allocated flexible
// records. This header preserves their scalar state and gives the following
// four byte ranges explicit lengths:
//   trans frames, trans indices, quaternion frames, quaternion indices.
struct VitaAnimationDeltaV3 {
    uint8_t hasTrans;
    uint8_t hasQuat;
    uint8_t transSmall;
    uint8_t reserved8;
    uint16_t transSize;
    uint16_t quatSize;
    uint32_t reserved32;
    float transBase[3]; // frame0 when size=0, otherwise frames.mins
    float transRange[3];
    int16_t quatFrame0[2];
    uint32_t transFrameBytes;
    uint32_t transIndexBytes;
    uint32_t quatFrameBytes;
    uint32_t quatIndexBytes;
};

#pragma pack(pop)

static_assert(sizeof(VitaAnimationRecordV3) == 194);
static_assert(sizeof(VitaAnimationBoneNameV3) == 64);
static_assert(sizeof(VitaAnimationNotifyV3) == 132);
static_assert(sizeof(VitaAnimationDeltaV3) == 56);

} // namespace kisak::vita
