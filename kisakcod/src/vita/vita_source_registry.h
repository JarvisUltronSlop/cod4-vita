#pragma once

#include <cstddef>
#include <cstdint>

#include <vita/vita_source_platform.h>
#if KISAK_VITA_CPU_RUNTIME
#include <vita/vita_registry_storage.h>
#endif

struct clipMap_t;
struct ComWorld;
struct Font_s;
struct FxEffectDef;
struct FxImpactTable;
struct GameWorldMp;
struct GfxImage;
struct GfxWorld;
struct MapEnts;
struct Material;
struct MenuList;
struct RawFile;
struct StringTable;
struct WeaponDef;
struct XAnimParts;
struct XModel;
struct menuDef_t;
struct snd_alias_list_t;

namespace kisak::vita {

struct VitaFontRecord;
class VitaFontBundle;
struct VitaImageRecord;
struct VitaAnimationClip;
struct VitaMaterialRecord;
struct VitaModelRecord;
struct VitaStringTableRecord;
struct VitaWeaponRecordV5;
class VitaImageBundle;
class VitaAnimationBundle;
class VitaEffectBundle;
class VitaImpactFxBundle;
class VitaMaterialBundle;
class VitaModelBundle;
class VitaStringTableBundle;
class VitaSoundBundle;
class ShipmentWeaponTable;
class ShipmentCollision;
class ShipmentMapEntities;

enum class VitaAssetLifetime : uint8_t {
    Frontend = 0,
    Map,
    Transient,
};

struct VitaAssetRegistryStats {
    size_t capacityBytes = 0;
    size_t residentBytes = 0;
    size_t highWaterBytes = 0;
    uint32_t assetCount = 0;
#if KISAK_VITA_CPU_RUNTIME
    size_t backingBytes = 0, backingPeakBytes = 0, maximumAllocationBytes = 0, backingBlocks = 0;
#endif
};

struct VitaMissingSourceAsset {
    uint32_t assetType = 0;
    char name[96] = {};
};

struct VitaSourceClosureReport {
    static constexpr uint32_t kMaxMissing = 16;
    bool ready = false;
    uint32_t missingCount = 0;
    VitaMissingSourceAsset missing[kMaxMissing] = {};
};

struct VitaWeaponMountReport {
    static constexpr uint32_t kMaxMissing = 16;
    bool ready = false;
    uint32_t mountedCount = 0;
    uint32_t missingCount = 0;
    char failedWeapon[64] = {};
    VitaMissingSourceAsset missing[kMaxMissing] = {};
};

struct VitaMapRootReport {
    bool ready = false;
    char error[160] = {};
    uint32_t submodelCount = 0;
    uint32_t entityBytes = 0;
};

// Transaction result for one typed source-asset family. Constructors stop at
// the first invalid record and rewind the registry arena; a caller therefore
// never observes a partially mounted image/material/model generation.
struct VitaSourceAssetMountReport {
    static constexpr uint32_t kMaxMissing = 16;
    bool ready = false;
    uint32_t assetType = 0;
    uint32_t sourceCount = 0;
    uint32_t mountedCount = 0;
    uint32_t missingCount = 0;
    char failedAsset[96] = {};
    VitaMissingSourceAsset missing[kMaxMissing] = {};
};

struct VitaSourceModelClosureReport {
    bool ready = false;
    VitaSourceAssetMountReport images{};
    VitaSourceAssetMountReport materials{};
    VitaSourceAssetMountReport models{};
};

// Arena-backed, source-facing asset registry. Pointer-free KVAP data is
// decoded by typed constructors into this arena; only those reconstructed
// objects are exposed through DB_FindXAssetHeader. Source string references
// are released before the arena containing a complete map is reset.
class VitaAssetRegistry {
public:
    static constexpr uint32_t kMaxAssets = 8192;
    static constexpr size_t kMaximumArenaBytes = 96u * 1024u * 1024u;
    static constexpr size_t kMaximumMapRootArenaBytes = 24u * 1024u * 1024u;

    // Asset constructors retain genuine SL references, just like the source
    // object loaders. Release them after DObj/XAnim users stop and before
    // SL_Shutdown; the typed arena may be freed later by its map owner.
    void ReleaseScriptStrings();

    VitaAssetRegistry() = default;
    ~VitaAssetRegistry();
    VitaAssetRegistry(const VitaAssetRegistry&) = delete;
    VitaAssetRegistry& operator=(const VitaAssetRegistry&) = delete;

    bool Initialize(size_t capacityBytes);
    void Destroy();
    void Reset(VitaAssetLifetime lifetime);

    void* Allocate(size_t bytes, size_t alignment);
    char* CopyString(const char* text);

    bool RegisterAnimation(const char* name, XAnimParts* asset,
                           VitaAssetLifetime lifetime);
    bool RegisterModel(const char* name, XModel* asset,
                       VitaAssetLifetime lifetime);
    bool RegisterMaterial(const char* name, Material* asset,
                          VitaAssetLifetime lifetime);
    bool RegisterImage(const char* name, GfxImage* asset,
                       VitaAssetLifetime lifetime);
    bool RegisterSound(const char* name, snd_alias_list_t* asset,
                       VitaAssetLifetime lifetime);
    bool RegisterClipMap(const char* name, clipMap_t* asset,
                         VitaAssetLifetime lifetime);
    bool RegisterComWorld(const char* name, ComWorld* asset,
                          VitaAssetLifetime lifetime);
    bool RegisterGameWorld(const char* name, GameWorldMp* asset,
                           VitaAssetLifetime lifetime);
    bool RegisterMapEnts(const char* name, MapEnts* asset,
                         VitaAssetLifetime lifetime);
    bool RegisterGfxWorld(const char* name, GfxWorld* asset,
                          VitaAssetLifetime lifetime);
    bool RegisterFont(const char* name, Font_s* asset,
                      VitaAssetLifetime lifetime);
    bool RegisterMenuList(const char* name, MenuList* asset,
                          VitaAssetLifetime lifetime);
    bool RegisterMenu(const char* name, menuDef_t* asset,
                      VitaAssetLifetime lifetime);
    bool RegisterWeapon(const char* name, WeaponDef* asset,
                        VitaAssetLifetime lifetime);
    bool RegisterEffect(const char* name, const FxEffectDef* asset,
                        VitaAssetLifetime lifetime);
    bool RegisterImpactEffects(const char* name, FxImpactTable* asset,
                               VitaAssetLifetime lifetime);
    bool RegisterRawFile(const char* name, RawFile* asset,
                         VitaAssetLifetime lifetime);
    bool RegisterStringTable(const char* name, StringTable* asset,
                             VitaAssetLifetime lifetime);

    // Small source structures that are safe to construct completely from the
    // corresponding pointer-free records.
    snd_alias_list_t* RehydrateSoundAlias(const char* aliasName,
                                          int sequence,
                                          VitaAssetLifetime lifetime);
    VitaSourceAssetMountReport RehydrateSourceSounds(const VitaSoundBundle& source);
    Font_s* RehydrateFont(const VitaFontRecord& source,
                          Material* material, Material* glowMaterial,
                          VitaAssetLifetime lifetime);
    VitaSourceAssetMountReport RehydrateSourceFonts(const VitaFontBundle& source,
        VitaAssetLifetime lifetime = VitaAssetLifetime::Map);
    GfxImage* RehydrateImage(const VitaImageRecord& source,
                             VitaAssetLifetime lifetime,
                             bool retainPixelPayload = true);
    Material* RehydrateMaterial(const VitaMaterialRecord& source,
                                VitaAssetLifetime lifetime,
                                VitaSourceAssetMountReport* report = nullptr);
    XModel* RehydrateModel(const VitaModelRecord& source,
                           VitaAssetLifetime lifetime,
                           VitaSourceAssetMountReport* report = nullptr,
                           bool borrowMapIndices = false);
    XAnimParts* RehydrateAnimation(const VitaAnimationClip& source,
                                   VitaAssetLifetime lifetime,
                                   VitaSourceAssetMountReport* report = nullptr,
                                   bool borrowCompressedPayload = false);
    VitaSourceAssetMountReport RehydrateSourceImages(
        const VitaImageBundle& source,
        VitaAssetLifetime lifetime = VitaAssetLifetime::Map,
        bool retainPixelPayload = true);
    VitaSourceAssetMountReport RehydrateSourceMaterials(
        const VitaMaterialBundle& source,
        VitaAssetLifetime lifetime = VitaAssetLifetime::Map);
    VitaSourceAssetMountReport RehydrateSourceModels(
        const VitaModelBundle& source,
        VitaAssetLifetime lifetime = VitaAssetLifetime::Map, bool borrowMapIndices = false);
    VitaSourceAssetMountReport RehydrateSourceAnimations(
        const VitaAnimationBundle& source,
        VitaAssetLifetime lifetime = VitaAssetLifetime::Map,
        bool borrowCompressedPayload = false);
    VitaSourceAssetMountReport RehydrateSourceEffects(
        const VitaEffectBundle& source, const VitaImpactFxBundle& impacts,
        VitaAssetLifetime lifetime = VitaAssetLifetime::Map);
    StringTable* RehydrateStringTable(
        const VitaStringTableRecord& source,
        VitaAssetLifetime lifetime = VitaAssetLifetime::Map);
    VitaSourceAssetMountReport RehydrateSourceStringTables(
        const VitaStringTableBundle& source,
        VitaAssetLifetime lifetime = VitaAssetLifetime::Map);
    VitaSourceModelClosureReport RehydrateSourceModelClosure(
        const VitaImageBundle& images, const VitaMaterialBundle& materials,
        const VitaModelBundle& models,
        VitaAssetLifetime lifetime = VitaAssetLifetime::Map,
        bool retainImagePixelPayload = true, bool borrowMapIndices = false);
    WeaponDef* RehydrateWeapon(const VitaWeaponRecordV5& source,
                               VitaAssetLifetime lifetime,
                               VitaWeaponMountReport* report = nullptr);
    VitaWeaponMountReport RehydrateSourceWeapons(
        const ShipmentWeaponTable& source,
        VitaAssetLifetime lifetime = VitaAssetLifetime::Map);
    VitaMapRootReport RehydrateMapRoots(
        const char* mapName, const ShipmentCollision& collision,
        const ShipmentMapEntities& entities, uint32_t checksum,
        VitaAssetLifetime lifetime = VitaAssetLifetime::Map);

    void* Find(uint32_t assetType, const char* name) const;
    VitaSourceClosureReport ValidateOfflineMap(const char* mapName) const;
    VitaAssetRegistryStats Stats() const;
#if KISAK_VITA_CPU_RUNTIME
    bool AllocationLimitReached() const {
        return storage_.LastFailure()==VitaRegistryStorage::Failure::LogicalCapacity
            || mapRootStorage_.LastFailure()==VitaRegistryStorage::Failure::LogicalCapacity;
    }
#endif

    // Wrap non-registry platform services while making this registry the
    // sole DB lookup context. The returned hooks remain valid while `this`
    // and `services`' referenced context remain alive.
    VitaSourcePlatformHooks MakePlatformHooks(
        const VitaSourcePlatformHooks& services,
        uint32_t worldChecksum,
        const char* fastfileNames,
        const char* fastfileChecksums);

private:
#if KISAK_VITA_CPU_TEST
    friend struct VitaRegistryTestAccess;
#endif
    struct ScriptStringRef {
        ScriptStringRef* previous = nullptr;
        uint16_t value = 0;
    };
    uint32_t InternScriptString(const char* text, uint32_t bytes, int type);
    void ReleaseScriptStringsTo(ScriptStringRef* checkpoint);
    ScriptStringRef* scriptStringRefs_ = nullptr;

    struct Entry {
        uint32_t type = 0;
        uint32_t nameHash = 0;
        const char* name = nullptr;
        void* object = nullptr;
        VitaAssetLifetime lifetime = VitaAssetLifetime::Map;
    };

    bool Register(uint32_t type, const char* name, void* object,
                  VitaAssetLifetime lifetime, bool allowEmptyName = false);
    static uint32_t HashName(const char* name);
    static bool NamesEqual(const char* left, const char* right);
    void AddMissing(VitaSourceClosureReport* report, uint32_t type,
                    const char* name) const;

    static void* FindThunk(uint32_t type, const char* name, void* context);
    static int SoundThunk(VitaSourceSoundAction action, const char* aliasName,
                          const float* origin, int localClientNum,
                          void* context);
    static void LoadingThunk(const char* stage, void* context);
    static float LoadingFractionThunk(void* context);
    static void FogThunk(bool enabled, float start, float density,
                         uint8_t red, uint8_t green, uint8_t blue,
                         int startTime, int transitionTime, void* context);
    static void WarningThunk(uint32_t warningType, void* context);
    uint8_t* arena_ = nullptr;
#if KISAK_VITA_CPU_RUNTIME
    VitaRegistryStorage storage_,mapRootStorage_;
#endif
    size_t capacity_ = 0;
    size_t cursor_ = 0;
    size_t highWater_ = 0;
    // Source clipMap/GfxWorld roots are reconstructed only after the typed
    // asset closure is mounted.  Keep their exact-size allocation separate
    // from the 16 MiB asset arena: Crossfire's collision tables are larger
    // than Shipment's, while reserving a worst-case monolithic arena starves
    // the source script/parse hunks on a 178 MiB Vita process.
    uint8_t* mapRootArena_ = nullptr;
    size_t mapRootCapacity_ = 0;
    size_t mapRootCursor_ = 0;
    size_t mapRootHighWater_ = 0;
    bool allocatingMapRoots_ = false;
    // Included sound variants/loaded samples take the source closure beyond
    // 4096 identities. Charge the bounded index to the same arena as assets,
    // instead of hiding it in the application's main-thread stack.
    Entry* entries_ = nullptr;
    uint32_t entryCount_ = 0;
    bool mapRootsMounted_ = false;
    VitaSourcePlatformHooks services_{};
    const VitaSoundBundle* soundCatalog_ = nullptr;
};

} // namespace kisak::vita
