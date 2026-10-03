#pragma once

#include <cstddef>
#include <cstdint>

#include <vita/kvap_registry.h>

namespace kisak::vita {

// The package backend exposes only these fixed-width semantic kinds.  A Vita
// caller never receives an x86 XAsset pointer or a raw fastfile address.
enum class VitaAssetType : uint32_t {
    Manifest = static_cast<uint32_t>(KvapChunkType::Manifest),
    Strings = static_cast<uint32_t>(KvapChunkType::Strings),
    Collision = static_cast<uint32_t>(KvapChunkType::Collision),
    World = static_cast<uint32_t>(KvapChunkType::World),
    Material = static_cast<uint32_t>(KvapChunkType::Material),
    Image = static_cast<uint32_t>(KvapChunkType::Image),
    Model = static_cast<uint32_t>(KvapChunkType::Model),
    Animation = static_cast<uint32_t>(KvapChunkType::Animation),
    Weapon = static_cast<uint32_t>(KvapChunkType::Weapon),
    Script = static_cast<uint32_t>(KvapChunkType::Script),
    Menu = static_cast<uint32_t>(KvapChunkType::Menu),
    Font = static_cast<uint32_t>(KvapChunkType::Font),
    Sound = static_cast<uint32_t>(KvapChunkType::Sound),
    Hud = static_cast<uint32_t>(KvapChunkType::Hud),
    Thumbnail = static_cast<uint32_t>(KvapChunkType::Thumbnail),
    Environment = static_cast<uint32_t>(KvapChunkType::Environment),
    PrimaryLights = static_cast<uint32_t>(KvapChunkType::PrimaryLights),
    Effects = static_cast<uint32_t>(KvapChunkType::Effects),
};

class VitaAssetDatabase {
public:
    KvapError Mount(KvapReadFn read, void* context, uint64_t fileSize);
    // Mount a read-only dependency behind the primary package.  Map packs use
    // this for common.kvap: map-local assets win, while fonts, menus, weapons,
    // scripts and sounds resolve from the dependency without copying or
    // reinterpreting package pointers.
    KvapError MountOverlay(KvapReadFn read, void* context, uint64_t fileSize);
    void Unmount();

    bool Mounted() const { return registry_.Mounted(); }
    const KvapHeader* Header() const { return registry_.Header(); }
    const KvapHeader* HeaderFor(const KvapChunk* chunk) const;
    uint32_t ChunkCount() const;
    const KvapChunk* Chunk(uint32_t index) const;

    const KvapChunk* Find(VitaAssetType type, uint64_t assetId) const;
    uint32_t Count(VitaAssetType type) const;
    KvapError Read(const KvapChunk* chunk, void* destination, size_t destinationSize) const;
    KvapError ReadRange(const KvapChunk* chunk, uint64_t offset,
                        void* destination, size_t size) const;

    // Intermediate/reference packages (including the typed-geometry package)
    // are intentionally not playable. Keep this metadata gate explicit so a
    // bootstrap cannot mistake partial payload conversion for a full map.
    bool IsReferenceOnly() const;

    // Final launch gate for a typed runtime package.  A package is playable
    // only when its manifest opts in with runtime-ready-v1, valid VITW/VITC
    // geometry is present, and the minimum gameplay payload classes exist;
    // name-only reference records never pass.
    bool IsPlayableShipment() const;

private:
    static bool IsKnownType(uint32_t rawType);
    static KvapError ValidateRegistry(const KvapRegistry& registry);
    KvapRegistry registry_;
    KvapRegistry overlay_;
};

} // namespace kisak::vita
