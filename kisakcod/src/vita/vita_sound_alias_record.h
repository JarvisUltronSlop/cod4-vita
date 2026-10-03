#pragma once
#include <cstdint>

namespace kisak::vita {
constexpr uint32_t kVitaSoundAliasMagic = 0x4c415356; // VSAL
constexpr uint32_t kVitaSoundAliasVersion = 1;
constexpr uint32_t kVitaSoundAliasMaxRecords = 32768;
struct VitaSpeakerLevels { int32_t speaker, numLevels; float levels[2]; };
struct VitaSpeakerChannelMap { int32_t speakerCount; VitaSpeakerLevels speakers[6]; };
// Fixed-width offline record. No source pointers or live mixer state cross
// the desktop/Vita boundary. Ordering matches the original alias list.
struct VitaSoundAliasRecord {
    char aliasName[128], subtitle[128], secondaryAliasName[128], chainAliasName[128];
    char soundFile[128], curveName[128], speakerMapName[128];
    int32_t sequence, flags, startDelay;
    uint32_t soundType, sourceFileExists, curveKnotCount, speakerMapDefault;
    float volMin, volMax, pitchMin, pitchMax, distMin, distMax;
    float slavePercentage, probability, lfePercentage, centerPercentage;
    float envelopMin, envelopMax, envelopPercentage;
    float curveKnots[8][2];
    VitaSpeakerChannelMap speakerMaps[2][2];
};
static_assert(sizeof(VitaSoundAliasRecord) == 1440);
}
