#include "vita_map_assets.h"
#if KISAK_VITA_CPU_RUNTIME
#include <vita/vita_terrain_partition.h>
#include <vita/vita_ordered_brush_groups.h>
#endif
#include <vita/vita_source_registry.h>
#include <vita/vita_model_indices.h>
#include <vita/vita_slice_capabilities.h>
#if KISAK_VITA_CPU_RUNTIME
#include <vita/vita_cpu_profile.h>
#endif

#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include <universal/q_shared.h>
#include <game/g_bsp.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/fxprimitives.h>
#include <gfx_d3d/r_material.h>
#include <qcommon/com_bsp.h>
#include <qcommon/qcommon.h>
#include <sound/snd_public.h>
#include <universal/com_sndalias.h>
#include <gfx_d3d/r_font.h>
#include <script/scr_stringlist.h>
#include <vita/vita_animation_bundle.h>
#include <vita/vita_animation_record.h>
#include <vita/vita_effect_bundle.h>
#include <vita/vita_impact_fx_bundle.h>
#include <vita/vita_font_bundle.h>
#include <vita/vita_image_bundle.h>
#include <vita/vita_material_bundle.h>
#include <vita/vita_model_bundle.h>
#include <vita/vita_shipment_collision.h>
#include <vita/vita_shipment_entities.h>
#include <vita/vita_shipment_weapons.h>
#include <vita/vita_stringtable_bundle.h>
#include <vita/vita_sound_bundle.h>
#include <xanim/xanim.h>
#include <xanim/xmodel.h>

namespace kisak::vita {
namespace {

constexpr uint32_t kAssetAnimation = 0x02;
constexpr uint32_t kAssetModel = 0x03;
constexpr uint32_t kAssetMaterial = 0x04;
constexpr uint32_t kAssetImage = 0x06;
constexpr uint32_t kAssetSound = 0x07;
constexpr uint32_t kAssetSoundCurve = 0x08;
constexpr uint32_t kAssetLoadedSound = 0x09;
constexpr uint32_t kAssetClipMapPvs = 0x0B;
constexpr uint32_t kAssetComWorld = 0x0C;
constexpr uint32_t kAssetGameWorldMp = 0x0E;
constexpr uint32_t kAssetMapEnts = 0x0F;
constexpr uint32_t kAssetGfxWorld = 0x10;
constexpr uint32_t kAssetFont = 0x13;
constexpr uint32_t kAssetMenuList = 0x14;
constexpr uint32_t kAssetMenu = 0x15;
constexpr uint32_t kAssetWeapon = 0x17;
constexpr uint32_t kAssetEffect = 0x19;
constexpr uint32_t kAssetImpactFx = 0x1A;
constexpr uint32_t kAssetRawFile = 0x1F;
constexpr uint32_t kAssetStringTable = 0x20;
// Private constructor identity, outside the source XAssetType range. The
// source alias loader shares SoundFile objects for aliases of the same file;
// shellshock blending and looping playback compare that pointer identity.
constexpr uint32_t kAssetSoundFile = 0x100;

char NormalizedChar(char value) {
    if (value == '\\')
        return '/';
    if (value >= 'A' && value <= 'Z')
        return static_cast<char>(value + ('a' - 'A'));
    return value;
}

size_t AlignUp(size_t value, size_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

uint32_t MaterialNameHash(const char* string) {
    uint32_t hash = 0;
    if (!string)
        return hash;
    while (*string)
        hash = (static_cast<uint8_t>(*string++) | 0x20u) ^ (33u * hash);
    return hash;
}

uint16_t PackTexCoord(float coord) {
    uint32_t bits = 0;
    std::memcpy(&bits, &coord, sizeof(bits));
    int low14 = static_cast<int32_t>((2u * bits) ^ 0x80000000u) >> 14;
    low14 = std::max(-16384, std::min(0x3FFF, low14));
    return static_cast<uint16_t>((low14 & 0x3FFF)
        | ((bits >> 16) & 0xC000u));
}

PackedTexCoords PackTexCoords(const float value[2]) {
    const uint32_t u = PackTexCoord(value[0]);
    const uint32_t v = PackTexCoord(value[1]);
    return PackedTexCoords(v | (u << 16));
}

void AddMissingSourceAsset(VitaSourceAssetMountReport* report,
                           uint32_t type, const char* name) {
    if (!report || !name || !name[0])
        return;
    const uint32_t index = report->missingCount++;
    if (index >= VitaSourceAssetMountReport::kMaxMissing)
        return;
    report->missing[index].assetType = type;
    std::strncpy(report->missing[index].name, name,
                 sizeof(report->missing[index].name) - 1u);
}

} // namespace

VitaAssetRegistry::~VitaAssetRegistry() { Destroy(); }

uint32_t VitaAssetRegistry::InternScriptString(const char* text, uint32_t bytes, int type) {
    // Allocate bookkeeping first: a failed arena allocation must not acquire
    // a reference which no owner can subsequently release. Nodes are charged
    // to the same bounded asset arena and participate in constructor rollback.
    auto* reference = static_cast<ScriptStringRef*>(Allocate(sizeof(ScriptStringRef), alignof(ScriptStringRef)));
    if (!reference) return 0;
    const uint32_t value = SL_GetStringOfSize(text, 0, bytes, static_cast<mtType_t>(type));
    if (!value || value > UINT16_MAX) {
        if (value) SL_RemoveRefToString(value);
        return 0;
    }
    reference->value = static_cast<uint16_t>(value);
    reference->previous = scriptStringRefs_;
    scriptStringRefs_ = reference;
    return value;
}

void VitaAssetRegistry::ReleaseScriptStringsTo(ScriptStringRef* checkpoint) {
    while (scriptStringRefs_ != checkpoint) {
        const auto* reference = scriptStringRefs_;
        scriptStringRefs_ = reference->previous;
        SL_RemoveRefToString(reference->value);
    }
}

void VitaAssetRegistry::ReleaseScriptStrings() { ReleaseScriptStringsTo(nullptr); }

bool VitaAssetRegistry::Initialize(size_t capacityBytes) {
    Destroy();
    if (!capacityBytes || capacityBytes > kMaximumArenaBytes)
        return false;
#if !KISAK_VITA_CPU_RUNTIME
    arena_ = static_cast<uint8_t*>(std::calloc(1, capacityBytes));
    if (!arena_)
        return false;
#endif
    capacity_ = capacityBytes;
    entries_ = static_cast<Entry*>(Allocate(kMaxAssets * sizeof(Entry), alignof(Entry)));
    if (!entries_) {
        Destroy();
        return false;
    }
    return true;
}

void VitaAssetRegistry::Destroy() {
    ReleaseScriptStrings();
    if (mapRootsMounted_) {
#if KISAK_VITA_CPU_RUNTIME
        worldBrushGroups.Reset();
#endif
        std::memset(&cm, 0, sizeof(cm));
        std::memset(&comWorld, 0, sizeof(comWorld));
    }
    std::free(arena_);
    std::free(mapRootArena_);
#if KISAK_VITA_CPU_RUNTIME
    storage_.Clear();mapRootStorage_.Clear();
#endif
    arena_ = nullptr;
    entries_ = nullptr;
    mapRootArena_ = nullptr;
    capacity_ = 0;
    cursor_ = 0;
    highWater_ = 0;
    mapRootCapacity_ = 0;
    mapRootCursor_ = 0;
    mapRootHighWater_ = 0;
    allocatingMapRoots_ = false;
    entryCount_ = 0;
    mapRootsMounted_ = false;
    services_ = {};
    soundCatalog_ = nullptr;
}

void VitaAssetRegistry::Reset(VitaAssetLifetime lifetime) {
    ReleaseScriptStrings();
    // Map construction uses a dedicated registry instance today, so a map or
    // transient reset can rewind the entire arena. A frontend reset
    // is equivalent to full destruction of the current generation.
    (void)lifetime;
    if (mapRootsMounted_) {
#if KISAK_VITA_CPU_RUNTIME
        worldBrushGroups.Reset();
#endif
        std::memset(&cm, 0, sizeof(cm));
        std::memset(&comWorld, 0, sizeof(comWorld));
        mapRootsMounted_ = false;
    }
    std::free(mapRootArena_);
#if KISAK_VITA_CPU_RUNTIME
    mapRootStorage_.Clear();
#endif
    mapRootArena_ = nullptr;
    mapRootCapacity_ = 0;
    mapRootCursor_ = 0;
    mapRootHighWater_ = 0;
    allocatingMapRoots_ = false;
    cursor_ = entries_ ? kMaxAssets * sizeof(Entry) : 0;
#if KISAK_VITA_CPU_RUNTIME
    storage_.Rewind(cursor_);
#endif
    entryCount_ = 0;
    services_ = {};
    soundCatalog_ = nullptr;
}

void* VitaAssetRegistry::Allocate(size_t bytes, size_t alignment) {
    if (!capacity_ || !bytes || !alignment || (alignment & (alignment - 1u)))
        return nullptr;
#if KISAK_VITA_CPU_RUNTIME
    auto& storage=allocatingMapRoots_?mapRootStorage_:storage_;
    auto& cursor=allocatingMapRoots_?mapRootCursor_:cursor_;
    auto& highWater=allocatingMapRoots_?mapRootHighWater_:highWater_;
    const size_t capacity=allocatingMapRoots_?mapRootCapacity_:capacity_;
    void* result=storage.Allocate(bytes,alignment,capacity,cursor);
    if (!result) {
        VitaCpuProfile_Counter("registry-allocation-failed",bytes,static_cast<unsigned>(storage.LastFailure()));
        VitaCpuProfile_Counter("registry-allocation-limit",cursor,capacity);
    }
    highWater=std::max(highWater,cursor);
    return result;
#else
    uint8_t* targetArena = allocatingMapRoots_ ? mapRootArena_ : arena_;
    size_t* targetCursor = allocatingMapRoots_ ? &mapRootCursor_ : &cursor_;
    size_t* targetHighWater = allocatingMapRoots_
        ? &mapRootHighWater_ : &highWater_;
    const size_t targetCapacity = allocatingMapRoots_
        ? mapRootCapacity_ : capacity_;
    if (!targetArena)
        return nullptr;
    const size_t aligned = AlignUp(*targetCursor, alignment);
    if (aligned > targetCapacity || bytes > targetCapacity - aligned)
        return nullptr;
    void* result = targetArena + aligned;
    std::memset(result, 0, bytes);
    *targetCursor = aligned + bytes;
    if (*targetCursor > *targetHighWater)
        *targetHighWater = *targetCursor;
    return result;
#endif
}

char* VitaAssetRegistry::CopyString(const char* text) {
    if (!text)
        return nullptr;
    const size_t length = std::strlen(text);
    char* copy = static_cast<char*>(Allocate(length + 1u, alignof(char)));
    if (!copy)
        return nullptr;
    std::memcpy(copy, text, length + 1u);
    return copy;
}

uint32_t VitaAssetRegistry::HashName(const char* name) {
    uint32_t hash = 2166136261u;
    if (!name)
        return hash;
    while (*name) {
        hash ^= static_cast<uint8_t>(NormalizedChar(*name++));
        hash *= 16777619u;
    }
    return hash;
}

bool VitaAssetRegistry::NamesEqual(const char* left, const char* right) {
    if (!left || !right)
        return left == right;
    while (*left || *right) {
        if (NormalizedChar(*left++) != NormalizedChar(*right++))
            return false;
    }
    return true;
}

bool VitaAssetRegistry::Register(uint32_t type, const char* name,
                                 void* object,
                                 VitaAssetLifetime lifetime,
                                 bool allowEmptyName) {
    if (!capacity_ || !name || (!allowEmptyName && !name[0]) || !object)
        return false;
    const uint32_t hash = HashName(name);
    for (uint32_t index = 0; index < entryCount_; ++index) {
        Entry& entry = entries_[index];
        if (entry.type == type && entry.nameHash == hash
            && NamesEqual(entry.name, name)) {
            entry.object = object;
            entry.lifetime = lifetime;
            return true;
        }
    }
    if (entryCount_ >= kMaxAssets)
        return false;
    char* ownedName = CopyString(name);
    if (!ownedName)
        return false;
    Entry& entry = entries_[entryCount_++];
    entry.type = type;
    entry.nameHash = hash;
    entry.name = ownedName;
    entry.object = object;
    entry.lifetime = lifetime;
    return true;
}

#define VITA_REGISTER_TYPED(Method, TypeId, CppType)                         \
    bool VitaAssetRegistry::Method(const char* name, CppType* asset,         \
                                   VitaAssetLifetime lifetime) {             \
        return Register(TypeId, name, const_cast<void*>(                    \
            static_cast<const void*>(asset)), lifetime);                     \
    }

VITA_REGISTER_TYPED(RegisterAnimation, kAssetAnimation, XAnimParts)
VITA_REGISTER_TYPED(RegisterModel, kAssetModel, XModel)
VITA_REGISTER_TYPED(RegisterMaterial, kAssetMaterial, Material)
VITA_REGISTER_TYPED(RegisterImage, kAssetImage, GfxImage)
VITA_REGISTER_TYPED(RegisterSound, kAssetSound, snd_alias_list_t)
VITA_REGISTER_TYPED(RegisterClipMap, kAssetClipMapPvs, clipMap_t)
VITA_REGISTER_TYPED(RegisterComWorld, kAssetComWorld, ComWorld)
VITA_REGISTER_TYPED(RegisterGameWorld, kAssetGameWorldMp, GameWorldMp)
VITA_REGISTER_TYPED(RegisterMapEnts, kAssetMapEnts, MapEnts)
VITA_REGISTER_TYPED(RegisterGfxWorld, kAssetGfxWorld, GfxWorld)
VITA_REGISTER_TYPED(RegisterFont, kAssetFont, Font_s)
VITA_REGISTER_TYPED(RegisterMenuList, kAssetMenuList, MenuList)
VITA_REGISTER_TYPED(RegisterMenu, kAssetMenu, menuDef_t)
VITA_REGISTER_TYPED(RegisterWeapon, kAssetWeapon, WeaponDef)
VITA_REGISTER_TYPED(RegisterRawFile, kAssetRawFile, RawFile)
VITA_REGISTER_TYPED(RegisterStringTable, kAssetStringTable, StringTable)

#undef VITA_REGISTER_TYPED

bool VitaAssetRegistry::RegisterEffect(const char* name,
                                       const FxEffectDef* asset,
                                       VitaAssetLifetime lifetime) {
    return Register(kAssetEffect, name, const_cast<FxEffectDef*>(asset),
                    lifetime);
}

bool VitaAssetRegistry::RegisterImpactEffects(
    const char* name, FxImpactTable* asset, VitaAssetLifetime lifetime) {
    return Register(kAssetImpactFx, name, asset, lifetime, true);
}

snd_alias_list_t* VitaAssetRegistry::RehydrateSoundAlias(
    const char* aliasName, int sequence, VitaAssetLifetime lifetime) {
    (void)sequence; // The exported source records own initial sequence state.
    if (!aliasName || !aliasName[0] || !soundCatalog_) return nullptr;
    if (auto* existing = Find(kAssetSound, aliasName))
        return static_cast<snd_alias_list_t*>(existing);
    std::vector<VitaSoundAliasRecord> records;
    if (!soundCatalog_->ReadAlias(aliasName, &records) || records.empty()) return nullptr;
    const size_t savedCursor = cursor_;
    const uint32_t savedCount = entryCount_;
    const auto fail = [&]() -> snd_alias_list_t* {
        cursor_ = savedCursor;
        entryCount_ = savedCount;
        return nullptr;
    };
    auto* list = static_cast<snd_alias_list_t*>(
        Allocate(sizeof(snd_alias_list_t), alignof(snd_alias_list_t)));
    auto* aliases = static_cast<snd_alias_t*>(
        Allocate(records.size() * sizeof(snd_alias_t), alignof(snd_alias_t)));
    char* name = CopyString(aliasName);
    if (!list || !aliases || !name) return fail();
    for (size_t index = 0; index < records.size(); ++index) {
        const auto& input = records[index];
        auto& alias = aliases[index];
        alias.aliasName = name;
        if (input.subtitle[0] && !(alias.subtitle = CopyString(input.subtitle))) return fail();
        if (input.secondaryAliasName[0] && !(alias.secondaryAliasName = CopyString(input.secondaryAliasName))) return fail();
        if (input.chainAliasName[0] && !(alias.chainAliasName = CopyString(input.chainAliasName))) return fail();
        alias.sequence = input.sequence;
        alias.volMin = input.volMin; alias.volMax = input.volMax;
        alias.pitchMin = input.pitchMin; alias.pitchMax = input.pitchMax;
        alias.distMin = input.distMin; alias.distMax = input.distMax;
        alias.flags = input.flags; alias.startDelay = input.startDelay;
        alias.slavePercentage = input.slavePercentage; alias.probability = input.probability;
        alias.lfePercentage = input.lfePercentage; alias.centerPercentage = input.centerPercentage;
        alias.envelopMin = input.envelopMin; alias.envelopMax = input.envelopMax;
        alias.envelopPercentage = input.envelopPercentage;
        // The original default falloff curve has an empty filename but
        // retains its two authored knots. Empty identity is not a null curve.
        if (input.curveName[0] || input.curveKnotCount) {
            auto* curve = static_cast<SndCurve*>(Find(kAssetSoundCurve, input.curveName));
            if (!curve) {
                curve = static_cast<SndCurve*>(Allocate(sizeof(SndCurve), alignof(SndCurve)));
                if (!curve || !(curve->filename = CopyString(input.curveName))) return fail();
                curve->knotCount = input.curveKnotCount;
                std::memcpy(curve->knots, input.curveKnots, sizeof(curve->knots));
                if (!Register(kAssetSoundCurve, input.curveName, curve, lifetime, true)) return fail();
            } else if (curve->knotCount != static_cast<int>(input.curveKnotCount)
                       || std::memcmp(curve->knots, input.curveKnots, sizeof(curve->knots))) return fail();
            alias.volumeFalloffCurve = curve;
        }
        if (input.speakerMapName[0]) {
            auto* speaker = static_cast<SpeakerMap*>(Allocate(sizeof(SpeakerMap), alignof(SpeakerMap)));
            if (!speaker || !(speaker->name = CopyString(input.speakerMapName))) return fail();
            speaker->isDefault = input.speakerMapDefault != 0;
            static_assert(sizeof(speaker->channelMaps) == sizeof(input.speakerMaps));
            std::memcpy(speaker->channelMaps, input.speakerMaps, sizeof(speaker->channelMaps));
            alias.speakerMap = speaker;
        }
        char fileKey[144]{};
        std::snprintf(fileKey, sizeof(fileKey), "%u:%s", input.soundType, input.soundFile);
        if (auto* shared = static_cast<SoundFile*>(Find(kAssetSoundFile, fileKey))) {
            if (shared->type != input.soundType
                || shared->exists != (input.sourceFileExists != 0)) return fail();
            alias.soundFile = shared;
            continue;
        }
        auto* file = static_cast<SoundFile*>(Allocate(sizeof(SoundFile), alignof(SoundFile)));
        if (!file) return fail();
        alias.soundFile = file;
        file->type = static_cast<uint8_t>(input.soundType);
        file->exists = static_cast<uint8_t>(input.sourceFileExists);
        if (input.soundType == SAT_LOADED && input.sourceFileExists) {
            auto* loaded = static_cast<LoadedSound*>(Find(kAssetLoadedSound, input.soundFile));
            if (!loaded) {
                const auto* pcm = soundCatalog_->PreparePcm(input.soundFile);
                if (!pcm || pcm->bytes.empty()) return fail();
                loaded = static_cast<LoadedSound*>(Allocate(sizeof(LoadedSound), alignof(LoadedSound)));
                if (!loaded || !(loaded->name = CopyString(input.soundFile))) return fail();
                auto& info = loaded->sound.info;
                info.format = pcm->format; info.rate = pcm->rate;
                info.bits = pcm->bits; info.channels = pcm->channels;
                info.samples = pcm->samples; info.data_len = pcm->bytes.size();
                info.data_ptr = pcm->bytes.data();
                loaded->sound.data = const_cast<uint8_t*>(pcm->bytes.data());
                if (!Register(kAssetLoadedSound, input.soundFile, loaded, lifetime)) return fail();
            }
            file->u.loadSnd = loaded;
        } else if (input.soundType == SAT_STREAMED) {
            char* path = CopyString(input.soundFile);
            if (!path) return fail();
            char* separator = nullptr;
            for (char* at = path; *at; ++at) if (*at == '/' || *at == '\\') separator = at;
            if (separator) {
                *separator = 0;
                file->u.streamSnd.filename.info.raw.dir = path;
                file->u.streamSnd.filename.info.raw.name = separator + 1;
            } else {
                file->u.streamSnd.filename.info.raw.dir = "";
                file->u.streamSnd.filename.info.raw.name = path;
            }
        }
        if (!Register(kAssetSoundFile, fileKey, file, lifetime)) return fail();
    }
    list->aliasName = name;
    list->head = aliases;
    list->count = static_cast<int>(records.size());
    return RegisterSound(name, list, lifetime) ? list : fail();
}

VitaSourceAssetMountReport VitaAssetRegistry::RehydrateSourceSounds(const VitaSoundBundle& source) {
    VitaSourceAssetMountReport report{};
    report.assetType = kAssetSound;
    report.sourceCount = source.AliasCount();
    if (!source.Loaded() || !source.HasAliasCatalog()) return report;
    const size_t savedCursor = cursor_;
    const uint32_t savedCount = entryCount_;
    soundCatalog_ = &source;
    for (uint32_t index = 0; index < source.AliasCount(); ++index) {
        const char* name = source.AliasName(index);
        if (!RehydrateSoundAlias(name, 0, VitaAssetLifetime::Map)) {
            std::snprintf(report.failedAsset, sizeof(report.failedAsset), "%s", name);
            AddMissingSourceAsset(&report, kAssetSound, name);
            cursor_ = savedCursor;
            entryCount_ = savedCount;
            soundCatalog_ = nullptr;
            return report;
        }
        ++report.mountedCount;
    }
    report.ready = true;
    return report;
}

Font_s* VitaAssetRegistry::RehydrateFont(const VitaFontRecord& source,
                                         Material* material,
                                         Material* glowMaterial,
                                         VitaAssetLifetime lifetime) {
    if (source.name.empty() || source.glyphs.empty() || !material)
        return nullptr;
    auto* font = static_cast<Font_s*>(
        Allocate(sizeof(Font_s), alignof(Font_s)));
    auto* glyphs = static_cast<Glyph*>(Allocate(
        source.glyphs.size() * sizeof(Glyph), alignof(Glyph)));
    char* name = CopyString(source.name.c_str());
    if (!font || !glyphs || !name)
        return nullptr;
    for (size_t index = 0; index < source.glyphs.size(); ++index) {
        const VitaFontGlyph& input = source.glyphs[index];
        Glyph& output = glyphs[index];
        output.letter = input.letter;
        output.x0 = input.x0;
        output.y0 = input.y0;
        output.dx = input.dx;
        output.pixelWidth = input.pixelWidth;
        output.pixelHeight = input.pixelHeight;
        output.s0 = input.s0;
        output.t0 = input.t0;
        output.s1 = input.s1;
        output.t1 = input.t1;
    }
    font->fontName = name;
    font->pixelHeight = source.pixelHeight;
    font->glyphCount = static_cast<int>(source.glyphs.size());
    font->material = material;
    font->glowMaterial = glowMaterial;
    font->glyphs = glyphs;
    return RegisterFont(name, font, lifetime) ? font : nullptr;
}

VitaSourceAssetMountReport VitaAssetRegistry::RehydrateSourceFonts(
    const VitaFontBundle& source, VitaAssetLifetime lifetime) {
    VitaSourceAssetMountReport report{};
    report.assetType = kAssetFont;
    report.sourceCount = source.Count();
    if (!source.Loaded() || source.Version() != 2 || !source.Count()) {
        std::strncpy(report.failedAsset, "VITF v2 source material identities",
                     sizeof(report.failedAsset) - 1u);
        return report;
    }
    const size_t arenaCheckpoint = cursor_;
    auto* const stringCheckpoint = scriptStringRefs_;
    const uint32_t entryCheckpoint = entryCount_;
    for (uint32_t index = 0; index < source.Count(); ++index) {
        const VitaFontRecord& input = *source.Record(index);
        auto* material = static_cast<Material*>(Find(kAssetMaterial, input.materialName.c_str()));
        auto* glow = input.glowMaterialName.empty() ? nullptr
            : static_cast<Material*>(Find(kAssetMaterial, input.glowMaterialName.c_str()));
        if (!material) AddMissingSourceAsset(&report, kAssetMaterial, input.materialName.c_str());
        if (!input.glowMaterialName.empty() && !glow)
            AddMissingSourceAsset(&report, kAssetMaterial, input.glowMaterialName.c_str());
        if (report.missingCount || !RehydrateFont(input, material, glow, lifetime)) {
            std::strncpy(report.failedAsset, input.name.c_str(), sizeof(report.failedAsset) - 1u);
            ReleaseScriptStringsTo(stringCheckpoint);
            cursor_ = arenaCheckpoint;
            entryCount_ = entryCheckpoint;
            report.mountedCount = 0;
            return report;
        }
        ++report.mountedCount;
    }
    report.ready = true;
    return report;
}

StringTable* VitaAssetRegistry::RehydrateStringTable(
    const VitaStringTableRecord& source, VitaAssetLifetime lifetime) {
    if (source.name.empty()
        || ((source.rowCount == 0) != (source.columnCount == 0))
        || source.rowCount > static_cast<uint32_t>(
            std::numeric_limits<int>::max())
        || source.columnCount > static_cast<uint32_t>(
            std::numeric_limits<int>::max()))
        return nullptr;
    const uint64_t cellCount64 = static_cast<uint64_t>(source.rowCount)
        * source.columnCount;
    if (cellCount64 != source.values.size()
        || cellCount64 > std::numeric_limits<size_t>::max()
            / sizeof(const char*)
        || Find(kAssetStringTable, source.name.c_str()))
        return nullptr;

    const size_t arenaCheckpoint = cursor_;
    auto* const stringCheckpoint = scriptStringRefs_;
    const uint32_t entryCheckpoint = entryCount_;
    const auto rollback = [this, arenaCheckpoint, entryCheckpoint, stringCheckpoint] {
        ReleaseScriptStringsTo(stringCheckpoint);
        cursor_ = arenaCheckpoint;
        entryCount_ = entryCheckpoint;
    };
    auto* table = static_cast<StringTable*>(
        Allocate(sizeof(StringTable), alignof(StringTable)));
    auto** values = cellCount64 == 0 ? nullptr
        : static_cast<const char**>(Allocate(
            static_cast<size_t>(cellCount64) * sizeof(const char*),
            alignof(const char*)));
    char* name = CopyString(source.name.c_str());
    if (!table || (cellCount64 != 0 && !values) || !name) {
        rollback();
        return nullptr;
    }
    for (size_t index = 0; index < source.values.size(); ++index) {
        values[index] = CopyString(source.values[index].c_str());
        if (!values[index]) {
            rollback();
            return nullptr;
        }
    }
    table->name = name;
    table->columnCount = static_cast<int>(source.columnCount);
    table->rowCount = static_cast<int>(source.rowCount);
    table->values = values;
    if (!RegisterStringTable(name, table, lifetime)) {
        rollback();
        return nullptr;
    }
    return table;
}

VitaSourceAssetMountReport VitaAssetRegistry::RehydrateSourceStringTables(
    const VitaStringTableBundle& source, VitaAssetLifetime lifetime) {
    VitaSourceAssetMountReport report{};
    report.assetType = kAssetStringTable;
    report.sourceCount = source.Count();
    if (!source.Loaded() || source.Count() == 0)
        return report;

    const size_t arenaCheckpoint = cursor_;
    auto* const stringCheckpoint = scriptStringRefs_;
    const uint32_t entryCheckpoint = entryCount_;
    for (uint32_t index = 0; index < source.Count(); ++index) {
        const VitaStringTableRecord* record = source.Record(index);
        if (!record || !RehydrateStringTable(*record, lifetime)) {
            std::strncpy(report.failedAsset,
                         record ? record->name.c_str() : "<missing-record>",
                         sizeof(report.failedAsset) - 1u);
            AddMissingSourceAsset(&report, kAssetStringTable,
                                  record ? record->name.c_str()
                                         : "<missing-record>");
            ReleaseScriptStringsTo(stringCheckpoint);
            cursor_ = arenaCheckpoint;
            entryCount_ = entryCheckpoint;
            return report;
        }
        ++report.mountedCount;
    }
    report.ready = report.mountedCount == report.sourceCount;
    return report;
}

GfxImage* VitaAssetRegistry::RehydrateImage(
    const VitaImageRecord& source, VitaAssetLifetime lifetime,
    bool retainPixelPayload) {
    if (source.name.empty() || source.name.size() > 4096u
        || source.width == 0 || source.width > 4096u
        || source.height == 0 || source.height > 4096u
        || source.depth != 1u || source.levelCount == 0
        || source.levelCount > 16u
        || (retainPixelPayload && source.bytes.empty())
        || source.bytes.size() > 8u * 1024u * 1024u
        || source.bytes.size() > static_cast<size_t>(
            std::numeric_limits<int>::max()))
        return nullptr;

    const size_t arenaCheckpoint = cursor_;
    auto* const stringCheckpoint = scriptStringRefs_;
    const uint32_t entryCheckpoint = entryCount_;
    const auto rollback = [this, arenaCheckpoint, entryCheckpoint, stringCheckpoint] {
        ReleaseScriptStringsTo(stringCheckpoint);
        cursor_ = arenaCheckpoint;
        entryCount_ = entryCheckpoint;
    };
    auto* image = static_cast<GfxImage*>(
        Allocate(sizeof(GfxImage), alignof(GfxImage)));
    GfxImageLoadDef* loadDef = nullptr;
    if (retainPixelPayload) {
        const size_t loadDefBytes = offsetof(GfxImageLoadDef, data)
            + source.bytes.size();
        loadDef = static_cast<GfxImageLoadDef*>(
            Allocate(loadDefBytes, alignof(GfxImageLoadDef)));
    }
    char* name = CopyString(source.name.c_str());
    if (!image || (retainPixelPayload && !loadDef) || !name) {
        rollback();
        return nullptr;
    }

    if (loadDef) {
        loadDef->levelCount = static_cast<uint8_t>(source.levelCount);
        loadDef->flags = static_cast<uint8_t>(source.flags & 0xFFu);
        loadDef->dimensions[0] = static_cast<int16_t>(source.width);
        loadDef->dimensions[1] = static_cast<int16_t>(source.height);
        loadDef->dimensions[2] = static_cast<int16_t>(source.depth);
        loadDef->format = source.format;
        loadDef->resourceSize = static_cast<int>(source.bytes.size());
        std::memcpy(loadDef->data, source.bytes.data(), source.bytes.size());
    }

    // Image load-def flag 0x4 is the source cubemap bit used by the desktop
    // loader. VITI otherwise contains 2D textures for this slice.
    image->mapType = (source.flags & 0x4u) ? MAPTYPE_CUBE : MAPTYPE_2D;
    // The native renderer has already uploaded the VITI bytes into its map
    // archive. Source gameplay and materials need stable image identity and
    // dimensions, not a second CPU copy of those pixels; a null loadDef is
    // therefore intentional in metadata-only sessions.
    image->texture.loadDef = loadDef;
    image->picmip.platform[0] = 0;
    image->picmip.platform[1] = 0;
    image->noPicmip = false;
    image->semantic = 0;
    image->track = 0;
    image->cardMemory.platform[0] = static_cast<int>(source.bytes.size());
    image->cardMemory.platform[1] = static_cast<int>(source.bytes.size());
    image->width = static_cast<uint16_t>(source.width);
    image->height = static_cast<uint16_t>(source.height);
    image->depth = static_cast<uint16_t>(source.depth);
    image->category = 0;
    image->delayLoadPixels = false;
    image->name = name;
    if (!RegisterImage(name, image, lifetime)) {
        rollback();
        return nullptr;
    }
    return image;
}

Material* VitaAssetRegistry::RehydrateMaterial(
    const VitaMaterialRecord& source, VitaAssetLifetime lifetime,
    VitaSourceAssetMountReport* report) {
    if (source.name.empty() || source.name.size() > 4096u)
        return nullptr;
    const size_t arenaCheckpoint = cursor_;
    auto* const stringCheckpoint = scriptStringRefs_;
    const uint32_t entryCheckpoint = entryCount_;
    const auto rollback = [this, arenaCheckpoint, entryCheckpoint, stringCheckpoint] {
        ReleaseScriptStringsTo(stringCheckpoint);
        cursor_ = arenaCheckpoint;
        entryCount_ = entryCheckpoint;
    };

    auto* material = static_cast<Material*>(
        Allocate(sizeof(Material), alignof(Material)));
    char* name = CopyString(source.name.c_str());
    if (!material || !name) {
        rollback();
        return nullptr;
    }
    material->info.name = name;
    material->info.gameFlags = source.gameFlags;
    material->info.sortKey = source.sortKey;
    material->info.surfaceTypeBits = source.surfaceTypeBits;
    material->info.drawSurf.fields.primarySortKey = source.sortKey & 0x3Fu;
    material->stateFlags = source.stateFlags;
    material->cameraRegion = source.cameraRegion;
    std::memset(material->stateBitsEntry, 0xFF,
                sizeof(material->stateBitsEntry));

    if (!source.techniqueSet.empty()) {
        material->techniqueSet = static_cast<MaterialTechniqueSet*>(Allocate(
            sizeof(MaterialTechniqueSet), alignof(MaterialTechniqueSet)));
        if (!material->techniqueSet) {
            rollback();
            return nullptr;
        }
        material->techniqueSet->remappedTechniqueSet = material->techniqueSet;
        material->techniqueSet->name = CopyString(source.techniqueSet.c_str());
        if (!material->techniqueSet->name) {
            rollback();
            return nullptr;
        }
    }
    if (source.litStateValid || source.unlitStateValid) {
        material->stateBitsTable = static_cast<GfxStateBits*>(Allocate(
            (source.litStateValid + source.unlitStateValid) * sizeof(GfxStateBits), alignof(GfxStateBits)));
        if (!material->stateBitsTable) {
            rollback();
            return nullptr;
        }
        material->stateBitsCount = source.litStateValid + source.unlitStateValid;
        // The pointer-free record stores the first source lit pass. Publish
        // it for every lit technique slot; absent technique objects remain
        // null and therefore cannot accidentally enter desktop shader code.
        if (source.litStateValid) {
            material->stateBitsTable[0].loadBits[0] = source.litStateBits0;
            material->stateBitsTable[0].loadBits[1] = source.litStateBits1;
            for (uint32_t index = 7; index < 21; ++index)
                material->stateBitsEntry[index] = 0;
        }
        if (source.unlitStateValid) {
            const uint8_t slot = source.litStateValid;
            material->stateBitsTable[slot].loadBits[0] = source.unlitStateBits0;
            material->stateBitsTable[slot].loadBits[1] = source.unlitStateBits1;
            material->stateBitsEntry[TECHNIQUE_UNLIT] = slot;
        }
    }

    struct TextureSource {
        const std::string* name;
        const char* semanticName;
        uint8_t semantic;
        uint8_t sampler;
    };
    const TextureSource candidates[] = {
        {&source.colorMap, "colorMap", source.colorSemantic, source.colorSamplerState},
        {&source.normalMap, "normalMap", 5u, source.normalSamplerState},
        {&source.specularMap, "specularMap", 8u, source.specularSamplerState},
    };
    uint32_t textureCount = 0;
    for (const TextureSource& candidate : candidates) {
        if (!candidate.name->empty()
            && Find(kAssetImage, candidate.name->c_str()))
            ++textureCount;
    }
    if (textureCount != 0) {
        material->textureTable = static_cast<MaterialTextureDef*>(Allocate(
            textureCount * sizeof(MaterialTextureDef),
            alignof(MaterialTextureDef)));
        if (!material->textureTable) {
            rollback();
            return nullptr;
        }
    }
    uint32_t outputTexture = 0;
    for (const TextureSource& candidate : candidates) {
        if (candidate.name->empty())
            continue;
        auto* image = static_cast<GfxImage*>(
            Find(kAssetImage, candidate.name->c_str()));
        if (!image) {
            // VITL contains many database materials outside the certified
            // map/model closure. Keep those safe and textureless; selected
            // closure validation still reports a missing material/image at
            // its concrete consumer rather than failing on unrelated UI.
            continue;
        }
        MaterialTextureDef& texture = material->textureTable[outputTexture++];
        texture.nameHash = MaterialNameHash(candidate.semanticName);
        texture.nameStart = candidate.semanticName[0];
        texture.nameEnd = candidate.semanticName[
            std::strlen(candidate.semanticName) - 1u];
        texture.samplerState = candidate.sampler;
        texture.semantic = candidate.semantic;
        texture.u.image = image;
    }
    material->textureCount = static_cast<uint8_t>(textureCount);
    if (!RegisterMaterial(name, material, lifetime)) {
        if (report)
            AddMissingSourceAsset(report, kAssetMaterial, name);
        rollback();
        return nullptr;
    }
    return material;
}

XModel* VitaAssetRegistry::RehydrateModel(
    const VitaModelRecord& source, VitaAssetLifetime lifetime,
    VitaSourceAssetMountReport* report, bool borrowMapIndices) {
    if (borrowMapIndices && lifetime != VitaAssetLifetime::Map) return nullptr;
    if (source.name.empty() || source.name.size() > 256u
        || source.bones.size() > 128u
        || source.boneNames.size() != source.bones.size()
        || source.parents.size() != source.bones.size()
        || source.surfaces.empty() || source.surfaces.size() > 64u
        || source.vertices.empty() || source.vertices.size() > 65535u
        || source.weights.size() != source.vertices.size()
        || source.indices.empty() || source.indices.size() > 300000u)
        return nullptr;

    uint32_t rootBoneCount = 0;
    while (rootBoneCount < source.parents.size()
           && source.parents[rootBoneCount] == 0xFFu)
        ++rootBoneCount;
    if (rootBoneCount > source.bones.size())
        return nullptr;
    for (uint32_t bone = rootBoneCount; bone < source.parents.size(); ++bone) {
        if (source.parents[bone] == 0xFFu || source.parents[bone] >= bone
            || bone - source.parents[bone] > UINT8_MAX)
            return nullptr;
    }

    const size_t arenaCheckpoint = cursor_;
    auto* const stringCheckpoint = scriptStringRefs_;
    const uint32_t entryCheckpoint = entryCount_;
    const auto rollback = [this, arenaCheckpoint, entryCheckpoint, stringCheckpoint] {
        ReleaseScriptStringsTo(stringCheckpoint);
        cursor_ = arenaCheckpoint;
        entryCount_ = entryCheckpoint;
    };
    auto fail = [&](uint32_t type, const char* name) -> XModel* {
        if (report)
            AddMissingSourceAsset(report, type, name);
        rollback();
        return nullptr;
    };

    auto* model = static_cast<XModel*>(
        Allocate(sizeof(XModel), alignof(XModel)));
    char* name = CopyString(source.name.c_str());
    const size_t boneCount = source.bones.size();
    const size_t childBoneCount = boneCount - rootBoneCount;
    auto* boneNames = boneCount ? static_cast<uint16_t*>(Allocate(
        boneCount * sizeof(uint16_t), alignof(uint16_t))) : nullptr;
    auto* parentList = childBoneCount ? static_cast<uint8_t*>(Allocate(
        childBoneCount, alignof(uint8_t))) : nullptr;
    auto* quats = childBoneCount ? static_cast<int16_t*>(Allocate(
        childBoneCount * 4u * sizeof(int16_t), alignof(int16_t))) : nullptr;
    auto* trans = childBoneCount ? static_cast<float*>(Allocate(
        childBoneCount * 3u * sizeof(float), alignof(float))) : nullptr;
    auto* classifications = boneCount ? static_cast<uint8_t*>(Allocate(
        boneCount, alignof(uint8_t))) : nullptr;
    auto* baseMat = boneCount ? static_cast<DObjAnimMat*>(Allocate(
        boneCount * sizeof(DObjAnimMat), alignof(DObjAnimMat))) : nullptr;
    auto* surfaces = static_cast<XSurface*>(Allocate(
        source.surfaces.size() * sizeof(XSurface), alignof(XSurface)));
    auto* materialHandles = static_cast<Material**>(Allocate(
        source.surfaces.size() * sizeof(Material*), alignof(Material*)));
    auto* boneInfo = boneCount ? static_cast<XBoneInfo*>(Allocate(
        boneCount * sizeof(XBoneInfo), alignof(XBoneInfo))) : nullptr;
    if (!model || !name || (boneCount && (!boneNames || !classifications
            || !baseMat || !boneInfo))
        || (childBoneCount && (!parentList || !quats || !trans))
        || !surfaces || !materialHandles)
        return fail(kAssetModel, source.name.c_str());

    constexpr float kPackedQuatScale = 0.00003051850944757462f;
    for (size_t boneIndex = 0; boneIndex < boneCount; ++boneIndex) {
        const std::string& sourceName = source.boneNames[boneIndex];
        if (sourceName.size() >= 64u)
            return fail(kAssetModel, source.name.c_str());
        // Empty is a genuine XModel root-bone string in the retail assets.
        // Interning the one-byte C string recreates that source identity
        // locally (normally id 1) without carrying a desktop handle.
        const uint32_t interned = InternScriptString(
            sourceName.c_str(), static_cast<uint32_t>(sourceName.size() + 1u),
            MT_TYPE_MODEL_PART);
        if (interned == 0 || interned > UINT16_MAX)
            return fail(kAssetModel, sourceName.c_str());
        boneNames[boneIndex] = static_cast<uint16_t>(interned);
        const VitaModelBone& input = source.bones[boneIndex];
        std::memcpy(baseMat[boneIndex].quat, input.bindQuat,
                    sizeof(baseMat[boneIndex].quat));
        std::memcpy(baseMat[boneIndex].trans, input.bindTrans,
                    sizeof(baseMat[boneIndex].trans));
        baseMat[boneIndex].transWeight = input.bindTransWeight;
        std::memcpy(boneInfo[boneIndex].bounds[0], source.mins,
                    sizeof(source.mins));
        std::memcpy(boneInfo[boneIndex].bounds[1], source.maxs,
                    sizeof(source.maxs));
        if (boneIndex >= rootBoneCount) {
            const size_t child = boneIndex - rootBoneCount;
            parentList[child] = static_cast<uint8_t>(
                boneIndex - source.parents[boneIndex]);
            std::memcpy(&trans[child * 3u], input.trans,
                        sizeof(input.trans));
            for (uint32_t component = 0; component < 4; ++component) {
                const long packed = std::lround(
                    input.quat[component] / kPackedQuatScale);
                quats[child * 4u + component] = static_cast<int16_t>(
                    std::max<long>(INT16_MIN, std::min<long>(INT16_MAX, packed)));
            }
        }
    }

    int modelPartBits[4] = {};
    for (size_t surfaceIndex = 0; surfaceIndex < source.surfaces.size();
         ++surfaceIndex) {
        const VitaModelSurface& inputSurface = source.surfaces[surfaceIndex];
        if (inputSurface.vertexCount == 0 || inputSurface.indexCount == 0
            || inputSurface.vertexCount > UINT16_MAX
            || inputSurface.indexCount / 3u > UINT16_MAX
            || static_cast<uint64_t>(inputSurface.vertexStart)
                + inputSurface.vertexCount > source.vertices.size()
            || static_cast<uint64_t>(inputSurface.indexStart)
                + inputSurface.indexCount > source.indices.size())
            return fail(kAssetModel, source.name.c_str());
        auto* material = static_cast<Material*>(Find(
            kAssetMaterial, inputSurface.materialName.c_str()));
        if (!material)
            return fail(kAssetMaterial, inputSurface.materialName.c_str());

        XSurface& output = surfaces[surfaceIndex];
        output.deformed = true;
        output.vertCount = static_cast<uint16_t>(inputSurface.vertexCount);
        output.triCount = static_cast<uint16_t>(inputSurface.indexCount / 3u);
        bool borrowedIndices=false;
        output.triIndices = VitaSourceModelIndices(source,inputSurface,borrowMapIndices,
            [this](size_t bytes,size_t alignment){return Allocate(bytes,alignment);}, &borrowedIndices);
#if KISAK_VITA_CPU_RUNTIME
        if (borrowedIndices)
            VitaCpuProfile_Counter("registry-model-index-shared-bytes",inputSurface.indexCount*sizeof(uint16_t));
#endif
        output.verts0 = static_cast<GfxPackedVertex*>(Allocate(
            inputSurface.vertexCount * sizeof(GfxPackedVertex),
            alignof(GfxPackedVertex)));
        if (!output.triIndices || !output.verts0)
            return fail(kAssetModel, source.name.c_str());

        uint32_t blendWords = 0;
        int16_t groupCounts[4] = {};
        uint32_t previousGroup = 0;
        for (uint32_t localVertex = 0;
             localVertex < inputSurface.vertexCount; ++localVertex) {
            const VitaModelVertexWeight& weight = source.weights[
                inputSurface.vertexStart + localVertex];
            uint32_t group = 0;
            for (uint32_t slot = 1; slot < 4; ++slot) {
                if (weight.weight[slot] != 0)
                    group = slot;
            }
            // Source XSurface vertices are grouped by influence count. A
            // non-monotonic stream cannot be represented without remapping
            // every authored triangle, so reject it as a corrupt VITX.
            if (group < previousGroup || groupCounts[group] == INT16_MAX)
                return fail(kAssetModel, source.name.c_str());
            previousGroup = group;
            ++groupCounts[group];
            blendWords += 1u + group * 2u;
        }
        output.vertInfo.vertsBlend = static_cast<uint16_t*>(Allocate(
            blendWords * sizeof(uint16_t), alignof(uint16_t)));
        if (!output.vertInfo.vertsBlend)
            return fail(kAssetModel, source.name.c_str());
        std::memcpy(output.vertInfo.vertCount, groupCounts,
                    sizeof(groupCounts));

        uint16_t* blend = output.vertInfo.vertsBlend;
        int surfacePartBits[4] = {};
        for (uint32_t localVertex = 0;
             localVertex < inputSurface.vertexCount; ++localVertex) {
            const size_t sourceVertexIndex = inputSurface.vertexStart
                + localVertex;
            const VitaModelVertex& input = source.vertices[sourceVertexIndex];
            const VitaModelVertexWeight& weight = source.weights[sourceVertexIndex];
            GfxPackedVertex& vertex = output.verts0[localVertex];
            std::memcpy(vertex.xyz, input.xyz, sizeof(vertex.xyz));
            vertex.binormalSign = 1.0f;
            vertex.color.packed = input.color;
            vertex.texCoord = PackTexCoords(input.texCoord);
            vertex.normal.packed = input.normal;
            vertex.tangent.packed = input.tangent;
            uint32_t group = 0;
            for (uint32_t slot = 1; slot < 4; ++slot) {
                if (weight.weight[slot] != 0)
                    group = slot;
            }
            if (weight.bone[0] >= boneCount)
                return fail(kAssetModel, source.name.c_str());
            *blend++ = static_cast<uint16_t>(weight.bone[0] << 6);
            surfacePartBits[weight.bone[0] >> 5] |=
                0x80000000u >> (weight.bone[0] & 31u);
            modelPartBits[weight.bone[0] >> 5] |=
                0x80000000u >> (weight.bone[0] & 31u);
            for (uint32_t slot = 1; slot <= group; ++slot) {
                if (weight.bone[slot] >= boneCount
                    || weight.weight[slot] == 0)
                    return fail(kAssetModel, source.name.c_str());
                *blend++ = static_cast<uint16_t>(weight.bone[slot] << 6);
                *blend++ = weight.weight[slot];
                surfacePartBits[weight.bone[slot] >> 5] |=
                    0x80000000u >> (weight.bone[slot] & 31u);
                modelPartBits[weight.bone[slot] >> 5] |=
                    0x80000000u >> (weight.bone[slot] & 31u);
            }
        }
        std::memcpy(output.partBits, surfacePartBits, sizeof(output.partBits));
        materialHandles[surfaceIndex] = material;
    }

    model->name = name;
    model->numBones = static_cast<uint8_t>(boneCount);
    model->numRootBones = static_cast<uint8_t>(rootBoneCount);
    model->numsurfs = static_cast<uint8_t>(source.surfaces.size());
    model->lodRampType = XMODEL_LOD_RAMP_SKINNED;
    model->boneNames = boneNames;
    model->parentList = parentList;
    model->quats = quats;
    model->trans = trans;
    model->partClassification = classifications;
    model->baseMat = baseMat;
    model->surfs = surfaces;
    model->materialHandles = materialHandles;
    model->lodInfo[0].numsurfs = model->numsurfs;
    model->lodInfo[0].surfIndex = 0;
    model->lodInfo[0].lod = 0;
    std::memcpy(model->lodInfo[0].partBits, modelPartBits,
                sizeof(modelPartBits));
    model->boneInfo = boneInfo;
    std::memcpy(model->mins, source.mins, sizeof(model->mins));
    std::memcpy(model->maxs, source.maxs, sizeof(model->maxs));
    float radiusSquared = 0.0f;
    for (uint32_t component = 0; component < 3; ++component) {
        const float extent = std::max(std::fabs(source.mins[component]),
                                      std::fabs(source.maxs[component]));
        radiusSquared += extent * extent;
    }
    model->radius = std::sqrt(radiusSquared);
    model->numLods = 1;
    model->collLod = -1;
    model->memUsage = static_cast<int>(std::min<size_t>(
        cursor_ - arenaCheckpoint,
        static_cast<size_t>(std::numeric_limits<int>::max())));
    if (!RegisterModel(name, model, lifetime))
        return fail(kAssetModel, source.name.c_str());
    return model;
}

XAnimParts* VitaAssetRegistry::RehydrateAnimation(
    const VitaAnimationClip& source, VitaAssetLifetime lifetime,
    VitaSourceAssetMountReport* report, bool borrowCompressedPayload) {
    if (!source.SourceComplete() || source.name.empty()
        || source.name.size() >= 128u
        || source.boneNames.size() > 128u
        || (borrowCompressedPayload && lifetime != VitaAssetLifetime::Map))
        return nullptr;

    uint32_t quatParts = 0;
    uint32_t transParts = 0;
    for (uint32_t index = 0; index < 5; ++index)
        quatParts += source.boneCount[index];
    for (uint32_t index = 5; index < 9; ++index)
        transParts += source.boneCount[index];
    if (quatParts != source.boneCount[9]
        || transParts != source.boneCount[9]
        || source.dataByteCount > UINT16_MAX
        || source.dataShortCount > UINT16_MAX
        || source.dataIntCount > UINT16_MAX
        || source.randomDataByteCount > UINT16_MAX
        || source.randomDataIntCount > UINT16_MAX)
        return nullptr;

    const size_t arenaCheckpoint = cursor_;
    auto* const stringCheckpoint = scriptStringRefs_;
    const uint32_t entryCheckpoint = entryCount_;
    const auto rollback = [this, arenaCheckpoint, entryCheckpoint, stringCheckpoint] {
        ReleaseScriptStringsTo(stringCheckpoint);
        cursor_ = arenaCheckpoint;
        entryCount_ = entryCheckpoint;
    };
    const auto fail = [&](const char* name) -> XAnimParts* {
        if (report)
            AddMissingSourceAsset(report, kAssetAnimation, name);
        rollback();
        return nullptr;
    };
    const auto copyBytes = [this, borrowCompressedPayload](const void* data, size_t bytes,
                                  size_t alignment) -> void* {
        if (bytes == 0)
            return nullptr;
        if (!data)
            return nullptr;
        // XAnim reads these compressed arrays without mutating them. The map
        // bundle owns their stable, correctly typed vector storage until the
        // source session and registry have shut down. Construct the native
        // pointer graph here, without a second copy of every animation stream.
        if (borrowCompressedPayload) {
            if (reinterpret_cast<uintptr_t>(data) & (alignment - 1u))
                return nullptr;
            return const_cast<void*>(data);
        }
        void* result = Allocate(bytes, alignment);
        if (result)
            std::memcpy(result, data, bytes);
        return result;
    };

    auto* parts = static_cast<XAnimParts*>(
        Allocate(sizeof(XAnimParts), alignof(XAnimParts)));
    char* name = CopyString(source.name.c_str());
    if (!parts || !name)
        return fail(source.name.c_str());

    parts->name = name;
    parts->dataByteCount = static_cast<uint16_t>(source.dataByteCount);
    parts->dataShortCount = static_cast<uint16_t>(source.dataShortCount);
    parts->dataIntCount = static_cast<uint16_t>(source.dataIntCount);
    parts->randomDataByteCount = static_cast<uint16_t>(
        source.randomDataByteCount);
    parts->randomDataIntCount = static_cast<uint16_t>(
        source.randomDataIntCount);
    parts->numframes = source.numFrames;
    parts->bLoop = source.loop;
    parts->bDelta = source.delta;
    std::memcpy(parts->boneCount, source.boneCount,
                sizeof(parts->boneCount));
    parts->notifyCount = source.notifyCount;
    parts->assetType = source.assetType;
    parts->isDefault = source.isDefault;
    parts->randomDataShortCount = source.randomDataShortCount;
    parts->indexCount = source.indexCount;
    parts->framerate = source.framerate;
    parts->frequency = source.frequency;

    if (!source.boneNames.empty()) {
        parts->names = static_cast<uint16_t*>(Allocate(
            source.boneNames.size() * sizeof(uint16_t), alignof(uint16_t)));
        if (!parts->names)
            return fail(source.name.c_str());
        for (size_t index = 0; index < source.boneNames.size(); ++index) {
            const std::string& boneName = source.boneNames[index];
            if (boneName.empty()
                || boneName.size() >= kVitaAnimationBoneNameBytes)
                return fail(source.name.c_str());
            const uint32_t interned = InternScriptString(
                boneName.c_str(),
                static_cast<uint32_t>(boneName.size() + 1u),
                MT_TYPE_ANIM_PART);
            if (interned == 0 || interned > UINT16_MAX)
                return fail(boneName.c_str());
            parts->names[index] = static_cast<uint16_t>(interned);
        }
    }

    if (!source.notifies.empty()) {
        parts->notify = static_cast<XAnimNotifyInfo*>(Allocate(
            source.notifies.size() * sizeof(XAnimNotifyInfo),
            alignof(XAnimNotifyInfo)));
        if (!parts->notify)
            return fail(source.name.c_str());
        for (size_t index = 0; index < source.notifies.size(); ++index) {
            const VitaAnimationNotify& notify = source.notifies[index];
            if (notify.name.empty()
                || notify.name.size() >= kVitaAnimationNotifyNameBytes
                || !std::isfinite(notify.time)
                || notify.time < 0.0f || notify.time > 1.0f)
                return fail(source.name.c_str());
            const uint32_t interned = InternScriptString(
                notify.name.c_str(),
                static_cast<uint32_t>(notify.name.size() + 1u),
                MT_TYPE_NOTETRACK);
            if (interned == 0 || interned > UINT16_MAX)
                return fail(notify.name.c_str());
            parts->notify[index].name = static_cast<uint16_t>(interned);
            parts->notify[index].time = notify.time;
        }
    }

    parts->dataByte = static_cast<uint8_t*>(copyBytes(
        source.dataByte.data(), source.dataByte.size(), alignof(uint8_t)));
    parts->dataShort = static_cast<int16_t*>(copyBytes(
        source.dataShort.data(), source.dataShort.size() * sizeof(int16_t),
        alignof(int16_t)));
    parts->dataInt = static_cast<int*>(copyBytes(
        source.dataInt.data(), source.dataInt.size() * sizeof(int32_t),
        alignof(int32_t)));
    parts->randomDataByte = static_cast<uint8_t*>(copyBytes(
        source.randomDataByte.data(), source.randomDataByte.size(),
        alignof(uint8_t)));
    parts->randomDataShort = static_cast<int16_t*>(copyBytes(
        source.randomDataShort.data(),
        source.randomDataShort.size() * sizeof(int16_t), alignof(int16_t)));
    parts->randomDataInt = static_cast<int*>(copyBytes(
        source.randomDataInt.data(),
        source.randomDataInt.size() * sizeof(int32_t), alignof(int32_t)));
    parts->indices.data = copyBytes(source.indices.data(),
                                    source.indices.size(),
                                    source.numFrames >= 256
                                        ? alignof(uint16_t) : alignof(uint8_t));
    if ((source.dataByteCount && !parts->dataByte)
        || (source.dataShortCount && !parts->dataShort)
        || (source.dataIntCount && !parts->dataInt)
        || (source.randomDataByteCount && !parts->randomDataByte)
        || (source.randomDataShortCount && !parts->randomDataShort)
        || (source.randomDataIntCount && !parts->randomDataInt)
        || (source.indexCount && !parts->indices.data))
        return fail(source.name.c_str());

    if (source.delta) {
        parts->deltaPart = static_cast<XAnimDeltaPart*>(Allocate(
            sizeof(XAnimDeltaPart), alignof(XAnimDeltaPart)));
        if (!parts->deltaPart)
            return fail(source.name.c_str());
        const VitaAnimationDelta& input = source.deltaPart;
        if (input.hasTrans) {
            constexpr size_t kTransIndexOffset = offsetof(XAnimPartTrans, u)
                + offsetof(XAnimPartTransFrames, indices);
            const size_t transBytes = std::max(
                sizeof(XAnimPartTrans),
                kTransIndexOffset + input.transIndices.size());
            auto* trans = static_cast<XAnimPartTrans*>(Allocate(
                transBytes, alignof(XAnimPartTrans)));
            if (!trans)
                return fail(source.name.c_str());
            trans->size = input.transSize;
            trans->smallTrans = input.transSmall ? 1u : 0u;
            if (input.transSize == 0) {
                std::memcpy(trans->u.frame0, input.transBase,
                            sizeof(input.transBase));
            } else {
                std::memcpy(trans->u.frames.mins, input.transBase,
                            sizeof(input.transBase));
                std::memcpy(trans->u.frames.size, input.transRange,
                            sizeof(input.transRange));
                void* frames = copyBytes(input.transFrames.data(),
                                         input.transFrames.size(),
                                         input.transSmall
                                             ? alignof(uint8_t)
                                             : alignof(uint16_t));
                if (!frames)
                    return fail(source.name.c_str());
                if (input.transSmall)
                    trans->u.frames.frames._1 =
                        static_cast<uint8_t (*)[3]>(frames);
                else
                    trans->u.frames.frames._2 =
                        static_cast<uint16_t (*)[3]>(frames);
                std::memcpy(&trans->u.frames.indices,
                            input.transIndices.data(),
                            input.transIndices.size());
            }
            parts->deltaPart->trans = trans;
        }
        if (input.hasQuat) {
            constexpr size_t kQuatIndexOffset =
                offsetof(XAnimDeltaPartQuat, u)
                + offsetof(XAnimDeltaPartQuatDataFrames, indices);
            const size_t quatBytes = std::max(
                sizeof(XAnimDeltaPartQuat),
                kQuatIndexOffset + input.quatIndices.size());
            auto* quat = static_cast<XAnimDeltaPartQuat*>(Allocate(
                quatBytes, alignof(XAnimDeltaPartQuat)));
            if (!quat)
                return fail(source.name.c_str());
            quat->size = input.quatSize;
            if (input.quatSize == 0) {
                std::memcpy(quat->u.frame0, input.quatFrame0,
                            sizeof(input.quatFrame0));
            } else {
                void* frames = copyBytes(input.quatFrames.data(),
                                         input.quatFrames.size(),
                                         alignof(int16_t));
                if (!frames)
                    return fail(source.name.c_str());
                quat->u.frames.frames = static_cast<int16_t (*)[2]>(frames);
                std::memcpy(&quat->u.frames.indices,
                            input.quatIndices.data(),
                            input.quatIndices.size());
            }
            parts->deltaPart->quat = quat;
        }
    }

    if (!RegisterAnimation(name, parts, lifetime))
        return fail(source.name.c_str());
    return parts;
}

VitaSourceAssetMountReport VitaAssetRegistry::RehydrateSourceImages(
    const VitaImageBundle& source, VitaAssetLifetime lifetime,
    bool retainPixelPayload) {
    VitaSourceAssetMountReport report{};
    report.assetType = kAssetImage;
    report.sourceCount = source.Count();
    const size_t arenaCheckpoint = cursor_;
    auto* const stringCheckpoint = scriptStringRefs_;
    const uint32_t entryCheckpoint = entryCount_;
    if (!source.Loaded())
        return report;
    for (uint32_t index = 0; index < source.Count(); ++index) {
        const VitaImageRecord* record = source.Record(index);
        if (!record || !RehydrateImage(*record, lifetime,
                                        retainPixelPayload)) {
            if (record)
                std::strncpy(report.failedAsset, record->name.c_str(),
                             sizeof(report.failedAsset) - 1u);
            ReleaseScriptStringsTo(stringCheckpoint);
            cursor_ = arenaCheckpoint;
            entryCount_ = entryCheckpoint;
            report.mountedCount = 0;
            return report;
        }
        ++report.mountedCount;
    }
    report.ready = report.mountedCount == report.sourceCount;
    return report;
}

VitaSourceAssetMountReport VitaAssetRegistry::RehydrateSourceMaterials(
    const VitaMaterialBundle& source, VitaAssetLifetime lifetime) {
    VitaSourceAssetMountReport report{};
    report.assetType = kAssetMaterial;
    report.sourceCount = source.Count();
    const size_t arenaCheckpoint = cursor_;
    auto* const stringCheckpoint = scriptStringRefs_;
    const uint32_t entryCheckpoint = entryCount_;
    if (!source.Loaded())
        return report;
    for (uint32_t index = 0; index < source.Count(); ++index) {
        const VitaMaterialRecord* record = source.Record(index);
        if (!record || !RehydrateMaterial(*record, lifetime, &report)) {
            if (record)
                std::strncpy(report.failedAsset, record->name.c_str(),
                             sizeof(report.failedAsset) - 1u);
            ReleaseScriptStringsTo(stringCheckpoint);
            cursor_ = arenaCheckpoint;
            entryCount_ = entryCheckpoint;
            report.mountedCount = 0;
            return report;
        }
        ++report.mountedCount;
    }
    report.ready = report.mountedCount == report.sourceCount;
    return report;
}

VitaSourceAssetMountReport VitaAssetRegistry::RehydrateSourceModels(
    const VitaModelBundle& source, VitaAssetLifetime lifetime, bool borrowMapIndices) {
    VitaSourceAssetMountReport report{};
    report.assetType = kAssetModel;
    report.sourceCount = source.Count();
    const size_t arenaCheckpoint = cursor_;
    auto* const stringCheckpoint = scriptStringRefs_;
    const uint32_t entryCheckpoint = entryCount_;
    if (!source.SourceComplete()) {
        std::strncpy(report.failedAsset, "VITX v5 bone-name closure",
                     sizeof(report.failedAsset) - 1u);
        return report;
    }
    for (const VitaModelRecord& record : source.Records()) {
        if (!RehydrateModel(record, lifetime, &report, borrowMapIndices)) {
            std::strncpy(report.failedAsset, record.name.c_str(),
                         sizeof(report.failedAsset) - 1u);
            ReleaseScriptStringsTo(stringCheckpoint);
            cursor_ = arenaCheckpoint;
            entryCount_ = entryCheckpoint;
            report.mountedCount = 0;
            return report;
        }
        ++report.mountedCount;
    }
    report.ready = report.mountedCount == report.sourceCount;
    return report;
}

VitaSourceAssetMountReport VitaAssetRegistry::RehydrateSourceAnimations(
    const VitaAnimationBundle& source, VitaAssetLifetime lifetime,
    bool borrowCompressedPayload) {
    VitaSourceAssetMountReport report{};
    report.assetType = kAssetAnimation;
    report.sourceCount = source.Count();
    const size_t arenaCheckpoint = cursor_;
    auto* const stringCheckpoint = scriptStringRefs_;
    const uint32_t entryCheckpoint = entryCount_;
    if (!source.SourceComplete()) {
        std::strncpy(report.failedAsset, "VITQ v3 compressed closure",
                     sizeof(report.failedAsset) - 1u);
        return report;
    }
    for (const VitaAnimationClip& record : source.Records()) {
        if (!RehydrateAnimation(record, lifetime, &report, borrowCompressedPayload)) {
            std::strncpy(report.failedAsset, record.name.c_str(),
                         sizeof(report.failedAsset) - 1u);
            ReleaseScriptStringsTo(stringCheckpoint);
            cursor_ = arenaCheckpoint;
            entryCount_ = entryCheckpoint;
            report.mountedCount = 0;
            return report;
        }
        ++report.mountedCount;
    }
    report.ready = report.mountedCount == report.sourceCount;
    return report;
}

VitaSourceAssetMountReport VitaAssetRegistry::RehydrateSourceEffects(
    const VitaEffectBundle& source, const VitaImpactFxBundle& impacts,
    VitaAssetLifetime lifetime) {
    VitaSourceAssetMountReport report{};
    report.assetType = kAssetEffect;
    report.sourceCount = source.Count() + impacts.Count();
    const size_t arenaCheckpoint = cursor_;
    auto* const stringCheckpoint = scriptStringRefs_;
    const uint32_t entryCheckpoint = entryCount_;
    const auto rollback = [this, arenaCheckpoint, entryCheckpoint, stringCheckpoint] {
        ReleaseScriptStringsTo(stringCheckpoint);
        cursor_ = arenaCheckpoint;
        entryCount_ = entryCheckpoint;
    };
    const auto fail = [&](const char* name) {
        if (name && name[0])
            std::strncpy(report.failedAsset, name,
                         sizeof(report.failedAsset) - 1u);
        rollback();
        report.mountedCount = 0;
        report.ready = false;
        return report;
    };
    if (!source.SourceComplete())
        return fail("VFXA v3 source effect closure");
    if (!impacts.SourceComplete())
        return fail("VIFX v1 source impact table");

    struct PendingEffect {
        const VitaEffectRecord* source = nullptr;
        FxEffectDef* effect = nullptr;
        FxElemDef* elements = nullptr;
    };
    std::vector<PendingEffect> pending;
    pending.reserve(source.Count());

    // Publish all effect identities before fixing graph edges. Runner,
    // impact, death and emission references can form cycles in retail FX.
    for (uint32_t index = 0; index < source.Count(); ++index) {
        const VitaEffectRecord* input = source.Record(index);
        if (!input || input->path.empty())
            return fail(input ? input->name.c_str() : "missing VFXA effect");
        auto* effect = static_cast<FxEffectDef*>(Allocate(
            sizeof(FxEffectDef), alignof(FxEffectDef)));
        auto* elements = input->elements.empty() ? nullptr
            : static_cast<FxElemDef*>(Allocate(
                input->elements.size() * sizeof(FxElemDef),
                alignof(FxElemDef)));
        char* name = CopyString(input->path.c_str());
        if (!effect || (!input->elements.empty() && !elements) || !name)
            return fail(input->path.c_str());
        effect->name = name;
        effect->flags = static_cast<int>(input->flags);
        effect->msecLoopingLife = input->msecLoopingLife;
        effect->elemDefCountLooping = static_cast<int>(
            input->elemDefCountLooping);
        effect->elemDefCountOneShot = static_cast<int>(
            input->elemDefCountOneShot);
        effect->elemDefCountEmission = static_cast<int>(
            input->elemDefCountEmission);
        effect->elemDefs = elements;
        if (!RegisterEffect(name, effect, lifetime))
            return fail(input->path.c_str());
        // VFXA deliberately retains both identities: the fastfile asset path
        // used by LoadFx and the level._effect/script alias used by runner
        // visuals.  Retail graphs legitimately reference either form (for
        // example weapon_dragunov_clip -> shellejects/clip_dragunov), so
        // publish both names to the typed registry before resolving edges.
        if (!input->name.empty()
            && !NamesEqual(input->name.c_str(), input->path.c_str())
            && !RegisterEffect(input->name.c_str(), effect, lifetime))
            return fail(input->name.c_str());
        pending.push_back({input, effect, elements});
    }

    const auto resolve = [this, &report](uint32_t type,
                                         const std::string& name) -> void* {
        if (name.empty())
            return nullptr;
        void* asset = Find(type, name.c_str());
        if (!asset)
            AddMissingSourceAsset(&report, type, name.c_str());
        return asset;
    };
    const auto copyRange = [](FxFloatRange* output, const float input[2]) {
        output->base = input[0];
        output->amplitude = input[1];
    };
    static_assert(sizeof(FxElemVelStateSample) == 24u * sizeof(float));

    for (PendingEffect& item : pending) {
        size_t estimatedBytes = sizeof(FxEffectDef)
            + item.source->elements.size() * sizeof(FxElemDef)
            + item.source->path.size() + 1u;
        for (size_t elementIndex = 0;
             elementIndex < item.source->elements.size(); ++elementIndex) {
            const VitaEffectElement& input = item.source->elements[elementIndex];
            FxElemDef& output = item.elements[elementIndex];
            output.flags = static_cast<int>(input.flags);
            output.spawn.looping.intervalMsec = input.spawnDef[0];
            output.spawn.looping.count = input.spawnDef[1];
            copyRange(&output.spawnRange, input.spawnRange);
            copyRange(&output.fadeInRange, input.fadeInRange);
            copyRange(&output.fadeOutRange, input.fadeOutRange);
            output.spawnFrustumCullRadius = input.spawnFrustumCullRadius;
            output.spawnDelayMsec.base = input.spawnDelayMsec[0];
            output.spawnDelayMsec.amplitude = input.spawnDelayMsec[1];
            output.lifeSpanMsec.base = input.lifeSpanMsec[0];
            output.lifeSpanMsec.amplitude = input.lifeSpanMsec[1];
            for (uint32_t axis = 0; axis < 3; ++axis) {
                copyRange(&output.spawnOrigin[axis], input.spawnOrigin[axis]);
                copyRange(&output.spawnAngles[axis], input.spawnAngles[axis]);
                copyRange(&output.angularVelocity[axis],
                          input.angularVelocity[axis]);
            }
            copyRange(&output.spawnOffsetRadius, input.spawnOffsetRadius);
            copyRange(&output.spawnOffsetHeight, input.spawnOffsetHeight);
            copyRange(&output.initialRotation, input.initialRotation);
            copyRange(&output.gravity, input.gravity);
            copyRange(&output.reflectionFactor, input.reflectionFactor);
            output.atlas.behavior = input.atlasBehavior;
            output.atlas.index = input.atlasIndex;
            output.atlas.fps = input.atlasFps;
            output.atlas.loopCount = input.atlasLoopCount;
            output.atlas.colIndexBits = input.atlasColIndexBits;
            output.atlas.rowIndexBits = input.atlasRowIndexBits;
            output.atlas.entryCount = static_cast<int16_t>(
                input.atlasEntryCount);
            output.elemType = static_cast<uint8_t>(input.elemType);
            output.visualCount = static_cast<uint8_t>(input.visualCount);
            output.velIntervalCount = static_cast<uint8_t>(
                input.velIntervalCount);
            output.visStateIntervalCount = static_cast<uint8_t>(
                input.visStateIntervalCount);
            std::memcpy(output.collMins, input.collMins,
                        sizeof(output.collMins));
            std::memcpy(output.collMaxs, input.collMaxs,
                        sizeof(output.collMaxs));
            copyRange(&output.emitDist, input.emitDist);
            copyRange(&output.emitDistVariance, input.emitDistVariance);
            output.sortOrder = static_cast<uint8_t>(input.sortOrder);
            output.lightingFrac = static_cast<uint8_t>(std::lround(
                input.lightingFrac));
            output.useItemClip = static_cast<uint8_t>(input.useItemClip);

            output.velSamples = static_cast<FxElemVelStateSample*>(Allocate(
                input.velocitySamples.size() * sizeof(FxElemVelStateSample),
                alignof(FxElemVelStateSample)));
            if (!output.velSamples)
                return fail(item.source->path.c_str());
            std::memcpy(output.velSamples, input.velocitySamples.data(),
                        input.velocitySamples.size()
                            * sizeof(FxElemVelStateSample));
            estimatedBytes += input.velocitySamples.size()
                * sizeof(FxElemVelStateSample);

            if (!input.visualSamples.empty()) {
                output.visSamples = static_cast<FxElemVisStateSample*>(Allocate(
                    input.visualSamples.size() * sizeof(FxElemVisStateSample),
                    alignof(FxElemVisStateSample)));
                if (!output.visSamples)
                    return fail(item.source->path.c_str());
                for (size_t sampleIndex = 0;
                     sampleIndex < input.visualSamples.size(); ++sampleIndex) {
                    const VitaEffectVisualSample& in =
                        input.visualSamples[sampleIndex];
                    FxElemVisStateSample& out = output.visSamples[sampleIndex];
                    std::memcpy(out.base.color, in.baseColor,
                                sizeof(out.base.color));
                    out.base.rotationDelta = in.baseRotationDelta;
                    out.base.rotationTotal = in.baseRotationTotal;
                    std::memcpy(out.base.size, in.baseSize,
                                sizeof(out.base.size));
                    out.base.scale = in.baseScale;
                    std::memcpy(out.amplitude.color, in.amplitudeColor,
                                sizeof(out.amplitude.color));
                    out.amplitude.rotationDelta = in.amplitudeRotationDelta;
                    out.amplitude.rotationTotal = in.amplitudeRotationTotal;
                    std::memcpy(out.amplitude.size, in.amplitudeSize,
                                sizeof(out.amplitude.size));
                    out.amplitude.scale = in.amplitudeScale;
                }
                estimatedBytes += input.visualSamples.size()
                    * sizeof(FxElemVisStateSample);
            }

            if (input.trailPresent) {
                output.trailDef = static_cast<FxTrailDef*>(Allocate(
                    sizeof(FxTrailDef), alignof(FxTrailDef)));
                if (!output.trailDef)
                    return fail(item.source->path.c_str());
                output.trailDef->scrollTimeMsec = input.trailScrollTimeMsec;
                output.trailDef->repeatDist = input.trailRepeatDist;
                output.trailDef->splitDist = input.trailSplitDist;
                output.trailDef->vertCount = static_cast<int>(
                    input.trailVertices.size());
                output.trailDef->indCount = static_cast<int>(
                    input.trailIndices.size());
                if (!input.trailVertices.empty()) {
                    output.trailDef->verts = static_cast<FxTrailVertex*>(Allocate(
                        input.trailVertices.size() * sizeof(FxTrailVertex),
                        alignof(FxTrailVertex)));
                    if (!output.trailDef->verts)
                        return fail(item.source->path.c_str());
                    for (size_t vertexIndex = 0;
                         vertexIndex < input.trailVertices.size(); ++vertexIndex) {
                        const auto& in = input.trailVertices[vertexIndex];
                        FxTrailVertex& out = output.trailDef->verts[vertexIndex];
                        std::memcpy(out.pos, in.data(), sizeof(out.pos));
                        std::memcpy(out.normal, in.data() + 2,
                                    sizeof(out.normal));
                        out.texCoord = in[4];
                    }
                }
                if (!input.trailIndices.empty()) {
                    output.trailDef->inds = static_cast<uint16_t*>(Allocate(
                        input.trailIndices.size() * sizeof(uint16_t),
                        alignof(uint16_t)));
                    if (!output.trailDef->inds)
                        return fail(item.source->path.c_str());
                    std::memcpy(output.trailDef->inds,
                                input.trailIndices.data(),
                                input.trailIndices.size() * sizeof(uint16_t));
                }
                estimatedBytes += sizeof(FxTrailDef)
                    + input.trailVertices.size() * sizeof(FxTrailVertex)
                    + input.trailIndices.size() * sizeof(uint16_t);
            }

            const auto effectRef = [&](const std::string& name,
                                       FxEffectDefRef* destination) {
                if (name.empty())
                    return true;
                destination->handle = static_cast<const FxEffectDef*>(
                    resolve(kAssetEffect, name));
                return destination->handle != nullptr;
            };
            if (!effectRef(input.effectOnImpact, &output.effectOnImpact)
                || !effectRef(input.effectOnDeath, &output.effectOnDeath)
                || !effectRef(input.effectEmitted, &output.effectEmitted))
                return fail(item.source->path.c_str());

            if (input.elemType == 9u) {
                output.visuals.markArray = static_cast<FxElemMarkVisuals*>(Allocate(
                    input.visualCount * sizeof(FxElemMarkVisuals),
                    alignof(FxElemMarkVisuals)));
                if (input.visualCount && !output.visuals.markArray)
                    return fail(item.source->path.c_str());
                for (uint32_t visualIndex = 0;
                     visualIndex < input.visualCount; ++visualIndex) {
                    for (uint32_t slot = 0; slot < 2; ++slot) {
                        const std::string& name = input.visualNames[
                            visualIndex * 2u + slot];
                        if (!name.empty()) {
                            output.visuals.markArray[visualIndex].materials[slot] =
                                static_cast<Material*>(resolve(
                                    kAssetMaterial, name));
                            if (!output.visuals.markArray[visualIndex]
                                     .materials[slot])
                                return fail(item.source->path.c_str());
                        }
                    }
                }
                estimatedBytes += input.visualCount
                    * sizeof(FxElemMarkVisuals);
            } else if (input.elemType != 6u && input.elemType != 7u
                       && input.visualCount != 0u) {
                FxElemVisuals* visuals = nullptr;
                if (input.visualCount == 1u) {
                    visuals = &output.visuals.instance;
                } else {
                    output.visuals.array = static_cast<FxElemVisuals*>(Allocate(
                        input.visualCount * sizeof(FxElemVisuals),
                        alignof(FxElemVisuals)));
                    visuals = output.visuals.array;
                    if (!visuals)
                        return fail(item.source->path.c_str());
                    estimatedBytes += input.visualCount
                        * sizeof(FxElemVisuals);
                }
                for (uint32_t visualIndex = 0;
                     visualIndex < input.visualCount; ++visualIndex) {
                    const std::string& name = input.visualNames[visualIndex];
                    if (name.empty())
                        return fail(item.source->path.c_str());
                    if (input.elemType == 5u) {
                        visuals[visualIndex].model = static_cast<XModel*>(
                            resolve(kAssetModel, name));
                        if (!visuals[visualIndex].model)
                            return fail(item.source->path.c_str());
                    } else if (input.elemType == 8u) {
                        char* soundName = CopyString(name.c_str());
                        if (!soundName)
                            return fail(item.source->path.c_str());
                        visuals[visualIndex].soundName = soundName;
                        estimatedBytes += name.size() + 1u;
                        if (!Find(kAssetSound, name.c_str())
                            && !RehydrateSoundAlias(name.c_str(), 0, lifetime)) {
                            AddMissingSourceAsset(&report, kAssetSound,
                                                  name.c_str());
                            return fail(item.source->path.c_str());
                        }
                    } else if (input.elemType == 10u) {
                        visuals[visualIndex].effectDef.handle =
                            static_cast<const FxEffectDef*>(resolve(
                                kAssetEffect, name));
                        if (!visuals[visualIndex].effectDef.handle)
                            return fail(item.source->path.c_str());
                    } else {
                        visuals[visualIndex].material = static_cast<Material*>(
                            resolve(kAssetMaterial, name));
                        if (!visuals[visualIndex].material)
                            return fail(item.source->path.c_str());
                    }
                }
            }
        }
        item.effect->totalSize = static_cast<int>(std::min<size_t>(
            estimatedBytes, static_cast<size_t>(
                std::numeric_limits<int>::max())));
        ++report.mountedCount;
    }
    // Rebuild the fastfile-owned impact table only after every FxEffectDef is
    // registered.  The source cgame asks DB_FindXAssetHeader for the empty
    // table name, exactly as on PC; no Vita combat/effect switch is involved.
    for (uint32_t tableIndex = 0; tableIndex < impacts.Count(); ++tableIndex) {
        const VitaImpactFxTableRecord* input = impacts.Table(tableIndex);
        if (!input)
            return fail("missing VIFX table");
        auto* table = static_cast<FxImpactTable*>(Allocate(
            sizeof(FxImpactTable), alignof(FxImpactTable)));
        auto* entries = static_cast<FxImpactEntry*>(Allocate(
            kVitaImpactTypeCount * sizeof(FxImpactEntry),
            alignof(FxImpactEntry)));
        char* name = CopyString(input->name.c_str());
        if (!table || !entries || !name)
            return fail(input->name.empty() ? "<default impactfx>"
                                            : input->name.c_str());
        table->name = name;
        table->table = entries;
        for (uint32_t impactType = 0;
             impactType < kVitaImpactTypeCount; ++impactType) {
            const VitaImpactFxEntryRecord& sourceEntry =
                input->entries[impactType];
            FxImpactEntry& output = entries[impactType];
            for (uint32_t index = 0;
                 index < kVitaImpactNonFleshCount; ++index) {
                const std::string& effectName = sourceEntry.nonFlesh[index];
                if (!effectName.empty()) {
                    output.nonflesh[index] = static_cast<const FxEffectDef*>(
                        resolve(kAssetEffect, effectName));
                    if (!output.nonflesh[index])
                        return fail(effectName.c_str());
                }
            }
            for (uint32_t index = 0; index < kVitaImpactFleshCount; ++index) {
                const std::string& effectName = sourceEntry.flesh[index];
                if (!effectName.empty()) {
                    output.flesh[index] = static_cast<const FxEffectDef*>(
                        resolve(kAssetEffect, effectName));
                    if (!output.flesh[index])
                        return fail(effectName.c_str());
                }
            }
        }
        if (!RegisterImpactEffects(name, table, lifetime))
            return fail(input->name.empty() ? "<default impactfx>"
                                            : input->name.c_str());
        ++report.mountedCount;
    }
    report.ready = report.mountedCount == report.sourceCount
        && report.missingCount == 0;
    return report.ready ? report : fail("VFXA/VIFX dependency closure");
}

VitaSourceModelClosureReport VitaAssetRegistry::RehydrateSourceModelClosure(
    const VitaImageBundle& images, const VitaMaterialBundle& materials,
    const VitaModelBundle& models, VitaAssetLifetime lifetime,
    bool retainImagePixelPayload, bool borrowMapIndices) {
    VitaSourceModelClosureReport report{};
    const size_t arenaCheckpoint = cursor_;
    auto* const stringCheckpoint = scriptStringRefs_;
    const uint32_t entryCheckpoint = entryCount_;
    const auto rollback = [this, arenaCheckpoint, entryCheckpoint, stringCheckpoint] {
        ReleaseScriptStringsTo(stringCheckpoint);
        cursor_ = arenaCheckpoint;
        entryCount_ = entryCheckpoint;
    };
    report.images = RehydrateSourceImages(images, lifetime,
                                           retainImagePixelPayload);
    if (!report.images.ready) {
        rollback();
        return report;
    }
    report.materials = RehydrateSourceMaterials(materials, lifetime);
    if (!report.materials.ready) {
        rollback();
        return report;
    }
    report.models = RehydrateSourceModels(models, lifetime, borrowMapIndices);
    if (!report.models.ready) {
        rollback();
        return report;
    }
    report.ready = true;
    return report;
}

namespace {

void AddMissingWeaponAsset(VitaWeaponMountReport* report, uint32_t type,
                           const char* name) {
    if (!report || !name || !name[0])
        return;
    const uint32_t index = report->missingCount++;
    if (index >= VitaWeaponMountReport::kMaxMissing)
        return;
    report->missing[index].assetType = type;
    std::strncpy(report->missing[index].name, name,
                 sizeof(report->missing[index].name) - 1u);
}

} // namespace

WeaponDef* VitaAssetRegistry::RehydrateWeapon(
    const VitaWeaponRecordV5& source, VitaAssetLifetime lifetime,
    VitaWeaponMountReport* report) {
    if (!source.SourceComplete() || !source.name[0])
        return nullptr;
    VitaWeaponMountReport localReport{};
    VitaWeaponMountReport* activeReport = report ? report : &localReport;
    std::strncpy(activeReport->failedWeapon, source.name,
                 sizeof(activeReport->failedWeapon) - 1u);
    const size_t arenaCheckpoint = cursor_;
    auto* const stringCheckpoint = scriptStringRefs_;
    const uint32_t entryCheckpoint = entryCount_;
    const uint32_t missingCheckpoint = activeReport->missingCount;
    const auto rollback = [this, arenaCheckpoint, entryCheckpoint, stringCheckpoint] {
        ReleaseScriptStringsTo(stringCheckpoint);
        cursor_ = arenaCheckpoint;
        entryCount_ = entryCheckpoint;
    };

    auto* weapon = static_cast<WeaponDef*>(
        Allocate(sizeof(WeaponDef), alignof(WeaponDef)));
    if (!weapon)
        return nullptr;

    weapon->szInternalName = CopyString(source.name);
    if (!weapon->szInternalName) {
        rollback();
        return nullptr;
    }

#define KISAK_VITA_REHYDRATE_WEAPON_STRING(label, member)                  \
    do {                                                                    \
        weapon->member = CopyString(source.stringNames[static_cast<uint32_t>(\
            VitaWeaponStringField::label)]);                               \
        if (!weapon->member) {                                              \
            rollback();                                                     \
            return nullptr;                                                 \
        }                                                                   \
    } while (0);
    KISAK_VITA_WEAPON_STRING_FIELDS(KISAK_VITA_REHYDRATE_WEAPON_STRING)
#undef KISAK_VITA_REHYDRATE_WEAPON_STRING

#define KISAK_VITA_REHYDRATE_WEAPON_INT(label, member)                     \
    do {                                                                    \
        const int32_t value = source.integerFields[static_cast<uint32_t>(   \
            VitaWeaponIntField::label)];                                   \
        static_assert(sizeof(weapon->member) == sizeof(value));            \
        std::memcpy(&weapon->member, &value, sizeof(value));                \
    } while (0);
    KISAK_VITA_WEAPON_INT_FIELDS(KISAK_VITA_REHYDRATE_WEAPON_INT)
#undef KISAK_VITA_REHYDRATE_WEAPON_INT

#define KISAK_VITA_REHYDRATE_WEAPON_FLOAT(label, member)                   \
    weapon->member = source.floatFields[static_cast<uint32_t>(              \
        VitaWeaponFloatField::label)];
    KISAK_VITA_WEAPON_FLOAT_FIELDS(KISAK_VITA_REHYDRATE_WEAPON_FLOAT)
#undef KISAK_VITA_REHYDRATE_WEAPON_FLOAT

    // Runtime indexes belong to the source BG table generation, never to the
    // PC export. BG_SetupWeaponDef deterministically fills these fields.
    weapon->iAmmoIndex = 0;
    weapon->iClipIndex = 0;
    weapon->iSharedAmmoCapIndex = -1;
    weapon->altWeaponIndex = 0;

    const auto resolve = [this, activeReport](uint32_t type,
                                        const char* name) -> void* {
        if (!name || !name[0])
            return nullptr;
        void* asset = Find(type, name);
        if (!asset)
            AddMissingWeaponAsset(activeReport, type, name);
        return asset;
    };

    for (uint32_t index = 0; index < kVitaWeaponGunModelCount; ++index) {
        weapon->gunXModel[index] = static_cast<XModel*>(
            resolve(kAssetModel, source.gunModelNames[index]));
    }
    weapon->handXModel = static_cast<XModel*>(
        resolve(kAssetModel, source.handModelName));
    for (uint32_t index = 0; index < kVitaWeaponWorldModelCount; ++index) {
        weapon->worldModel[index] = static_cast<XModel*>(
            resolve(kAssetModel, source.worldModelNames[index]));
    }
    weapon->worldClipModel = static_cast<XModel*>(
        resolve(kAssetModel, source.worldClipModelName));
    weapon->rocketModel = static_cast<XModel*>(
        resolve(kAssetModel, source.rocketModelName));
    weapon->knifeModel = static_cast<XModel*>(
        resolve(kAssetModel, source.knifeModelName));
    weapon->worldKnifeModel = static_cast<XModel*>(
        resolve(kAssetModel, source.worldKnifeModelName));
    weapon->projectileModel = static_cast<XModel*>(
        resolve(kAssetModel, source.projectileModelName));

    for (uint32_t index = 0; index < kVitaWeaponAnimationCount; ++index) {
        const char* name = source.viewAnimationNames[index];
        // Original cgame dereferences each slot before falling back to idle.
        // An authored empty animation is an empty C string, never a null
        // asset pointer. Do not turn that valid source state into a fault.
        weapon->szXAnims[index] = CopyString(name);
        if (!weapon->szXAnims[index]) {
            rollback();
            return nullptr;
        }
        if (name[0])
            resolve(kAssetAnimation, name);
    }

    for (uint32_t index = 0; index < kVitaWeaponHideTagCount; ++index) {
        if (!source.hideTagNames[index][0])
            continue;
        const uint32_t value = InternScriptString(source.hideTagNames[index],
            static_cast<uint32_t>(std::strlen(source.hideTagNames[index]) + 1u), MT_TYPE_MODEL_PART);
        if (!value) {
            AddMissingWeaponAsset(activeReport, kAssetRawFile,
                                  source.hideTagNames[index]);
            continue;
        }
        weapon->hideTags[index] = static_cast<uint16_t>(value);
    }
    for (uint32_t index = 0; index < kVitaWeaponNotetrackCount; ++index) {
        const char* key = source.notetrackSoundKeyNames[index];
        const char* value = source.notetrackSoundValueNames[index];
        if (key[0]) {
            const uint32_t interned = InternScriptString(key,
                static_cast<uint32_t>(std::strlen(key) + 1u), MT_TYPE_NOTETRACK);
            if (interned)
                weapon->notetrackSoundMapKeys[index] =
                    static_cast<uint16_t>(interned);
            else
                AddMissingWeaponAsset(activeReport, kAssetRawFile, key);
        }
        if (value[0]) {
            const uint32_t interned = InternScriptString(value,
                static_cast<uint32_t>(std::strlen(value) + 1u), MT_TYPE_NOTETRACK);
            if (interned)
                weapon->notetrackSoundMapValues[index] =
                    static_cast<uint16_t>(interned);
            else
                AddMissingWeaponAsset(activeReport, kAssetRawFile, value);
        }
    }

#define KISAK_VITA_REHYDRATE_WEAPON_MATERIAL(label, member)                \
    weapon->member = static_cast<Material*>(resolve(                        \
        kAssetMaterial, source.materialNames[static_cast<uint32_t>(         \
            VitaWeaponMaterialField::label)]));
    KISAK_VITA_WEAPON_MATERIAL_FIELDS(KISAK_VITA_REHYDRATE_WEAPON_MATERIAL)
#undef KISAK_VITA_REHYDRATE_WEAPON_MATERIAL

#define KISAK_VITA_REHYDRATE_WEAPON_EFFECT(label, member)                  \
    weapon->member = static_cast<const FxEffectDef*>(resolve(               \
        kAssetEffect, source.effectNames[static_cast<uint32_t>(             \
            VitaWeaponEffectField::label)]));
    KISAK_VITA_WEAPON_EFFECT_FIELDS(KISAK_VITA_REHYDRATE_WEAPON_EFFECT)
#undef KISAK_VITA_REHYDRATE_WEAPON_EFFECT

    const auto resolveSound = [this, activeReport, lifetime](
                                  const char* name) -> snd_alias_list_t* {
        if (!name || !name[0])
            return nullptr;
        auto* sound = static_cast<snd_alias_list_t*>(Find(kAssetSound, name));
        if (!sound)
            sound = RehydrateSoundAlias(name, 0, lifetime);
        if (!sound)
            AddMissingWeaponAsset(activeReport, kAssetSound, name);
        return sound;
    };

#define KISAK_VITA_REHYDRATE_WEAPON_SOUND(label, member)                   \
    weapon->member = resolveSound(source.soundNames[static_cast<uint32_t>(  \
        VitaWeaponSoundField::label)]);
    KISAK_VITA_WEAPON_SOUND_FIELDS(KISAK_VITA_REHYDRATE_WEAPON_SOUND)
#undef KISAK_VITA_REHYDRATE_WEAPON_SOUND

    bool hasBounceSound = false;
    for (uint32_t index = 0; index < kVitaWeaponBounceSoundCount; ++index)
        hasBounceSound |= source.bounceSoundNames[index][0] != '\0';
    if (hasBounceSound) {
        weapon->bounceSound = static_cast<snd_alias_list_t**>(Allocate(
            sizeof(snd_alias_list_t*) * kVitaWeaponBounceSoundCount,
            alignof(snd_alias_list_t*)));
        if (!weapon->bounceSound) {
            rollback();
            return nullptr;
        }
        for (uint32_t index = 0; index < kVitaWeaponBounceSoundCount; ++index)
            weapon->bounceSound[index] =
                resolveSound(source.bounceSoundNames[index]);
    }

    for (uint32_t index = 0; index < 2; ++index) {
        if (source.accuracyGraphNames[index][0]) {
            weapon->accuracyGraphName[index] =
                CopyString(source.accuracyGraphNames[index]);
            if (!weapon->accuracyGraphName[index]) {
                rollback();
                return nullptr;
            }
        }
        // Offline FFA has no AI consumer. Never publish desktop knot
        // pointers; a future bot slice must add a pointer-free knot record.
        weapon->accuracyGraphKnots[index] = nullptr;
        weapon->originalAccuracyGraphKnots[index] = nullptr;
        weapon->accuracyGraphKnotCount[index] = 0;
        weapon->originalAccuracyGraphKnotCount[index] = 0;
    }

    std::memcpy(weapon->vStandMove, source.movementVectors[0],
                sizeof(weapon->vStandMove));
    std::memcpy(weapon->vStandRot, source.movementVectors[1],
                sizeof(weapon->vStandRot));
    std::memcpy(weapon->vDuckedOfs, source.movementVectors[2],
                sizeof(weapon->vDuckedOfs));
    std::memcpy(weapon->vDuckedMove, source.movementVectors[3],
                sizeof(weapon->vDuckedMove));
    std::memcpy(weapon->vDuckedRot, source.movementVectors[4],
                sizeof(weapon->vDuckedRot));
    std::memcpy(weapon->vProneOfs, source.movementVectors[5],
                sizeof(weapon->vProneOfs));
    std::memcpy(weapon->vProneMove, source.movementVectors[6],
                sizeof(weapon->vProneMove));
    std::memcpy(weapon->vProneRot, source.movementVectors[7],
                sizeof(weapon->vProneRot));
    std::memcpy(weapon->parallelBounce, source.parallelBounce,
                sizeof(weapon->parallelBounce));
    std::memcpy(weapon->perpendicularBounce, source.perpendicularBounce,
                sizeof(weapon->perpendicularBounce));
    std::memcpy(weapon->vProjectileColor, source.projectileColor,
                sizeof(weapon->vProjectileColor));
    std::memcpy(weapon->locationDamageMultipliers,
                source.locationDamageMultipliers,
                sizeof(weapon->locationDamageMultipliers));

    if (activeReport->missingCount != missingCheckpoint) {
        rollback();
        return nullptr;
    }
    if (!RegisterWeapon(source.name, weapon, lifetime)) {
        rollback();
        return nullptr;
    }
    return weapon;
}

VitaWeaponMountReport VitaAssetRegistry::RehydrateSourceWeapons(
    const ShipmentWeaponTable& source, VitaAssetLifetime lifetime) {
    VitaWeaponMountReport report{};
    if (!source.SourceComplete()) {
        std::strncpy(report.failedWeapon, "VITP v5 fixed-loadout closure",
                     sizeof(report.failedWeapon) - 1u);
        return report;
    }

    for (const char* name : kVitaSourceWeaponNames) {
        const VitaWeaponRecordV5* record = source.Find(name);
        if (!record || !RehydrateWeapon(*record, lifetime, &report)) {
            if (!report.failedWeapon[0]) {
                std::strncpy(report.failedWeapon, name,
                             sizeof(report.failedWeapon) - 1u);
            }
            return report;
        }
        ++report.mountedCount;
        report.failedWeapon[0] = '\0';
    }
    report.ready = report.mountedCount == std::size(kVitaSourceWeaponNames)
        && report.missingCount == 0;
    return report;
}

VitaMapRootReport VitaAssetRegistry::RehydrateMapRoots(
    const char* mapName, const ShipmentCollision& collision,
    const ShipmentMapEntities& entities, uint32_t checksum,
    VitaAssetLifetime lifetime) {
    VitaMapRootReport report{};
    size_t failedRootBytes=0;
    unsigned rootFailureKind=0;
    const auto allocateRoot=[&](size_t bytes,size_t alignment)->void* {
        void* memory=Allocate(bytes,alignment);
#if KISAK_VITA_CPU_RUNTIME
        if (!memory && !failedRootBytes) {
            failedRootBytes=bytes;
            rootFailureKind=static_cast<unsigned>(mapRootStorage_.LastFailure());
        }
#endif
        return memory;
    };
    const size_t arenaCheckpoint = cursor_;
    auto* const stringCheckpoint = scriptStringRefs_;
    const uint32_t entryCheckpoint = entryCount_;
    const auto fail = [&](const char* reason) {
#if KISAK_VITA_CPU_RUNTIME
        worldBrushGroups.Reset(); // Failed map construction cannot retain borrowed source addresses.
#endif
        ReleaseScriptStringsTo(stringCheckpoint);
        cursor_ = arenaCheckpoint;
        entryCount_ = entryCheckpoint;
        allocatingMapRoots_ = false;
        std::free(mapRootArena_);
#if KISAK_VITA_CPU_RUNTIME
        mapRootStorage_.Clear();
#endif
        mapRootArena_ = nullptr;
        mapRootCapacity_ = 0;
        mapRootCursor_ = 0;
        mapRootHighWater_ = 0;
        if (failedRootBytes && rootFailureKind==5u)
            std::snprintf(report.error,sizeof(report.error),"source map heap allocation failed: %u bytes (%.80s)",
                static_cast<unsigned>(failedRootBytes),reason?reason:"collision roots");
        else if (failedRootBytes && rootFailureKind==2u)
            std::snprintf(report.error,sizeof(report.error),"source map arena limit: %u bytes (%.80s)",
                static_cast<unsigned>(failedRootBytes),reason?reason:"collision roots");
        else
            std::strncpy(report.error, reason ? reason : "map root construction failed",
                         sizeof(report.error) - 1u);
        return report;
    };

    if (!capacity_ || mapRootsMounted_ || cm.isInUse || mapRootCapacity_)
        return fail("map roots require an empty initialized map arena");
    uint32_t certifiedMapIndex=0;
    if(!VitaFindMapIndex(mapName,&certifiedMapIndex))
        return fail("map is outside the original multiplayer catalog");
    char normalizedMapName[32]{};
    std::snprintf(normalizedMapName,sizeof(normalizedMapName),"mp_%s",kVitaMpMaps[certifiedMapIndex].name);
    mapName=normalizedMapName;
    if (!collision.SourceComplete())
        return fail("VITC v3 source collision closure is required");
    if (!entities.Loaded() || entities.Text().empty()
        || entities.Text().size() > 2'000'000u)
        return fail("VITE map entity text is missing or out of bounds");
    if (collision.submodels_.empty()
        || collision.submodels_.size() != collision.modelBrushes_.size()
        || collision.brushes_.empty()
        || collision.brushes_.size() > 65535u)
        return fail("collision cmodel/brush tables are incomplete");
    for (const auto& membership : collision.modelBrushes_) {
        if (membership.size() > static_cast<size_t>(
                std::numeric_limits<int16_t>::max()))
            return fail("a collision cmodel exceeds the source leaf brush limit");
    }
    if (collision.modelBrushes_.front().empty())
        return fail("the source world cmodel has no brushes");

    const size_t planeCount = collision.planes_.size();
    const size_t brushCount = collision.brushes_.size();
    const size_t sideCount = collision.sides_.size();
    const size_t submodelCount = collision.submodels_.size();
    const size_t triangleCount = collision.triangles_.size();
    const size_t vertexCount = collision.vertices_.size();
    const size_t terrainRootCount = (triangleCount + 254u) / 255u;
#if KISAK_VITA_CPU_RUNTIME
    const size_t partitionCount = TerrainPartitionLayout::Leaves(triangleCount);
    const size_t terrainTreeCount = TerrainPartitionLayout::Trees(triangleCount);
#else
    const size_t partitionCount = terrainRootCount;
    const size_t terrainTreeCount = partitionCount;
#endif

    std::vector<uint32_t> materialIds;
    materialIds.reserve(sideCount);
    for (const ShipmentCollision::BrushSide& side : collision.sides_)
        materialIds.push_back(side.material);
    std::sort(materialIds.begin(), materialIds.end());
    materialIds.erase(std::unique(materialIds.begin(), materialIds.end()),
                      materialIds.end());
    if (materialIds.empty())
        materialIds.push_back(0u);
    if (materialIds.size() > 65535u)
        return fail("source collision material table is out of bounds");

    size_t membershipCount = 0;
    for (const auto& membership : collision.modelBrushes_) {
        if (membership.size() > std::numeric_limits<size_t>::max()
                - membershipCount)
            return fail("source cmodel membership size overflowed");
        membershipCount += membership.size();
    }

    char bspName[96] = {};
    std::snprintf(bspName, sizeof(bspName), "maps/mp/%s.d3dbsp", mapName);

    // Compute the exact source-facing root footprint before allocating.  The
    // root set is independent of the already-mounted image/model/animation
    // arena, so Crossfire pays only for its larger collision closure instead
    // of forcing every map to reserve a 32 MiB worst-case registry.
    size_t requiredRootBytes = 0;
    const auto addRootBytes = [&requiredRootBytes](size_t bytes,
                                                   size_t alignment) {
        if (!bytes || !alignment || (alignment & (alignment - 1u)))
            return bytes == 0;
        if (requiredRootBytes > std::numeric_limits<size_t>::max()
                - (alignment - 1u))
            return false;
        const size_t aligned = AlignUp(requiredRootBytes, alignment);
        if (bytes > std::numeric_limits<size_t>::max() - aligned)
            return false;
        requiredRootBytes = aligned + bytes;
        return true;
    };
    bool rootSizeValid = true;
    rootSizeValid &= addRootBytes(std::strlen(bspName) + 1u, alignof(char));
    rootSizeValid &= addRootBytes(std::strlen(mapName) + 1u, alignof(char));
    rootSizeValid &= addRootBytes(entities.Text().size() + 1u, alignof(char));
    rootSizeValid &= addRootBytes(sizeof(MapEnts), alignof(MapEnts));
    rootSizeValid &= addRootBytes(sizeof(GameWorldMp), alignof(GameWorldMp));
    rootSizeValid &= addRootBytes(sizeof(GfxWorld), alignof(GfxWorld));
    rootSizeValid &= addRootBytes((planeCount + 1u) * sizeof(cplane_s),
                                  alignof(cplane_s));
    rootSizeValid &= addRootBytes(sideCount * sizeof(cplane_s),
                                  alignof(cplane_s));
    rootSizeValid &= addRootBytes(sideCount * sizeof(cbrushside_t),
                                  alignof(cbrushside_t));
    rootSizeValid &= addRootBytes(brushCount * sizeof(cbrush_t),
                                  alignof(cbrush_t));
    rootSizeValid &= addRootBytes(submodelCount * sizeof(cmodel_t),
                                  alignof(cmodel_t));
    rootSizeValid &= addRootBytes(submodelCount * sizeof(GfxBrushModel),
                                  alignof(GfxBrushModel));
    rootSizeValid &= addRootBytes(sizeof(cNode_t), alignof(cNode_t));
    rootSizeValid &= addRootBytes(sizeof(cLeaf_t), alignof(cLeaf_t));
    rootSizeValid &= addRootBytes((submodelCount + 1u)
                                      * sizeof(cLeafBrushNode_s),
                                  alignof(cLeafBrushNode_s));
    rootSizeValid &= addRootBytes(sizeof(cbrush_t), alignof(cbrush_t));
    rootSizeValid &= addRootBytes(materialIds.size() * sizeof(dmaterial_t),
                                  alignof(dmaterial_t));
    rootSizeValid &= addRootBytes(membershipCount * sizeof(uint16_t),
                                  alignof(uint16_t));
    rootSizeValid &= addRootBytes(vertexCount * sizeof(float[3]),
                                  alignof(float));
    rootSizeValid &= addRootBytes(triangleCount * 3u * sizeof(uint16_t),
                                  alignof(uint16_t));
    rootSizeValid &= addRootBytes((triangleCount * 3u + 7u) / 8u,
                                  alignof(uint8_t));
    rootSizeValid &= addRootBytes(partitionCount * sizeof(CollisionPartition),
                                  alignof(CollisionPartition));
    rootSizeValid &= addRootBytes(terrainTreeCount * sizeof(CollisionAabbTree),
                                  alignof(CollisionAabbTree));
    rootSizeValid &= addRootBytes(sizeof(uint8_t), alignof(uint8_t));
    // Register() owns six lookup names after the root objects are complete.
    rootSizeValid &= addRootBytes((std::strlen(bspName) + 1u) * 5u,
                                  alignof(char));
    rootSizeValid &= addRootBytes(sizeof("__active__"), alignof(char));
    constexpr size_t kRootArenaGuardBytes = 4096u;
    if (!rootSizeValid
        || requiredRootBytes > std::numeric_limits<size_t>::max()
            - kRootArenaGuardBytes)
        return fail("source map root size overflowed");
    const size_t requestedRootCapacity = AlignUp(
        requiredRootBytes + kRootArenaGuardBytes, 64u);
    if (requestedRootCapacity > kMaximumMapRootArenaBytes
        || requestedRootCapacity > kMaximumArenaBytes - capacity_) {
        char detail[sizeof(report.error)] = {};
        std::snprintf(detail, sizeof(detail),
                      "source map roots require %u bytes (limit %u)",
                      static_cast<unsigned>(requestedRootCapacity),
                      static_cast<unsigned>(kMaximumMapRootArenaBytes));
        return fail(detail);
    }
#if !KISAK_VITA_CPU_RUNTIME
    mapRootArena_ = static_cast<uint8_t*>(
        std::calloc(1, requestedRootCapacity));
    if (!mapRootArena_)
        return fail("source map root arena allocation failed");
#endif
    mapRootCapacity_ = requestedRootCapacity;
    mapRootCursor_ = 0;
    mapRootHighWater_ = 0;
    allocatingMapRoots_ = true;

    char* ownedBspName = CopyString(bspName);
    char* ownedBaseName = CopyString(mapName);
    char* entityText = static_cast<char*>(allocateRoot(
        entities.Text().size() + 1u, alignof(char)));
    auto* mapEnts = static_cast<MapEnts*>(
        allocateRoot(sizeof(MapEnts), alignof(MapEnts)));
    auto* gameWorld = static_cast<GameWorldMp*>(
        allocateRoot(sizeof(GameWorldMp), alignof(GameWorldMp)));
    auto* gfxWorld = static_cast<GfxWorld*>(
        allocateRoot(sizeof(GfxWorld), alignof(GfxWorld)));
    if (!ownedBspName || !ownedBaseName || !entityText || !mapEnts
        || !gameWorld || !gfxWorld)
        return fail("map root metadata allocation failed");
    std::memcpy(entityText, entities.Text().data(), entities.Text().size());
    entityText[entities.Text().size()] = '\0';
    mapEnts->name = ownedBspName;
    mapEnts->entityString = entityText;
    mapEnts->numEntityChars = static_cast<int>(entities.Text().size());
    gameWorld->name = ownedBspName;

    auto* planes = static_cast<cplane_s*>(allocateRoot(
        (planeCount + 1u) * sizeof(cplane_s), alignof(cplane_s)));
    auto* sidePlanes = sideCount ? static_cast<cplane_s*>(allocateRoot(
        sideCount * sizeof(cplane_s), alignof(cplane_s))) : nullptr;
    auto* brushSides = sideCount ? static_cast<cbrushside_t*>(allocateRoot(
        sideCount * sizeof(cbrushside_t), alignof(cbrushside_t))) : nullptr;
    auto* brushes = static_cast<cbrush_t*>(allocateRoot(
        brushCount * sizeof(cbrush_t), alignof(cbrush_t)));
    auto* models = static_cast<cmodel_t*>(allocateRoot(
        submodelCount * sizeof(cmodel_t), alignof(cmodel_t)));
    auto* gfxModels = static_cast<GfxBrushModel*>(allocateRoot(
        submodelCount * sizeof(GfxBrushModel), alignof(GfxBrushModel)));
    auto* nodes = static_cast<cNode_t*>(
        allocateRoot(sizeof(cNode_t), alignof(cNode_t)));
    auto* leafs = static_cast<cLeaf_t*>(
        allocateRoot(sizeof(cLeaf_t), alignof(cLeaf_t)));
    auto* leafBrushNodes = static_cast<cLeafBrushNode_s*>(allocateRoot(
        (submodelCount + 1u) * sizeof(cLeafBrushNode_s),
        alignof(cLeafBrushNode_s)));
    auto* boxBrush = static_cast<cbrush_t*>(
        allocateRoot(sizeof(cbrush_t), alignof(cbrush_t)));
    if (!planes || (sideCount && (!sidePlanes || !brushSides)) || !brushes
        || !models || !gfxModels || !nodes || !leafs || !leafBrushNodes
        || !boxBrush)
        return fail("source collision roots allocation failed");

    for (size_t index = 0; index < planeCount; ++index) {
        const ShipmentCollision::Plane& input = collision.planes_[index];
        cplane_s& output = planes[index];
        std::memcpy(output.normal, input.normal, sizeof(output.normal));
        output.dist = input.dist;
        output.type = input.type;
        output.signbits = input.signbits;
    }
    cplane_s& rootPlane = planes[planeCount];
    rootPlane.normal[0] = 1.0f;
    rootPlane.type = 0;
    nodes[0].plane = &rootPlane;
    nodes[0].children[0] = -1;
    nodes[0].children[1] = -1;

    auto* materials = static_cast<dmaterial_t*>(allocateRoot(
        materialIds.size() * sizeof(dmaterial_t), alignof(dmaterial_t)));
    if (!materials)
        return fail("source collision materials allocation failed");
    for (size_t index = 0; index < materialIds.size(); ++index) {
        std::snprintf(materials[index].material,
                      sizeof(materials[index].material),
                      "vita_surface_%u", materialIds[index]);
    }

    for (size_t sideIndex = 0; sideIndex < sideCount; ++sideIndex) {
        const ShipmentCollision::BrushSide& input = collision.sides_[sideIndex];
        cplane_s& plane = sidePlanes[sideIndex];
        std::memcpy(plane.normal, input.plane.normal, sizeof(plane.normal));
        plane.dist = input.plane.dist;
        plane.type = 3;
        for (uint8_t axis = 0; axis < 3; ++axis) {
            const uint8_t next = static_cast<uint8_t>((axis + 1u) % 3u);
            const uint8_t last = static_cast<uint8_t>((axis + 2u) % 3u);
            if (std::fabs(plane.normal[axis]) == 1.0f
                && plane.normal[next] == 0.0f
                && plane.normal[last] == 0.0f) {
                plane.type = axis;
                break;
            }
        }
        plane.signbits = static_cast<uint8_t>(
            (plane.normal[0] < 0.0f ? 1u : 0u)
            | (plane.normal[1] < 0.0f ? 2u : 0u)
            | (plane.normal[2] < 0.0f ? 4u : 0u));
        cbrushside_t& output = brushSides[sideIndex];
        output.plane = &plane;
        const auto material = std::lower_bound(
            materialIds.begin(), materialIds.end(), input.material);
        output.materialNum = static_cast<uint32_t>(
            material - materialIds.begin());
        materials[output.materialNum].surfaceFlags = input.surfaceFlags;
    }

    uint32_t aggregateWorldContents = 0;
    for (size_t brushIndex = 0; brushIndex < brushCount; ++brushIndex) {
        const ShipmentCollision::Brush& input = collision.brushes_[brushIndex];
        cbrush_t& output = brushes[brushIndex];
        std::memcpy(output.mins, input.mins, sizeof(output.mins));
        std::memcpy(output.maxs, input.maxs, sizeof(output.maxs));
        output.contents = static_cast<int>(input.contents);
        output.numsides = input.sideCount;
        output.sides = input.sideCount ? &brushSides[input.sideOffset] : nullptr;
        for (uint32_t side = 0; side < input.sideCount; ++side) {
            const uint32_t material = output.sides[side].materialNum;
            materials[material].contentFlags |= output.contents;
        }
    }
    for (uint16_t brush : collision.modelBrushes_.front())
        aggregateWorldContents |= collision.brushes_[brush].contents;
    materials[0].contentFlags |= static_cast<int>(aggregateWorldContents);

    auto* leafBrushes = membershipCount ? static_cast<uint16_t*>(allocateRoot(
        membershipCount * sizeof(uint16_t), alignof(uint16_t))) : nullptr;
    if (membershipCount && !leafBrushes)
        return fail("source cmodel membership allocation failed");
    size_t membershipCursor = 0;
    for (size_t modelIndex = 0; modelIndex < submodelCount; ++modelIndex) {
        const ShipmentCollisionSubmodel& input = collision.submodels_[modelIndex];
        cmodel_t& model = models[modelIndex];
        std::memcpy(model.mins, input.mins, sizeof(model.mins));
        std::memcpy(model.maxs, input.maxs, sizeof(model.maxs));
        model.radius = input.radius;
        std::memcpy(model.leaf.mins, input.mins, sizeof(model.leaf.mins));
        std::memcpy(model.leaf.maxs, input.maxs, sizeof(model.leaf.maxs));
        model.leaf.cluster = modelIndex == 0 ? 0 : -1;

        const auto& membership = collision.modelBrushes_[modelIndex];
        cLeafBrushNode_s& leafNode = leafBrushNodes[modelIndex + 1u];
        uint32_t modelContents = 0;
        if (!membership.empty()) {
            std::memcpy(&leafBrushes[membershipCursor], membership.data(),
                        membership.size() * sizeof(uint16_t));
            leafNode.leafBrushCount = static_cast<int16_t>(membership.size());
            leafNode.data.leaf.brushes = &leafBrushes[membershipCursor];
            model.leaf.leafBrushNode = static_cast<int>(modelIndex + 1u);
            for (uint16_t brush : membership)
                modelContents |= collision.brushes_[brush].contents;
            leafNode.contents = static_cast<int>(modelContents);
            membershipCursor += membership.size();
        }
        model.leaf.brushContents = static_cast<int>(modelContents);

        GfxBrushModel& gfxModel = gfxModels[modelIndex];
        std::memcpy(gfxModel.writable.mins, input.mins,
                    sizeof(gfxModel.writable.mins));
        std::memcpy(gfxModel.writable.maxs, input.maxs,
                    sizeof(gfxModel.writable.maxs));
        std::memcpy(gfxModel.bounds[0], input.mins,
                    sizeof(gfxModel.bounds[0]));
        std::memcpy(gfxModel.bounds[1], input.maxs,
                    sizeof(gfxModel.bounds[1]));
    }

    float (*vertices)[3] = nullptr;
    uint16_t* triangleIndices = nullptr;
    uint8_t* triangleWalkable = nullptr;
    CollisionPartition* partitions = nullptr;
    CollisionAabbTree* aabbTrees = nullptr;
    if (vertexCount) {
        vertices = static_cast<float (*)[3]>(allocateRoot(
            vertexCount * sizeof(float[3]), alignof(float)));
    }
    if (triangleCount) {
        triangleIndices = static_cast<uint16_t*>(allocateRoot(
            triangleCount * 3u * sizeof(uint16_t), alignof(uint16_t)));
        triangleWalkable = static_cast<uint8_t*>(allocateRoot(
            (triangleCount * 3u + 7u) / 8u, alignof(uint8_t)));
        partitions = static_cast<CollisionPartition*>(allocateRoot(
            partitionCount * sizeof(CollisionPartition),
            alignof(CollisionPartition)));
        aabbTrees = static_cast<CollisionAabbTree*>(allocateRoot(
            terrainTreeCount * sizeof(CollisionAabbTree),
            alignof(CollisionAabbTree)));
    }
    if ((vertexCount && !vertices)
        || (triangleCount && (!triangleIndices || !triangleWalkable
                              || !partitions || !aabbTrees)))
        return fail("source terrain collision allocation failed");
    for (size_t index = 0; index < vertexCount; ++index)
        std::memcpy(vertices[index], collision.vertices_[index].data(),
                    sizeof(float[3]));
    for (size_t index = 0; index < triangleCount; ++index)
        std::memcpy(&triangleIndices[index * 3u],
                    collision.triangles_[index].data(),
                    sizeof(uint16_t) * 3u);
    if (triangleWalkable)
        std::memset(triangleWalkable, 0xFF,
                    (triangleCount * 3u + 7u) / 8u);
#if KISAK_VITA_CPU_RUNTIME
    BuildTerrainPartitions(collision.vertices_, collision.triangles_, partitions, aabbTrees);
    VitaCpuProfile_Counter("terrain-partition-layout", terrainRootCount, partitionCount);
#else
    for (size_t partitionIndex = 0; partitionIndex < partitionCount;
         ++partitionIndex) {
        const size_t firstTriangle = partitionIndex * 255u;
        const size_t count = std::min<size_t>(
            255u, triangleCount - firstTriangle);
        CollisionPartition& partition = partitions[partitionIndex];
        partition.triCount = static_cast<uint8_t>(count);
        partition.firstTri = static_cast<int>(firstTriangle);
        CollisionAabbTree& tree = aabbTrees[partitionIndex];
        tree.materialIndex = 0;
        tree.u.partitionIndex = static_cast<int>(partitionIndex);
        float minimum[3] = {std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::max()};
        float maximum[3] = {-std::numeric_limits<float>::max(),
                            -std::numeric_limits<float>::max(),
                            -std::numeric_limits<float>::max()};
        for (size_t triangle = firstTriangle;
             triangle < firstTriangle + count; ++triangle) {
            for (uint32_t corner = 0; corner < 3; ++corner) {
                const auto& point = collision.vertices_[
                    collision.triangles_[triangle][corner]];
                for (uint32_t axis = 0; axis < 3; ++axis) {
                    minimum[axis] = std::min(minimum[axis], point[axis]);
                    maximum[axis] = std::max(maximum[axis], point[axis]);
                }
            }
        }
        for (uint32_t axis = 0; axis < 3; ++axis) {
            tree.origin[axis] = (minimum[axis] + maximum[axis]) * 0.5f;
            tree.halfSize[axis] = (maximum[axis] - minimum[axis]) * 0.5f
                + 0.125f;
        }
    }
#endif
    models[0].leaf.firstCollAabbIndex = 0;
    models[0].leaf.collAabbCount = static_cast<uint16_t>(terrainRootCount);
    models[0].leaf.terrainContents = static_cast<int>(aggregateWorldContents);
    leafs[0] = models[0].leaf;
    leafs[0].cluster = 0;

    uint8_t* visibility = static_cast<uint8_t*>(
        allocateRoot(sizeof(uint8_t), alignof(uint8_t)));
    if (!visibility)
        return fail("source PVS root allocation failed");
    *visibility = 0xFFu;
    boxBrush->contents = ~0;

    clipMap_t constructed{};
    constructed.name = ownedBspName;
    constructed.planeCount = static_cast<int>(planeCount);
    constructed.planes = planes;
    constructed.numMaterials = static_cast<uint32_t>(materialIds.size());
    constructed.materials = materials;
    constructed.numBrushSides = static_cast<uint32_t>(sideCount);
    constructed.brushsides = brushSides;
    constructed.numNodes = 1;
    constructed.nodes = nodes;
    constructed.numLeafs = 1;
    constructed.leafs = leafs;
    constructed.leafbrushNodesCount = static_cast<uint32_t>(
        submodelCount + 1u);
    constructed.leafbrushNodes = leafBrushNodes;
    constructed.numLeafBrushes = static_cast<uint32_t>(membershipCount);
    constructed.leafbrushes = leafBrushes;
    constructed.vertCount = static_cast<uint32_t>(vertexCount);
    constructed.verts = vertices;
    constructed.triCount = static_cast<int>(triangleCount);
    constructed.triIndices = triangleIndices;
    constructed.triEdgeIsWalkable = triangleWalkable;
    constructed.partitionCount = static_cast<int>(partitionCount);
    constructed.partitions = partitions;
    constructed.aabbTreeCount = static_cast<int>(terrainTreeCount);
    constructed.aabbTrees = aabbTrees;
    constructed.numSubModels = static_cast<uint32_t>(submodelCount);
    constructed.cmodels = models;
    constructed.numBrushes = static_cast<uint16_t>(brushCount);
    constructed.brushes = brushes;
    constructed.numClusters = 1;
    constructed.clusterBytes = 1;
    constructed.visibility = visibility;
    constructed.vised = 1;
    constructed.mapEnts = mapEnts;
    constructed.box_brush = boxBrush;
    constructed.box_model.leaf.brushContents = ~0;
    constructed.box_model.leaf.cluster = -1;
    constructed.checksum = checksum;

    gfxWorld->name = ownedBspName;
    gfxWorld->baseName = ownedBaseName;
    gfxWorld->modelCount = static_cast<int>(submodelCount);
    gfxWorld->models = gfxModels;
    std::memcpy(gfxWorld->mins, collision.submodels_.front().mins,
                sizeof(gfxWorld->mins));
    std::memcpy(gfxWorld->maxs, collision.submodels_.front().maxs,
                sizeof(gfxWorld->maxs));
    gfxWorld->checksum = checksum;

    cm = constructed;
    std::memset(&comWorld, 0, sizeof(comWorld));
    comWorld.name = ownedBspName;
    comWorld.isInUse = 1;
    mapRootsMounted_ = true;
    if (!RegisterClipMap(bspName, &cm, lifetime)
        || !RegisterComWorld(bspName, &comWorld, lifetime)
        || !RegisterGameWorld(bspName, gameWorld, lifetime)
        || !RegisterMapEnts(bspName, mapEnts, lifetime)
        || !RegisterGfxWorld(bspName, gfxWorld, lifetime)
        || !RegisterGfxWorld("__active__", gfxWorld, lifetime)) {
        std::memset(&cm, 0, sizeof(cm));
        std::memset(&comWorld, 0, sizeof(comWorld));
        mapRootsMounted_ = false;
        return fail("source map roots could not be registered atomically");
    }

#if KISAK_VITA_CPU_RUNTIME
    const auto& worldLeaf=leafBrushNodes[1];
    const bool grouped=worldBrushGroups.Build(brushes,worldLeaf.data.leaf.brushes,
        static_cast<size_t>(std::max<int>(0,worldLeaf.leafBrushCount)));
    VitaCpuProfile_Counter("world-brush-groups",grouped?worldLeaf.leafBrushCount:0,
        grouped?(worldLeaf.leafBrushCount+15)/16:0);
#endif
    allocatingMapRoots_ = false;
    report.ready = true;
    report.submodelCount = static_cast<uint32_t>(submodelCount);
    report.entityBytes = static_cast<uint32_t>(entities.Text().size());
    return report;
}

void* VitaAssetRegistry::Find(uint32_t assetType, const char* name) const {
    if (!name)
        return nullptr;
    const uint32_t hash = HashName(name);
    for (uint32_t index = entryCount_; index > 0; --index) {
        const Entry& entry = entries_[index - 1u];
        if (entry.type == assetType && entry.nameHash == hash
            && NamesEqual(entry.name, name))
            return entry.object;
    }
    return nullptr;
}

void VitaAssetRegistry::AddMissing(VitaSourceClosureReport* report,
                                   uint32_t type, const char* name) const {
    if (!report)
        return;
    const uint32_t index = report->missingCount++;
    if (index >= VitaSourceClosureReport::kMaxMissing)
        return;
    report->missing[index].assetType = type;
    std::strncpy(report->missing[index].name, name ? name : "",
                 sizeof(report->missing[index].name) - 1u);
}

VitaSourceClosureReport VitaAssetRegistry::ValidateOfflineMap(
    const char* mapName) const {
    VitaSourceClosureReport report{};
    uint32_t mapIndex=0;
    if(!VitaFindMapIndex(mapName,&mapIndex)) {
        AddMissing(&report,UINT32_MAX,"original multiplayer map required");
        return report;
    }

    char bspName[96] = {};
    std::snprintf(bspName, sizeof(bspName), "maps/mp/mp_%s.d3dbsp", kVitaMpMaps[mapIndex].name);
    const struct Requirement { uint32_t type; const char* name; } roots[] = {
        {kAssetClipMapPvs, bspName},
        {kAssetComWorld, bspName},
        {kAssetGfxWorld, bspName},
        {kAssetWeapon, "none"},
        {kAssetWeapon, "defaultweapon_mp"},
        {kAssetWeapon, "mp5_mp"},
        {kAssetWeapon, "usp_mp"},
        {kAssetWeapon, "frag_grenade_mp"},
        {kAssetWeapon, "flash_grenade_mp"},
    };
    for (const Requirement& requirement : roots) {
        if (!Find(requirement.type, requirement.name))
            AddMissing(&report, requirement.type, requirement.name);
    }
    report.ready = report.missingCount == 0;
    return report;
}

VitaAssetRegistryStats VitaAssetRegistry::Stats() const {
    VitaAssetRegistryStats result{};
    result.capacityBytes = capacity_ + mapRootCapacity_;
    result.residentBytes = cursor_ + mapRootCursor_;
    result.highWaterBytes = highWater_ + mapRootHighWater_;
    result.assetCount = entryCount_;
#if KISAK_VITA_CPU_RUNTIME
    result.backingBytes=storage_.Reserved()+mapRootStorage_.Reserved();
    result.backingPeakBytes=storage_.PeakReserved()+mapRootStorage_.PeakReserved();
    result.maximumAllocationBytes=std::max(storage_.MaximumRequest(),mapRootStorage_.MaximumRequest());
    result.backingBlocks=storage_.Blocks()+mapRootStorage_.Blocks();
#endif
    return result;
}

void* VitaAssetRegistry::FindThunk(uint32_t type, const char* name,
                                   void* context) {
    auto* registry = static_cast<VitaAssetRegistry*>(context);
    return registry ? registry->Find(type, name) : nullptr;
}

int VitaAssetRegistry::SoundThunk(VitaSourceSoundAction action,
                                  const char* aliasName,
                                  const float* origin,
                                  int localClientNum, void* context) {
    auto* registry = static_cast<VitaAssetRegistry*>(context);
    return registry && registry->services_.sound
        ? registry->services_.sound(action, aliasName, origin, localClientNum,
                                    registry->services_.context)
        : 0;
}

void VitaAssetRegistry::LoadingThunk(const char* stage, void* context) {
    auto* registry = static_cast<VitaAssetRegistry*>(context);
    if (registry && registry->services_.loadingPulse)
        registry->services_.loadingPulse(stage, registry->services_.context);
}

float VitaAssetRegistry::LoadingFractionThunk(void* context) {
    auto* registry = static_cast<VitaAssetRegistry*>(context);
    return registry && registry->services_.loadingFraction
        ? registry->services_.loadingFraction(registry->services_.context) : 0.0f;
}

void VitaAssetRegistry::FogThunk(bool enabled, float start, float density,
                                 uint8_t red, uint8_t green, uint8_t blue,
                                 int startTime, int transitionTime,
                                 void* context) {
    auto* registry = static_cast<VitaAssetRegistry*>(context);
    if (registry && registry->services_.fogChanged) {
        registry->services_.fogChanged(enabled, start, density, red, green,
                                       blue, startTime, transitionTime,
                                       registry->services_.context);
    }
}

void VitaAssetRegistry::WarningThunk(uint32_t warningType, void* context) {
    auto* registry = static_cast<VitaAssetRegistry*>(context);
    if (registry && registry->services_.warning)
        registry->services_.warning(warningType, registry->services_.context);
}

VitaSourcePlatformHooks VitaAssetRegistry::MakePlatformHooks(
    const VitaSourcePlatformHooks& services, uint32_t worldChecksum,
    const char* fastfileNames, const char* fastfileChecksums) {
    services_ = services;
    VitaSourcePlatformHooks hooks{};
    hooks.findAsset = &FindThunk;
    hooks.sound = &SoundThunk;
    hooks.loadingPulse = &LoadingThunk;
    hooks.loadingFraction = services.loadingFraction ? &LoadingFractionThunk : nullptr;
    hooks.fogChanged = &FogThunk;
    hooks.warning = &WarningThunk;
    hooks.referencedFastfileNames = fastfileNames ? fastfileNames : "";
    hooks.referencedFastfileChecksums = fastfileChecksums
        ? fastfileChecksums : "";
    hooks.worldChecksum = worldChecksum;
    hooks.context = this;
    return hooks;
}

} // namespace kisak::vita
