#include <vita/vita_asset_db.h>

#if defined(__vita__)
#include <psp2/kernel/clib.h>
#else
#include <cstdio>
#define sceClibPrintf std::printf
#endif

#include <cstring>

namespace kisak::vita {

bool VitaAssetDatabase::IsKnownType(uint32_t rawType) {
    return rawType >= static_cast<uint32_t>(KvapChunkType::Manifest)
        && rawType <= static_cast<uint32_t>(KvapChunkType::Effects);
}

KvapError VitaAssetDatabase::Mount(KvapReadFn read, void* context, uint64_t fileSize) {
    overlay_.Unmount();
    const KvapError error = registry_.Mount(read, context, fileSize);
    if (error != KvapError::Ok)
        return error;
    const KvapError validation = ValidateRegistry(registry_);
    if (validation != KvapError::Ok)
        registry_.Unmount();
    return validation;
}

KvapError VitaAssetDatabase::ValidateRegistry(const KvapRegistry& registry) {
    for (uint32_t i = 0; i < registry.ChunkCount(); ++i) {
        const KvapChunk* chunk = registry.Chunk(i);
        if (!chunk || !IsKnownType(chunk->type)) {
            sceClibPrintf("[kisak-vita] vita asset db unknown chunk index=%u type=%u\\n",
                          static_cast<unsigned>(i),
                          chunk ? static_cast<unsigned>(chunk->type) : 0u);
            return KvapError::BadChunk;
        }
    }
    return KvapError::Ok;
}

KvapError VitaAssetDatabase::MountOverlay(KvapReadFn read, void* context,
                                          uint64_t fileSize) {
    if (!registry_.Mounted())
        return KvapError::InvalidArgument;
    const KvapError error = overlay_.Mount(read, context, fileSize);
    if (error != KvapError::Ok)
        return error;
    const KvapError validation = ValidateRegistry(overlay_);
    if (validation != KvapError::Ok)
        overlay_.Unmount();
    return validation;
}

void VitaAssetDatabase::Unmount() {
    overlay_.Unmount();
    registry_.Unmount();
}

uint32_t VitaAssetDatabase::ChunkCount() const {
    return registry_.ChunkCount() + overlay_.ChunkCount();
}

const KvapChunk* VitaAssetDatabase::Chunk(uint32_t index) const {
    const uint32_t primaryCount = registry_.ChunkCount();
    return index < primaryCount ? registry_.Chunk(index)
        : overlay_.Chunk(index - primaryCount);
}

const KvapHeader* VitaAssetDatabase::HeaderFor(const KvapChunk* chunk) const {
    if (!chunk)
        return nullptr;
    for (uint32_t index = 0; index < registry_.ChunkCount(); ++index) {
        if (registry_.Chunk(index) == chunk)
            return registry_.Header();
    }
    for (uint32_t index = 0; index < overlay_.ChunkCount(); ++index) {
        if (overlay_.Chunk(index) == chunk)
            return overlay_.Header();
    }
    return nullptr;
}

const KvapChunk* VitaAssetDatabase::Find(VitaAssetType type, uint64_t assetId) const {
    const KvapChunkType chunkType = static_cast<KvapChunkType>(type);
    if (const KvapChunk* primary = registry_.Find(chunkType, assetId))
        return primary;
    return overlay_.Find(chunkType, assetId);
}

uint32_t VitaAssetDatabase::Count(VitaAssetType type) const {
    if (!Mounted())
        return 0;
    const uint32_t rawType = static_cast<uint32_t>(type);
    uint32_t count = 0;
    for (uint32_t i = 0; i < ChunkCount(); ++i) {
        const KvapChunk* chunk = Chunk(i);
        if (chunk && chunk->type == rawType)
            ++count;
    }
    return count;
}

KvapError VitaAssetDatabase::Read(const KvapChunk* chunk, void* destination,
                                  size_t destinationSize) const {
    const KvapError primary = registry_.Read(chunk, destination, destinationSize);
    return primary != KvapError::InvalidArgument
        ? primary : overlay_.Read(chunk, destination, destinationSize);
}

KvapError VitaAssetDatabase::ReadRange(const KvapChunk* chunk, uint64_t offset,
                                       void* destination, size_t size) const {
    const KvapError primary = registry_.ReadRange(chunk, offset, destination, size);
    return primary != KvapError::InvalidArgument
        ? primary : overlay_.ReadRange(chunk, offset, destination, size);
}

bool VitaAssetDatabase::IsReferenceOnly() const {
    if (!Mounted())
        return false;
    static constexpr char kMarker[] = "reference-only";
    uint8_t buffer[4096]{};
    for (uint32_t index = 0; index < ChunkCount(); ++index) {
        const KvapChunk* chunk = Chunk(index);
        if (!chunk || chunk->type != static_cast<uint32_t>(KvapChunkType::Manifest))
            continue;
        const size_t bytes = chunk->rawSize < sizeof(buffer)
            ? static_cast<size_t>(chunk->rawSize) : sizeof(buffer);
        if (ReadRange(chunk, 0, buffer, bytes) != KvapError::Ok)
            return false;
        for (size_t offset = 0; offset + sizeof(kMarker) - 1 <= bytes; ++offset) {
            if (std::memcmp(buffer + offset, kMarker, sizeof(kMarker) - 1) == 0)
                return true;
        }
    }
    return false;
}

bool VitaAssetDatabase::IsPlayableShipment() const {
    if (!Mounted() || IsReferenceOnly())
        return false;
    const KvapChunk* manifest = nullptr;
    for (uint32_t index = 0; index < ChunkCount(); ++index) {
        const auto* candidate = Chunk(index);
        if (candidate && candidate->type == static_cast<uint32_t>(VitaAssetType::Manifest)) {
            manifest = candidate;
            break;
        }
    }
    if (!manifest)
        return false;
    uint8_t buffer[4096]{};
    const size_t bytes = manifest->rawSize < sizeof(buffer)
        ? static_cast<size_t>(manifest->rawSize) : sizeof(buffer);
    if (ReadRange(manifest, 0, buffer, bytes) != KvapError::Ok)
        return false;
    static constexpr char kPlayable[] = "\"playable\": true";
    static constexpr char kRuntimeReady[] = "\"payloadPolicy\": \"runtime-ready-v1\"";
    bool manifestOptIn = false;
    bool runtimeReady = false;
    for (size_t offset = 0; offset + sizeof(kPlayable) - 1 <= bytes; ++offset) {
        if (std::memcmp(buffer + offset, kPlayable, sizeof(kPlayable) - 1) == 0) {
            manifestOptIn = true;
            break;
        }
    }
    for (size_t offset = 0; offset + sizeof(kRuntimeReady) - 1 <= bytes; ++offset) {
        if (std::memcmp(buffer + offset, kRuntimeReady, sizeof(kRuntimeReady) - 1) == 0) {
            runtimeReady = true;
            break;
        }
    }
    if (!manifestOptIn || !runtimeReady)
        return false;

    const auto hasBinaryHeader = [this](VitaAssetType type, uint32_t magic,
                                         uint32_t version) {
        uint8_t header[8]{};
        for (uint32_t index = 0; index < ChunkCount(); ++index) {
            const auto* chunk = Chunk(index);
            if (!chunk || chunk->type != static_cast<uint32_t>(type) || chunk->rawSize < sizeof(header))
                continue;
            if (ReadRange(chunk, 0, header, sizeof(header)) != KvapError::Ok)
                continue;
            uint32_t chunkMagic = 0;
            uint32_t chunkVersion = 0;
            std::memcpy(&chunkMagic, header, sizeof(chunkMagic));
            std::memcpy(&chunkVersion, header + sizeof(chunkMagic), sizeof(chunkVersion));
            if (chunkMagic == magic && chunkVersion == version)
                return true;
        }
        return false;
    };
    const auto hasDecodedSound = [this]() {
        uint8_t header[20]{};
        for (uint32_t index = 0; index < ChunkCount(); ++index) {
            const auto* chunk = Chunk(index);
            if (!chunk || chunk->type != static_cast<uint32_t>(VitaAssetType::Sound)
                || chunk->rawSize < sizeof(header))
                continue;
            if (ReadRange(chunk, 0, header, sizeof(header)) != KvapError::Ok)
                continue;
            uint32_t magic = 0;
            uint32_t version = 0;
            uint32_t records = 0;
            std::memcpy(&magic, header, sizeof(magic));
            std::memcpy(&version, header + 4, sizeof(version));
            std::memcpy(&records, header + 8, sizeof(records));
            if (magic == 0x53544956 && version == 1 && records > 0)
                return true;
        }
        return false;
    };
    const auto hasRecordsHeader = [this](VitaAssetType type, uint32_t magic,
                                         uint32_t version) {
        uint8_t header[12]{};
        for (uint32_t index = 0; index < ChunkCount(); ++index) {
            const auto* chunk = Chunk(index);
            if (!chunk || chunk->type != static_cast<uint32_t>(type)
                || chunk->rawSize < sizeof(header))
                continue;
            if (ReadRange(chunk, 0, header, sizeof(header)) != KvapError::Ok)
                continue;
            uint32_t chunkMagic = 0;
            uint32_t chunkVersion = 0;
            uint32_t records = 0;
            std::memcpy(&chunkMagic, header, sizeof(chunkMagic));
            std::memcpy(&chunkVersion, header + 4, sizeof(chunkVersion));
            std::memcpy(&records, header + 8, sizeof(records));
            if (chunkMagic == magic && chunkVersion == version && records > 0)
                return true;
        }
        return false;
    };

    // VITL v4 carries the source semantic texture slots/state, authored
    // lit-pass lightmap sampler bits, and the first source GfxStateBits pair.
    // Keep v3/v2/v1 compatibility so older development packages remain
    // mountable while a fresh dump is being qualified.
    const bool hasMaterialRecords =
        hasRecordsHeader(VitaAssetType::Material, 0x4C544956, 5)
        || hasRecordsHeader(VitaAssetType::Material, 0x4C544956, 4)
        || hasRecordsHeader(VitaAssetType::Material, 0x4C544956, 3)
        || hasRecordsHeader(VitaAssetType::Material, 0x4C544956, 2)
        || hasRecordsHeader(VitaAssetType::Material, 0x4C544956, 1);
    // VITW v3 is the current map bundle format: it carries the authored
    // reflection-probe index in each surface record. Keep v1/v2 readable for
    // older development packages while allowing the multi-map runtime gate
    // to promote the package that the Vita bootstrap actually loads.
    const bool hasWorldGeometry =
        hasBinaryHeader(VitaAssetType::World, 0x57544956, 3)
        || hasBinaryHeader(VitaAssetType::World, 0x57544956, 2)
        || hasBinaryHeader(VitaAssetType::World, 0x57544956, 1);
    return hasWorldGeometry
        && hasBinaryHeader(VitaAssetType::World, 0x45544956, 1)
        // VITC v3 adds exact source cmodel/world brush membership plus plane
        // type/sign bits and per-side surface flags.  It is the release
        // source-runtime format; v2 remains readable by the scalar oracle.
        && (hasBinaryHeader(VitaAssetType::Collision, 0x43544956, 3)
            || hasBinaryHeader(VitaAssetType::Collision, 0x43544956, 2))
        && Count(VitaAssetType::Collision) > 0
        // The local route still consumes the bounded rawfile, localization,
        // and menu records.  A geometry-only package must not be promoted to
        // a playable Shipment session if those route services are absent.
        && hasRecordsHeader(VitaAssetType::Script, 0x52544956, 1)
        && hasRecordsHeader(VitaAssetType::Strings, 0x5A544956, 1)
        // VITT contains the live StringTable cells consumed by the authored
        // create-a-class tablelookup actions.  A package without it cannot
        // faithfully edit or reset the five retail class slots.
        && hasRecordsHeader(VitaAssetType::Strings, 0x54544956, 1)
        && hasRecordsHeader(VitaAssetType::Menu, 0x4D544956, 1)
        // The world cannot be considered runtime-ready until its material
        // names resolve to uploadable image payloads.
        && hasMaterialRecords
        && hasRecordsHeader(VitaAssetType::Image, 0x49544956, 1)
        // VITX v5 adds the textual bone identities required to intern a
        // genuine source XModel::boneNames table. v4/v3 remain readable by
        // compatibility and scalar-oracle paths while fresh source-owned
        // packages use v5.
        && (hasRecordsHeader(VitaAssetType::Model, 0x58544956, 5)
            || hasRecordsHeader(VitaAssetType::Model, 0x58544956, 4)
            || hasRecordsHeader(VitaAssetType::Model, 0x58544956, 3))
        // VITQ v3 owns the compressed XAnimParts arrays, textual string-table
        // identities, notetracks and delta graph. v2 remains readable by the
        // sampled compatibility renderer but cannot satisfy source-session
        // animation construction.
        && (hasRecordsHeader(VitaAssetType::Animation, 0x51544956, 3)
            || hasRecordsHeader(VitaAssetType::Animation, 0x51544956, 2))
        // VFXA v3 adds the trail timing/distance fields and preserves both
        // decal material slots. Earlier graph records remain useful to the
        // scalar oracle but cannot reconstruct an exact source FxEffectDef.
        && hasBinaryHeader(VitaAssetType::Effects, 0x41584656, 3)
        // VITP v5 is the pointer-free, named-field source WeaponDef record.
        // VITP v2 adds the source-resolved WeaponDef::hideTags surface mask;
        // v3 additionally carries the exact szXAnims[33] table used by
        // CG_CreateWeaponViewModelXAnim; v4 adds the source freeze flag.
        // CG_CreateWeaponViewModelXAnim. Older versions remain accepted so
        // existing development packages can still boot through compatibility
        // paths while a fresh dump is being qualified.
        && (hasRecordsHeader(VitaAssetType::Weapon, 0x50544956, 5)
            || hasRecordsHeader(VitaAssetType::Weapon, 0x50544956, 4)
            || hasRecordsHeader(VitaAssetType::Weapon, 0x50544956, 3)
            || hasRecordsHeader(VitaAssetType::Weapon, 0x50544956, 2)
            || hasRecordsHeader(VitaAssetType::Weapon, 0x50544956, 1))
        && (hasRecordsHeader(VitaAssetType::Font, 0x46544956, 2)
            || hasRecordsHeader(VitaAssetType::Font, 0x46544956, 1))
        && hasRecordsHeader(VitaAssetType::Hud, 0x48544956, 1)
        // A typed name-only sound list is not enough to launch. Require the
        // bounded VITS decoded-PCM stream before the manifest can opt in.
        && hasDecodedSound()
        && Count(VitaAssetType::Hud) > 0;
}

} // namespace kisak::vita
