#pragma once

#include <cstdint>

namespace kisak::vita {

constexpr uint8_t kKvapLittleEndian = 1;
constexpr uint16_t kKvapMajor = 1;
constexpr uint16_t kKvapMinor = 0;

enum class KvapChunkType : uint32_t {
    Manifest = 1,
    Strings = 2,
    Collision = 3,
    World = 4,
    Material = 5,
    Image = 6,
    Model = 7,
    Animation = 8,
    Weapon = 9,
    Script = 10,
    Menu = 11,
    Font = 12,
    Sound = 13,
    Hud = 14,
    Thumbnail = 15,
    // Per-map source environment: createart fog, vision/film settings, and
    // map FX/weather presence. The payload is pointer-free and versioned so
    // Vita never needs to interpret a desktop GfxWorld pointer.
    Environment = 16,
    // Pointer-free ComPrimaryLight records. The sidecar carries the
    // authored directional/local light parameters without defName pointers.
    PrimaryLights = 17,
    // Pointer-free loadfx/createfx definitions and authored effect/sound
    // placement anchors. Kept separate from VENV so its version can evolve
    // without shifting the compact environment wire format.
    Effects = 18,
};

enum class KvapCodec : uint32_t {
    None = 0,
    Zlib = 1,
};

// These records are deliberately independent of compiler ABI and native
// pointers. They mirror tools/vita_asset_prep/kvap.py exactly.
#pragma pack(push, 1)
struct KvapHeader {
    char magic[4];
    uint16_t major;
    uint16_t minor;
    uint8_t endian;
    uint8_t flags;
    uint16_t headerSize;
    uint64_t tocOffset;
    uint32_t tocCount;
    uint64_t packageId;
    uint8_t sourceHash[32];
};

struct KvapChunk {
    uint32_t type;
    uint32_t flags;
    uint64_t assetId;
    uint64_t offset;
    uint64_t storedSize;
    uint64_t rawSize;
    uint32_t alignment;
    uint32_t codec;
    uint32_t crc32;
    uint8_t reserved[12];
};
#pragma pack(pop)

static_assert(sizeof(KvapHeader) == 64, "KVAP header ABI changed");
static_assert(sizeof(KvapChunk) == 64, "KVAP chunk ABI changed");
static_assert(alignof(KvapHeader) == 1, "KVAP header must be packed");
static_assert(alignof(KvapChunk) == 1, "KVAP chunk must be packed");

} // namespace kisak::vita
