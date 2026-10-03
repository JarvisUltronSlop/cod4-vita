#pragma once

#include <cstdint>

namespace kisak::vita {

constexpr uint32_t VITA_PRIMARY_LIGHT_MAGIC = 0x50544956; // "VITP"
constexpr uint32_t VITA_PRIMARY_LIGHT_VERSION = 1;
constexpr uint32_t VITA_PRIMARY_LIGHT_RECORD_BYTES = 64;
constexpr uint32_t VITA_PRIMARY_LIGHT_MAX_RECORDS = 128;

// VITP deliberately mirrors ComPrimaryLight without its defName pointer.
// The byte layout is shared by the Windows exporter, KVAP validator, and Vita
// runtime, so no compiler ABI or host address can cross the package boundary.
#pragma pack(push, 1)
struct VitaPrimaryLightHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t recordCount;
    uint32_t recordBytes;
    uint32_t sunPrimaryLightIndex;
};

struct VitaPrimaryLightRecord {
    uint8_t type;
    uint8_t canUseShadowMap;
    uint8_t exponent;
    uint8_t unused;
    float color[3];
    float dir[3];
    float origin[3];
    float radius;
    float cosHalfFovOuter;
    float cosHalfFovInner;
    float cosHalfFovExpanded;
    float rotationLimit;
    float translationLimit;
};
#pragma pack(pop)

static_assert(sizeof(VitaPrimaryLightHeader) == 20,
              "VITP header ABI changed");
static_assert(sizeof(VitaPrimaryLightRecord) == VITA_PRIMARY_LIGHT_RECORD_BYTES,
              "VITP record ABI changed");

} // namespace kisak::vita
