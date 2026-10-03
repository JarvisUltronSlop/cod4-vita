#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace kisak::vita {

// The base-game MP map order is shared by the authored changemap route, the
// package exporter, and the runtime.  Keeping this in one header prevents a
// map's world from being paired with another map's collision or image closure.
struct VitaMapDefinition {
    const char* name;             // fastfile/map basename without mp_
    const char* displayName;      // retail-facing map title
    const char* localizationKey; // VITZ key used by the changemap menu
};

inline constexpr VitaMapDefinition kVitaMpMaps[] = {
#define VITA_MAP(id, name, title, key, original) {name, title, key},
#include "vita_map_catalog.inc"
#undef VITA_MAP
};
inline constexpr size_t kVitaMpMapCount = sizeof(kVitaMpMaps) / sizeof(kVitaMpMaps[0]);
constexpr bool VitaMapIsEnabled(uint32_t mapIndex) {
    switch(mapIndex) {
#define VITA_MAP(id, name, title, key, original) case id: return original;
#include "vita_map_catalog.inc"
#undef VITA_MAP
    default: return false;
    }
}
inline constexpr uint32_t kVitaCertifiedMapMask = 0
#define VITA_MAP(id, name, title, key, original) | (original ? (1u << id) : 0u)
#include "vita_map_catalog.inc"
#undef VITA_MAP
;
// Shipment's measured small-map path keeps immutable textures resident.
// The disk-backed working set was introduced for expanded maps only.
constexpr bool VitaMapUsesTexturePaging(uint32_t index) {
    return index != 0 && VitaMapIsEnabled(index);
}
// Only known original MP names become file paths or source map commands.
inline bool VitaFindMapIndex(const char* name, uint32_t* index) {
    if(!name || !index)return false;
    if(std::strncmp(name,"mp_",3)==0)name+=3;
    for(uint32_t i=0;i<kVitaMpMapCount;++i)
        if(VitaMapIsEnabled(i) && std::strcmp(name,kVitaMpMaps[i].name)==0) {*index=i;return true;}
    return false;
}
inline const char* VitaMapPackagePath(uint32_t index) {
    switch(index) {
#define VITA_MAP(id, name, title, key, original) case id: return original ? "ux0:data/CODVIT001/paks/mp_" name ".kvap" : nullptr;
#include "vita_map_catalog.inc"
#undef VITA_MAP
    default:return nullptr;
    }
}

// Per-map chunk ids are deliberately numeric.  The high 16 bits identify the
// Vita map family and the low byte is the catalog index; this keeps the KVAP
// TOC compact and makes selecting a map independent of host-side SHA-256.
constexpr uint64_t VitaMapAssetId(uint64_t family, uint32_t mapIndex) {
    return family | static_cast<uint64_t>(mapIndex);
}

constexpr uint64_t VitaMapWorldAssetId(uint32_t mapIndex) {
    return VitaMapAssetId(0x4D50000000000000ull, mapIndex);
}
constexpr uint64_t VitaMapCollisionAssetId(uint32_t mapIndex) {
    return VitaMapAssetId(0x4D43000000000000ull, mapIndex);
}
constexpr uint64_t VitaMapLightGridAssetId(uint32_t mapIndex) {
    return VitaMapAssetId(0x4D47000000000000ull, mapIndex);
}
constexpr uint64_t VitaMapEntitiesAssetId(uint32_t mapIndex) {
    return VitaMapAssetId(0x4D45000000000000ull, mapIndex);
}
constexpr uint64_t VitaMapModelsAssetId(uint32_t mapIndex) {
    return VitaMapAssetId(0x4D58000000000000ull, mapIndex);
}
// VLOD is a render-only companion to the source-facing VITX model closure.
// Keeping a distinct family lets old VITX v5 packs remain readable while
// certified packs opt into authored static-model LOD geometry.
constexpr uint64_t VitaMapModelLodsAssetId(uint32_t mapIndex) {
    return VitaMapAssetId(0x4D58010000000000ull, mapIndex);
}
constexpr uint64_t VitaMapMaterialsAssetId(uint32_t mapIndex) {
    return VitaMapAssetId(0x4D4C000000000000ull, mapIndex);
}
constexpr uint64_t VitaMapImagesAssetId(uint32_t mapIndex) {
    return VitaMapAssetId(0x4D49000000000000ull, mapIndex);
}
constexpr uint64_t VitaMapReflectionImagesAssetId(uint32_t mapIndex) {
    return VitaMapAssetId(0x4D49010000000000ull, mapIndex);
}
// Source semantic image families use the byte above the map index as a
// segment selector. Segment zero is the base id; larger segments let a
// Crossfire-sized normal/specular closure stay below the VITI raw-chunk
// parser bound and be loaded one at a time after startup compaction.
constexpr uint64_t VitaMapNormalImagesAssetId(uint32_t mapIndex,
                                              uint32_t segment = 0) {
    return VitaMapAssetId(0x4D49020000000000ull
                              | (static_cast<uint64_t>(segment) << 8),
                          mapIndex);
}
constexpr uint64_t VitaMapSpecularImagesAssetId(uint32_t mapIndex,
                                                uint32_t segment = 0) {
    return VitaMapAssetId(0x4D49030000000000ull
                              | (static_cast<uint64_t>(segment) << 8),
                          mapIndex);
}
// Placed XModels and first-person viewmodels use a separate optional package
// because their complete semantic closure is much larger than the bounded
// startup/world envelope. Keep their family namespace distinct from the BSP
// normal/specular streams while retaining the same per-family segment byte.
constexpr uint64_t VitaMapModelNormalImagesAssetId(uint32_t mapIndex,
                                                   uint32_t segment = 0) {
    return VitaMapAssetId(0x4D49040000000000ull
                              | (static_cast<uint64_t>(segment) << 8),
                          mapIndex);
}
constexpr uint64_t VitaMapModelSpecularImagesAssetId(uint32_t mapIndex,
                                                     uint32_t segment = 0) {
    return VitaMapAssetId(0x4D49050000000000ull
                              | (static_cast<uint64_t>(segment) << 8),
                          mapIndex);
}
// Common authored first-person color maps are streamed separately from each
// map's bounded world closure.  The companion has no map index because the
// MP5/hands/optic records are shared by every multiplayer map.
constexpr uint64_t VitaMapModelColorImagesAssetId() {
    return 0x4D49060000000000ull;
}
// Frontend/HUD images are map-independent and survive map-arena resets. The
// certified common pack owns this small exact VITI so a map exporter cannot
// accidentally omit the normal font atlas or menu chrome and force the
// renderer into hundreds of per-glyph fallback primitives.
constexpr uint64_t VitaCommonPresentationImagesAssetId() {
    return 0x4D49090000000000ull;
}
constexpr uint64_t VitaMapHudAssetId(uint32_t mapIndex) {
    return VitaMapAssetId(0x4D48000000000000ull, mapIndex);
}
constexpr uint64_t VitaMapEnvironmentAssetId(uint32_t mapIndex) {
    return VitaMapAssetId(0x4D56000000000000ull, mapIndex);
}
constexpr uint64_t VitaMapPrimaryLightsAssetId(uint32_t mapIndex) {
    return VitaMapAssetId(0x4D4B000000000000ull, mapIndex);
}
constexpr uint64_t VitaMapEffectsAssetId(uint32_t mapIndex) {
    return VitaMapAssetId(0x4D46000000000000ull, mapIndex);
}
// Exact color images referenced by a selected map's matched VFXA graph. The
// companion is intentionally separate from the bounded world VITI so an
// effect closure cannot evict authored BSP/model surfaces.
constexpr uint64_t VitaMapEffectImagesAssetId(uint32_t mapIndex) {
    return VitaMapAssetId(0x4D49070000000000ull, mapIndex);
}
// Authored BSP/model color records that did not fit the bounded startup VITI
// use the next image family. The segment byte follows the same convention as
// the normal/specular companions, allowing a large map to load exact retail
// records after the startup payload has been compacted.
constexpr uint64_t VitaMapColorImagesAssetId(uint32_t mapIndex,
                                             uint32_t segment = 0) {
    return VitaMapAssetId(0x4D49080000000000ull
                              | (static_cast<uint64_t>(segment) << 8),
                          mapIndex);
}

} // namespace kisak::vita
