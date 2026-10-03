#include <universal/q_shared.h>
#include <vita/vita_slice_capabilities.h>
#include <vita/vita_model_format.h>
#include "database.h"

#include <qcommon/files.h>
#include <qcommon/mem_track.h>

#include <xanim/xmodel.h>
#include <xanim/xanim_calc.h>
#include <win32/win_net.h>
#include <qcommon/threads.h>
#include <qcommon/com_bsp.h>
#include <gfx_d3d/r_init.h>
#include <win32/win_local.h>
#include <gfx_d3d/rb_uploadshaders.h>
#include <gfx_d3d/r_image.h>
#include <universal/com_files.h>
#include <game/game_public.h>
#ifdef KISAK_MP
#include <game_mp/g_utils_mp.h>
#include <server_mp/server_mp.h>
#endif
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/fxprimitives.h>
#include <stringed/stringed_hooks.h>
#include <qcommon/cmd.h>
#include <universal/physicalmemory.h>
#include <gfx_d3d/rb_shade.h>
#include <gfx_d3d/r_staticmodelcache.h>
#include <gfx_d3d/r_font.h>
#include <win32/win_localize.h>
#include <universal/profile.h>
#include <ui/ui_shared.h>
#include <sound/snd_public.h>
#include <universal/com_sndalias.h>
#include <vita/vita_animation_record.h>
#include <vita/vita_primary_lights.h>
#include <vita/vita_weapon_record.h>
#include <script/scr_animtree.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <setjmp.h>
#include <game/g_bsp.h>
#include <cgame/cg_local.h>

GfxWorld s_world;
MaterialGlobals materialGlobals;
ImgGlobals imageGlobals;
r_globals_t rg{ 0 };

struct DBReorderAssetEntry // sizeof=0x10
{                                       // ...
    uint32_t sequence;
    int32_t type;
    const char *typeString;
    const char *assetName;
};

#define POOLSIZE_XMODELPIECES   64
#define POOLSIZE_PHYSPRESET     64
#define POOLSIZE_XANIMPARTS     4096
#define POOLSIZE_XMODEL         1000
#define POOLSIZE_MATERIAL       2048
#define POOLSIZE_TECHNIQUE_SET  1024 // 512 on SP (XBox?)
#define POOLSIZE_IMAGE          2400
#define POOLSIZE_SOUND          16'000
#define POOLSIZE_SOUND_CURVE    64
#define POOLSIZE_LOADED_SOUND   1200
#define POOLSIZE_CLIPMAP        1
#define POOLSIZE_CLIPMAP_PVS    1
#define POOLSIZE_COMWORLD       1
#define POOLSIZE_GAMEWORLD_SP   1
#define POOLSIZE_GAMEWORLD_MP   1
#define POOLSIZE_MAP_ENTS       2
#define POOLSIZE_GFXWORLD       1
#define POOLSIZE_LIGHT_DEF      32
#define POOLSIZE_UI_MAP         0
#define POOLSIZE_FONT           16
#define POOLSIZE_MENULIST       128
#define POOLSIZE_MENU           640 // 512 on SP
#define POOLSIZE_LOCALIZE_ENTRY 6144
#define POOLSIZE_WEAPON         128
#define POOLSIZE_SNDDRIVER_GLOBALS 1
#define POOLSIZE_FX             400
#define POOLSIZE_IMPACT_FX      4
#define POOLSIZE_AITYPE         0
#define POOLSIZE_MPTYPE         0
#define POOLSIZE_CHARACTER      0
#define POOLSIZE_XMODELALIAS    0
#define POOLSIZE_RAWFILE        1024
#define POOLSIZE_STRINGTABLE    50

int32_t g_poolSize[ASSET_TYPE_COUNT] =
{
    POOLSIZE_XMODELPIECES,
    POOLSIZE_PHYSPRESET,
    POOLSIZE_XANIMPARTS,
    POOLSIZE_XMODEL,
    POOLSIZE_MATERIAL,
    POOLSIZE_TECHNIQUE_SET,
    POOLSIZE_IMAGE,
    POOLSIZE_SOUND,
    POOLSIZE_SOUND_CURVE,
    POOLSIZE_LOADED_SOUND,
    POOLSIZE_CLIPMAP,
    POOLSIZE_CLIPMAP_PVS,
    POOLSIZE_COMWORLD,
    POOLSIZE_GAMEWORLD_SP,
    POOLSIZE_GAMEWORLD_MP,
    POOLSIZE_MAP_ENTS,
    POOLSIZE_GFXWORLD,
    POOLSIZE_LIGHT_DEF,
    POOLSIZE_UI_MAP,
    POOLSIZE_FONT,
    POOLSIZE_MENULIST,
    POOLSIZE_MENU,
    POOLSIZE_LOCALIZE_ENTRY,
    POOLSIZE_WEAPON,
    POOLSIZE_SNDDRIVER_GLOBALS,
    POOLSIZE_FX,
    POOLSIZE_IMPACT_FX,
    POOLSIZE_AITYPE,
    POOLSIZE_MPTYPE,
    POOLSIZE_CHARACTER,
    POOLSIZE_XMODELALIAS,
    POOLSIZE_RAWFILE,
    POOLSIZE_STRINGTABLE,
}; // idb

bool g_archiveBuf;

// --- file-local forward declarations (moved out of database.h) ---
static void __cdecl DB_InitSingleton(void *pool, int32_t size);
static void __cdecl DB_RemoveClipMap(XAssetHeader ass);
static void __cdecl DB_RemoveComWorld(XAssetHeader ass);
static void __cdecl DB_RemoveGfxWorld(XAssetHeader ass);
static void __cdecl DB_DynamicCloneMenu(XAssetHeader from, XAssetHeader to, int32_t swag = 0);
static void __cdecl DB_RemoveWindowFocus(windowDef_t *window);
static XAssetHeader __cdecl DB_AllocMaterial(void *arg);
static void __cdecl DB_FreeMaterial(void *pool, XAssetHeader header);
static void __cdecl DB_Sleep(uint32_t msec);
static void __cdecl DB_LogMissingAsset(XAssetType type, const char *name);
static void __cdecl DB_RegisteredReorderAsset(int32_t type, const char *assetName, XAssetEntry *assetEntry);
static XAssetEntryPoolEntry *__cdecl DB_FindXAssetEntry(XAssetType type, const char *name);
static uint32_t __cdecl DB_HashForName(const char *name, XAssetType type);
static XAssetEntry *__cdecl DB_CreateDefaultEntry(XAssetType type, char *name);
static XAssetEntryPoolEntry *__cdecl DB_AllocXAssetEntry(XAssetType type, uint8_t zoneIndex);
static XAssetHeader __cdecl DB_AllocXAssetHeader(XAssetType type);
static void __cdecl DB_PrintAssetName(XAssetHeader header, int32_t *data);
static void __cdecl DB_CloneXAssetInternal(const XAsset *from, XAsset *to);
static XAssetHeader __cdecl DB_FindXAssetDefaultHeaderInternal(XAssetType type);
static void __cdecl PrintWaitedError(XAssetType type, const char *name, int32_t waitedMsec);
static bool __cdecl DB_GetInitializing();
static XAssetHeader __cdecl DB_AddXAsset(XAssetType type, XAssetHeader header);
static XAssetEntryPoolEntry *__cdecl DB_LinkXAssetEntry(XAssetEntryPoolEntry *newEntry, int32_t allowOverride);
static void __cdecl DB_FreeXAssetEntry(XAssetEntryPoolEntry *assetEntry);
static void __cdecl DB_FreeXAssetHeader(XAssetType type, XAssetHeader header);
static void __cdecl DB_CloneXAssetEntry(const XAssetEntry *from, XAssetEntry *to);
static void __cdecl DB_DynamicCloneXAsset(XAssetHeader from, XAssetHeader to, XAssetType type, int32_t fromDefault);
static void __cdecl DB_DelayedCloneXAsset(XAssetEntry *newEntry);
static bool __cdecl DB_OverrideAsset(uint32_t newZoneIndex, uint32_t existingZoneIndex);
static void __cdecl DB_GetXAsset(XAssetType type, XAssetHeader header);
static void DB_PostLoadXZone();
static void DB_Init();
static void __cdecl DB_InitPoolHeader(XAssetType type);
static void __cdecl DB_LoadXZone(XZoneInfo *zoneInfo, uint32_t zoneCount);
static void __cdecl DB_LoadZone_f();
static void __cdecl DB_DumpVitaAssets_f();
static void __cdecl DB_DumpVitaGeometry_f();
static bool DB_DumpVitaEffects(int file);
static bool DB_DumpVitaImpactEffects(int file);
static void __cdecl  DB_Thread(uint32_t threadContext);
static void DB_TryLoadXFile();
static int32_t __cdecl DB_TryLoadXFileInternal(char *zoneName, int32_t zoneFlags);
static void __cdecl DB_BuildOSPath(const char *zoneName, uint32_t size, char *filename);
static int32_t __cdecl DB_GetZoneAllocType(int32_t zoneFlags);
static void __cdecl DB_UnloadXZone(uint32_t zoneIndex, bool createDefault);
static void __cdecl DB_RemoveXAsset(XAsset *asset);
static void __cdecl DB_UnloadXZoneMemory(XZone *zone);
static void DB_FreeDefaultEntries();
static void __cdecl DB_UnloadXAssetsMemoryForZone(int32_t zoneFreeFlags, int32_t zoneFreeBit);
static void __cdecl DB_UnloadXAssetsMemory(XZone *zone, int32_t sortedIndex);
static void __cdecl DB_ReplaceXAsset(XAssetType type, const char *original, const char *replacement);
static void __cdecl DB_CloneXAsset(const XAsset *from, XAsset *to);
static void DB_SyncExternalAssets();
static void DB_ArchiveAssets();
static void DB_FreeUnusedResources();
static void DB_ExternalInitAssets();
static void DB_UnarchiveAssets();
static int32_t __cdecl DB_GetAllXAssetOfType_LoadObj(XAssetType type, XAssetHeader* assets, int32_t maxCount);
static void __cdecl DB_EnumXAssets_LoadObj(XAssetType type, void(* func)(void*, void*), void* inData);

static XAssetHeader __cdecl node1_(void *pool)
{
    return (XAssetHeader)pool;
}

static XAssetHeader __cdecl DB_AllocXAsset_StringTable_(void *arg)
{
    XAssetHeader *pool = (XAssetHeader*)arg;
    XAssetHeader header; // [esp+4h] [ebp-8h]

    if (pool->xmodelPieces)
    {
        header.xmodelPieces = pool->xmodelPieces;
        pool->xmodelPieces = (XModelPieces *)pool->xmodelPieces->name;
    }
    else
    {
        header.xmodelPieces = 0;
    }
    return header;
}


XAssetHeader(__cdecl *DB_AllocXAssetHeaderHandler[ASSET_TYPE_COUNT])(void *) =
{
  &DB_AllocXAsset_StringTable_,
  &DB_AllocXAsset_StringTable_,
  &DB_AllocXAsset_StringTable_,
  &DB_AllocXAsset_StringTable_,
  &DB_AllocMaterial,
  &DB_AllocXAsset_StringTable_,
  &DB_AllocXAsset_StringTable_,
  &DB_AllocXAsset_StringTable_,
  &DB_AllocXAsset_StringTable_,
  &DB_AllocXAsset_StringTable_,
  &node1_,
  &node1_,
  &node1_,
  &node1_,
  &node1_,
  &DB_AllocXAsset_StringTable_,
  &node1_,
  &DB_AllocXAsset_StringTable_,
  NULL,
  &DB_AllocXAsset_StringTable_,
  &DB_AllocXAsset_StringTable_,
  &DB_AllocXAsset_StringTable_,
  &DB_AllocXAsset_StringTable_,
  &DB_AllocXAsset_StringTable_,
  NULL,
  &DB_AllocXAsset_StringTable_,
  &DB_AllocXAsset_StringTable_,
  NULL,
  NULL,
  NULL,
  NULL,
  &DB_AllocXAsset_StringTable_,
  &DB_AllocXAsset_StringTable_
}; // idb

void __cdecl DB_FreeXAssetHeader_StringTable_(void *arg, XAssetHeader header)
{
    XAssetPoolEntry<StringTable> **pool = (XAssetPoolEntry<StringTable> **)arg;
    XAssetPoolEntry<StringTable> *oldFreeHead; // [esp+8h] [ebp-4h]

    oldFreeHead = *pool;
    *pool = (XAssetPoolEntry<StringTable> *)header.xmodelPieces;
    header.xmodelPieces->name = (const char *)oldFreeHead;
}

void NULLSUB(void *crap, XAssetHeader head)
{
}

void(__cdecl *DB_FreeXAssetHeaderHandler[ASSET_TYPE_COUNT])(void *, XAssetHeader) =
{
  DB_FreeXAssetHeader_StringTable_,
  DB_FreeXAssetHeader_StringTable_,
  DB_FreeXAssetHeader_StringTable_,
  DB_FreeXAssetHeader_StringTable_,
  DB_FreeMaterial,
  DB_FreeXAssetHeader_StringTable_,
  DB_FreeXAssetHeader_StringTable_,
  DB_FreeXAssetHeader_StringTable_,
  DB_FreeXAssetHeader_StringTable_,
  DB_FreeXAssetHeader_StringTable_,
  NULLSUB,
  NULLSUB,
  NULLSUB,
  NULLSUB,
  NULLSUB,
  DB_FreeXAssetHeader_StringTable_,
  NULLSUB,
  DB_FreeXAssetHeader_StringTable_,
  NULL,
  DB_FreeXAssetHeader_StringTable_,
  DB_FreeXAssetHeader_StringTable_,
  DB_FreeXAssetHeader_StringTable_,
  DB_FreeXAssetHeader_StringTable_,
  DB_FreeXAssetHeader_StringTable_,
  NULL,
  DB_FreeXAssetHeader_StringTable_,
  DB_FreeXAssetHeader_StringTable_,
  NULL,
  NULL,
  NULL,
  NULL,
  DB_FreeXAssetHeader_StringTable_,
  DB_FreeXAssetHeader_StringTable_
}; // idb

const char *g_defaultAssetName[ASSET_TYPE_COUNT] =
{
  "",
  "default",
  "void",
  "void",
  "$default",
  "default",
  "$white",
  "null",
  "default",
  "null.wav",
  "",
  "",
  "",
  "",
  "",
  "",
  "",
  "light_dynamic",
  "",
  "fonts/consolefont",
  "ui/default.menu",
  "default_menu",
  "CGAME_UNKNOWN",
#ifdef KISAK_MP
  "defaultweapon_mp",
#elif KISAK_SP
  "defaultweapon",
#endif
  "",
  "misc/missing_fx",
  "default",
  "",
  "",
  "",
  "",
  "",
  "mp/defaultStringTable.csv"
}; // idb

const char *g_assetNames[ASSET_TYPE_COUNT] = // SP/MP same
{
  "xmodelpieces",
  "physpreset",
  "xanim",
  "xmodel",
  "material",
  "techset",
  "image",
  "sound",
  "sndcurve",
  "loaded_sound",
  "col_map_sp",
  "col_map_mp",
  "com_map",
  "game_map_sp",
  "game_map_mp",
  "map_ents",
  "gfx_map",
  "lightdef",
  "ui_map",
  "font",
  "menufile",
  "menu",
  "localize",
  "weapon",
  "snddriverglobals",
  "fx",
  "impactfx",
  "aitype",
  "mptype",
  "character",
  "xmodelalias",
  "rawfile",
  "stringtable"
};

struct XZoneInfoInternal // sizeof=0x44
{                                       // ...
    char name[64];
    int32_t flags;                          // ...
};

struct $CDDDFFEA12416D380697EB22F7449911 // sizeof=0x8011C
{                                       // ...
    FastCriticalSection critSect;
    char zoneName[256];                 // ...
    bool alreadyFinished;               // ...
    bool loadedSound;                   // ...
    bool loadedLocalization;            // ...
    // padding byte
    DBReorderAssetEntry *lastEntry;     // ...
    uint32_t sequence;              // ...
    uint32_t sequenceForIncludes;   // ...
    uint32_t entryCount;            // ...
    DBReorderAssetEntry entries[32768]; // ...
};

$CDDDFFEA12416D380697EB22F7449911 s_dbReorder;
int32_t g_missingAssetFile;
//int32_t marker_db_registry   828e570c     db_registry.obj
//uint32_t volatile g_mainThreadBlocked  829f278c     db_registry.obj

uint32_t volatile g_mainThreadBlocked;
XAssetEntryPoolEntry *g_freeAssetEntryHead;

uint16_t db_hashTable[32768];
XAssetEntry *g_copyInfo[0x800];
uint32_t g_copyInfoCount;
XZone g_zones[ASSET_TYPE_COUNT]{ 0 };
uint8_t g_zoneHandles[32];
char g_zoneNameList[2080];
XAssetPool<XModelPieces, POOLSIZE_XMODELPIECES> g_XModelPiecesPool;
XAssetPool<PhysPreset, POOLSIZE_PHYSPRESET> g_PhysPresetPool;
XAssetPool<XAnimParts, POOLSIZE_XANIMPARTS> g_XAnimPartsPool;
XAssetPool<XModel, POOLSIZE_XMODEL> g_XModelPool;
XAssetPool<Material, POOLSIZE_MATERIAL> g_MaterialPool;
XAssetPool<MaterialTechniqueSet, POOLSIZE_TECHNIQUE_SET> g_MaterialTechniqueSetPool;
XAssetPool<GfxImage, POOLSIZE_IMAGE> g_GfxImagePool;
XAssetPool<snd_alias_list_t, POOLSIZE_SOUND> g_SoundPool;
XAssetPool<SndCurve, POOLSIZE_SOUND_CURVE> g_SndCurvePool;
XAssetPool<LoadedSound, POOLSIZE_LOADED_SOUND> g_LoadedSoundPool;
XAssetPool<MapEnts, POOLSIZE_MAP_ENTS> g_MapEntsPool;
XAssetPool<GfxLightDef, POOLSIZE_LIGHT_DEF> g_GfxLightDefPool;
XAssetPool<Font_s, POOLSIZE_FONT> g_FontPool;
XAssetPool<MenuList, POOLSIZE_MENULIST> g_MenuListPool;
XAssetPool<menuDef_t, POOLSIZE_MENU> g_MenuPool;
XAssetPool<LocalizeEntry, POOLSIZE_LOCALIZE_ENTRY> g_LocalizeEntryPool;
XAssetPool<WeaponDef, POOLSIZE_WEAPON> g_WeaponDefPool;
XAssetPool<FxEffectDef, POOLSIZE_FX> g_FxEffectDefPool;
XAssetPool<FxImpactTable, POOLSIZE_IMPACT_FX> g_FxImpactTablePool;
XAssetPool<RawFile, POOLSIZE_RAWFILE> g_RawFilePool;
XAssetPool<StringTable, POOLSIZE_STRINGTABLE> g_StringTablePool;

XAssetEntryPoolEntry g_assetEntryPool[32768];
uint8_t g_fileBuf[524288];

fileData_s *com_fileDataHashTable[1024];

FastCriticalSection db_hashCritSect;

bool g_zoneInited;
int32_t g_zoneCount;

bool g_isRecoveringLostDevice;
bool g_mayRecoverLostAssets;
volatile bool g_loadingZone;
volatile uint32_t g_zoneInfoCount;
bool g_initializing;

char g_debugZoneName[64];
uint32_t g_zoneAllocType;
uint32_t g_zoneIndex;
uint32_t _S1;
const dvar_t *zone_reorder;
volatile uint32_t g_loadingAssets;
XZoneInfoInternal g_zoneInfo[8];

char *__cdecl DB_ReferencedFFChecksums()
{
    int32_t v0; // kr00_4
    int32_t i; // [esp+10h] [ebp-20h]
    char zoneSizeStr[16]; // [esp+1Ch] [ebp-14h] BYREF

    v0 = strlen("localized_");
    g_zoneNameList[0] = 0;
    for (i = 0; i < 32; ++i)
    {
        if (g_zones[i].name[0] && I_strncmp(g_zones[i].name, "localized_", v0))
        {
            if (g_zoneNameList[0])
                I_strncat(g_zoneNameList, 2080, " ");
            //itoa(g_zones[i].fileSize, zoneSizeStr, 0xAu);
            _itoa(g_zones[i].fileSize, zoneSizeStr, 0xAu);
            I_strncat(g_zoneNameList, 2080, zoneSizeStr);
        }
    }
    return g_zoneNameList;
}

char *__cdecl DB_ReferencedFFNameList()
{
    int32_t v0; // kr00_4
    int32_t i; // [esp+10h] [ebp-Ch]

    v0 = strlen("localized_");
    g_zoneNameList[0] = 0;
    for (i = 0; i < 32; ++i)
    {
        if (g_zones[i].name[0] && I_strncmp(g_zones[i].name, "localized_", v0))
        {
            if (g_zoneNameList[0])
                I_strncat(g_zoneNameList, 2080, " ");
            if (g_zones[i].modZone)
            {
                I_strncat(g_zoneNameList, 2080, (const char*)fs_gameDirVar->current.integer);
                I_strncat(g_zoneNameList, 2080, "/");
            }
            I_strncat(g_zoneNameList, 2080, g_zones[i].name);
        }
    }
    return g_zoneNameList;
}

void __cdecl Hunk_OverrideDataForFile(int32_t type, const char *name, void *data)
{
    fileData_s *searchFileData; // [esp+4h] [ebp-4h]

    if (!Sys_IsMainThread())
        MyAssertHandler(".\\universal\\com_memory.cpp", 1539, 0, "%s", "Sys_IsMainThread()");
    for (searchFileData = com_fileDataHashTable[FS_HashFileName(name, 1024)];
        searchFileData;
        searchFileData = searchFileData->next)
    {
        if (searchFileData->type == type && !I_stricmp(searchFileData->name, name))
        {
            searchFileData->data = data;
            return;
        }
    }
    if (!alwaysfails)
        MyAssertHandler(".\\universal\\com_memory.cpp", 1554, 0, "Hunk_OverrideDataForFile: could not find data");
}

template <typename T>
void __cdecl DB_InitPool(void *arg, int32_t size)
{
    //XAssetPool<RawFile, POOLSIZE_RAWFILE> *pool = (XAssetPool<RawFile, POOLSIZE_RAWFILE>*)arg;
    T *pool = (T *)arg;
    pool->freeHead = &pool->entries[0];
    for (int32_t i = 0; i < size - 1; i++)
    {
        pool->entries[i].next = &pool->entries[i + 1];
    }
    pool->entries[size - 1].next = NULL;
}

void(__cdecl *DB_InitPoolHeaderHandler[ASSET_TYPE_COUNT])(void *, int) =
{
  DB_InitPool<XAssetPool<XModelPieces, POOLSIZE_XMODELPIECES>>,
  DB_InitPool<XAssetPool<PhysPreset, POOLSIZE_PHYSPRESET>>,
  DB_InitPool<XAssetPool<XAnimParts, POOLSIZE_XANIMPARTS>>,
  DB_InitPool<XAssetPool<XModel, POOLSIZE_XMODEL>>,
  DB_InitPool<XAssetPool<Material, POOLSIZE_MATERIAL>>,
  DB_InitPool<XAssetPool<MaterialTechniqueSet, POOLSIZE_TECHNIQUE_SET>>,
  DB_InitPool<XAssetPool<GfxImage, POOLSIZE_IMAGE>>,
  DB_InitPool<XAssetPool<snd_alias_list_t, POOLSIZE_SOUND>>,
  DB_InitPool<XAssetPool<SndCurve, POOLSIZE_SOUND_CURVE>>,
  DB_InitPool<XAssetPool<LoadedSound, POOLSIZE_LOADED_SOUND>>,
  &DB_InitSingleton,
  &DB_InitSingleton,
  &DB_InitSingleton,
  &DB_InitSingleton,
  &DB_InitSingleton,
  DB_InitPool<XAssetPool<MapEnts, POOLSIZE_MAP_ENTS>>,
  &DB_InitSingleton,
  DB_InitPool<XAssetPool<GfxLightDef, POOLSIZE_LIGHT_DEF>>,
  NULL,
  DB_InitPool<XAssetPool<Font_s, POOLSIZE_FONT>>,
  DB_InitPool<XAssetPool<MenuList, POOLSIZE_MENULIST>>,
  DB_InitPool<XAssetPool<menuDef_t, POOLSIZE_MENU>>,
  DB_InitPool<XAssetPool<LocalizeEntry, POOLSIZE_LOCALIZE_ENTRY>>,
  DB_InitPool<XAssetPool<WeaponDef, POOLSIZE_WEAPON>>,
  DB_InitPool<XAssetPool<SndDriverGlobals, POOLSIZE_SNDDRIVER_GLOBALS>>,
  DB_InitPool<XAssetPool<FxEffectDef, POOLSIZE_FX>>,
  DB_InitPool<XAssetPool<FxImpactTable, POOLSIZE_IMPACT_FX>>,
  NULL,
  NULL,
  NULL,
  NULL,
  DB_InitPool<XAssetPool<RawFile, POOLSIZE_RAWFILE>>,
  DB_InitPool<XAssetPool<StringTable, POOLSIZE_STRINGTABLE>>,
}; // idb

void *DB_XAssetPool[ASSET_TYPE_COUNT] =
{
  &g_XModelPiecesPool,
  &g_PhysPresetPool,
  &g_XAnimPartsPool,
  &g_XModelPool,
  &g_MaterialPool,
  &g_MaterialTechniqueSetPool,
  &g_GfxImagePool,
  &g_SoundPool,
  &g_SndCurvePool,
  &g_LoadedSoundPool,
  &cm,
  &cm,
  &comWorld,
#ifdef KISAK_MP
  NULL,         // GAMEWORLD_SP
  &gameWorldMp, // GAMEWORLD_MP
#elif KISAK_SP
  &gameWorldSp, // GAMEWORLD_SP
  NULL,         // GAMEWORLD_MP
#endif
  &g_MapEntsPool,
  &s_world,
  &g_GfxLightDefPool,
  NULL,
  &g_FontPool,
  &g_MenuListPool,
  &g_MenuPool,
  &g_LocalizeEntryPool,
  &g_WeaponDefPool,
  NULL, // &g_SndDriverGlobalsPool (Set in SP?)
  &g_FxEffectDefPool,
  &g_FxImpactTablePool,
  NULL,
  NULL,
  NULL,
  NULL,
  &g_RawFilePool,
  &g_StringTablePool
}; // idb

void __cdecl TRACK_db_registry()
{
    track_static_alloc_internal(db_hashTable, 0x10000, "db_hashTable", 10);
    track_static_alloc_internal(g_copyInfo, 0x2000, "g_copyInfo", 10);
    track_static_alloc_internal(g_zones, 5544, "g_zones", 10);
    track_static_alloc_internal(g_zoneHandles, 32, "g_zoneHandles", 10);
    track_static_alloc_internal(g_zoneNameList, 2080, "g_zoneNameList", 10);
    track_static_alloc_internal(&g_XModelPiecesPool, 772, "g_XModelPiecesPool", 10);
    track_static_alloc_internal(&g_PhysPresetPool, 2820, "g_PhysPresetPool", 10);
    track_static_alloc_internal(&g_XAnimPartsPool, 360452, "g_XAnimPartsPool", 10);
    track_static_alloc_internal(&g_XModelPool, 220004, "g_XModelPool", 10);
    track_static_alloc_internal(&g_MaterialPool, 163848, "g_MaterialPool", 10);
    track_static_alloc_internal(&g_MaterialTechniqueSetPool, 151556, "g_MaterialTechniqueSetPool", 10);
    track_static_alloc_internal(&g_GfxImagePool, 86404, "g_GfxImagePool", 10);
    track_static_alloc_internal(&g_SoundPool, 192004, "g_SoundPool", 10);
    track_static_alloc_internal(&g_SndCurvePool, 4612, "g_SndCurvePool", 10);
    track_static_alloc_internal(&g_LoadedSoundPool, 52804, "g_LoadedSoundPool", 10);
    track_static_alloc_internal(&g_MapEntsPool, 28, "g_MapEntsPool", 10);
    track_static_alloc_internal(&g_GfxLightDefPool, 516, "g_GfxLightDefPool", 10);
    track_static_alloc_internal(&g_FontPool, 388, "g_FontPool", 10);
    track_static_alloc_internal(&g_MenuListPool, 1540, "g_MenuListPool", 10);
    track_static_alloc_internal(&g_MenuPool, 181764, "g_MenuPool", 10);
    track_static_alloc_internal(&g_LocalizeEntryPool, 49156, "g_LocalizeEntryPool", 10);
    track_static_alloc_internal(&g_WeaponDefPool, 277508, "g_WeaponDefPool", 10);
    track_static_alloc_internal(&g_FxEffectDefPool, 12804, "g_FxEffectDefPool", 10);
    track_static_alloc_internal(&g_FxImpactTablePool, 36, "g_FxImpactTablePool", 10);
    track_static_alloc_internal(&g_RawFilePool, 12292, "g_RawFilePool", 10);
    track_static_alloc_internal(&g_StringTablePool, 804, "g_StringTablePool", 10);
    track_static_alloc_internal(g_assetEntryPool, 0x80000, "g_assetEntryPool", 10);
    track_static_alloc_internal(g_fileBuf, 0x80000, "g_fileBuf", 10);
}

void __cdecl DB_GetIndexBufferAndBase(uint8_t zoneHandle, void *indices, void **ib, int32_t *baseIndex)
{
    *ib = g_zones[zoneHandle].mem.indexBuffer;
    *baseIndex = ((uint32_t)indices - (uint32_t)g_zones[zoneHandle].mem.blocks[8].data) >> 1;
}

void __cdecl DB_GetVertexBufferAndOffset(uint8_t zoneHandle, _BYTE *verts, void **vb, int32_t *vertexOffset)
{
    *vertexOffset = verts - g_zones[zoneHandle].mem.blocks[7].data;
    *vb = g_zones[zoneHandle].mem.vertexBuffer;
}

void __cdecl DB_BuildOSPath_Mod(const char *zoneName, uint32_t size, char *filename)
{
    char *v3; // eax
    const char *string; // [esp-8h] [ebp-8h]

    if (!*(_BYTE *)fs_gameDirVar->current.integer)
        MyAssertHandler(".\\database\\db_registry.cpp", 3204, 0, "%s", "IsUsingMods()");
    string = fs_gameDirVar->current.string;
    v3 = (fs_basepath && fs_basepath->current.string && *fs_basepath->current.string)
        ? (char *)fs_basepath->current.string : Sys_DefaultInstallPath();
    Com_sprintf(filename, size, "%s\\%s\\%s.ff", v3, string, zoneName);
}

bool __cdecl DB_ModFileExists()
{
    char filename[256]; // [esp+0h] [ebp-108h] BYREF
    void *zoneFile; // [esp+104h] [ebp-4h]

    if (!*(_BYTE *)fs_gameDirVar->current.integer)
        return 0;
    DB_BuildOSPath_Mod("mod", 0x100u, filename);
    zoneFile = CreateFileA(filename, 0x80000000, 1u, 0, 3u, 0x60000000u, 0);
    if (zoneFile == (void *)-1)
        return 0;
    CloseHandle(zoneFile);
    return 1;
}

void __cdecl DB_EndRecoverLostDevice()
{
    int32_t zoneIter; // [esp+4h] [ebp-4h]

    InterlockedIncrement(&db_hashCritSect.readCount);
    while (db_hashCritSect.writeCount)
        NET_Sleep(0);
    for (zoneIter = 0; zoneIter < g_zoneCount; ++zoneIter)
        DB_RecoverGeometryBuffers(&g_zones[g_zoneHandles[zoneIter]].mem);
    if (db_hashCritSect.readCount <= 0)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\gfx_d3d\\../qcommon/threads_interlock.h",
            76,
            0,
            "%s",
            "critSect->readCount > 0");
    InterlockedDecrement(&db_hashCritSect.readCount);
    if (!Sys_IsMainThread())
        MyAssertHandler(".\\database\\db_registry.cpp", 2929, 0, "%s", "Sys_IsMainThread()");
    if (!g_isRecoveringLostDevice)
        MyAssertHandler(".\\database\\db_registry.cpp", 2930, 0, "%s", "g_isRecoveringLostDevice");
    if (!g_mayRecoverLostAssets)
        MyAssertHandler(".\\database\\db_registry.cpp", 2931, 0, "%s", "g_mayRecoverLostAssets");
    g_mayRecoverLostAssets = !g_loadingZone;
    g_isRecoveringLostDevice = 0;
}

void __cdecl DB_BeginRecoverLostDevice()
{
    int32_t zoneIter; // [esp+4h] [ebp-4h]

    if (!Sys_IsMainThread())
        MyAssertHandler(".\\database\\db_registry.cpp", 2896, 0, "%s", "Sys_IsMainThread()");
    if (g_isRecoveringLostDevice)
        MyAssertHandler(".\\database\\db_registry.cpp", 2897, 0, "%s", "!g_isRecoveringLostDevice");
    g_isRecoveringLostDevice = 1;
    while (!g_mayRecoverLostAssets)
        NET_Sleep(0);
    InterlockedIncrement(&db_hashCritSect.readCount);
    while (db_hashCritSect.writeCount)
        NET_Sleep(0);
    for (zoneIter = 0; zoneIter < g_zoneCount; ++zoneIter)
        DB_ReleaseGeometryBuffers(&g_zones[g_zoneHandles[zoneIter]].mem);
    if (db_hashCritSect.readCount <= 0)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\gfx_d3d\\../qcommon/threads_interlock.h",
            76,
            0,
            "%s",
            "critSect->readCount > 0");
    InterlockedDecrement(&db_hashCritSect.readCount);
}

void __cdecl DB_InitSingleton(void *pool, int32_t size)
{
    if (size != 1)
        MyAssertHandler(".\\database\\db_registry.cpp", 528, 0, "%s\n\t(size) = %i", "(size == 1)", size);
}

void __cdecl Load_PhysPresetAsset(XAssetHeader *physPreset)
{
    physPreset->xmodelPieces = DB_AddXAsset(ASSET_TYPE_PHYSPRESET, (XAssetHeader)physPreset->xmodelPieces).xmodelPieces;
}

void __cdecl Mark_PhysPresetAsset(PhysPreset *physPreset)
{
    DB_GetXAsset(ASSET_TYPE_PHYSPRESET, (XAssetHeader)physPreset);
}

void __cdecl Load_XAnimPartsAsset(XAssetHeader *parts)
{
    parts->xmodelPieces = DB_AddXAsset(ASSET_TYPE_XANIMPARTS, (XAssetHeader)parts->xmodelPieces).xmodelPieces;
}

void __cdecl Mark_XAnimPartsAsset(XAnimParts *parts)
{
    DB_GetXAsset(ASSET_TYPE_XANIMPARTS, (XAssetHeader)parts);
}

void __cdecl Load_XModelAsset(XAssetHeader *model)
{
    model->xmodelPieces = DB_AddXAsset(ASSET_TYPE_XMODEL, (XAssetHeader)model->xmodelPieces).xmodelPieces;
}

void __cdecl Mark_XModelAsset(XModel *model)
{
    DB_GetXAsset(ASSET_TYPE_XMODEL, (XAssetHeader)model);
}

void __cdecl Load_MaterialAsset(XAssetHeader *material)
{
    material->xmodelPieces = DB_AddXAsset(ASSET_TYPE_MATERIAL, (XAssetHeader)material->xmodelPieces).xmodelPieces;
}

void __cdecl Mark_MaterialAsset(Material *material)
{
    DB_GetXAsset(ASSET_TYPE_MATERIAL, (XAssetHeader)material);
}

void __cdecl Load_MaterialTechniqueSetAsset(XAssetHeader *techniqueSet)
{
    techniqueSet->xmodelPieces = DB_AddXAsset(ASSET_TYPE_TECHNIQUE_SET, (XAssetHeader)techniqueSet->xmodelPieces).xmodelPieces;
    Material_OriginalRemapTechniqueSet(techniqueSet->techniqueSet);
    Material_UploadShaders(techniqueSet->techniqueSet);
}

void __cdecl Mark_MaterialTechniqueSetAsset(MaterialTechniqueSet *techniqueSet)
{
    DB_GetXAsset(ASSET_TYPE_TECHNIQUE_SET, (XAssetHeader)techniqueSet);
}

void __cdecl Load_GfxImageAsset(XAssetHeader *image)
{
    image->xmodelPieces = DB_AddXAsset(ASSET_TYPE_IMAGE, (XAssetHeader)image->xmodelPieces).xmodelPieces;
}

void __cdecl Mark_GfxImageAsset(GfxImage *image)
{
    DB_GetXAsset(ASSET_TYPE_IMAGE, (XAssetHeader)image);
}

void __cdecl Load_snd_alias_list_Asset(XAssetHeader *sound)
{
    sound->xmodelPieces = DB_AddXAsset(ASSET_TYPE_SOUND, (XAssetHeader)sound->xmodelPieces).xmodelPieces;
}

void __cdecl Mark_snd_alias_list_Asset(snd_alias_list_t *sound)
{
    DB_GetXAsset(ASSET_TYPE_SOUND, (XAssetHeader)sound);
}

void __cdecl Load_SndCurveAsset(XAssetHeader *sndCurve)
{
    sndCurve->xmodelPieces = DB_AddXAsset(ASSET_TYPE_SOUND_CURVE, (XAssetHeader)sndCurve->xmodelPieces).xmodelPieces;
}

void __cdecl Mark_SndCurveAsset(SndCurve *sndCurve)
{
    DB_GetXAsset(ASSET_TYPE_SOUND_CURVE, (XAssetHeader)sndCurve);
}

void __cdecl Load_LoadedSoundAsset(XAssetHeader *loadSnd)
{
    loadSnd->xmodelPieces = DB_AddXAsset(ASSET_TYPE_LOADED_SOUND, (XAssetHeader)loadSnd->xmodelPieces).xmodelPieces;
}

void __cdecl Mark_LoadedSoundAsset(LoadedSound *loadSnd)
{
    DB_GetXAsset(ASSET_TYPE_LOADED_SOUND, (XAssetHeader)loadSnd);
}

void __cdecl Load_ClipMapAsset(XAssetHeader *clipMap)
{
#ifdef KISAK_MP
    clipMap->clipMap = DB_AddXAsset(ASSET_TYPE_CLIPMAP_PVS, (XAssetHeader)clipMap->clipMap).clipMap;
#elif KISAK_SP
    clipMap->clipMap = DB_AddXAsset(ASSET_TYPE_CLIPMAP, (XAssetHeader)clipMap->clipMap).clipMap;
#endif
}

void __cdecl Mark_ClipMapAsset(clipMap_t *clipMap)
{
#ifdef KISAK_MP
    DB_GetXAsset(ASSET_TYPE_CLIPMAP_PVS, (XAssetHeader)clipMap);
#elif KISAK_SP
    DB_GetXAsset(ASSET_TYPE_CLIPMAP, (XAssetHeader)clipMap);
#endif
}

void __cdecl DB_RemoveLoadedSound(XAssetHeader header)
{
    //Z_Free((char *)header.xmodelPieces[3].numpieces, 15);
    Z_Free(header.loadSnd->sound.data, 15);
}

void __cdecl DB_RemoveClipMap(XAssetHeader ass)
{
    CM_Unload();
}

void __cdecl Load_ComWorldAsset(XAssetHeader *comWorld)
{
    comWorld->xmodelPieces = DB_AddXAsset(ASSET_TYPE_COMWORLD, (XAssetHeader)comWorld->xmodelPieces).xmodelPieces;
}

void __cdecl Mark_ComWorldAsset(ComWorld *comWorld)
{
    DB_GetXAsset(ASSET_TYPE_COMWORLD, (XAssetHeader)comWorld);
}

void __cdecl DB_RemoveComWorld(XAssetHeader ass)
{
    Com_UnloadWorld();
}

void __cdecl Load_GameWorldSpAsset(XAssetHeader *gameWorldSp)
{
    gameWorldSp->xmodelPieces = DB_AddXAsset(ASSET_TYPE_GAMEWORLD_SP, (XAssetHeader)gameWorldSp->xmodelPieces).xmodelPieces;
}

void __cdecl Mark_GameWorldSpAsset(GameWorldSp *gameWorldSp)
{
    DB_GetXAsset(ASSET_TYPE_GAMEWORLD_SP, (XAssetHeader)gameWorldSp);
}

void __cdecl Load_GameWorldMpAsset(XAssetHeader *gameWorldMp)
{
    gameWorldMp->xmodelPieces = DB_AddXAsset(ASSET_TYPE_GAMEWORLD_MP, (XAssetHeader)gameWorldMp->xmodelPieces).xmodelPieces;
}

void __cdecl Mark_GameWorldMpAsset(GameWorldMp *gameWorldMp)
{
    DB_GetXAsset(ASSET_TYPE_GAMEWORLD_MP, (XAssetHeader)gameWorldMp);
}

void __cdecl Load_MapEntsAsset(XAssetHeader *mapEnts)
{
    mapEnts->xmodelPieces = DB_AddXAsset(ASSET_TYPE_MAP_ENTS, (XAssetHeader)mapEnts->xmodelPieces).xmodelPieces;
}

void __cdecl Mark_MapEntsAsset(MapEnts *mapEnts)
{
    DB_GetXAsset(ASSET_TYPE_MAP_ENTS, (XAssetHeader)mapEnts);
}

void __cdecl Load_GfxWorldAsset(XAssetHeader *gfxWorld)
{
    gfxWorld->xmodelPieces = DB_AddXAsset(ASSET_TYPE_GFXWORLD, (XAssetHeader)gfxWorld->xmodelPieces).xmodelPieces;
}

void __cdecl Mark_GfxWorldAsset(GfxWorld *gfxWorld)
{
    DB_GetXAsset(ASSET_TYPE_GFXWORLD, (XAssetHeader)gfxWorld);
}

void __cdecl DB_RemoveGfxWorld(XAssetHeader ass)
{
    R_UnloadWorld();
}

void __cdecl Load_LightDefAsset(XAssetHeader *lightDef)
{
    lightDef->xmodelPieces = DB_AddXAsset(ASSET_TYPE_LIGHT_DEF, (XAssetHeader)lightDef->xmodelPieces).xmodelPieces;
}

void __cdecl Mark_LightDefAsset(GfxLightDef *lightDef)
{
    DB_GetXAsset(ASSET_TYPE_LIGHT_DEF, (XAssetHeader)lightDef);
}

void __cdecl Load_FontAsset(XAssetHeader *font)
{
    font->xmodelPieces = DB_AddXAsset(ASSET_TYPE_FONT, (XAssetHeader)font->xmodelPieces).xmodelPieces;
}

void __cdecl Mark_FontAsset(Font_s *font)
{
    DB_GetXAsset(ASSET_TYPE_FONT, (XAssetHeader)font);
}

void __cdecl Load_MenuListAsset(XAssetHeader *menuList)
{
    menuList->xmodelPieces = DB_AddXAsset(ASSET_TYPE_MENULIST, (XAssetHeader)menuList->xmodelPieces).xmodelPieces;
}

void __cdecl Mark_MenuListAsset(MenuList *menuList)
{
    DB_GetXAsset(ASSET_TYPE_MENULIST, (XAssetHeader)menuList);
}

void __cdecl Load_MenuAsset(XAssetHeader *menu)
{
    XAssetHeader header; // [esp+4h] [ebp-8h]
    int32_t i; // [esp+8h] [ebp-4h]

    header.menu = menu->menu;
    menu->menu = DB_AddXAsset(ASSET_TYPE_MENU, *menu).menu;

    for (i = 0; i < header.menu->itemCount; ++i)
        header.menu->items[i]->parent = menu->menu;
    //for (i = 0; i < (int)header.xmodelPieces[13].pieces; ++i)
    //    *(XAssetHeader *)(*(uint32_t *)(header.xmodelPieces[23].numpieces + 4 * i) + 232) = (XAssetHeader)menu->xmodelPieces;
}

void __cdecl Mark_MenuAsset(menuDef_t *menu)
{
    DB_GetXAsset(ASSET_TYPE_MENU, (XAssetHeader)menu);
}

void __cdecl DB_DynamicCloneMenu(XAssetHeader from, XAssetHeader to, int32_t swag)
{
    windowDef_t *toWindow; // [esp+14h] [ebp-18h]
    int32_t toIndex; // [esp+18h] [ebp-14h]
    int32_t fromIndex; // [esp+1Ch] [ebp-10h]
    windowDef_t *fromWindow; // [esp+24h] [ebp-8h]

    to.xmodelPieces[6].pieces = from.xmodelPieces[6].pieces;
    for (toIndex = 0; toIndex < (int)to.xmodelPieces[13].pieces; ++toIndex)
    {
        toWindow = *(windowDef_t **)(to.xmodelPieces[23].numpieces + 4 * toIndex);
        if (toWindow->name)
        {
            for (fromIndex = 0; fromIndex < (int)from.xmodelPieces[13].pieces; ++fromIndex)
            {
                fromWindow = *(windowDef_t **)(from.xmodelPieces[23].numpieces + 4 * fromIndex);
                if (fromWindow->name && !strcmp(fromWindow->name, toWindow->name))
                {
                    toWindow->dynamicFlags[0] = fromWindow->dynamicFlags[0];
                    break;
                }
            }
        }
        DB_RemoveWindowFocus(toWindow);
    }
}

void __cdecl DB_RemoveWindowFocus(windowDef_t *window)
{
    uint32_t i; // [esp+0h] [ebp-4h]

    for (i = 0; !i; i = 1)
        window->dynamicFlags[0] &= ~2u;
}

void __cdecl Load_LocalizeEntryAsset(XAssetHeader *localize)
{
    localize->xmodelPieces = DB_AddXAsset(ASSET_TYPE_LOCALIZE_ENTRY, (XAssetHeader)localize->xmodelPieces).xmodelPieces;
}

void __cdecl Mark_LocalizeEntryAsset(LocalizeEntry *localize)
{
    DB_GetXAsset(ASSET_TYPE_LOCALIZE_ENTRY, (XAssetHeader)localize);
}

void __cdecl Load_WeaponDefAsset(XAssetHeader *weapon)
{
    weapon->xmodelPieces = DB_AddXAsset(ASSET_TYPE_WEAPON, (XAssetHeader)weapon->xmodelPieces).xmodelPieces;
}

void __cdecl Mark_WeaponDefAsset(WeaponDef *weapon)
{
    DB_GetXAsset(ASSET_TYPE_WEAPON, (XAssetHeader)weapon);
}

void __cdecl Load_FxEffectDefAsset(XAssetHeader *fx)
{
    fx->xmodelPieces = DB_AddXAsset(ASSET_TYPE_FX, (XAssetHeader)fx->xmodelPieces).xmodelPieces;
}

void __cdecl Mark_FxEffectDefAsset(FxEffectDef *fx)
{
    DB_GetXAsset(ASSET_TYPE_FX, (XAssetHeader)fx);
}

void __cdecl Load_FxEffectDefFromName(const char **name)
{
    if (*name)
        *(XAssetHeader *)name = DB_FindXAssetHeader(ASSET_TYPE_FX, *name);
}

void __cdecl Load_FxImpactTableAsset(XAssetHeader *impactFx)
{
    impactFx->xmodelPieces = DB_AddXAsset(ASSET_TYPE_IMPACT_FX, (XAssetHeader)impactFx->xmodelPieces).xmodelPieces;
}

void __cdecl Mark_FxImpactTableAsset(FxImpactTable *impactFx)
{
    DB_GetXAsset(ASSET_TYPE_IMPACT_FX, (XAssetHeader)impactFx);
}

void __cdecl Load_RawFileAsset(XAssetHeader *rawfile)
{
    rawfile->xmodelPieces = DB_AddXAsset(ASSET_TYPE_RAWFILE, (XAssetHeader)rawfile->xmodelPieces).xmodelPieces;
}

void __cdecl Mark_RawFileAsset(RawFile *rawfile)
{
    DB_GetXAsset(ASSET_TYPE_RAWFILE, (XAssetHeader)rawfile);
}

void __cdecl Load_StringTableAsset(XAssetHeader *stringTable)
{
    stringTable->xmodelPieces = DB_AddXAsset(ASSET_TYPE_STRINGTABLE, (XAssetHeader)stringTable->xmodelPieces).xmodelPieces;
}

void __cdecl Mark_StringTableAsset(StringTable *stringTable)
{
    DB_GetXAsset(ASSET_TYPE_STRINGTABLE, (XAssetHeader)stringTable);
}

XAssetHeader __cdecl DB_AllocMaterial(void *arg)
{
    XAssetHeader *pool = (XAssetHeader*)arg;
    Material_DirtySort();
    return DB_AllocXAsset_StringTable_(pool);
}

void __cdecl DB_FreeMaterial(void* arg, XAssetHeader header)
{
    XAssetPoolEntry<StringTable> **pool = (XAssetPoolEntry<StringTable> **)arg;
    Material_DirtySort();
    DB_FreeXAssetHeader_StringTable_(pool, header);
}

void __cdecl DB_EnumXAssets_FastFile(
    XAssetType type,
    void(__cdecl *func)(XAssetHeader, void *),
    void *inData,
    bool includeOverride)
{
    uint32_t hash; // [esp+4h] [ebp-14h]
    uint32_t assetEntryIndex; // [esp+8h] [ebp-10h]
    XAssetEntryPoolEntry *assetEntry; // [esp+10h] [ebp-8h]
    uint32_t overrideAssetEntryIndex; // [esp+14h] [ebp-4h]

    InterlockedIncrement(&db_hashCritSect.readCount);
    while (db_hashCritSect.writeCount)
        NET_Sleep(0);
    for (hash = 0; hash < 0x8000; ++hash)
    {
        for (assetEntryIndex = db_hashTable[hash]; assetEntryIndex; assetEntryIndex = assetEntry->entry.nextHash)
        {
            assetEntry = &g_assetEntryPool[assetEntryIndex];
            if (assetEntry->entry.asset.type == type)
            {
                func(assetEntry->entry.asset.header, inData);
                if (includeOverride)
                {
                    for (overrideAssetEntryIndex = assetEntry->entry.nextOverride;
                        overrideAssetEntryIndex;
                        overrideAssetEntryIndex = g_assetEntryPool[overrideAssetEntryIndex].entry.nextOverride)
                    {
                        func(g_assetEntryPool[overrideAssetEntryIndex].entry.asset.header, inData);
                    }
                }
            }
        }
    }
    if (db_hashCritSect.readCount <= 0)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\gfx_d3d\\../qcommon/threads_interlock.h",
            76,
            0,
            "%s",
            "critSect->readCount > 0");
    InterlockedDecrement(&db_hashCritSect.readCount);
}

int32_t __cdecl DB_GetAllXAssetOfType(XAssetType type, XAssetHeader* assets, int32_t maxCount)
{
    if (IsFastFileLoad())
        return DB_GetAllXAssetOfType_FastFile(type, assets, maxCount);
    else
        return DB_GetAllXAssetOfType_LoadObj(type, assets, maxCount);
}

int32_t __cdecl DB_GetAllXAssetOfType_LoadObj(XAssetType type, XAssetHeader* assets, int32_t maxCount)
{
    AssetList assetList; // [esp+0h] [ebp-Ch] BYREF

    assetList.assets = assets;
    assetList.assetCount = 0;
    assetList.maxCount = maxCount;
    DB_EnumXAssets(type, (void(__cdecl*)(XAssetHeader, void*))Hunk_AddAsset, &assetList, 0);
    return assetList.assetCount;
}

void __cdecl DB_EnumXAssets(
    XAssetType type,
    void(__cdecl* func)(XAssetHeader, void*),
    void* inData,
    bool includeOverride)
{
    if (IsFastFileLoad())
        DB_EnumXAssets_FastFile(type, func, inData, includeOverride);
    else
        DB_EnumXAssets_LoadObj(type, (void(*)(void *, void *))func, inData);
}

void __cdecl R_EnumMaterials(void(__cdecl *func)(Material *, void *), void *data)
{
    Material *header; // [esp+0h] [ebp-8h]
    uint32_t hashIndex; // [esp+4h] [ebp-4h]

    for (hashIndex = 0; hashIndex < 0x800; ++hashIndex)
    {
        header = rg.materialHashTable[hashIndex];
        if (header)
            func(header, data);
    }
}

void __cdecl R_EnumTechniqueSets(void(__cdecl *func)(MaterialTechniqueSet *, void *), void *data)
{
    MaterialTechniqueSet *header; // [esp+0h] [ebp-8h]
    uint32_t hashIndex; // [esp+4h] [ebp-4h]

    for (hashIndex = 0; hashIndex < 0x400; ++hashIndex)
    {
        header = materialGlobals.techniqueSetHashTable[hashIndex];
        if (header)
            func(header, data);
    }
}

void __cdecl R_EnumImages(void(__cdecl *func)(GfxImage *, void *), void *data)
{
    GfxImage *header; // [esp+0h] [ebp-8h]
    uint32_t imageIndex; // [esp+4h] [ebp-4h]

    for (imageIndex = 0; imageIndex < IMAGE_HASH_TABLE_SIZE; ++imageIndex)
    {
        header = imageGlobals.imageHashTable[imageIndex];
        if (header)
        {
            if (!Image_IsProg(header))
                func(header, data);
        }
    }
}

void __cdecl DB_EnumXAssets_LoadObj(XAssetType type, void(* func)(void*, void*), void* inData)
{
    uint32_t hash; // [esp+4h] [ebp-Ch]

    switch (type)
    {
    case ASSET_TYPE_XMODEL:
        for (hash = 0; hash < 0x400; ++hash)
            DB_EnumXAssetsFor(com_fileDataHashTable[hash], 5, func, inData);
        break;
    case ASSET_TYPE_MATERIAL:
        R_EnumMaterials((void(__cdecl*)(Material*, void*))func, inData);
        break;
    case ASSET_TYPE_TECHNIQUE_SET:
        R_EnumTechniqueSets((void(__cdecl*)(MaterialTechniqueSet*, void*))func, inData);
        break;
    case ASSET_TYPE_IMAGE:
        R_EnumImages((void(__cdecl*)(GfxImage*, void*))func, inData);
        break;
    default:
        return;
    }
}

void __cdecl DB_EnumXAssetsFor(
    fileData_s* fileData,
    int32_t fileDataType,
    void(__cdecl* func)(void*, void*),
    void* inData)
{
    while (fileData)
    {
        if (fileData->type == fileDataType && fileData->type == 5)
            func(fileData->data, inData);
        fileData = fileData->next;
    }
}

XAssetHeader __cdecl DB_FindXAssetHeader(XAssetType type, const char *name)
{
    const char *v5; // [esp-4h] [ebp-24h]
    int32_t suspendedThread; // [esp+10h] [ebp-10h]
    uint32_t start; // [esp+14h] [ebp-Ch]
    XAssetEntry *assetEntry; // [esp+18h] [ebp-8h]
    XAssetEntry *newEntry; // [esp+1Ch] [ebp-4h]

    iassert(IsFastFileLoad());

    start = 0;
    while (1)
    {
        while (1)
        {
            InterlockedIncrement(&db_hashCritSect.readCount);
            while (db_hashCritSect.writeCount)
                NET_Sleep(0);
            assetEntry = &DB_FindXAssetEntry(type, name)->entry;
            if (db_hashCritSect.readCount <= 0)
                MyAssertHandler(
                    "c:\\trees\\cod3\\src\\gfx_d3d\\../qcommon/threads_interlock.h",
                    76,
                    0,
                    "%s",
                    "critSect->readCount > 0");
            InterlockedDecrement(&db_hashCritSect.readCount);
            DB_RegisteredReorderAsset(type, name, assetEntry);
            if (assetEntry && (assetEntry->zoneIndex || Sys_IsDatabaseReady2()))
                goto returnAsset;
            if (Sys_IsDatabaseThread())
                goto LABEL_39;
            if (start)
                break;
            ProfLoad_Begin("Wait for fastfile asset");
            start = Sys_Milliseconds();
            if (!Sys_IsDatabaseReady2())
            {
                if (Sys_IsMainThread())
                    R_ReleaseThreadOwnership();
                break;
            }
        }
        if (Sys_IsDatabaseReady2() || DB_IsMinimumFastFileLoaded() && DB_GetInitializing())
            break;
        if (Sys_IsDatabaseReady() && (Sys_IsMainThread() || Sys_IsRenderThread() && R_IsInRemoteScreenUpdate() && g_mainThreadBlocked))
        {
            DB_PostLoadXZone();
        }
        else
        {
            if (Sys_IsMainThread())
                KISAK_NULLSUB();
            suspendedThread = Sys_HaveSuspendedDatabaseThread(THREAD_OWNER_DATABASE);
            if (suspendedThread)
                Sys_ResumeDatabaseThread(THREAD_OWNER_DATABASE);
            DB_Sleep(1);
            if (suspendedThread)
                Sys_SuspendDatabaseThread(THREAD_OWNER_DATABASE);
        }
    }
    ProfLoad_End();
LABEL_39:
    if (assetEntry)
    {
    returnAsset:
        if (!assetEntry->asset.header.xmodelPieces)
            MyAssertHandler(".\\database\\db_registry.cpp", 2716, 0, "%s", "assetEntry->asset.header.data");
        assetEntry->inuse = 1;
        if (start)
        {
            v5 = g_assetNames[type];
            Com_Printf(10, "Waited %i msec for asset '%s' of type '%s'.\n", Sys_Milliseconds() - start, name, v5);
            ProfLoad_End();
        }
        return assetEntry->asset.header;
    }
    Sys_LockWrite(&db_hashCritSect);
    assetEntry = &DB_FindXAssetEntry(type, name)->entry;
    if (assetEntry)
    {
        if (!assetEntry->asset.header.xmodelPieces)
            MyAssertHandler(".\\database\\db_registry.cpp", 2774, 0, "%s", "assetEntry->asset.header.data");
        Sys_UnlockWrite(&db_hashCritSect);
        goto returnAsset;
    }
    DB_LogMissingAsset(type, name);
    if (start)
    {
        PrintWaitedError(type, name, Sys_Milliseconds() - start);
    }
    if (type == ASSET_TYPE_LOCALIZE_ENTRY || type == ASSET_TYPE_RAWFILE)
    {
        Sys_UnlockWrite(&db_hashCritSect);
        return 0;
    }
    else
    {
        newEntry = DB_CreateDefaultEntry(type, (char*)name);
        Sys_UnlockWrite(&db_hashCritSect);
        return newEntry->asset.header;
    }
}

void __cdecl DB_Sleep(uint32_t msec)
{
    R_BeginRemoteScreenUpdate();
    NET_Sleep(msec);
    R_EndRemoteScreenUpdate();
}

void __cdecl DB_LogMissingAsset(XAssetType type, const char *name)
{
    char msg[1028]; // [esp+14h] [ebp-408h] BYREF

    switch (type)
    {
    case ASSET_TYPE_SOUND:
    case ASSET_TYPE_MENU:
    case ASSET_TYPE_LOCALIZE_ENTRY:
    case ASSET_TYPE_SNDDRIVER_GLOBALS:
        return;
    case ASSET_TYPE_WEAPON:
#ifdef KISAK_SP
        Com_sprintf(msg, 0x400u, "%s,sp/%s\n", g_assetNames[type], name);
#else
        Com_sprintf(msg, 0x400u, "%s,mp/%s\n", g_assetNames[type], name);
#endif
        goto LABEL_4;
    default:
        Com_sprintf(msg, 0x400u, "%s,%s\n", g_assetNames[type], name);
    LABEL_4:
        Sys_EnterCriticalSection(CRITSECT_MISSING_ASSET);
        if (g_missingAssetFile)
            g_missingAssetFile = FS_FOpenFileAppend((char*)"missingasset.csv");
        else
            g_missingAssetFile = FS_FOpenTextFileWrite("missingasset.csv");
        if (g_missingAssetFile)
        {
            FS_Write(msg, &msg[strlen(msg) + 1] - &msg[1], g_missingAssetFile);
            FS_FCloseFile(g_missingAssetFile);
        }
        else
        {
            com_missingAssetOpenFailed = 1;
        }
        Sys_LeaveCriticalSection(CRITSECT_MISSING_ASSET);
        break;
    }
}

XAssetEntryPoolEntry *__cdecl DB_FindXAssetEntry(XAssetType type, const char *name)
{
    const char *XAssetName; // eax
    uint32_t assetEntryIndex; // [esp+4h] [ebp-8h]
    XAssetEntryPoolEntry *assetEntry; // [esp+8h] [ebp-4h]

    for (assetEntryIndex = db_hashTable[DB_HashForName(name, type)];
        assetEntryIndex;
        assetEntryIndex = assetEntry->entry.nextHash)
    {
        assetEntry = &g_assetEntryPool[assetEntryIndex];
        if (assetEntry->entry.asset.type == type)
        {
            XAssetName = DB_GetXAssetName(&assetEntry->entry.asset);
            if (!I_stricmp(XAssetName, name))
                return &g_assetEntryPool[assetEntryIndex];
        }
    }
    return 0;
}

void DB_SetReorderIncludeSequence()
{
    DBReorderAssetEntry *entry; // [esp+0h] [ebp-8h]
    uint32_t entryIter; // [esp+4h] [ebp-4h]

    for (entryIter = 0; entryIter < s_dbReorder.entryCount; ++entryIter)
    {
        entry = &s_dbReorder.entries[entryIter];
        if (entry->type == 33 && !I_strnicmp(entry->typeString, "include", 7))
            entry->sequence = s_dbReorder.sequenceForIncludes;
    }
}

bool __cdecl DB_CompareReorderEntries(const DBReorderAssetEntry& e0, const DBReorderAssetEntry& e1)
{
    int32_t comparison; // [esp+0h] [ebp-4h]

    if (e0.sequence != e1.sequence)
        return e0.sequence < e1.sequence;
    if (e0.type == 33)
    {
        if (e1.type != 33)
            return 1;
        comparison = _stricmp(e0.typeString, e1.typeString);
        if (comparison)
            return comparison < 0;
        return _stricmp(e0.assetName, e1.assetName) < 0;
    }
    if (e1.type == 33)
        return 0;
    if (e0.type == e1.type)
        return _stricmp(e0.assetName, e1.assetName) < 0;
    if (e0.sequence != -1)
        return e0.type < e1.type;
    if (e0.type == 7)
        return 1;
    if (e1.type == 7)
        return 0;
    if (e0.type == 22)
        return 1;
    return e1.type != 22 && e0.type < e1.type;
}

void DB_EndReorderZone()
{
    DBReorderAssetEntry *entry; // [esp+180h] [ebp-41Ch]
    bool wroteBlank; // [esp+187h] [ebp-415h]
    DWORD bytesa; // [esp+188h] [ebp-414h]
    DWORD bytes; // [esp+188h] [ebp-414h]
    HANDLE file; // [esp+18Ch] [ebp-410h]
    DWORD written; // [esp+190h] [ebp-40Ch] BYREF
    char csvName[256]; // [esp+194h] [ebp-408h] BYREF
    char line[512]; // [esp+294h] [ebp-308h] BYREF
    char bakName[256]; // [esp+494h] [ebp-108h] BYREF
    DWORD entryIter; // [esp+598h] [ebp-4h]

    if (s_dbReorder.entryCount)
    {
        s_dbReorder.alreadyFinished = 1;
        Com_sprintf(csvName, 0x100u, "..\\share\\zone_source\\%s.csv", s_dbReorder.zoneName);
        Com_sprintf(bakName, 0x100u, "%s.bak", csvName);
        DeleteFileA(bakName);
        rename(csvName, bakName);
        file = CreateFileA(csvName, 0x40000000u, 0, 0, 2u, 0, 0);
        if (file != (HANDLE)-1)
        {
            wroteBlank = 0;
            DB_SetReorderIncludeSequence();
            //std::_Sort<DBReorderAssetEntry *, int, bool(__cdecl *)(DBReorderAssetEntry const &, DBReorderAssetEntry const &)>(
            //    (GfxSModelSurfStats *)s_dbReorder.entries,
            //    (GfxSModelSurfStats *)&s_dbReorder.entries[s_dbReorder.entryCount],
            //    (int32_t)(16 * s_dbReorder.entryCount) >> 4,
            //    (bool(__cdecl *)(GfxSModelSurfStats *, GfxSModelSurfStats *))DB_CompareReorderEntries);
            std::sort(s_dbReorder.entries + 0, s_dbReorder.entries + s_dbReorder.entryCount, DB_CompareReorderEntries);
            for (entryIter = 0; entryIter < s_dbReorder.entryCount; ++entryIter)
            {
                entry = &s_dbReorder.entries[entryIter];
                if (!wroteBlank && entry->sequence == -1)
                {
                    switch (entry->type)
                    {
                    case 7:
                    case 0xA:
                    case 0xB:
                    case 0x16:
                    case 0x21:
                        break;
                    default:
                        wroteBlank = 1;
                        WriteFile(file, "\r\n", 2u, &written, 0);
                        break;
                    }
                }
                if (entry->type == 23)
                {
                    bytesa = Com_sprintf(line, 0x200u, "%s,%s%s\r\n", entry->typeString, "mp/", entry->assetName);
                    WriteFile(file, line, bytesa, &written, 0);
                }
                else
                {
                    if (entry->type == 7)
                        bytes = Com_sprintf(
                            line,
                            0x200u,
                            "%s,%s,%s,%s\r\n",
                            entry->typeString,
                            entry->assetName,
                            s_dbReorder.zoneName,
                            "all_mp");
                    else
                        bytes = Com_sprintf(line, 0x200u, "%s,%s\r\n", entry->typeString, entry->assetName);
                    WriteFile(file, line, bytes, &written, 0);
                }
            }
            CloseHandle(file);
        }
    }
}

char __cdecl DB_RegisterAllReorderAssetsOfType(int32_t type, XAssetEntry *assetEntry)
{
    DBReorderAssetEntry *entry; // [esp+0h] [ebp-8h]
    uint32_t entryIter; // [esp+4h] [ebp-4h]

    if (assetEntry && _stricmp(g_zones[assetEntry->zoneIndex].name, s_dbReorder.zoneName))
        return 0;
    if (Sys_IsDatabaseThread())
        return 0;
    Sys_LockWrite(&s_dbReorder.critSect);
    for (entryIter = 0; entryIter < s_dbReorder.entryCount; ++entryIter)
    {
        entry = &s_dbReorder.entries[entryIter];
        if (entry->type == type && entry->sequence == -1)
            entry->sequence = s_dbReorder.sequence;
    }
    s_dbReorder.sequence += 2;
    if (s_dbReorder.alreadyFinished)
        DB_EndReorderZone();
    Sys_UnlockWrite(&s_dbReorder.critSect);
    return 1;
}

void __cdecl DB_RegisteredReorderAsset(int32_t type, const char *assetName, XAssetEntry *assetEntry)
{
    DBReorderAssetEntry *entry; // [esp+0h] [ebp-Ch]
    const char *extension; // [esp+4h] [ebp-8h]
    uint32_t entryIter; // [esp+8h] [ebp-4h]

    if (s_dbReorder.entryCount)
    {
        if (type == 22)
        {
            if (!s_dbReorder.loadedLocalization)
                s_dbReorder.loadedLocalization = DB_RegisterAllReorderAssetsOfType(22, assetEntry) != 0;
        }
        else if (type == 7)
        {
            if (!s_dbReorder.loadedSound)
                s_dbReorder.loadedSound = DB_RegisterAllReorderAssetsOfType(7, assetEntry) != 0;
        }
        else if (!s_dbReorder.lastEntry
            || s_dbReorder.lastEntry->type != type
            || _stricmp(s_dbReorder.lastEntry->assetName, assetName))
        {
            Sys_LockWrite(&s_dbReorder.critSect);
            s_dbReorder.lastEntry = 0;
            for (entryIter = 0; entryIter < s_dbReorder.entryCount; ++entryIter)
            {
                entry = &s_dbReorder.entries[entryIter];
                if (entry->type == type && !_stricmp(entry->assetName, assetName))
                {
                    s_dbReorder.lastEntry = &s_dbReorder.entries[entryIter];
                    if (entry->sequence == -1)
                    {
                        entry->sequence = s_dbReorder.sequence;
                        if (entry->type == 31)
                        {
                            extension = Com_GetExtensionSubString(assetName);
                            if (!I_stricmp(extension, ".gsc"))
                                s_dbReorder.sequenceForIncludes = s_dbReorder.sequence + 1;
                        }
                        s_dbReorder.sequence += 2;
                        if (s_dbReorder.alreadyFinished)
                            DB_EndReorderZone();
                    }
                    break;
                }
            }
            Sys_UnlockWrite(&s_dbReorder.critSect);
        }
    }
}

uint32_t __cdecl DB_HashForName(const char *name, XAssetType type)
{
    int32_t c; // [esp+8h] [ebp-4h]
    int32_t out_val = (int)type;

    while (1)
    {
        c = tolower(*name);
        if (c == '\\')
            c = '/';
        if (!c)
            break;
        out_val = c + 31 * out_val;
        ++name;
    }
    return out_val % 0x8000u;
}

int32_t g_defaultAssetCount;
XAssetEntry *__cdecl DB_CreateDefaultEntry(XAssetType type, char *name)
{
    XAsset asset; // [esp+Ch] [ebp-Ch] BYREF
    XAssetEntry *newEntry; // [esp+14h] [ebp-4h]

    asset.header = DB_FindXAssetDefaultHeaderInternal(type);
    if (!asset.header.data)
    {
        Sys_UnlockWrite(&db_hashCritSect);
        if (type == ASSET_TYPE_CLIPMAP || type == ASSET_TYPE_CLIPMAP_PVS)
            Com_Error(
                ERR_DROP,
                "Couldn't find the bsp for this map.  Please build the fast file associated with %s and try again.",
                name);
        else
            Com_Error(
                ERR_DROP,
                "Could not load default asset '%s' for asset type '%s'.\nTried to load asset '%s'.",
                g_defaultAssetName[type],
                g_assetNames[type],
                name);
    }
    asset.type = type;
    ++g_defaultAssetCount;
    newEntry = (XAssetEntry *)DB_AllocXAssetEntry(type, 0);
    DB_CloneXAssetInternal(&asset, &newEntry->asset);
    if (type == ASSET_TYPE_SOUND)
    {
        newEntry->asset.header.sound->count = 0;
        newEntry->asset.header.sound->head = NULL;
    }
    newEntry->nextHash = db_hashTable[DB_HashForName(name, type)];
    db_hashTable[DB_HashForName(name, type)] = ((char *)newEntry - (char *)g_assetEntryPool) >> 4;
    DB_SetXAssetName(&newEntry->asset, SL_ConvertToString(SL_GetString(name, 4)));
    newEntry->inuse = 1;
    return newEntry;
}

XAssetEntryPoolEntry *__cdecl DB_AllocXAssetEntry(XAssetType type, uint8_t zoneIndex)
{
    XAssetEntryPoolEntry *freeHead; // [esp+4h] [ebp-8h]

    freeHead = g_freeAssetEntryHead;
    if (!g_freeAssetEntryHead)
    {
        Sys_UnlockWrite(&db_hashCritSect);
        Com_Error(ERR_DROP, "Could not allocate asset - increase XASSET_ENTRY_POOL_SIZE");
    }
    g_freeAssetEntryHead = freeHead->next;
    freeHead->entry.asset.type = type;
    freeHead->entry.asset.header = DB_AllocXAssetHeader(type);
    freeHead->entry.zoneIndex = zoneIndex;
    freeHead->entry.inuse = 0;
    freeHead->entry.nextHash = 0;
    freeHead->entry.nextOverride = 0;
    return freeHead;
}

XAssetHeader __cdecl DB_AllocXAssetHeader(XAssetType type)
{
    XAssetHeader header; // [esp+4h] [ebp-4h]

    header.data = DB_AllocXAssetHeaderHandler[type](DB_XAssetPool[type]).data;
    if (!header.data)
    {
        Sys_UnlockWrite(&db_hashCritSect);
        Com_PrintError(1, "Exceeded limit of %d '%s' assets.\n", g_poolSize[type], g_assetNames[type]);
        DB_EnumXAssets(type, (void(__cdecl *)(XAssetHeader, void *))DB_PrintAssetName, &type, 1);
        Com_Error(ERR_DROP, "Exceeded limit of %d '%s' assets.\n", g_poolSize[type], g_assetNames[type]);
    }
    return header;
}

void __cdecl DB_PrintAssetName(XAssetHeader header, int32_t *data)
{
    const char *XAssetHeaderName; // eax

    XAssetHeaderName = DB_GetXAssetHeaderName(*data, &header);
    Com_Printf(0, "%s\n", XAssetHeaderName);
}

struct VitaAssetDumpContext
{
    int file;
    XAssetType type;
    uint32_t count;
};

static void __cdecl DB_DumpVitaAssetName(XAssetHeader header, void *rawContext)
{
    auto *context = static_cast<VitaAssetDumpContext *>(rawContext);
    if (!context)
        return;
    const char *name = DB_GetXAssetHeaderName(static_cast<int32_t>(context->type), &header);
    if (!name)
        name = "";
    // CSV keeps this host-side provenance file easy to consume from Python
    // while avoiding any serialized XAsset pointer or native record layout.
    FS_Printf(context->file, "%u,%s,%s\n", static_cast<unsigned>(context->type),
              g_assetNames[context->type], name);
    ++context->count;
}

// The desktop database owns the live world and clip-map allocations.  The
// Vita package must never contain those native pointers, so this exporter
// writes a deliberately small, little-endian, pointer-free representation.
// It is kept next to the asset-name dump so a reference run produces a
// self-contained source bundle for the offline KVAP converter.
static bool DB_VitaWrite(int file, const void *data, uint32_t size)
{
    return size == 0 || FS_Write(static_cast<const char *>(data), size, file) == size;
}

static bool DB_VitaBoundedString(const char *value, uint32_t *bytes);

template <typename T>
static bool DB_VitaWriteValue(int file, const T &value)
{
    return DB_VitaWrite(file, &value, static_cast<uint32_t>(sizeof(value)));
}

static bool DB_VitaValidCount(int value, uint32_t limit = 2'000'000)
{
    return value >= 0 && static_cast<uint32_t>(value) <= limit;
}

static uint32_t DB_VitaPlaneIndex(const cplane_s *plane)
{
    if (!plane || !cm.planes || cm.planeCount <= 0)
        return UINT32_MAX;

    const uintptr_t begin = reinterpret_cast<uintptr_t>(cm.planes);
    const uintptr_t address = reinterpret_cast<uintptr_t>(plane);
    const uintptr_t end = begin + sizeof(cplane_s) * static_cast<uintptr_t>(cm.planeCount);
    if (address < begin || address >= end || ((address - begin) % sizeof(cplane_s)) != 0)
        return UINT32_MAX;
    return static_cast<uint32_t>((address - begin) / sizeof(cplane_s));
}

// VITW v2 keeps the original pointer-free geometry records intact and adds
// the live KisakCOD sun state that the desktop renderer feeds into its world
// shader. VITW v3 appends the authored GfxSurface reflectionProbeIndex to
// each surface record. The record is deliberately scalar and little-endian:
// no GfxLight/GfxImage pointer or host-only allocation crosses into the Vita
// package.
struct VitaWorldLightingRecord
{
    float sunDirection[3];
    float sunLightColor[3];
    float ambientColor[3];
    float diffuseColor[3];
    float ambientScale;
    float diffuseFraction;
    float sunLight;
    uint32_t diffuseColorHasBeenSet;
    float angles[3];
};
static_assert(sizeof(VitaWorldLightingRecord) == 76);

// VITG is a pointer-free copy of the live GfxLightGrid.  KisakCOD stores the
// grid as a sparse row/RLE stream plus 56-direction colour records; retaining
// those exact arrays lets the Vita model-light bridge use the same authored
// ambient data without serializing any desktop pointers.  The format is kept
// as a separate World chunk so older VITW readers continue to accept their
// unchanged geometry records.
#pragma pack(push, 1)
struct VitaLightGridHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t sunPrimaryLightIndex;
    uint32_t rowAxis;
    uint32_t colAxis;
    uint16_t mins[3];
    uint16_t maxs[3];
    uint32_t rowCount;
    uint32_t rawRowDataSize;
    uint32_t entryCount;
    uint32_t colorCount;
};
#pragma pack(pop)

static_assert(sizeof(VitaLightGridHeader) == 52);

static bool DB_DumpVitaLightGrid(int file)
{
    const GfxLightGrid& source = s_world.lightGrid;
    VitaLightGridHeader header{};
    header.magic = 0x47544956; // "VITG"
    header.version = 1;
    header.flags = source.hasLightRegions ? 1u : 0u;
    header.sunPrimaryLightIndex = source.sunPrimaryLightIndex;
    header.rowAxis = source.rowAxis;
    header.colAxis = source.colAxis;
    std::memcpy(header.mins, source.mins, sizeof(header.mins));
    std::memcpy(header.maxs, source.maxs, sizeof(header.maxs));

    bool validAxes = (source.rowAxis < 3 && source.colAxis < 3
                      && source.rowAxis != source.colAxis
                      && source.rowAxis < 2 && source.colAxis < 2);
    uint32_t rowCount = 0;
    if (validAxes && source.maxs[source.rowAxis] >= source.mins[source.rowAxis])
        rowCount = static_cast<uint32_t>(source.maxs[source.rowAxis])
            - source.mins[source.rowAxis] + 1u;
    // The desktop loader caps the row axis at WORLD_SIZE >> 5 (0x2000).
    // Keep the same bounded envelope on the host export and reject malformed
    // pointers rather than emitting a package that only appears playable.
    const bool validCounts = validAxes && rowCount <= 0x2000u
        && source.rawRowDataSize <= 0x300000u
        && source.entryCount <= 1'000'000u
        && source.colorCount <= 65'536u
        && (!rowCount || source.rowDataStart)
        && (!source.rawRowDataSize || source.rawRowData)
        && (!source.entryCount || source.entries)
        && (!source.colorCount || source.colors);
    if (validCounts) {
        header.rowCount = rowCount;
        header.rawRowDataSize = source.rawRowDataSize;
        header.entryCount = source.entryCount;
        header.colorCount = source.colorCount;
    } else {
        // Preserve the source bounds/axes for diagnostics, but make the grid
        // explicitly empty when a partially initialized reference load cannot
        // provide a safe pointer-free payload.
        header.flags = 0;
        header.rowCount = 0;
        header.rawRowDataSize = 0;
        header.entryCount = 0;
        header.colorCount = 0;
    }

    if (!DB_VitaWriteValue(file, header)
        || (header.rowCount && !DB_VitaWrite(file, source.rowDataStart,
                                             header.rowCount * sizeof(uint16_t)))
        || (header.rawRowDataSize && !DB_VitaWrite(file, source.rawRowData,
                                                   header.rawRowDataSize))
        || (header.entryCount && !DB_VitaWrite(file, source.entries,
                                               header.entryCount * sizeof(GfxLightGridEntry)))
        || (header.colorCount && !DB_VitaWrite(file, source.colors,
                                               header.colorCount * sizeof(GfxLightGridColors))))
        return false;

    Com_Printf(0, "Vita light grid export: rows=%u raw=%u entries=%u colors=%u axes=%u,%u\n",
               header.rowCount, header.rawRowDataSize, header.entryCount,
               header.colorCount, header.rowAxis, header.colAxis);
    return true;
}

static bool DB_DumpVitaPrimaryLights(int file)
{
    // ComWorld owns the authored primary-light records used by both the BSP
    // light-grid lookup and the dynamic model/shadow paths. Serialize the
    // fixed fields only; defName is a host pointer and is not a lighting
    // input on Vita.
    const uint32_t sourceCount = (comWorld.isInUse && comWorld.primaryLights)
        ? comWorld.primaryLightCount : 0u;
    if (sourceCount > kisak::vita::VITA_PRIMARY_LIGHT_MAX_RECORDS)
    {
        Com_PrintError(1, "Vita primary-light export skipped: count %u exceeds %u\n",
                       sourceCount,
                       kisak::vita::VITA_PRIMARY_LIGHT_MAX_RECORDS);
        return false;
    }

    kisak::vita::VitaPrimaryLightHeader header{};
    header.magic = kisak::vita::VITA_PRIMARY_LIGHT_MAGIC;
    header.version = kisak::vita::VITA_PRIMARY_LIGHT_VERSION;
    header.recordCount = sourceCount;
    header.recordBytes = kisak::vita::VITA_PRIMARY_LIGHT_RECORD_BYTES;
    header.sunPrimaryLightIndex = sourceCount != 0
        && s_world.sunPrimaryLightIndex < sourceCount
        ? s_world.sunPrimaryLightIndex : 0u;
    if (!DB_VitaWriteValue(file, header))
        return false;

    for (uint32_t index = 0; index < sourceCount; ++index)
    {
        const ComPrimaryLight& source = comWorld.primaryLights[index];
        kisak::vita::VitaPrimaryLightRecord record{};
        record.type = source.type;
        record.canUseShadowMap = source.canUseShadowMap;
        record.exponent = source.exponent;
        record.unused = source.unused;
        std::memcpy(record.color, source.color, sizeof(record.color));
        std::memcpy(record.dir, source.dir, sizeof(record.dir));
        std::memcpy(record.origin, source.origin, sizeof(record.origin));
        record.radius = source.radius;
        record.cosHalfFovOuter = source.cosHalfFovOuter;
        record.cosHalfFovInner = source.cosHalfFovInner;
        record.cosHalfFovExpanded = source.cosHalfFovExpanded;
        record.rotationLimit = source.rotationLimit;
        record.translationLimit = source.translationLimit;
        if (!DB_VitaWriteValue(file, record))
            return false;
    }

    Com_Printf(0, "Vita primary-light export: records=%u sun=%u\n",
               header.recordCount, header.sunPrimaryLightIndex);
    return true;
}

static bool DB_DumpVitaWorldGeometry(int file)
{
    if (!s_world.vd.vertices || !s_world.indices || !s_world.dpvs.surfaces
        || !DB_VitaValidCount(s_world.vertexCount)
        || !DB_VitaValidCount(s_world.indexCount)
        || !DB_VitaValidCount(s_world.surfaceCount))
    {
        Com_PrintError(1, "Vita world export skipped: world geometry is incomplete\n");
        return false;
    }

    const uint32_t magic = 0x57544956; // "VITW"
    const uint32_t version = 3;
    const uint32_t vertexCount = static_cast<uint32_t>(s_world.vertexCount);
    const uint32_t indexCount = static_cast<uint32_t>(s_world.indexCount);
    const uint32_t surfaceCount = static_cast<uint32_t>(s_world.surfaceCount);

    VitaWorldLightingRecord lighting{};
    // R_InterpretSunLightParseParamsIntoLights is the authoritative source
    // conversion.  Prefer the resulting live GfxLight when the world owns
    // one; derive the same direction/colour only for a partially initialized
    // reference load so the sidecar remains deterministic.
    float parsedDirection[3]{};
    AngleVectors(s_world.sunParse.angles, parsedDirection, nullptr, nullptr);
    const float sunScale = s_world.sunParse.sunLight - s_world.sunParse.ambientScale;
    const float sunFactor = (1.0f - s_world.sunParse.diffuseFraction) * sunScale;
    for (size_t component = 0; component < 3; ++component)
    {
        lighting.sunDirection[component] = s_world.sunLight
            ? s_world.sunLight->dir[component] : parsedDirection[component];
        lighting.sunLightColor[component] = s_world.sunLight
            ? s_world.sunLight->color[component]
            : s_world.sunParse.sunColor[component] * sunFactor;
        lighting.ambientColor[component] = s_world.sunParse.ambientColor[component];
        lighting.diffuseColor[component] = s_world.sunParse.diffuseColor[component];
        lighting.angles[component] = s_world.sunParse.angles[component];
    }
    lighting.ambientScale = s_world.sunParse.ambientScale;
    lighting.diffuseFraction = s_world.sunParse.diffuseFraction;
    lighting.sunLight = s_world.sunParse.sunLight;
    lighting.diffuseColorHasBeenSet = s_world.sunParse.diffuseColorHasBeenSet ? 1u : 0u;
    if (!DB_VitaWriteValue(file, magic) || !DB_VitaWriteValue(file, version)
        || !DB_VitaWriteValue(file, vertexCount) || !DB_VitaWriteValue(file, indexCount)
        || !DB_VitaWriteValue(file, surfaceCount)
        || !DB_VitaWrite(file, s_world.mins, sizeof(s_world.mins))
        || !DB_VitaWrite(file, s_world.maxs, sizeof(s_world.maxs))
        || !DB_VitaWrite(file, &lighting, sizeof(lighting)))
        return false;

    Com_Printf(0,
               "Vita world lighting v3 dir=(%g,%g,%g) sun=(%g,%g,%g) ambient=%g diffuseFraction=%g sunLight=%g\n",
               lighting.sunDirection[0], lighting.sunDirection[1], lighting.sunDirection[2],
               lighting.sunLightColor[0], lighting.sunLightColor[1], lighting.sunLightColor[2],
               lighting.ambientScale, lighting.diffuseFraction, lighting.sunLight);

    for (uint32_t i = 0; i < vertexCount; ++i)
    {
        const GfxWorldVertex &vertex = s_world.vd.vertices[i];
        if (!DB_VitaWrite(file, vertex.xyz, sizeof(vertex.xyz))
            || !DB_VitaWrite(file, vertex.texCoord, sizeof(vertex.texCoord))
            || !DB_VitaWrite(file, vertex.lmapCoord, sizeof(vertex.lmapCoord))
            || !DB_VitaWriteValue(file, vertex.color.packed)
            || !DB_VitaWriteValue(file, vertex.normal.packed)
            || !DB_VitaWriteValue(file, vertex.tangent.packed))
            return false;
    }

    if (!DB_VitaWrite(file, s_world.indices, indexCount * sizeof(uint16_t)))
        return false;

    for (uint32_t i = 0; i < surfaceCount; ++i)
    {
        const GfxSurface &surface = s_world.dpvs.surfaces[i];
        const int firstVertex = surface.tris.firstVertex;
        const uint32_t surfaceVertexCount = surface.tris.vertexCount;
        const uint32_t triangleCount = surface.tris.triCount;
        const int baseIndex = surface.tris.baseIndex;
        if (firstVertex < 0 || static_cast<uint64_t>(firstVertex) + surfaceVertexCount > vertexCount
            || baseIndex < 0
            || static_cast<uint64_t>(baseIndex) + static_cast<uint64_t>(triangleCount) * 3 > indexCount)
        {
            Com_PrintError(1, "Vita world export skipped: invalid surface %u ranges\n", i);
            return false;
        }

        const uint32_t materialIndex = surface.material ? surface.material->info.hashIndex : UINT32_MAX;
        const uint32_t lightmapIndex = surface.lightmapIndex;
        if (!DB_VitaWriteValue(file, static_cast<uint32_t>(firstVertex))
            || !DB_VitaWriteValue(file, surfaceVertexCount)
            || !DB_VitaWriteValue(file, triangleCount)
            || !DB_VitaWriteValue(file, static_cast<uint32_t>(baseIndex))
            || !DB_VitaWriteValue(file, materialIndex)
            || !DB_VitaWriteValue(file, lightmapIndex))
            return false;

        char materialName[64]{};
        if (surface.material && surface.material->info.name)
            std::strncpy(materialName, surface.material->info.name, sizeof(materialName) - 1);
        if (!DB_VitaWrite(file, materialName, sizeof(materialName)))
            return false;
        if (!DB_VitaWriteValue(file,
                               static_cast<uint32_t>(surface.reflectionProbeIndex)))
            return false;
    }
    return true;
}

static bool DB_VitaCollectCollisionBrushNode(
    int rootNode, std::vector<uint8_t>* visited,
    std::vector<uint8_t>* membership)
{
    if (!visited || !membership || rootNode <= 0
        || !cm.leafbrushNodes || cm.leafbrushNodesCount == 0)
        return rootNode == 0;

    std::vector<uint32_t> work;
    work.push_back(static_cast<uint32_t>(rootNode));
    while (!work.empty()) {
        const uint32_t nodeIndex = work.back();
        work.pop_back();
        if (nodeIndex >= cm.leafbrushNodesCount)
            return false;
        if ((*visited)[nodeIndex])
            continue;
        (*visited)[nodeIndex] = 1;
        const cLeafBrushNode_s& node = cm.leafbrushNodes[nodeIndex];
        if (node.leafBrushCount > 0) {
            if (!node.data.leaf.brushes)
                return false;
            for (int item = 0; item < node.leafBrushCount; ++item) {
                const uint32_t brush = node.data.leaf.brushes[item];
                if (brush >= cm.numBrushes)
                    return false;
                (*membership)[brush] = 1;
            }
            continue;
        }

        // A negative count marks the overlapping-brush leaf placed directly
        // after this partition node. The two positive relative offsets are
        // its axial children.
        if (node.leafBrushCount < 0) {
            if (nodeIndex + 1u >= cm.leafbrushNodesCount)
                return false;
            work.push_back(nodeIndex + 1u);
        }
        for (uint32_t side = 0; side < 2; ++side) {
            const uint32_t child = node.data.children.childOffset[side];
            if (!child || child > cm.leafbrushNodesCount - nodeIndex - 1u)
                return false;
            work.push_back(nodeIndex + child);
        }
    }
    return true;
}

static bool DB_VitaBuildCollisionMembership(
    std::vector<std::vector<uint16_t>>* models)
{
    if (!models || !cm.cmodels || !cm.numSubModels || !cm.leafbrushNodes
        || cm.leafbrushNodesCount <= 1u || cm.numBrushes > 65535u)
        return false;
    models->assign(cm.numSubModels, {});
    std::vector<uint8_t> membership(cm.numBrushes, 0u);
    std::vector<uint8_t> visited(cm.leafbrushNodesCount, 0u);

    // Model zero is the static world: preserve the union of the authored
    // leaf brush trees rather than accidentally baking inline brush models
    // into world collision.
    for (uint32_t leaf = 0; leaf < cm.numLeafs; ++leaf) {
        if (cm.leafs[leaf].leafBrushNode
            && !DB_VitaCollectCollisionBrushNode(
                cm.leafs[leaf].leafBrushNode, &visited, &membership))
            return false;
    }
    for (uint32_t brush = 0; brush < cm.numBrushes; ++brush) {
        if (membership[brush])
            (*models)[0].push_back(static_cast<uint16_t>(brush));
    }
    if ((*models)[0].empty())
        return false;

    for (uint32_t model = 1; model < cm.numSubModels; ++model) {
        std::fill(membership.begin(), membership.end(), 0u);
        std::fill(visited.begin(), visited.end(), 0u);
        const int root = cm.cmodels[model].leaf.leafBrushNode;
        if (root && !DB_VitaCollectCollisionBrushNode(
                        root, &visited, &membership))
            return false;
        for (uint32_t brush = 0; brush < cm.numBrushes; ++brush) {
            if (membership[brush])
                (*models)[model].push_back(static_cast<uint16_t>(brush));
        }
    }
    return true;
}

static bool DB_DumpVitaCollisionGeometry(int file)
{
    if (!cm.isInUse || (cm.planeCount && !cm.planes) || (cm.numBrushes && !cm.brushes)
        || (cm.vertCount && !cm.verts) || (cm.triCount && !cm.triIndices)
        || !DB_VitaValidCount(cm.planeCount) || !DB_VitaValidCount(cm.numBrushes, 65535)
        || !DB_VitaValidCount(static_cast<int>(cm.vertCount))
        || !DB_VitaValidCount(cm.triCount) || !DB_VitaValidCount(cm.numSubModels, 65535))
    {
        Com_PrintError(1, "Vita collision export skipped: clip map is incomplete\n");
        return false;
    }

    uint64_t sideCount = 0;
    for (uint32_t i = 0; i < cm.numBrushes; ++i)
    {
        // A few clip-map brush records are intentionally side-less in the
        // retail data.  Preserve those records with zero sides instead of
        // making the whole export unusable.
        if (cm.brushes[i].sides && cm.brushes[i].numsides <= 65535)
            sideCount += cm.brushes[i].numsides;
    }
    if (sideCount > 2'000'000)
        return false;

    const uint32_t magic = 0x43544956; // "VITC"
    // VITC v3 appends source material surface flags and the exact brush list
    // for every cmodel. This is the minimum closure from which the Vita can
    // reconstruct genuine clipMap_t/cbrush_t roots without desktop pointers.
    const uint32_t version = 3;
    const uint32_t planeCount = static_cast<uint32_t>(cm.planeCount);
    const uint32_t brushCount = cm.numBrushes;
    const uint32_t brushSideCount = static_cast<uint32_t>(sideCount);
    const uint32_t vertexCount = cm.vertCount;
    const uint32_t triCount = static_cast<uint32_t>(cm.triCount);
    const uint32_t submodelCount = cm.numSubModels;
    std::vector<std::vector<uint16_t>> modelBrushes;
    if (!DB_VitaBuildCollisionMembership(&modelBrushes)) {
        Com_PrintError(1, "Vita collision export skipped: brush-model membership is incomplete\n");
        return false;
    }
    uint64_t membershipCount64 = 0;
    for (const auto& membership : modelBrushes)
        membershipCount64 += membership.size();
    if (membershipCount64 > 2'000'000u)
        return false;
    const uint32_t membershipCount =
        static_cast<uint32_t>(membershipCount64);
    if (!DB_VitaWriteValue(file, magic) || !DB_VitaWriteValue(file, version)
        || !DB_VitaWriteValue(file, planeCount) || !DB_VitaWriteValue(file, brushCount)
        || !DB_VitaWriteValue(file, brushSideCount) || !DB_VitaWriteValue(file, vertexCount)
        || !DB_VitaWriteValue(file, triCount) || !DB_VitaWriteValue(file, submodelCount))
        return false;

    for (uint32_t i = 0; i < planeCount; ++i)
    {
        const cplane_s &plane = cm.planes[i];
        if (!DB_VitaWrite(file, plane.normal, sizeof(plane.normal))
            || !DB_VitaWriteValue(file, plane.dist)
            || !DB_VitaWrite(file, &plane.type, sizeof(plane.type))
            || !DB_VitaWrite(file, &plane.signbits, sizeof(plane.signbits)))
            return false;
    }

    uint32_t sideOffset = 0;
    for (uint32_t i = 0; i < brushCount; ++i)
    {
        const cbrush_t &brush = cm.brushes[i];
        const uint32_t brushSideCountValue = brush.sides && brush.numsides <= 65535
            ? brush.numsides : 0;
        if (!DB_VitaWrite(file, brush.mins, sizeof(brush.mins))
            || !DB_VitaWriteValue(file, brush.contents)
            || !DB_VitaWrite(file, brush.maxs, sizeof(brush.maxs))
            || !DB_VitaWriteValue(file, sideOffset)
            || !DB_VitaWriteValue(file, brushSideCountValue))
            return false;
        sideOffset += brushSideCountValue;
    }

    for (uint32_t i = 0; i < brushCount; ++i)
    {
        const cbrush_t &brush = cm.brushes[i];
        const uint32_t brushSideCountValue = brush.sides && brush.numsides <= 65535
            ? brush.numsides : 0;
        for (uint32_t side = 0; side < brushSideCountValue; ++side)
        {
            const cbrushside_t &brushSide = brush.sides[side];
            const uint32_t planeIndex = DB_VitaPlaneIndex(brushSide.plane);
            // Keep the source index when available, but also embed the plane
            // equation so the Vita reader never depends on host addresses or
            // on the exact clip-map plane allocation strategy.
            const float zeroPlane[4] = {0, 0, 0, 0};
            const float *planeEquation = zeroPlane;
            float resolvedPlane[4]{};
            if (brushSide.plane)
            {
                resolvedPlane[0] = brushSide.plane->normal[0];
                resolvedPlane[1] = brushSide.plane->normal[1];
                resolvedPlane[2] = brushSide.plane->normal[2];
                resolvedPlane[3] = brushSide.plane->dist;
                planeEquation = resolvedPlane;
            }
            if (!DB_VitaWriteValue(file, planeIndex)
                || !DB_VitaWrite(file, planeEquation, sizeof(resolvedPlane))
                || !DB_VitaWriteValue(file, brushSide.materialNum))
                return false;
        }
    }

    if (!DB_VitaWrite(file, cm.verts, vertexCount * sizeof(float[3]))
        || !DB_VitaWrite(file, cm.triIndices, triCount * 3 * sizeof(uint16_t)))
        return false;

    for (uint32_t i = 0; i < submodelCount; ++i)
    {
        if (!DB_VitaWrite(file, cm.cmodels[i].mins, sizeof(cm.cmodels[i].mins))
            || !DB_VitaWrite(file, cm.cmodels[i].maxs, sizeof(cm.cmodels[i].maxs))
            || !DB_VitaWriteValue(file, cm.cmodels[i].radius))
            return false;
    }

    // Keep surface semantics beside the pointer-free brush sides. Movement,
    // impact events, and footstep selection consume trace.surfaceFlags even
    // though the source dmaterial_t names themselves are renderer-owned.
    for (uint32_t i = 0; i < brushCount; ++i) {
        const cbrush_t& brush = cm.brushes[i];
        const uint32_t brushSideCountValue = brush.sides
            && brush.numsides <= 65535 ? brush.numsides : 0;
        for (uint32_t side = 0; side < brushSideCountValue; ++side) {
            const uint32_t material = brush.sides[side].materialNum;
            const int32_t surfaceFlags = cm.materials
                && material < cm.numMaterials
                ? cm.materials[material].surfaceFlags : 0;
            if (!DB_VitaWriteValue(file, surfaceFlags))
                return false;
        }
    }

    if (!DB_VitaWriteValue(file, submodelCount)
        || !DB_VitaWriteValue(file, membershipCount))
        return false;
    uint32_t membershipOffset = 0;
    if (!DB_VitaWriteValue(file, membershipOffset))
        return false;
    for (const auto& membership : modelBrushes) {
        membershipOffset += static_cast<uint32_t>(membership.size());
        if (!DB_VitaWriteValue(file, membershipOffset))
            return false;
    }
    for (const auto& membership : modelBrushes) {
        if (!membership.empty()
            && !DB_VitaWrite(file, membership.data(),
                             membership.size() * sizeof(uint16_t)))
            return false;
    }
    return true;
}

static uint32_t DB_VitaAppendWorldStaticModelEntities(std::string* output);

static bool DB_DumpVitaMapEntities(int file)
{
    if (!cm.mapEnts || !cm.mapEnts->entityString || cm.mapEnts->numEntityChars <= 0
        || cm.mapEnts->numEntityChars > 2'000'000)
    {
        Com_PrintError(1, "Vita map-entity export skipped: entity string is unavailable\n");
        return false;
    }
    const uint32_t magic = 0x45544956; // "VITE"
    const uint32_t version = 1;
    std::string entityText(cm.mapEnts->entityString,
                           static_cast<size_t>(cm.mapEnts->numEntityChars));
    DB_VitaAppendWorldStaticModelEntities(&entityText);
    if (entityText.size() > UINT32_MAX)
        return false;
    const uint32_t bytes = static_cast<uint32_t>(entityText.size());
    return DB_VitaWriteValue(file, magic) && DB_VitaWriteValue(file, version)
        && DB_VitaWriteValue(file, bytes)
        && DB_VitaWrite(file, entityText.data(), bytes);
}

using VitaWeaponRecord = kisak::vita::VitaWeaponRecordV5;

static_assert(sizeof(VitaWeaponRecord) == kisak::vita::kVitaWeaponRecordV5Bytes);

static bool DB_VitaTargetWeapon(const char* name);

template <size_t N>
static void DB_VitaCopyWeaponName(char (&destination)[N], const char* source)
{
    if (source)
        std::strncpy(destination, source, N - 1u);
}

static const char* DB_VitaWeaponModelName(const XModel* model)
{
    return model ? model->name : nullptr;
}

static const char* DB_VitaWeaponMaterialName(const Material* material)
{
    return material ? material->info.name : nullptr;
}

static const char* DB_VitaWeaponEffectName(const FxEffectDef* effect)
{
    return effect ? effect->name : nullptr;
}

static const char* DB_VitaWeaponSoundName(const snd_alias_list_t* sound)
{
    return sound ? sound->aliasName : nullptr;
}

static bool DB_VitaBuildWeaponHideSurfaceMask(const WeaponDef& weapon,
                                              VitaWeaponRecord* record)
{
    if (!record || !weapon.gunXModel[0])
        return false;
    const XModel* model = weapon.gunXModel[0];
    if (model->name)
        std::strncpy(record->viewModelName, model->name,
                     sizeof(record->viewModelName) - 1);
    const XModelLodInfo& lod = model->lodInfo[0];
    if (!model->surfs || lod.surfIndex > model->numsurfs
        || static_cast<uint32_t>(lod.surfIndex) + lod.numsurfs > model->numsurfs)
        return false;

    uint32_t hidePartBits[4] = {};
    for (uint32_t tagIndex = 0; tagIndex < 8 && weapon.hideTags[tagIndex]; ++tagIndex) {
        ++record->hideTagCount;
        const char* tagName = SL_ConvertToString(weapon.hideTags[tagIndex]);
        bool resolved = false;
        if (tagName && model->boneNames) {
            for (uint32_t boneIndex = 0; boneIndex < model->numBones; ++boneIndex) {
                const char* boneName = SL_ConvertToString(model->boneNames[boneIndex]);
                if (boneName && !I_stricmp(boneName, tagName)) {
                    hidePartBits[boneIndex >> 5] |=
                        0x80000000u >> (boneIndex & 0x1F);
                    resolved = true;
                    break;
                }
            }
        }
        if (resolved)
            ++record->hideTagResolvedCount;
    }

    // DB_VitaBuildModel emits valid LOD0 surfaces in this same order. Keep
    // the mask indexed by that exported order, not by the model's global
    // surface array, so VitaModelRecord can consume it directly.
    for (uint32_t localSurface = 0; localSurface < lod.numsurfs; ++localSurface) {
        const uint32_t surfaceIndex = static_cast<uint32_t>(lod.surfIndex)
            + localSurface;
        const XSurface& surface = model->surfs[surfaceIndex];
        if (!surface.verts0 || !surface.triIndices || surface.vertCount == 0
            || surface.triCount == 0)
            continue;
        if (record->viewModelSurfaceCount < 64u) {
            const uint32_t part0 = static_cast<uint32_t>(surface.partBits[0]);
            const uint32_t part1 = static_cast<uint32_t>(surface.partBits[1]);
            const uint32_t part2 = static_cast<uint32_t>(surface.partBits[2]);
            const uint32_t part3 = static_cast<uint32_t>(surface.partBits[3]);
            if ((part0 & hidePartBits[0]) || (part1 & hidePartBits[1])
                || (part2 & hidePartBits[2]) || (part3 & hidePartBits[3]))
                record->hideSurfaceMask |= (uint64_t{1} << record->viewModelSurfaceCount);
        }
        ++record->viewModelSurfaceCount;
    }
    return record->viewModelSurfaceCount <= 64u;
}

struct VitaWeaponDumpContext
{
    int file;
    bool write;
    uint32_t count;
};

static void __cdecl DB_DumpVitaWeapon(XAssetHeader header, void *rawContext)
{
    auto *context = static_cast<VitaWeaponDumpContext *>(rawContext);
    if (!context || !header.weapon
        || !DB_VitaTargetWeapon(header.weapon->szInternalName))
        return;
    if (!context->write)
    {
        ++context->count;
        return;
    }

    const WeaponDef &weapon = *header.weapon;
    VitaWeaponRecord record{};
    DB_VitaCopyWeaponName(record.name, weapon.szInternalName);
    record.weaponType = static_cast<uint32_t>(weapon.weapType);
    record.weaponClass = static_cast<uint32_t>(weapon.weapClass);
    record.fireType = static_cast<uint32_t>(weapon.fireType);
    record.inventoryType = static_cast<uint32_t>(weapon.inventoryType);
    record.stance = static_cast<uint32_t>(weapon.stance);
    record.startAmmo = weapon.iStartAmmo;
    record.maxAmmo = weapon.iMaxAmmo;
    record.clipSize = weapon.iClipSize;
    record.damage = weapon.damage;
    record.playerDamage = weapon.playerDamage;
    record.fireDelay = weapon.iFireDelay;
    record.fireTime = weapon.iFireTime;
    record.rechamberTime = weapon.iRechamberTime;
    record.reloadTime = weapon.iReloadTime;
    record.reloadEmptyTime = weapon.iReloadEmptyTime;
    record.adsTransInTime = weapon.iAdsTransInTime;
    record.adsTransOutTime = weapon.iAdsTransOutTime;
    record.moveSpeedScale = weapon.moveSpeedScale;
    record.adsMoveSpeedScale = weapon.adsMoveSpeedScale;
    record.sprintDurationScale = weapon.sprintDurationScale;
    record.adsZoomFov = weapon.fAdsZoomFov;
    record.hipSpreadStandMin = weapon.fHipSpreadStandMin;
    record.hipSpreadStandMax = weapon.hipSpreadStandMax;
    record.adsSwayMaxAngle = weapon.adsSwayMaxAngle;
    record.freezeMovementWhenFiring = weapon.freezeMovementWhenFiring;

    record.sourceSchemaVersion = kisak::vita::kVitaWeaponVersionSource;
    record.sourceLayoutVersion = kisak::vita::kVitaWeaponSourceLayoutVersion;
    record.sourceFlags = kisak::vita::kVitaWeaponSourceRequiredFlags;

#define KISAK_VITA_EXPORT_WEAPON_INT(label, member)                        \
    record.integerFields[static_cast<uint32_t>(                             \
        kisak::vita::VitaWeaponIntField::label)] =                         \
        static_cast<int32_t>(weapon.member);
    KISAK_VITA_WEAPON_INT_FIELDS(KISAK_VITA_EXPORT_WEAPON_INT)
#undef KISAK_VITA_EXPORT_WEAPON_INT

#define KISAK_VITA_EXPORT_WEAPON_FLOAT(label, member)                      \
    record.floatFields[static_cast<uint32_t>(                               \
        kisak::vita::VitaWeaponFloatField::label)] =                       \
        static_cast<float>(weapon.member);
    KISAK_VITA_WEAPON_FLOAT_FIELDS(KISAK_VITA_EXPORT_WEAPON_FLOAT)
#undef KISAK_VITA_EXPORT_WEAPON_FLOAT

#define KISAK_VITA_EXPORT_WEAPON_STRING(label, member)                     \
    DB_VitaCopyWeaponName(                                                  \
        record.stringNames[static_cast<uint32_t>(                           \
            kisak::vita::VitaWeaponStringField::label)], weapon.member);
    KISAK_VITA_WEAPON_STRING_FIELDS(KISAK_VITA_EXPORT_WEAPON_STRING)
#undef KISAK_VITA_EXPORT_WEAPON_STRING

    for (uint32_t index = 0; index < kisak::vita::kVitaWeaponGunModelCount;
         ++index) {
        DB_VitaCopyWeaponName(record.gunModelNames[index],
                              DB_VitaWeaponModelName(weapon.gunXModel[index]));
    }
    DB_VitaCopyWeaponName(record.handModelName,
                          DB_VitaWeaponModelName(weapon.handXModel));
    for (uint32_t index = 0; index < kisak::vita::kVitaWeaponWorldModelCount;
         ++index) {
        DB_VitaCopyWeaponName(record.worldModelNames[index],
                              DB_VitaWeaponModelName(weapon.worldModel[index]));
    }
    DB_VitaCopyWeaponName(record.worldClipModelName,
                          DB_VitaWeaponModelName(weapon.worldClipModel));
    DB_VitaCopyWeaponName(record.rocketModelName,
                          DB_VitaWeaponModelName(weapon.rocketModel));
    DB_VitaCopyWeaponName(record.knifeModelName,
                          DB_VitaWeaponModelName(weapon.knifeModel));
    DB_VitaCopyWeaponName(record.worldKnifeModelName,
                          DB_VitaWeaponModelName(weapon.worldKnifeModel));
    DB_VitaCopyWeaponName(record.projectileModelName,
                          DB_VitaWeaponModelName(weapon.projectileModel));

    for (uint32_t index = 0; index < kisak::vita::kVitaWeaponAnimationCount;
         ++index) {
        DB_VitaCopyWeaponName(record.viewAnimationNames[index],
                              weapon.szXAnims[index]);
    }
    for (uint32_t index = 0; index < kisak::vita::kVitaWeaponHideTagCount;
         ++index) {
        DB_VitaCopyWeaponName(record.hideTagNames[index],
                              weapon.hideTags[index]
                                  ? SL_ConvertToString(weapon.hideTags[index])
                                  : nullptr);
    }
    for (uint32_t index = 0; index < kisak::vita::kVitaWeaponNotetrackCount;
         ++index) {
        DB_VitaCopyWeaponName(record.notetrackSoundKeyNames[index],
            weapon.notetrackSoundMapKeys[index]
                ? SL_ConvertToString(weapon.notetrackSoundMapKeys[index])
                : nullptr);
        DB_VitaCopyWeaponName(record.notetrackSoundValueNames[index],
            weapon.notetrackSoundMapValues[index]
                ? SL_ConvertToString(weapon.notetrackSoundMapValues[index])
                : nullptr);
    }

#define KISAK_VITA_EXPORT_WEAPON_MATERIAL(label, member)                   \
    DB_VitaCopyWeaponName(                                                  \
        record.materialNames[static_cast<uint32_t>(                         \
            kisak::vita::VitaWeaponMaterialField::label)],                 \
        DB_VitaWeaponMaterialName(weapon.member));
    KISAK_VITA_WEAPON_MATERIAL_FIELDS(KISAK_VITA_EXPORT_WEAPON_MATERIAL)
#undef KISAK_VITA_EXPORT_WEAPON_MATERIAL

#define KISAK_VITA_EXPORT_WEAPON_EFFECT(label, member)                     \
    DB_VitaCopyWeaponName(                                                  \
        record.effectNames[static_cast<uint32_t>(                           \
            kisak::vita::VitaWeaponEffectField::label)],                   \
        DB_VitaWeaponEffectName(weapon.member));
    KISAK_VITA_WEAPON_EFFECT_FIELDS(KISAK_VITA_EXPORT_WEAPON_EFFECT)
#undef KISAK_VITA_EXPORT_WEAPON_EFFECT

#define KISAK_VITA_EXPORT_WEAPON_SOUND(label, member)                      \
    DB_VitaCopyWeaponName(                                                  \
        record.soundNames[static_cast<uint32_t>(                            \
            kisak::vita::VitaWeaponSoundField::label)],                    \
        DB_VitaWeaponSoundName(weapon.member));
    KISAK_VITA_WEAPON_SOUND_FIELDS(KISAK_VITA_EXPORT_WEAPON_SOUND)
#undef KISAK_VITA_EXPORT_WEAPON_SOUND

    if (weapon.bounceSound) {
        for (uint32_t index = 0;
             index < kisak::vita::kVitaWeaponBounceSoundCount; ++index) {
            DB_VitaCopyWeaponName(record.bounceSoundNames[index],
                DB_VitaWeaponSoundName(weapon.bounceSound[index]));
        }
    }
    for (uint32_t index = 0; index < 2; ++index)
        DB_VitaCopyWeaponName(record.accuracyGraphNames[index],
                              weapon.accuracyGraphName[index]);

    std::memcpy(record.movementVectors[0], weapon.vStandMove,
                sizeof(weapon.vStandMove));
    std::memcpy(record.movementVectors[1], weapon.vStandRot,
                sizeof(weapon.vStandRot));
    std::memcpy(record.movementVectors[2], weapon.vDuckedOfs,
                sizeof(weapon.vDuckedOfs));
    std::memcpy(record.movementVectors[3], weapon.vDuckedMove,
                sizeof(weapon.vDuckedMove));
    std::memcpy(record.movementVectors[4], weapon.vDuckedRot,
                sizeof(weapon.vDuckedRot));
    std::memcpy(record.movementVectors[5], weapon.vProneOfs,
                sizeof(weapon.vProneOfs));
    std::memcpy(record.movementVectors[6], weapon.vProneMove,
                sizeof(weapon.vProneMove));
    std::memcpy(record.movementVectors[7], weapon.vProneRot,
                sizeof(weapon.vProneRot));
    std::memcpy(record.parallelBounce, weapon.parallelBounce,
                sizeof(record.parallelBounce));
    std::memcpy(record.perpendicularBounce, weapon.perpendicularBounce,
                sizeof(record.perpendicularBounce));
    std::memcpy(record.projectileColor, weapon.vProjectileColor,
                sizeof(record.projectileColor));
    std::memcpy(record.locationDamageMultipliers,
                weapon.locationDamageMultipliers,
                sizeof(record.locationDamageMultipliers));

    DB_VitaBuildWeaponHideSurfaceMask(weapon, &record);
    ++context->count;
    DB_VitaWrite(context->file, &record, sizeof(record));
}

static bool DB_DumpVitaWeapons(int file)
{
    VitaWeaponDumpContext countContext{file, false, 0};
    DB_EnumXAssets(ASSET_TYPE_WEAPON, DB_DumpVitaWeapon, &countContext, 0);
    const uint32_t magic = kisak::vita::kVitaWeaponMagic;
    const uint32_t version = kisak::vita::kVitaWeaponVersionSource;
    const uint32_t recordBytes = sizeof(VitaWeaponRecord);
    if (!DB_VitaWriteValue(file, magic) || !DB_VitaWriteValue(file, version)
        || !DB_VitaWriteValue(file, countContext.count)
        || !DB_VitaWriteValue(file, recordBytes))
        return false;
    VitaWeaponDumpContext writeContext{file, true, 0};
    DB_EnumXAssets(ASSET_TYPE_WEAPON, DB_DumpVitaWeapon, &writeContext, 0);
    return writeContext.count == countContext.count;
}

// Shared bounded-string helpers are defined with the menu exporter below.
static bool DB_VitaBoundedString(const char* value, uint32_t* bytes);
static bool DB_VitaWriteString(int file, const char* value, uint32_t bytes);

// VITQ v1 is the bounded animation metadata stream retained for compatibility
// with older Vita packages.  The actual XAnimParts arrays contain desktop
// pointers and are not copied; this record preserves clip identity, timing,
// looping, delta flag, and compression shape for the legacy clock-only path.
#pragma pack(push, 1)
struct VitaAnimationRecord
{
    char name[128];
    uint16_t numframes;
    uint8_t bLoop;
    uint8_t bDelta;
    uint8_t boneCount[10];
    uint8_t notifyCount;
    uint8_t assetType;
    uint16_t reserved;
    uint32_t dataByteCount;
    uint32_t dataShortCount;
    uint32_t dataIntCount;
    uint32_t randomDataByteCount;
    uint32_t randomDataIntCount;
    uint32_t randomDataShortCount;
    uint32_t indexCount;
    float framerate;
    float frequency;
};
#pragma pack(pop)

static_assert(sizeof(VitaAnimationRecord) == 182);

// VITQ v2 stores host-decoded XAnim samples rather than the desktop
// XAnimParts pointer graph.  Each track is associated with one exported
// XModel, so the Vita runtime can apply the same clip to the hand and gun
// submodels that make up the COD4 viewmodel DObj.
#pragma pack(push, 1)
struct VitaAnimationRecordV2
{
    char name[128];
    uint16_t numframes;
    uint8_t bLoop;
    uint8_t bDelta;
    uint8_t boneCount[10];
    uint8_t notifyCount;
    uint8_t assetType;
    uint16_t reserved;
    uint32_t trackCount;
    uint32_t payloadBytes;
    float framerate;
    float frequency;
};

struct VitaAnimationTrackHeader
{
    uint32_t modelNameBytes;
    uint32_t boneCount;
    uint32_t frameCount;
    uint32_t reserved;
};

struct VitaAnimationSample
{
    float quat[4];
    float trans[3];
    float transWeight;
};
#pragma pack(pop)

static_assert(sizeof(VitaAnimationRecordV2) == 162);
static_assert(sizeof(VitaAnimationTrackHeader) == 16);
static_assert(sizeof(VitaAnimationSample) == 32);

struct VitaAnimationTrackBuild
{
    std::string modelName;
    uint32_t boneCount = 0;
    uint32_t frameCount = 0;
    std::vector<VitaAnimationSample> samples;
};

struct VitaAnimationNotifyBuild
{
    std::string name;
    float time = 0.0f;
};

struct VitaAnimationDeltaBuild
{
    kisak::vita::VitaAnimationDeltaV3 record{};
    const void* transFrames = nullptr;
    const void* transIndices = nullptr;
    const void* quatFrames = nullptr;
    const void* quatIndices = nullptr;
};

struct VitaAnimationBuild
{
    const XAnimParts* source = nullptr;
    std::string name;
    uint16_t numframes = 0;
    bool loop = false;
    bool delta = false;
    bool isDefault = false;
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
    std::vector<VitaAnimationTrackBuild> tracks;
    std::vector<VitaAnimationNotifyBuild> notifies;
    VitaAnimationDeltaBuild deltaPart{};
    uint32_t nativePayloadBytes = 0;
    uint32_t sampledPayloadBytes = 0;
    uint32_t deltaPayloadBytes = 0;
};

struct VitaAnimationCollectContext
{
    std::unordered_set<std::string> names;
    std::vector<const XAnimParts*> parts;
    std::unordered_set<const XAnimParts*> seen;
    std::unordered_set<const XAnimParts*> sampledParts;
    std::vector<const XModel*> models;
    std::unordered_set<const XModel*> seenModels;
    const char* playerAnimScript = nullptr;
    uint32_t playerAnimScriptBytes = 0;
};

static bool DB_VitaIsScriptIdentifierCharacter(char value)
{
    return (value >= 'a' && value <= 'z')
        || (value >= 'A' && value <= 'Z')
        || (value >= '0' && value <= '9')
        || value == '_';
}

static char DB_VitaAsciiLower(char value)
{
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value + ('a' - 'A'))
        : value;
}

static bool DB_VitaPlayerAnimScriptReferences(
    const VitaAnimationCollectContext* context, const char* animationName)
{
    uint32_t nameBytes = 0;
    if (!context || !context->playerAnimScript
        || context->playerAnimScriptBytes == 0
        || !DB_VitaBoundedString(animationName, &nameBytes)
        || nameBytes == 0 || nameBytes > 127
        || nameBytes > context->playerAnimScriptBytes)
        return false;

    const char* script = context->playerAnimScript;
    const uint32_t limit = context->playerAnimScriptBytes - nameBytes;
    for (uint32_t offset = 0; offset <= limit; ++offset) {
        if (offset > 0 && DB_VitaIsScriptIdentifierCharacter(script[offset - 1]))
            continue;
        if (offset + nameBytes < context->playerAnimScriptBytes
            && DB_VitaIsScriptIdentifierCharacter(script[offset + nameBytes]))
            continue;
        uint32_t character = 0;
        for (; character < nameBytes; ++character) {
            if (DB_VitaAsciiLower(script[offset + character])
                != DB_VitaAsciiLower(animationName[character]))
                break;
        }
        if (character == nameBytes)
            return true;
    }
    return false;
}

static void __cdecl DB_VitaCollectTargetAnimationNames(XAssetHeader header,
                                                        void* rawContext)
{
    auto* context = static_cast<VitaAnimationCollectContext*>(rawContext);
    const WeaponDef* weapon = header.weapon;
    if (!context || !weapon || !DB_VitaTargetWeapon(weapon->szInternalName))
        return;
    for (uint32_t modelIndex = 0; modelIndex < 16; ++modelIndex) {
        const XModel* model = weapon->gunXModel[modelIndex];
        if (model && model->name && context->seenModels.insert(model).second)
            context->models.push_back(model);
    }
    if (weapon->handXModel && weapon->handXModel->name
        && context->seenModels.insert(weapon->handXModel).second)
        context->models.push_back(weapon->handXModel);
    for (uint32_t modelIndex = 0; modelIndex < 16; ++modelIndex) {
        const XModel* model = weapon->worldModel[modelIndex];
        if (model && model->name && context->seenModels.insert(model).second)
            context->models.push_back(model);
    }
    const XModel* extraModels[] = {
        weapon->worldClipModel, weapon->rocketModel, weapon->knifeModel,
        weapon->worldKnifeModel, weapon->projectileModel,
    };
    for (const XModel* model : extraModels) {
        if (model && model->name && context->seenModels.insert(model).second)
            context->models.push_back(model);
    }
    for (const char* animationName : weapon->szXAnims)
    {
        uint32_t bytes = 0;
        if (DB_VitaBoundedString(animationName, &bytes) && bytes > 0 && bytes <= 127)
            context->names.emplace(animationName, bytes);
    }
}

static void __cdecl DB_VitaCollectTargetAnimation(XAssetHeader header, void* rawContext)
{
    auto* context = static_cast<VitaAnimationCollectContext*>(rawContext);
    const XAnimParts* parts = header.parts;
    if (!context || !parts || !parts->name || !*parts->name
        || context->parts.size() >= 2048)
        return;
    // Weapon szXAnims is the authoritative closure when the desktop loader
    // has registered every variant.  Some MP boots retain only the currently
    // selected weapon's list, however, while the shared XAnim pool still has
    // the other first-person clips.  Keep the explicit list and add the
    // bounded authored viewmodel families so the Vita closure does not silently
    // lose M4/USP or hands animations just because the oracle boot selected
    // MP5 first.
    const bool baseM4 = !I_strnicmp(parts->name, "viewmodel_m4_", 13);
    const bool baseMp5 = !I_strnicmp(parts->name, "viewmodel_mp5_", 14)
        && I_strnicmp(parts->name, "viewmodel_mp5_acog_", 19)
        && I_strnicmp(parts->name, "viewmodel_mp5_reflex_", 20)
        && I_strnicmp(parts->name, "viewmodel_mp5_sd_", 17);
    const bool baseUsp = !I_strnicmp(parts->name, "viewmodel_usp_", 14);
    const bool targetPrefix = baseM4 || baseMp5 || baseUsp
        || !I_strnicmp(parts->name, "viewmodel_hands", 15);
    // Mantle_CreateAnims resolves these leaf assets directly during server
    // and client initialization, before a player weapon can drive an anim.
    // They are therefore part of the offline gameplay closure even though no
    // WeaponDef::szXAnims slot names them. Keep the retail MP workaround that
    // intentionally uses player_mantle_over_low for the low over transition.
    static constexpr const char* kMantleAnimations[] = {
        "mp_mantle_up_57",
        "mp_mantle_up_51",
        "mp_mantle_up_45",
        "mp_mantle_up_39",
        "mp_mantle_up_33",
        "mp_mantle_up_27",
        "mp_mantle_up_21",
        "mp_mantle_over_high",
        "mp_mantle_over_mid",
        "player_mantle_over_low",
    };
    const bool mantleRuntime = std::any_of(
        std::begin(kMantleAnimations), std::end(kMantleAnimations),
        [parts](const char* name) {
            return !I_stricmp(name, parts->name);
        });
    // WeaponDef::szXAnims preserves fastfile spelling while the XAnim asset
    // table often canonicalizes it (notably M4m203 vs m4m203). The source DB
    // lookup is case-insensitive, so the export closure must be as well.
    const bool explicitlyNamed = std::any_of(
        context->names.begin(), context->names.end(),
        [parts](const std::string& name) {
            return !I_stricmp(name.c_str(), parts->name);
        });
    const bool playerAnimRuntime = DB_VitaPlayerAnimScriptReferences(
        context, parts->name);
    if (!explicitlyNamed && !targetPrefix && !mantleRuntime && !playerAnimRuntime)
        return;
    if (explicitlyNamed || targetPrefix)
        context->sampledParts.insert(parts);
    if (context->seen.insert(parts).second)
        context->parts.push_back(parts);
}

static bool DB_VitaBuildAnimationTrack(const XAnimParts* parts, const XModel* model,
                                       VitaAnimationTrackBuild* output)
{
    if (!parts || !model || !output || !parts->names || !model->boneNames
        || !model->name || !*model->name || model->numBones == 0
        || model->numBones > DOBJ_MAX_PARTS || parts->numframes == 0
        || parts->numframes > 256
        || parts->boneCount[9] == 0 || parts->boneCount[9] > DOBJ_MAX_PARTS)
        return false;

    // XAnimGetAnimMap's serialized sentinel is 127 (not the DObj public
    // NO_BONEINDEX=255 value); the calculator masks that index through the
    // ignore-part bitset below.
    std::vector<unsigned __int8> animToModel(parts->boneCount[9], 127u);
    uint32_t mappedParts = 0;
    for (uint32_t part = 0; part < parts->boneCount[9]; ++part) {
        const uint16_t partName = parts->names[part];
        for (uint32_t bone = 0; bone < model->numBones; ++bone) {
            if (model->boneNames[bone] == partName) {
                animToModel[part] = static_cast<unsigned __int8>(bone);
                ++mappedParts;
                break;
            }
        }
    }
    if (mappedParts == 0)
        return false;

    output->modelName.assign(model->name);
    output->boneCount = model->numBones;
    output->frameCount = parts->numframes;
    const uint64_t sampleCount = static_cast<uint64_t>(output->boneCount)
        * output->frameCount;
    if (sampleCount == 0 || sampleCount > 128ull * 256ull)
        return false;
    output->samples.resize(static_cast<size_t>(sampleCount));

    std::vector<DObjAnimMat> matrices(output->boneCount);
    for (uint32_t frame = 0; frame < output->frameCount; ++frame) {
        float time = output->frameCount > 1
            ? static_cast<float>(frame) / static_cast<float>(output->frameCount - 1)
            : 0.0f;
        // XAnimCalcParts intentionally accepts a normalized time below one;
        // the last sample is an epsilon below the non-loop endpoint so both
        // looped and non-looped clips use the same decoder path.
        time = time < 0.999999f ? time : 0.999999f;
        XAnimSampleParts(parts, animToModel.data(), time, matrices.data(),
                         output->boneCount);
        for (uint32_t bone = 0; bone < output->boneCount; ++bone) {
            VitaAnimationSample sample{};
            const DObjAnimMat& source = matrices[bone];
            std::memcpy(sample.quat, source.quat, sizeof(sample.quat));
            std::memcpy(sample.trans, source.trans, sizeof(sample.trans));
            sample.transWeight = source.transWeight;
            float quatLength = 0.0f;
            for (float component : sample.quat)
                quatLength += component * component;
            if (!std::isfinite(quatLength)) {
                // A malformed sample is treated as an untouched channel. Do
                // not synthesize an identity quaternion here: the Vita pose
                // reader uses a zero quaternion to distinguish an absent
                // XAnim channel from an authored identity rotation and must
                // preserve that bone's bind-local orientation.
                std::memset(sample.quat, 0, sizeof(sample.quat));
            } else if (quatLength >= 0.000001f) {
                const float inverseLength = 1.0f / std::sqrt(quatLength);
                for (float& component : sample.quat)
                    component *= inverseLength;
            }
            for (float& component : sample.trans) {
                if (!std::isfinite(component))
                    component = 0.0f;
            }
            if (!std::isfinite(sample.transWeight) || sample.transWeight < 0.0f)
                sample.transWeight = 0.0f;
            output->samples[static_cast<size_t>(frame) * output->boneCount + bone] = sample;
        }
    }
    return true;
}

static bool DB_VitaBuildAnimationSource(const XAnimParts* parts,
                                        VitaAnimationBuild* output)
{
    if (!parts || !output || !parts->name || !parts->name[0]
        || !std::isfinite(parts->framerate)
        || !std::isfinite(parts->frequency)
        || parts->framerate < 0.0f || parts->frequency < 0.0f
        || parts->boneCount[9] > DOBJ_MAX_PARTS
        || parts->notifyCount > 128)
        return false;

    uint32_t nameBytes = 0;
    if (!DB_VitaBoundedString(parts->name, &nameBytes)
        || nameBytes == 0 || nameBytes >= sizeof(kisak::vita::VitaAnimationRecordV3::name))
        return false;

    uint32_t quatParts = 0;
    uint32_t transParts = 0;
    for (uint32_t index = 0; index < 5; ++index)
        quatParts += parts->boneCount[index];
    for (uint32_t index = 5; index < 9; ++index)
        transParts += parts->boneCount[index];
    if (quatParts != parts->boneCount[9] || transParts != parts->boneCount[9])
        return false;

    const auto hasArray = [](const void* data, uint64_t count,
                             uint64_t elementBytes) {
        return count == 0 || (data && count <= UINT32_MAX / elementBytes);
    };
    if (!hasArray(parts->dataByte, parts->dataByteCount, 1)
        || !hasArray(parts->dataShort, parts->dataShortCount, sizeof(int16_t))
        || !hasArray(parts->dataInt, parts->dataIntCount, sizeof(int32_t))
        || !hasArray(parts->randomDataByte, parts->randomDataByteCount, 1)
        || !hasArray(parts->randomDataShort, parts->randomDataShortCount,
                     sizeof(int16_t))
        || !hasArray(parts->randomDataInt, parts->randomDataIntCount,
                     sizeof(int32_t))
        || !hasArray(parts->indices.data, parts->indexCount,
                     parts->numframes >= 256 ? sizeof(uint16_t) : 1u))
        return false;

    output->source = parts;
    output->name.assign(parts->name, nameBytes);
    output->numframes = parts->numframes;
    output->loop = parts->bLoop;
    output->delta = parts->bDelta;
    output->isDefault = parts->isDefault;
    std::memcpy(output->boneCount, parts->boneCount,
                sizeof(output->boneCount));
    output->notifyCount = parts->notifyCount;
    output->assetType = parts->assetType;
    output->dataByteCount = parts->dataByteCount;
    output->dataShortCount = parts->dataShortCount;
    output->dataIntCount = parts->dataIntCount;
    output->randomDataByteCount = parts->randomDataByteCount;
    output->randomDataIntCount = parts->randomDataIntCount;
    output->randomDataShortCount = parts->randomDataShortCount;
    output->indexCount = parts->indexCount;
    output->framerate = parts->framerate;
    output->frequency = parts->frequency;

    if (parts->boneCount[9] != 0 && !parts->names)
        return false;
    output->boneNames.reserve(parts->boneCount[9]);
    for (uint32_t index = 0; index < parts->boneCount[9]; ++index) {
        const char* boneName = SL_ConvertToString(parts->names[index]);
        uint32_t boneNameBytes = 0;
        if (!boneName || !DB_VitaBoundedString(boneName, &boneNameBytes)
            || boneNameBytes == 0
            || boneNameBytes >= kisak::vita::kVitaAnimationBoneNameBytes)
            return false;
        output->boneNames.emplace_back(boneName, boneNameBytes);
    }

    if (parts->notifyCount != 0 && !parts->notify)
        return false;
    output->notifies.reserve(parts->notifyCount);
    float previousNotifyTime = -1.0f;
    for (uint32_t index = 0; index < parts->notifyCount; ++index) {
        const XAnimNotifyInfo& source = parts->notify[index];
        const char* notifyName = SL_ConvertToString(source.name);
        uint32_t notifyNameBytes = 0;
        if (!notifyName
            || !DB_VitaBoundedString(notifyName, &notifyNameBytes)
            || notifyNameBytes == 0
            || notifyNameBytes >= kisak::vita::kVitaAnimationNotifyNameBytes
            || !std::isfinite(source.time)
            || source.time < 0.0f || source.time > 1.0f
            || source.time < previousNotifyTime)
            return false;
        output->notifies.push_back(
            {std::string(notifyName, notifyNameBytes), source.time});
        previousNotifyTime = source.time;
    }

    uint64_t deltaBytes = 0;
    if (parts->bDelta) {
        if (!parts->deltaPart)
            return false;
        VitaAnimationDeltaBuild& delta = output->deltaPart;
        const uint32_t indexStride = parts->numframes >= 256 ? 2u : 1u;
        deltaBytes = sizeof(delta.record);
        if (parts->deltaPart->trans) {
            const XAnimPartTrans* trans = parts->deltaPart->trans;
            if (trans->smallTrans > 1 || trans->size > parts->numframes)
                return false;
            delta.record.hasTrans = 1;
            delta.record.transSmall = trans->smallTrans;
            delta.record.transSize = trans->size;
            if (trans->size == 0) {
                std::memcpy(delta.record.transBase, trans->u.frame0,
                            sizeof(delta.record.transBase));
            } else {
                const uint64_t sampleCount = static_cast<uint64_t>(trans->size) + 1u;
                const uint64_t frameBytes = sampleCount
                    * (trans->smallTrans ? 3u : 6u);
                const uint64_t indexBytes = sampleCount * indexStride;
                if (!trans->u.frames.frames._1
                    || frameBytes > UINT32_MAX || indexBytes > UINT32_MAX)
                    return false;
                std::memcpy(delta.record.transBase, trans->u.frames.mins,
                            sizeof(delta.record.transBase));
                std::memcpy(delta.record.transRange, trans->u.frames.size,
                            sizeof(delta.record.transRange));
                delta.record.transFrameBytes = static_cast<uint32_t>(frameBytes);
                delta.record.transIndexBytes = static_cast<uint32_t>(indexBytes);
                delta.transFrames = trans->u.frames.frames._1;
                delta.transIndices = &trans->u.frames.indices;
                deltaBytes += frameBytes + indexBytes;
            }
        }
        if (parts->deltaPart->quat) {
            const XAnimDeltaPartQuat* quat = parts->deltaPart->quat;
            if (quat->size > parts->numframes)
                return false;
            delta.record.hasQuat = 1;
            delta.record.quatSize = quat->size;
            if (quat->size == 0) {
                std::memcpy(delta.record.quatFrame0, quat->u.frame0,
                            sizeof(delta.record.quatFrame0));
            } else {
                const uint64_t sampleCount = static_cast<uint64_t>(quat->size) + 1u;
                const uint64_t frameBytes = sampleCount * 2u * sizeof(int16_t);
                const uint64_t indexBytes = sampleCount * indexStride;
                if (!quat->u.frames.frames
                    || frameBytes > UINT32_MAX || indexBytes > UINT32_MAX)
                    return false;
                delta.record.quatFrameBytes = static_cast<uint32_t>(frameBytes);
                delta.record.quatIndexBytes = static_cast<uint32_t>(indexBytes);
                delta.quatFrames = quat->u.frames.frames;
                delta.quatIndices = &quat->u.frames.indices;
                deltaBytes += frameBytes + indexBytes;
            }
        }
    } else if (parts->deltaPart) {
        // Do not serialize an unreachable pointer graph whose flag says it
        // cannot be consumed by XAnim_CalcDeltaForTime.
        return false;
    }

    uint64_t nativeBytes = static_cast<uint64_t>(output->boneNames.size())
        * sizeof(kisak::vita::VitaAnimationBoneNameV3);
    nativeBytes += parts->dataByteCount;
    nativeBytes += static_cast<uint64_t>(parts->dataShortCount) * sizeof(int16_t);
    nativeBytes += static_cast<uint64_t>(parts->dataIntCount) * sizeof(int32_t);
    nativeBytes += parts->randomDataByteCount;
    nativeBytes += static_cast<uint64_t>(parts->randomDataShortCount)
        * sizeof(int16_t);
    nativeBytes += static_cast<uint64_t>(parts->randomDataIntCount)
        * sizeof(int32_t);
    nativeBytes += static_cast<uint64_t>(parts->indexCount)
        * (parts->numframes >= 256 ? sizeof(uint16_t) : 1u);
    nativeBytes += static_cast<uint64_t>(output->notifies.size())
        * sizeof(kisak::vita::VitaAnimationNotifyV3);
    nativeBytes += deltaBytes;
    if (nativeBytes > 16ull * 1024ull * 1024ull
        || deltaBytes > UINT32_MAX)
        return false;
    output->nativePayloadBytes = static_cast<uint32_t>(nativeBytes);
    output->deltaPayloadBytes = static_cast<uint32_t>(deltaBytes);
    return true;
}

static bool DB_DumpVitaAnimations(int file)
{
    VitaAnimationCollectContext collect{};
    const RawFile* playerAnimScript = DB_FindXAssetHeader(
        ASSET_TYPE_RAWFILE, "mp/playeranim.script").rawfile;
    if (!playerAnimScript || !playerAnimScript->name
        || I_stricmp(playerAnimScript->name, "mp/playeranim.script")
        || !playerAnimScript->buffer || playerAnimScript->len <= 0) {
        Com_PrintError(1,
                       "Vita XAnim v3 missing required mp/playeranim.script closure\n");
        return false;
    }
    collect.playerAnimScript = playerAnimScript->buffer;
    collect.playerAnimScriptBytes = static_cast<uint32_t>(playerAnimScript->len);
    DB_EnumXAssets(ASSET_TYPE_WEAPON, DB_VitaCollectTargetAnimationNames, &collect, 0);
    DB_EnumXAssets(ASSET_TYPE_XANIMPARTS, DB_VitaCollectTargetAnimation, &collect, 0);
    // A map's compiled GSC animation trees are authoritative dependencies as
    // well as playeranim.script and the fixed weapons. Destructible cars,
    // for example, resolve their destroy clips before the first server tick.
    // Preserve their original compressed parts; no sampled viewmodel copy
    // or filename-family approximation is needed for these tree leaves.
    uint32_t treeClips = 0;
    for (uint32_t user = 0; user < 2; ++user) {
        if (scrAnimPub.xanim_num[user] >= MAX_XANIMTREE_NUM) {
            Com_PrintError(1, "Vita XAnim source tree count is invalid\n");
            return false;
        }
        for (uint32_t index = 1; index <= scrAnimPub.xanim_num[user]; ++index) {
            const XAnim_s* tree = scrAnimPub.xanim_lookup[user][index].anims;
            if (!tree) continue;
            if (tree->size > 65535u) return false;
            for (uint32_t entry = 0; entry < tree->size; ++entry) {
                if (tree->entries[entry].numAnims) continue;
                const XAnimParts* parts = tree->entries[entry].parts;
                if (!parts || !parts->name || !*parts->name) {
                    Com_PrintError(1, "Vita XAnim source tree %u leaf %u is missing\n", index, entry);
                    return false;
                }
                if (collect.seen.insert(parts).second) {
                    if (collect.parts.size() == 2048u) return false;
                    collect.parts.push_back(parts);
                    ++treeClips;
                }
            }
        }
    }
    Com_Printf(1, "Vita XAnim source trees added %u clips\n", treeClips);
    std::sort(collect.parts.begin(), collect.parts.end(), [](const XAnimParts* left,
                                                             const XAnimParts* right) {
        return I_stricmp(left->name, right->name) < 0;
    });
    std::sort(collect.models.begin(), collect.models.end(), [](const XModel* left,
                                                               const XModel* right) {
        return I_stricmp(left->name, right->name) < 0;
    });

    std::vector<VitaAnimationBuild> builds;
    builds.reserve(collect.parts.size());
    uint64_t aggregateBytes = 0;
    constexpr uint64_t kMaxDataBytes = 16ull * 1024ull * 1024ull;
    for (const XAnimParts* parts : collect.parts) {
        VitaAnimationBuild build{};
        if (!DB_VitaBuildAnimationSource(parts, &build)) {
            Com_PrintError(1, "Vita XAnim v3 source closure invalid clip=%s\n",
                           parts && parts->name ? parts->name : "<null>");
            return false;
        }
        // Sampled tracks are a compatibility input for the bounded Vita
        // viewmodel renderer. Player-animation script assets are rehydrated
        // as genuine compressed XAnimParts and evaluated by source XAnim, so
        // sampling every third-person clip against every weapon model would
        // waste tens of MiB and create semantically meaningless partial
        // tracks whenever a tag name happens to overlap.
        if (collect.sampledParts.find(parts) != collect.sampledParts.end()) {
            for (const XModel* model : collect.models) {
                VitaAnimationTrackBuild track{};
                if (DB_VitaBuildAnimationTrack(parts, model, &track))
                    build.tracks.push_back(std::move(track));
            }
        }
        if (build.tracks.size() > 64) {
            Com_PrintError(1, "Vita XAnim v3 track closure too large clip=%s\n",
                           build.name.c_str());
            return false;
        }
        uint64_t sampledBytes = 0;
        for (const VitaAnimationTrackBuild& track : build.tracks) {
            sampledBytes += sizeof(VitaAnimationTrackHeader) + track.modelName.size()
                + static_cast<uint64_t>(track.samples.size()) * sizeof(VitaAnimationSample);
        }
        if (sampledBytes > UINT32_MAX) {
            Com_PrintError(1, "Vita XAnim v3 sampled closure too large clip=%s\n",
                           build.name.c_str());
            return false;
        }
        build.sampledPayloadBytes = static_cast<uint32_t>(sampledBytes);
        const uint64_t buildBytes = sizeof(kisak::vita::VitaAnimationRecordV3)
            + build.nativePayloadBytes + build.sampledPayloadBytes;
        if (buildBytes > kMaxDataBytes || aggregateBytes + buildBytes > kMaxDataBytes) {
            Com_PrintError(1, "Vita XAnim v3 aggregate exceeds 16 MiB clip=%s\n",
                           build.name.c_str());
            return false;
        }
        aggregateBytes += buildBytes;
        builds.push_back(std::move(build));
    }

    const uint32_t magic = kisak::vita::kVitaAnimationMagic;
    const uint32_t version = kisak::vita::kVitaAnimationVersionSource;
    const uint32_t recordBytes = sizeof(kisak::vita::VitaAnimationRecordV3);
    if (!DB_VitaWriteValue(file, magic) || !DB_VitaWriteValue(file, version)
        || !DB_VitaWriteValue(file, static_cast<uint32_t>(builds.size()))
        || !DB_VitaWriteValue(file, recordBytes)) {
        Com_PrintError(1, "Vita XAnim v3 header write failed\n");
        return false;
    }
    for (const VitaAnimationBuild& build : builds) {
        kisak::vita::VitaAnimationRecordV3 record{};
        std::strncpy(record.name, build.name.c_str(), sizeof(record.name) - 1);
        record.numframes = build.numframes;
        record.bLoop = build.loop ? 1 : 0;
        record.bDelta = build.delta ? 1 : 0;
        std::memcpy(record.boneCount, build.boneCount, sizeof(record.boneCount));
        record.notifyCount = build.notifyCount;
        record.assetType = build.assetType;
        record.isDefault = build.isDefault ? 1 : 0;
        record.dataByteCount = static_cast<uint16_t>(build.dataByteCount);
        record.dataShortCount = static_cast<uint16_t>(build.dataShortCount);
        record.dataIntCount = static_cast<uint16_t>(build.dataIntCount);
        record.randomDataByteCount = static_cast<uint16_t>(
            build.randomDataByteCount);
        record.randomDataIntCount = static_cast<uint16_t>(
            build.randomDataIntCount);
        record.randomDataShortCount = build.randomDataShortCount;
        record.indexCount = build.indexCount;
        record.boneNameCount = static_cast<uint32_t>(build.boneNames.size());
        record.nativePayloadBytes = build.nativePayloadBytes;
        record.trackCount = static_cast<uint32_t>(build.tracks.size());
        record.sampledPayloadBytes = build.sampledPayloadBytes;
        record.deltaPayloadBytes = build.deltaPayloadBytes;
        record.framerate = build.framerate;
        record.frequency = build.frequency;
        if (!DB_VitaWriteValue(file, record)) {
            Com_PrintError(1, "Vita XAnim v3 record header write failed clip=%s\n",
                           build.name.c_str());
            return false;
        }

        for (const std::string& boneName : build.boneNames) {
            kisak::vita::VitaAnimationBoneNameV3 wire{};
            std::memcpy(wire.value, boneName.data(), boneName.size());
            if (!DB_VitaWriteValue(file, wire)) {
                Com_PrintError(1, "Vita XAnim v3 bone-name write failed clip=%s\n",
                               build.name.c_str());
                return false;
            }
        }
        const XAnimParts* parts = build.source;
        const uint32_t indexBytes = build.indexCount
            * (build.numframes >= 256 ? sizeof(uint16_t) : 1u);
        if ((build.dataByteCount && !DB_VitaWrite(
                 file, parts->dataByte, build.dataByteCount))
            || (build.dataShortCount && !DB_VitaWrite(
                 file, parts->dataShort,
                 build.dataShortCount * sizeof(int16_t)))
            || (build.dataIntCount && !DB_VitaWrite(
                 file, parts->dataInt,
                 build.dataIntCount * sizeof(int32_t)))
            || (build.randomDataByteCount && !DB_VitaWrite(
                 file, parts->randomDataByte, build.randomDataByteCount))
            || (build.randomDataShortCount && !DB_VitaWrite(
                 file, parts->randomDataShort,
                 build.randomDataShortCount * sizeof(int16_t)))
            || (build.randomDataIntCount && !DB_VitaWrite(
                 file, parts->randomDataInt,
                 build.randomDataIntCount * sizeof(int32_t)))
            || (indexBytes && !DB_VitaWrite(
                 file, parts->indices.data, indexBytes))) {
            Com_PrintError(1, "Vita XAnim v3 compressed-array write failed clip=%s\n",
                           build.name.c_str());
            return false;
        }
        for (const VitaAnimationNotifyBuild& notify : build.notifies) {
            kisak::vita::VitaAnimationNotifyV3 wire{};
            std::memcpy(wire.name, notify.name.data(), notify.name.size());
            wire.time = notify.time;
            if (!DB_VitaWriteValue(file, wire)) {
                Com_PrintError(1, "Vita XAnim v3 notify write failed clip=%s\n",
                               build.name.c_str());
                return false;
            }
        }
        if (build.delta) {
            const VitaAnimationDeltaBuild& delta = build.deltaPart;
            if (!DB_VitaWriteValue(file, delta.record)
                || (delta.record.transFrameBytes && !DB_VitaWrite(
                    file, delta.transFrames, delta.record.transFrameBytes))
                || (delta.record.transIndexBytes && !DB_VitaWrite(
                    file, delta.transIndices, delta.record.transIndexBytes))
                || (delta.record.quatFrameBytes && !DB_VitaWrite(
                    file, delta.quatFrames, delta.record.quatFrameBytes))
                || (delta.record.quatIndexBytes && !DB_VitaWrite(
                    file, delta.quatIndices, delta.record.quatIndexBytes))) {
                Com_PrintError(1, "Vita XAnim v3 delta write failed clip=%s\n",
                               build.name.c_str());
                return false;
            }
        }
        for (const VitaAnimationTrackBuild& track : build.tracks) {
            const VitaAnimationTrackHeader trackHeader{
                static_cast<uint32_t>(track.modelName.size()), track.boneCount,
                track.frameCount, 0u};
            if (!DB_VitaWriteValue(file, trackHeader)
                || !DB_VitaWriteString(file, track.modelName.c_str(),
                                       trackHeader.modelNameBytes)
                || (!track.samples.empty() && !DB_VitaWrite(
                    file, track.samples.data(), static_cast<uint32_t>(
                        track.samples.size() * sizeof(VitaAnimationSample)))))
            {
                Com_PrintError(1, "Vita XAnim v3 track write failed clip=%s model=%s\n",
                               build.name.c_str(), track.modelName.c_str());
                return false;
            }
        }
    }
    return true;
}

// VITX is the pointer-free LOD0/skeleton XModel stream used by the real source
// runtime and Vita viewmodel path. VLOD is its render-only companion: authored
// lower LOD geometry and thresholds remain independent of source-facing
// pointer rehydration and can be uploaded directly into the map GPU archive.
#pragma pack(push, 1)
struct VitaModelRecordHeader
{
    uint32_t nameBytes;
    uint32_t surfaceCount;
    uint32_t vertexCount;
    uint32_t indexCount;
    float mins[3];
    float maxs[3];
};

struct VitaModelSurfaceRecord
{
    uint32_t materialNameBytes;
    uint32_t vertexStart;
    uint32_t vertexCount;
    uint32_t indexStart;
    uint32_t indexCount;
    uint32_t reserved;
};

struct VitaModelRenderLodModelHeader
{
    uint32_t nameBytes;
    uint32_t lodCount;
    uint32_t renderLodCount;
    uint32_t lodRampType;
    float distances[4];
};

struct VitaModelRenderLodHeader
{
    uint32_t lodIndex;
    uint32_t surfaceCount;
    uint32_t vertexCount;
    uint32_t indexCount;
};

struct VitaModelVertex
{
    float xyz[3];
    float texCoord[2];
    PackedUnitVec normal;
    PackedUnitVec tangent;
    uint32_t color;
};

struct VitaModelSkinHeader
{
    uint32_t boneCount;
    uint32_t rootBoneCount;
    uint32_t parentBytes;
    uint32_t reserved;
};

// VITX v5 keeps the v4 tag layout and appends a bounded table of textual bone
// identities.  Vita interns those names into its own SL table; no desktop
// scr_string_t value or pointer crosses the package boundary.
struct VitaModelSkinHeaderV5
{
    uint32_t boneCount;
    uint32_t rootBoneCount;
    uint32_t parentBytes;
    uint32_t reserved;
    uint32_t flashTagBone;
    uint32_t boneNameBytes;
};

struct VitaModelBoneName
{
    char value[64];
};

struct VitaModelBone
{
    float quat[4];
    float trans[3];
    float transWeight;
    float bindQuat[4];
    float bindTrans[3];
    float bindTransWeight;
};

struct VitaModelVertexWeight
{
    uint16_t bone[4];
    uint16_t weight[4];
};
#pragma pack(pop)

static_assert(sizeof(VitaModelRecordHeader) == 40);
static_assert(sizeof(VitaModelSurfaceRecord) == 24);
static_assert(sizeof(VitaModelRenderLodModelHeader) == 32);
static_assert(sizeof(VitaModelRenderLodHeader) == 16);
static_assert(sizeof(VitaModelVertex) == 32);
static_assert(sizeof(VitaModelSkinHeader) == 16);
static_assert(sizeof(VitaModelSkinHeaderV5) == 24);
static_assert(sizeof(VitaModelBoneName) == 64);
static_assert(sizeof(VitaModelBone) == 64);
static_assert(sizeof(VitaModelVertexWeight) == 16);

struct VitaModelSurfaceBuild
{
    std::string materialName;
    uint32_t vertexStart = 0;
    uint32_t vertexCount = 0;
    uint32_t indexStart = 0;
    uint32_t indexCount = 0;
};

struct VitaModelRenderLodBuild
{
    uint32_t lodIndex = 0;
    std::vector<VitaModelSurfaceBuild> surfaces;
    std::vector<VitaModelVertex> vertices;
    std::vector<uint16_t> indices;
};

struct VitaModelBuild
{
    const XModel* source = nullptr;
    std::string name;
    std::vector<VitaModelSurfaceBuild> surfaces;
    std::vector<VitaModelVertex> vertices;
    std::vector<uint8_t> parents;
    std::vector<VitaModelBoneName> boneNames;
    std::vector<VitaModelBone> bones;
    // One-based on disk in VitaModelSkinHeader::reserved; the low half is the
    // tag_weapon index and the high half is tag_camera. VITX v4 appends the
    // source tag_flash index. Zero means absent.
    uint32_t weaponTagBone = 0;
    uint32_t cameraTagBone = 0;
    uint32_t flashTagBone = 0;
    std::vector<VitaModelVertexWeight> weights;
    std::vector<uint16_t> indices;
    uint32_t lodCount = 1;
    uint32_t lodRampType = 0;
    float lodDistances[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<VitaModelRenderLodBuild> renderLods;
    float mins[3] = {0.0f, 0.0f, 0.0f};
    float maxs[3] = {0.0f, 0.0f, 0.0f};
};

struct VitaModelCollectContext
{
    std::vector<const XModel*> models;
    std::vector<const XModel*> fxModels;
    std::unordered_set<const XModel*> seen;
    std::unordered_set<const XModel*> fxSeen;
    std::unordered_set<const XModel*> unavailableWeaponModels;
};

// A map's BSP/static-model closure is larger than the weapon-only closure.
// Keep the cap high enough to retain every unique script/static model on the
// stock MP maps together with their authored FX and viewmodel closure. The
// previous 256-record envelope evicted the last map-dressing models once FX
// reservation was applied, even though the resulting VITX payload still fit
// below the existing 16 MiB chunk bound.
static constexpr uint32_t kVitaMaxModelRecords = kisak::vita::kVitaModelMaxRecords;
static constexpr uint32_t kVitaMaxCollectedModelRecords = 4096;

static bool DB_VitaTargetWeapon(const char* name)
{
    if (!name)
        return false;
    // defaultweapon_mp is the retail MP bootstrap definition.  The game
    // registers it before any client exists so CS_WEAPONFILES is non-empty
    // when the first real cgame parses its gamestate.  It is not part of the
    // player's fixed loadout, but it is part of the source runtime closure.
    for (const char* required : kisak::vita::kVitaSourceWeaponNames)
        if (!I_stricmp(name, required)) return true;
    return false;
}

static void DB_VitaCollectModel(VitaModelCollectContext* context, const XModel* model)
{
    if (!context || !model || !model->name || !*model->name
        || context->models.size() >= kVitaMaxCollectedModelRecords)
        return;
    if (context->seen.insert(model).second)
        context->models.push_back(model);
}

static void __cdecl DB_VitaCollectWeaponModels(XAssetHeader header, void* rawContext)
{
    auto* context = static_cast<VitaModelCollectContext*>(rawContext);
    const WeaponDef* weapon = header.weapon;
    if (!context || !weapon || !DB_VitaTargetWeapon(weapon->szInternalName))
        return;
    for (uint32_t index = 0; index < 16; ++index)
        DB_VitaCollectModel(context, weapon->gunXModel[index]);
    DB_VitaCollectModel(context, weapon->handXModel);
    for (uint32_t index = 0; index < 16; ++index)
        DB_VitaCollectModel(context, weapon->worldModel[index]);
    DB_VitaCollectModel(context, weapon->worldClipModel);
    DB_VitaCollectModel(context, weapon->rocketModel);
    DB_VitaCollectModel(context, weapon->knifeModel);
    DB_VitaCollectModel(context, weapon->worldKnifeModel);
    DB_VitaCollectModel(context, weapon->projectileModel);
}

static void __cdecl DB_VitaCollectUnavailableWeaponModels(XAssetHeader header,
                                                          void* rawContext)
{
    auto* context = static_cast<VitaModelCollectContext*>(rawContext);
    const WeaponDef* weapon = header.weapon;
    if (!context || !weapon || DB_VitaTargetWeapon(weapon->szInternalName))
        return;
    const auto add = [context](const XModel* model) {
        if (model) context->unavailableWeaponModels.insert(model);
    };
    for (uint32_t index = 0; index < 16; ++index) {
        add(weapon->gunXModel[index]);
        add(weapon->worldModel[index]);
    }
    // View hands are also chosen by the original character scripts. They
    // remain in the server closure even when referenced by another weapon.
    add(weapon->worldClipModel);
    add(weapon->rocketModel);
    add(weapon->knifeModel);
    add(weapon->worldKnifeModel);
    add(weapon->projectileModel);
}

// Shipment keeps a small set of authored misc/script models in the map
// entity string (flags, bomb objectives, laptops and cases).  The original
// Vita closure only walked weapon XModels, which left those props invisible
// even though their pointer-free entity records were already packaged.  Walk
// the bounded entity text here and add only named script models; brush-model
// references ("*N") stay in the BSP/collision sidecar and are intentionally
// not treated as XModels.
static bool DB_VitaReadQuotedEntityValue(const std::string& block,
                                         const char* key, std::string* value)
{
    if (!key || !value)
        return false;
    const std::string token = std::string("\"") + key + "\"";
    const size_t keyPosition = block.find(token);
    if (keyPosition == std::string::npos)
        return false;
    const size_t firstQuote = block.find('"', keyPosition + token.size());
    if (firstQuote == std::string::npos)
        return false;
    const size_t secondQuote = block.find('"', firstQuote + 1);
    if (secondQuote == std::string::npos)
        return false;
    *value = block.substr(firstQuote + 1, secondQuote - firstQuote - 1);
    return true;
}

static void DB_VitaCollectMapEntityModels(VitaModelCollectContext* context)
{
    if (!context || !cm.mapEnts || !cm.mapEnts->entityString
        || cm.mapEnts->numEntityChars <= 0
        || cm.mapEnts->numEntityChars > 2'000'000)
        return;
    const std::string text(cm.mapEnts->entityString,
                           static_cast<size_t>(cm.mapEnts->numEntityChars));
    size_t cursor = 0;
    uint32_t candidates = 0;
    while (cursor < text.size() && context->models.size() < kVitaMaxModelRecords) {
        const size_t open = text.find('{', cursor);
        if (open == std::string::npos)
            break;
        const size_t close = text.find('}', open + 1);
        if (close == std::string::npos)
            break;
        const std::string block = text.substr(open + 1, close - open - 1);
        std::string classname;
        std::string modelName;
        if (DB_VitaReadQuotedEntityValue(block, "classname", &classname)
            && (classname == "script_model" || classname == "vita_static_model")
            && DB_VitaReadQuotedEntityValue(block, "model", &modelName)
            && !modelName.empty() && modelName.front() != '*') {
            ++candidates;
            const XModel* model = DB_FindXAssetHeader(
                ASSET_TYPE_XMODEL, modelName.c_str()).model;
            DB_VitaCollectModel(context, model);
        }
        cursor = close + 1;
    }
    Com_Printf(16, "Vita model closure: map script-model candidates=%u total=%u\n",
               candidates, static_cast<unsigned>(context->models.size()));
}

static void DB_VitaCollectWorldStaticModels(VitaModelCollectContext* context)
{
    if (!context || !s_world.dpvs.smodelDrawInsts
        || s_world.dpvs.smodelCount == 0
        || s_world.dpvs.smodelCount > 16384)
        return;
    uint32_t candidates = 0;
    for (uint32_t index = 0; index < s_world.dpvs.smodelCount; ++index) {
        const GfxStaticModelDrawInst& instance = s_world.dpvs.smodelDrawInsts[index];
        if (!instance.model || !instance.model->name || !*instance.model->name)
            continue;
        ++candidates;
        DB_VitaCollectModel(context, instance.model);
    }
    Com_Printf(16, "Vita model closure: world static-model candidates=%u total=%u\n",
               candidates, static_cast<unsigned>(context->models.size()));
}

// GSC precaches player bodies, view hands and scripted entity models that
// are absent from BSP placements and weapon definitions. Read the original
// server's published model configstrings after map startup, preserving the
// actual source dependency set rather than guessing names from script text.
static void DB_VitaCollectServerModels(VitaModelCollectContext* context)
{
#ifdef KISAK_MP
    if (!context || !SV_Loaded())
        return;
    const size_t before = context->models.size();
    uint32_t candidates = 0;
    uint32_t unavailable = 0;
    for (uint32_t index = 1; index < MAX_MODELS; ++index) {
        char name[256]{};
        SV_GetConfigstring(830 + index, name, sizeof(name));
        if (!name[0] || !I_stricmp(name, "_"))
            continue;
        ++candidates;
        const XModel* model = DB_FindXAssetHeader(ASSET_TYPE_XMODEL, name).model;
        if (!model || !model->name || I_stricmp(model->name, name)) {
            Com_PrintError(16, "Vita model closure: unresolved server model %s\n", name);
            continue;
        }
        if (context->unavailableWeaponModels.count(model)
            && !context->seen.count(model)) {
            ++unavailable;
            Com_Printf(16, "Vita model exclusion: %s reason=unavailable-weapon-precache-only\n", name);
            continue;
        }
        DB_VitaCollectModel(context, model);
    }
    Com_Printf(16, "Vita model closure: server precaches=%u new=%u total=%u unavailableWeaponOnly=%u\n",
        candidates, static_cast<unsigned>(context->models.size() - before),
        static_cast<unsigned>(context->models.size()), unavailable);
#endif
}

// FX model elements are not placed in the map entity string or the world
// static-model table.  They are still part of the authored map closure: a
// grenade impact, wood splinter, or debris burst can introduce an XModel at
// runtime through an FxElemDef.  Enumerate the exact source references so the
// pointer-free VITX dump contains the geometry and its material names before
// the Vita renderer evaluates the graph.  This deliberately walks the
// complete loaded FX asset set; child runners are represented by their own
// FxEffectDef and therefore do not need a guessed name list here.
static void __cdecl DB_VitaCollectFxModels(XAssetHeader header,
                                           void* rawContext)
{
    auto* context = static_cast<VitaModelCollectContext*>(rawContext);
    const FxEffectDef* effect = header.fx;
    if (!context || !effect || !effect->elemDefs
        || context->models.size() >= kVitaMaxCollectedModelRecords
        || effect->elemDefCountLooping < 0
        || effect->elemDefCountOneShot < 0
        || effect->elemDefCountEmission < 0)
        return;
    const uint64_t elementCount = static_cast<uint64_t>(
        effect->elemDefCountLooping)
        + static_cast<uint64_t>(effect->elemDefCountOneShot)
        + static_cast<uint64_t>(effect->elemDefCountEmission);
    if (elementCount > 32u)
        return;
    for (uint64_t elementIndex = 0; elementIndex < elementCount
         && context->models.size() < kVitaMaxCollectedModelRecords; ++elementIndex) {
        const FxElemDef& element = effect->elemDefs[elementIndex];
        if (element.elemType != 5u || element.visualCount == 0u
            || element.visualCount > 32u)
            continue;
        const FxElemVisuals* visuals = element.visualCount == 1u
            ? &element.visuals.instance : element.visuals.array;
        if (!visuals)
            continue;
        for (uint32_t visualIndex = 0; visualIndex < element.visualCount
             && context->models.size() < kVitaMaxCollectedModelRecords;
             ++visualIndex) {
            const XModel* model = visuals[visualIndex].model;
            if (!model || !model->name || !*model->name)
                continue;
            DB_VitaCollectModel(context, model);
            if (context->fxSeen.insert(model).second)
                context->fxModels.push_back(model);
        }
    }
}

// GfxWorld keeps authored map dressing—grass clumps, trees, barrels and
// container accessories—in the static-model draw-instance array rather than
// in the map entity string.  Preserve those exact source placements as
// pointer-free synthetic entities so the Vita renderer can reuse the same
// XModel/material closure and the same placement path as script props.
static uint32_t DB_VitaAppendWorldStaticModelEntities(std::string* output)
{
    if (!output || !s_world.dpvs.smodelDrawInsts
        || s_world.dpvs.smodelCount == 0
        || s_world.dpvs.smodelCount > 16384)
        return 0;

    uint32_t appended = 0;
    constexpr uint32_t kMaxStaticEntities = 4096;
    for (uint32_t index = 0; index < s_world.dpvs.smodelCount
         && appended < kMaxStaticEntities; ++index) {
        const GfxStaticModelDrawInst& instance = s_world.dpvs.smodelDrawInsts[index];
        if (!instance.model || !instance.model->name || !*instance.model->name
            || !std::isfinite(instance.placement.scale)
            || instance.placement.scale <= 0.0f
            || instance.placement.scale > 64.0f)
            continue;
        bool finite = true;
        for (uint32_t component = 0; component < 3; ++component) {
            finite = finite && std::isfinite(instance.placement.origin[component]);
            for (uint32_t axis = 0; axis < 3; ++axis)
                finite = finite && std::isfinite(instance.placement.axis[component][axis]);
        }
        if (!finite)
            continue;

        // Static map models are authored through AnglesToAxis in
        // R_LoadMiscModel. Round-trip that source basis through KisakCOD's
        // AxisToAngles so the Vita entity reader can use the same Euler
        // placement convention without serializing a host pointer or matrix.
        float angles[3] = {0.0f, 0.0f, 0.0f};
        AxisToAngles(*(const mat3x3*)instance.placement.axis, angles);
        char block[768]{};
        // R_LoadMiscModel decodes the authored gndLt key into
        // GfxStaticModelInst::groundLighting. Preserve that exact packed
        // colour in the pointer-free entity stream; the Vita draw path can
        // then feed the same per-instance model-light sample instead of
        // replacing it with a guessed ambient tint. The first four hex pairs
        // intentionally mirror R_DecodeGroundLighting's byte order, and the
        // fifth pair carries the source primary-light index.
        const GfxStaticModelInst* sourceInstance = s_world.dpvs.smodelInsts
            ? &s_world.dpvs.smodelInsts[index] : nullptr;
        const bool hasGroundLighting = sourceInstance
            && sourceInstance->groundLighting.packed != 0;
        const uint8_t* ground = hasGroundLighting
            ? sourceInstance->groundLighting.array : nullptr;
        const int written = hasGroundLighting
            ? std::snprintf(
                block, sizeof(block),
                "{\n\"classname\" \"vita_static_model\"\n"
                "\"model\" \"%s\"\n\"origin\" \"%.9g %.9g %.9g\"\n"
                "\"angles\" \"%.9g %.9g %.9g\"\n\"modelscale\" \"%.9g\"\n"
                "\"gndLt\" \"%02x%02x%02x%02x%02x\"\n}\n",
                instance.model->name,
                instance.placement.origin[0], instance.placement.origin[1],
                instance.placement.origin[2], angles[0], angles[1], angles[2],
                instance.placement.scale,
                static_cast<unsigned>(ground[2]), static_cast<unsigned>(ground[1]),
                static_cast<unsigned>(ground[0]), static_cast<unsigned>(ground[3]),
                static_cast<unsigned>(instance.primaryLightIndex))
            : std::snprintf(
                block, sizeof(block),
                "{\n\"classname\" \"vita_static_model\"\n"
                "\"model\" \"%s\"\n\"origin\" \"%.9g %.9g %.9g\"\n"
                "\"angles\" \"%.9g %.9g %.9g\"\n\"modelscale\" \"%.9g\"\n"
                "\"primaryLightIndex\" \"%u\"\n}\n",
                instance.model->name,
                instance.placement.origin[0], instance.placement.origin[1],
                instance.placement.origin[2], angles[0], angles[1], angles[2],
                instance.placement.scale,
                static_cast<unsigned>(instance.primaryLightIndex));
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(block)
            || output->size() > 2'000'000u - static_cast<size_t>(written))
            break;
        output->append(block, static_cast<size_t>(written));
        ++appended;
    }
    Com_Printf(16, "Vita world static-model closure: source=%u appended=%u\n",
               s_world.dpvs.smodelCount, appended);
    return appended;
}

static bool DB_VitaBuildSurfaceWeights(const XModel* model, const XSurface& surface,
                                       std::vector<VitaModelVertexWeight>* output)
{
    if (!model || !output)
        return false;
    output->assign(surface.vertCount, VitaModelVertexWeight{});
    if (surface.vertCount == 0 || model->numBones == 0)
        return true;

    const uint32_t boneCount = model->numBones;
    if (!surface.deformed) {
        if (!surface.vertList || surface.vertListCount == 0)
            return false;
        uint32_t vertexIndex = 0;
        for (uint32_t listIndex = 0; listIndex < surface.vertListCount; ++listIndex) {
            const XRigidVertList& list = surface.vertList[listIndex];
            const uint32_t bone = list.boneOffset >> 6;
            if (bone >= boneCount || vertexIndex + list.vertCount > surface.vertCount)
                return false;
            for (uint32_t local = 0; local < list.vertCount; ++local) {
                (*output)[vertexIndex].bone[0] = static_cast<uint16_t>(bone);
                (*output)[vertexIndex].weight[0] = 65535u;
                ++vertexIndex;
            }
        }
        return vertexIndex == surface.vertCount;
    }

    if (!surface.vertInfo.vertsBlend)
        return false;
    const int16_t groupCounts[4] = {
        surface.vertInfo.vertCount[0], surface.vertInfo.vertCount[1],
        surface.vertInfo.vertCount[2], surface.vertInfo.vertCount[3],
    };
    const uint16_t* cursor = surface.vertInfo.vertsBlend;
    uint32_t vertexIndex = 0;
    for (uint32_t group = 0; group < 4; ++group) {
        if (groupCounts[group] < 0
            || vertexIndex + static_cast<uint32_t>(groupCounts[group]) > surface.vertCount)
            return false;
        for (int32_t local = 0; local < groupCounts[group]; ++local) {
            VitaModelVertexWeight& weight = (*output)[vertexIndex++];
            const uint16_t baseOffset = *cursor++;
            const uint32_t baseBone = baseOffset >> 6;
            if (baseBone >= boneCount)
                return false;
            weight.bone[0] = static_cast<uint16_t>(baseBone);
            uint32_t extraTotal = 0;
            for (uint32_t extra = 0; extra < group; ++extra) {
                const uint16_t extraOffset = *cursor++;
                const uint16_t extraWeight = *cursor++;
                const uint32_t extraBone = extraOffset >> 6;
                if (extraBone >= boneCount || extraTotal + extraWeight > 65535u
                    || extra >= 3)
                    return false;
                weight.bone[extra + 1] = static_cast<uint16_t>(extraBone);
                weight.weight[extra + 1] = extraWeight;
                extraTotal += extraWeight;
            }
            weight.weight[0] = static_cast<uint16_t>(65535u - extraTotal);
        }
    }
    return vertexIndex == surface.vertCount;
}

static uint32_t DB_VitaFindTagBone(const XModel* model, const char* tagName)
{
    if (!model || !tagName || !model->boneNames || model->numBones == 0)
        return 0;
    for (uint32_t boneIndex = 0; boneIndex < model->numBones; ++boneIndex) {
        const char* name = SL_ConvertToString(model->boneNames[boneIndex]);
        if (name && !I_stricmp(name, tagName))
            return boneIndex + 1;
    }
    return 0;
}

static bool DB_VitaBuildModel(const XModel* model, VitaModelBuild* output,
                              const char** failureReason)
{
    const auto fail = [failureReason](const char* reason) {
        if (failureReason)
            *failureReason = reason;
        return false;
    };
    if (failureReason)
        *failureReason = nullptr;
    if (!model || !output || !model->name || !model->surfs || !model->materialHandles
        || model->lodInfo[0].numsurfs == 0
        || (model->numBones != 0 && !model->boneNames))
        return fail("missing required model storage or LOD0 surfaces");
    uint32_t nameBytes = 0;
    if (!DB_VitaBoundedString(model->name, &nameBytes) || nameBytes == 0 || nameBytes > 256)
        return fail("invalid model name");

    output->source = model;
    output->name.assign(model->name, nameBytes);
    output->weaponTagBone = DB_VitaFindTagBone(model, "tag_weapon");
    output->cameraTagBone = DB_VitaFindTagBone(model, "tag_camera");
    output->flashTagBone = DB_VitaFindTagBone(model, "tag_flash");
    if (model->numBones > 128 || model->numRootBones > model->numBones)
        return fail("unsupported bone count");
    output->parents.assign(model->numBones, 0xFFu);
    output->boneNames.resize(model->numBones);
    output->bones.resize(model->numBones);
    for (uint32_t boneIndex = 0; boneIndex < model->numBones; ++boneIndex) {
        const char* sourceBoneName = SL_ConvertToString(model->boneNames[boneIndex]);
        uint32_t boneNameBytes = 0;
        if (!sourceBoneName
            || !DB_VitaBoundedString(sourceBoneName, &boneNameBytes)
            || boneNameBytes >= sizeof(VitaModelBoneName::value)) {
            Com_PrintWarning(16,
                "Vita model bone name rejected model=%s bone=%u id=%u bytes=%u value=%s\n",
                model->name, boneIndex,
                static_cast<unsigned>(model->boneNames[boneIndex]),
                static_cast<unsigned>(boneNameBytes),
                sourceBoneName ? sourceBoneName : "<null>");
            return fail("invalid bone name");
        }
        // Static and projectile XModels legitimately use the source string
        // table's empty-string entry (id 1) for their anonymous root bone.
        // An all-zero fixed record preserves that identity without inventing
        // a tag or serializing the desktop scr_string_t value.
        std::memcpy(output->boneNames[boneIndex].value, sourceBoneName,
                    boneNameBytes);
        if (boneIndex >= model->numRootBones) {
            if (!model->parentList)
                return fail("missing parent list");
            // XModel parentList stores a backwards offset, not an absolute
            // bone index.  The desktop loader reconstructs the parent as
            // `boneIndex - parentOffset`; serializing the raw byte made
            // view-hands hierarchies self-parent (and collapse during Vita
            // pose evaluation).
            const uint32_t parentOffset =
                model->parentList[boneIndex - model->numRootBones];
            if (parentOffset == 0 || parentOffset > boneIndex)
                return fail("invalid parent offset");
            const uint32_t parentIndex = boneIndex - parentOffset;
            output->parents[boneIndex] = static_cast<uint8_t>(parentIndex);
        }
        VitaModelBone& bone = output->bones[boneIndex];
        // Keep both sides of KisakCOD's DObj/R_SkinXModel boundary.  The raw
        // local pose is what CalcSkelNonRootBones consumes, while baseMat is
        // already global and is the exact inverse-bind source used by the
        // desktop renderer.  Keeping both avoids guessing when an XModel's
        // useBones flag has zeroed its local translation array.
        bone.quat[0] = 0.0f;
        bone.quat[1] = 0.0f;
        bone.quat[2] = 0.0f;
        bone.quat[3] = 1.0f;
        bone.trans[0] = 0.0f;
        bone.trans[1] = 0.0f;
        bone.trans[2] = 0.0f;
        bone.transWeight = 2.0f;
        if (boneIndex >= model->numRootBones && model->quats && model->trans) {
            const uint32_t localIndex = boneIndex - model->numRootBones;
            const __int16* sourceQuat = model->quats + localIndex * 4u;
            const float* sourceTrans = model->trans + localIndex * 3u;
            constexpr float kQuatScale = 0.00003051850944757462f;
            for (uint32_t component = 0; component < 4; ++component)
                bone.quat[component] = static_cast<float>(sourceQuat[component])
                    * kQuatScale;
            std::memcpy(bone.trans, sourceTrans, sizeof(bone.trans));
            const float lengthSquared = bone.quat[0] * bone.quat[0]
                + bone.quat[1] * bone.quat[1] + bone.quat[2] * bone.quat[2]
                + bone.quat[3] * bone.quat[3];
            bone.transWeight = lengthSquared > 0.000001f
                ? 2.0f / lengthSquared : 2.0f;
        }
        if (model->baseMat) {
            std::memcpy(bone.bindQuat, model->baseMat[boneIndex].quat,
                        sizeof(bone.bindQuat));
            std::memcpy(bone.bindTrans, model->baseMat[boneIndex].trans,
                        sizeof(bone.bindTrans));
            bone.bindTransWeight = model->baseMat[boneIndex].transWeight;
        } else {
            std::memcpy(bone.bindQuat, bone.quat, sizeof(bone.bindQuat));
            std::memcpy(bone.bindTrans, bone.trans, sizeof(bone.bindTrans));
            bone.bindTransWeight = bone.transWeight;
        }
    }
    std::memcpy(output->mins, model->mins, sizeof(output->mins));
    std::memcpy(output->maxs, model->maxs, sizeof(output->maxs));
    if (model->numLods < 1 || model->numLods > 4
        || model->lodRampType >= XMODEL_LOD_RAMP_COUNT)
        return fail("invalid source LOD metadata");
    output->lodCount = static_cast<uint32_t>(model->numLods);
    output->lodRampType = model->lodRampType;
    for (uint32_t lodIndex = 0; lodIndex < output->lodCount; ++lodIndex) {
        const float distance = model->lodInfo[lodIndex].dist;
        if (!std::isfinite(distance) || distance < 0.0f)
            return fail("invalid source LOD distance");
        output->lodDistances[lodIndex] = distance;
    }
    const XModelLodInfo& lod = model->lodInfo[0];
    if (lod.surfIndex > model->numsurfs
        || static_cast<uint32_t>(lod.surfIndex) + lod.numsurfs > model->numsurfs)
        return fail("invalid LOD0 surface range");

    for (uint32_t localSurface = 0; localSurface < lod.numsurfs; ++localSurface) {
        const uint32_t surfaceIndex = static_cast<uint32_t>(lod.surfIndex) + localSurface;
        const XSurface& surface = model->surfs[surfaceIndex];
        if (!surface.verts0 || !surface.triIndices || surface.vertCount == 0
            || surface.triCount == 0
            || static_cast<uint64_t>(surface.triCount) * 3ull > UINT32_MAX)
            continue;
        const uint64_t nextVertices = static_cast<uint64_t>(output->vertices.size())
            + surface.vertCount;
        const uint64_t nextIndices = static_cast<uint64_t>(output->indices.size())
            + static_cast<uint64_t>(surface.triCount) * 3ull;
        if (nextVertices > 65535 || nextIndices > 300000)
            break;
        VitaModelSurfaceBuild builtSurface{};
        builtSurface.vertexStart = static_cast<uint32_t>(output->vertices.size());
        builtSurface.indexStart = static_cast<uint32_t>(output->indices.size());
        builtSurface.vertexCount = surface.vertCount;
        builtSurface.indexCount = static_cast<uint32_t>(surface.triCount) * 3u;
        Material* material = model->materialHandles[surfaceIndex];
        if (material && material->info.name) {
            uint32_t materialBytes = 0;
            if (DB_VitaBoundedString(material->info.name, &materialBytes)
                && materialBytes <= 256)
                builtSurface.materialName.assign(material->info.name, materialBytes);
        }
        std::vector<VitaModelVertexWeight> surfaceWeights;
        if (!DB_VitaBuildSurfaceWeights(model, surface, &surfaceWeights))
            return fail("invalid source skin weights");
        for (uint32_t vertexIndex = 0; vertexIndex < surface.vertCount; ++vertexIndex) {
            VitaModelVertex vertex{};
            const GfxPackedVertex& source = surface.verts0[vertexIndex];
            std::memcpy(vertex.xyz, source.xyz, sizeof(vertex.xyz));
            Vec2UnpackTexCoords(source.texCoord, vertex.texCoord);
            vertex.normal = source.normal;
            vertex.tangent = source.tangent;
            vertex.color = source.color.packed;
            output->vertices.push_back(vertex);
            output->weights.push_back(surfaceWeights[vertexIndex]);
        }
        for (uint32_t index = 0; index < builtSurface.indexCount; ++index) {
            const uint16_t value = surface.triIndices[index];
            if (value >= surface.vertCount) {
                output->vertices.resize(builtSurface.vertexStart);
                output->weights.resize(builtSurface.vertexStart);
                output->indices.resize(builtSurface.indexStart);
                return fail("out-of-range source triangle index");
            }
            output->indices.push_back(static_cast<uint16_t>(builtSurface.vertexStart + value));
        }
        output->surfaces.push_back(std::move(builtSurface));
    }
    if (output->name.empty() || output->surfaces.empty()
        || output->vertices.empty() || output->indices.empty())
        return fail("no exportable LOD0 geometry");

    // Lower XModel LODs are immutable render data. Keep them out of VITX's
    // source-facing skeleton contract and write a VLOD companion instead;
    // static-map rendering can then select the authored mesh without making
    // the gameplay registry reinterpret a desktop pointer.
    for (uint32_t lodIndex = 1; lodIndex < output->lodCount; ++lodIndex) {
        const XModelLodInfo& sourceLod = model->lodInfo[lodIndex];
        if (sourceLod.surfIndex > model->numsurfs
            || static_cast<uint32_t>(sourceLod.surfIndex)
                    + sourceLod.numsurfs > model->numsurfs)
            return fail("invalid lower LOD surface range");
        VitaModelRenderLodBuild builtLod{};
        builtLod.lodIndex = lodIndex;
        bool complete = true;
        for (uint32_t localSurface = 0; localSurface < sourceLod.numsurfs;
             ++localSurface) {
            const uint32_t surfaceIndex =
                static_cast<uint32_t>(sourceLod.surfIndex) + localSurface;
            const XSurface& surface = model->surfs[surfaceIndex];
            if (!surface.verts0 || !surface.triIndices
                || surface.vertCount == 0 || surface.triCount == 0
                || static_cast<uint64_t>(surface.triCount) * 3ull > UINT32_MAX)
                continue;
            const uint64_t nextVertices =
                static_cast<uint64_t>(builtLod.vertices.size())
                + surface.vertCount;
            const uint64_t nextIndices =
                static_cast<uint64_t>(builtLod.indices.size())
                + static_cast<uint64_t>(surface.triCount) * 3ull;
            if (nextVertices > 65535u || nextIndices > 300000u) {
                complete = false;
                break;
            }
            VitaModelSurfaceBuild builtSurface{};
            builtSurface.vertexStart = static_cast<uint32_t>(
                builtLod.vertices.size());
            builtSurface.indexStart = static_cast<uint32_t>(
                builtLod.indices.size());
            builtSurface.vertexCount = surface.vertCount;
            builtSurface.indexCount = static_cast<uint32_t>(
                surface.triCount) * 3u;
            Material* material = model->materialHandles[surfaceIndex];
            if (material && material->info.name) {
                uint32_t materialBytes = 0;
                if (DB_VitaBoundedString(material->info.name, &materialBytes)
                    && materialBytes <= 256u) {
                    builtSurface.materialName.assign(material->info.name,
                                                     materialBytes);
                }
            }
            for (uint32_t vertexIndex = 0; vertexIndex < surface.vertCount;
                 ++vertexIndex) {
                VitaModelVertex vertex{};
                const GfxPackedVertex& source = surface.verts0[vertexIndex];
                std::memcpy(vertex.xyz, source.xyz, sizeof(vertex.xyz));
                Vec2UnpackTexCoords(source.texCoord, vertex.texCoord);
                vertex.normal = source.normal;
                vertex.tangent = source.tangent;
                vertex.color = source.color.packed;
                builtLod.vertices.push_back(vertex);
            }
            for (uint32_t index = 0; index < builtSurface.indexCount; ++index) {
                const uint16_t value = surface.triIndices[index];
                if (value >= surface.vertCount)
                    return fail("out-of-range lower LOD triangle index");
                builtLod.indices.push_back(static_cast<uint16_t>(
                    builtSurface.vertexStart + value));
            }
            builtLod.surfaces.push_back(std::move(builtSurface));
        }
        if (complete && !builtLod.surfaces.empty()
            && !builtLod.vertices.empty() && !builtLod.indices.empty())
            output->renderLods.push_back(std::move(builtLod));
    }
    return true;
}

static bool DB_DumpVitaModels(int file, int renderLodFile)
{
    VitaModelCollectContext collect{};
    DB_EnumXAssets(ASSET_TYPE_WEAPON, DB_VitaCollectWeaponModels, &collect, 0);
    DB_VitaCollectMapEntityModels(&collect);
    DB_VitaCollectWorldStaticModels(&collect);
    const size_t beforeFxModels = collect.models.size();
    DB_EnumXAssets(ASSET_TYPE_FX, DB_VitaCollectFxModels, &collect, 0);
    Com_Printf(16, "Vita model closure: FX model references=%u new=%u total=%u\n",
               static_cast<unsigned>(collect.fxModels.size()),
               static_cast<unsigned>(collect.models.size() - beforeFxModels),
               static_cast<unsigned>(collect.models.size()));
    // Protect every map/FX and enabled weapon dependency before excluding
    // server precaches belonging solely to unavailable loadouts.
    DB_EnumXAssets(ASSET_TYPE_WEAPON, DB_VitaCollectUnavailableWeaponModels,
                  &collect, 0);
    DB_VitaCollectServerModels(&collect);
    // A valid stream containing only a prefix looks complete to its reader.
    // Reject overflow before writing a header instead of silently dropping
    // source models to fit the bounded format.
    if (collect.models.size() > kVitaMaxModelRecords) {
        Com_PrintError(16, "Vita model closure rejected: %u records exceed %u\n",
            static_cast<unsigned>(collect.models.size()), kVitaMaxModelRecords);
        return false;
    }
    std::sort(collect.models.begin(), collect.models.end(), [](const XModel* left,
                                                               const XModel* right) {
        return I_stricmp(left->name, right->name) < 0;
    });

    std::vector<VitaModelBuild> models;
    models.reserve(collect.models.size());
    uint64_t dataBytes = 0;
    constexpr uint64_t kMaxDataBytes = kisak::vita::kVitaModelMaxPayloadBytes;
    for (const XModel* model : collect.models) {
        VitaModelBuild built{};
        const char* failureReason = nullptr;
        if (!DB_VitaBuildModel(model, &built, &failureReason)) {
            Com_PrintError(16, "Vita model closure rejected %s: %s\n",
                             model && model->name ? model->name : "<null>",
                             failureReason ? failureReason : "unknown failure");
            return false;
        }
        const uint64_t modelBytes = sizeof(VitaModelRecordHeader) + built.name.size()
            + sizeof(VitaModelSkinHeaderV5) + built.parents.size()
            + static_cast<uint64_t>(built.boneNames.size())
                * sizeof(VitaModelBoneName)
            + static_cast<uint64_t>(built.bones.size()) * sizeof(VitaModelBone)
            + static_cast<uint64_t>(built.surfaces.size()) * sizeof(VitaModelSurfaceRecord)
            + built.vertices.size() * sizeof(VitaModelVertex)
            + built.weights.size() * sizeof(VitaModelVertexWeight)
            + built.indices.size() * sizeof(uint16_t);
        uint64_t materialBytes = 0;
        for (const VitaModelSurfaceBuild& surface : built.surfaces)
            materialBytes += surface.materialName.size();
        const uint64_t candidateBytes = modelBytes + materialBytes;
        if (candidateBytes > kMaxDataBytes || dataBytes + candidateBytes > kMaxDataBytes) {
            Com_PrintError(16, "Vita model closure rejected at %s: %llu bytes exceed %llu\n",
                model->name, static_cast<unsigned long long>(dataBytes + candidateBytes),
                static_cast<unsigned long long>(kMaxDataBytes));
            return false;
        }
        dataBytes += candidateBytes;
        models.push_back(std::move(built));
    }

    const uint32_t magic = 0x58544956; // "VITX"
    const uint32_t version = 5;
    if (!DB_VitaWriteValue(file, magic) || !DB_VitaWriteValue(file, version)
        || !DB_VitaWriteValue(file, static_cast<uint32_t>(models.size()))
        || !DB_VitaWriteValue(file, dataBytes))
        return false;
    for (const VitaModelBuild& model : models) {
        VitaModelRecordHeader header{};
        header.nameBytes = static_cast<uint32_t>(model.name.size());
        header.surfaceCount = static_cast<uint32_t>(model.surfaces.size());
        header.vertexCount = static_cast<uint32_t>(model.vertices.size());
        header.indexCount = static_cast<uint32_t>(model.indices.size());
        std::memcpy(header.mins, model.mins, sizeof(header.mins));
        std::memcpy(header.maxs, model.maxs, sizeof(header.maxs));
        if (!DB_VitaWriteValue(file, header)
            || !DB_VitaWriteString(file, model.name.c_str(), header.nameBytes))
            return false;
        const VitaModelSkinHeaderV5 skinHeader{
            static_cast<uint32_t>(model.bones.size()),
            model.source ? model.source->numRootBones : 0u,
            static_cast<uint32_t>(model.parents.size()),
            (model.cameraTagBone << 16) | model.weaponTagBone,
            model.flashTagBone,
            static_cast<uint32_t>(model.boneNames.size()
                * sizeof(VitaModelBoneName)),
        };
        if (!DB_VitaWriteValue(file, skinHeader)
            || (!model.parents.empty()
                && !DB_VitaWrite(file, model.parents.data(),
                                 static_cast<uint32_t>(model.parents.size())))
            || (!model.boneNames.empty()
                && !DB_VitaWrite(file, model.boneNames.data(),
                                 static_cast<uint32_t>(model.boneNames.size()
                                     * sizeof(VitaModelBoneName))))
            || (!model.bones.empty()
                && !DB_VitaWrite(file, model.bones.data(),
                                 static_cast<uint32_t>(model.bones.size()
                                     * sizeof(VitaModelBone)))))
            return false;
        for (const VitaModelSurfaceBuild& surface : model.surfaces) {
            VitaModelSurfaceRecord record{};
            record.materialNameBytes = static_cast<uint32_t>(surface.materialName.size());
            record.vertexStart = surface.vertexStart;
            record.vertexCount = surface.vertexCount;
            record.indexStart = surface.indexStart;
            record.indexCount = surface.indexCount;
            if (!DB_VitaWriteValue(file, record)
                || !DB_VitaWriteString(file, surface.materialName.c_str(),
                                       record.materialNameBytes))
                return false;
        }
        if (!DB_VitaWrite(file, model.vertices.data(),
                          static_cast<uint32_t>(model.vertices.size() * sizeof(VitaModelVertex)))
            || !DB_VitaWrite(file, model.weights.data(),
                             static_cast<uint32_t>(model.weights.size()
                                 * sizeof(VitaModelVertexWeight)))
            || !DB_VitaWrite(file, model.indices.data(),
                             static_cast<uint32_t>(model.indices.size() * sizeof(uint16_t))))
            return false;
    }

    std::vector<const VitaModelBuild*> lodModels;
    lodModels.reserve(models.size());
    uint64_t lodDataBytes = 0u;
    uint64_t lodVertexCount = 0u;
    uint64_t lodIndexCount = 0u;
    for (const VitaModelBuild& model : models) {
        if (model.renderLods.empty())
            continue;
        uint64_t modelBytes = sizeof(VitaModelRenderLodModelHeader)
            + model.name.size();
        for (const VitaModelRenderLodBuild& lod : model.renderLods) {
            modelBytes += sizeof(VitaModelRenderLodHeader)
                + static_cast<uint64_t>(lod.surfaces.size())
                    * sizeof(VitaModelSurfaceRecord)
                + static_cast<uint64_t>(lod.vertices.size())
                    * sizeof(VitaModelVertex)
                + static_cast<uint64_t>(lod.indices.size())
                    * sizeof(uint16_t);
            for (const VitaModelSurfaceBuild& surface : lod.surfaces)
                modelBytes += surface.materialName.size();
        }
        if (modelBytes > kMaxDataBytes
            || lodDataBytes > kMaxDataBytes - modelBytes) {
            Com_PrintWarning(
                16, "Vita render LOD closure skipped %s: VLOD budget exhausted\n",
                model.name.c_str());
            continue;
        }
        lodDataBytes += modelBytes;
        for (const VitaModelRenderLodBuild& lod : model.renderLods) {
            lodVertexCount += lod.vertices.size();
            lodIndexCount += lod.indices.size();
        }
        lodModels.push_back(&model);
    }

    const uint32_t lodMagic = 0x444F4C56; // "VLOD"
    const uint32_t lodVersion = 1u;
    if (!DB_VitaWriteValue(renderLodFile, lodMagic)
        || !DB_VitaWriteValue(renderLodFile, lodVersion)
        || !DB_VitaWriteValue(renderLodFile,
                              static_cast<uint32_t>(lodModels.size()))
        || !DB_VitaWriteValue(renderLodFile, lodDataBytes))
        return false;
    for (const VitaModelBuild* model : lodModels) {
        VitaModelRenderLodModelHeader modelHeader{};
        modelHeader.nameBytes = static_cast<uint32_t>(model->name.size());
        modelHeader.lodCount = model->lodCount;
        modelHeader.renderLodCount = static_cast<uint32_t>(
            model->renderLods.size());
        modelHeader.lodRampType = model->lodRampType;
        std::memcpy(modelHeader.distances, model->lodDistances,
                    sizeof(modelHeader.distances));
        if (!DB_VitaWriteValue(renderLodFile, modelHeader)
            || !DB_VitaWriteString(renderLodFile, model->name.c_str(),
                                    modelHeader.nameBytes))
            return false;
        for (const VitaModelRenderLodBuild& lod : model->renderLods) {
            const VitaModelRenderLodHeader lodHeader{
                lod.lodIndex,
                static_cast<uint32_t>(lod.surfaces.size()),
                static_cast<uint32_t>(lod.vertices.size()),
                static_cast<uint32_t>(lod.indices.size()),
            };
            if (!DB_VitaWriteValue(renderLodFile, lodHeader))
                return false;
            for (const VitaModelSurfaceBuild& surface : lod.surfaces) {
                VitaModelSurfaceRecord record{};
                record.materialNameBytes = static_cast<uint32_t>(
                    surface.materialName.size());
                record.vertexStart = surface.vertexStart;
                record.vertexCount = surface.vertexCount;
                record.indexStart = surface.indexStart;
                record.indexCount = surface.indexCount;
                if (!DB_VitaWriteValue(renderLodFile, record)
                    || !DB_VitaWriteString(renderLodFile,
                                            surface.materialName.c_str(),
                                            record.materialNameBytes))
                    return false;
            }
            if (!DB_VitaWrite(
                    renderLodFile, lod.vertices.data(),
                    static_cast<uint32_t>(lod.vertices.size()
                                          * sizeof(VitaModelVertex)))
                || !DB_VitaWrite(
                    renderLodFile, lod.indices.data(),
                    static_cast<uint32_t>(lod.indices.size()
                                          * sizeof(uint16_t))))
                return false;
        }
    }
    Com_Printf(
        16,
        "Vita render LOD export: models=%u vertices=%llu indices=%llu bytes=%llu\n",
        static_cast<unsigned>(lodModels.size()),
        static_cast<unsigned long long>(lodVertexCount),
        static_cast<unsigned long long>(lodIndexCount),
        static_cast<unsigned long long>(lodDataBytes + 20u));
    return true;
}

struct VitaRawFileDumpContext
{
    int file;
    bool write;
    uint32_t count;
    uint64_t dataBytes;
};

static void __cdecl DB_DumpVitaRawFile(XAssetHeader header, void *rawContext)
{
    auto *context = static_cast<VitaRawFileDumpContext *>(rawContext);
    if (!context || !header.rawfile || !header.rawfile->name
        || header.rawfile->len < 0 || header.rawfile->len > 32 * 1024 * 1024)
        return;
    const size_t nameBytes = std::strlen(header.rawfile->name);
    if (nameBytes == 0 || nameBytes > 4096)
        return;
    if (context->dataBytes + static_cast<uint64_t>(header.rawfile->len) > 256ull * 1024ull * 1024ull)
        return;
    if (context->write)
    {
        const uint32_t nameLength = static_cast<uint32_t>(nameBytes);
        const uint32_t dataLength = static_cast<uint32_t>(header.rawfile->len);
        if (!DB_VitaWriteValue(context->file, nameLength)
            || !DB_VitaWriteValue(context->file, dataLength)
            || !DB_VitaWrite(context->file, header.rawfile->name, nameLength)
            || (dataLength && (!header.rawfile->buffer
                || !DB_VitaWrite(context->file, header.rawfile->buffer, dataLength))))
            return;
    }
    ++context->count;
    context->dataBytes += static_cast<uint64_t>(header.rawfile->len);
}

static bool DB_DumpVitaRawFiles(int file)
{
    VitaRawFileDumpContext countContext{file, false, 0, 0};
    DB_EnumXAssets(ASSET_TYPE_RAWFILE, DB_DumpVitaRawFile, &countContext, 0);
    const uint32_t magic = 0x52544956; // "VITR"
    const uint32_t version = 1;
    if (!DB_VitaWriteValue(file, magic) || !DB_VitaWriteValue(file, version)
        || !DB_VitaWriteValue(file, countContext.count)
        || !DB_VitaWriteValue(file, countContext.dataBytes))
        return false;
    VitaRawFileDumpContext writeContext{file, true, 0, 0};
    DB_EnumXAssets(ASSET_TYPE_RAWFILE, DB_DumpVitaRawFile, &writeContext, 0);
    return writeContext.count == countContext.count
        && writeContext.dataBytes == countContext.dataBytes;
}

// Localized UI text is a tiny pointer graph in the desktop database. Export
// only the stable UTF-8 key/value pairs so Vita menu labels can resolve the
// original @MENU_* tokens without importing LocalizeEntry pointers.
struct VitaLocalizeDumpContext
{
    int file;
    bool write;
    uint32_t count;
    uint64_t dataBytes;
};

static void __cdecl DB_DumpVitaLocalize(XAssetHeader header, void *rawContext)
{
    auto *context = static_cast<VitaLocalizeDumpContext *>(rawContext);
    const LocalizeEntry *entry = header.localize;
    if (!context || !entry || !entry->name || !entry->value)
        return;
    uint32_t nameBytes = 0;
    uint32_t valueBytes = 0;
    if (!DB_VitaBoundedString(entry->name, &nameBytes)
        || !DB_VitaBoundedString(entry->value, &valueBytes)
        || nameBytes == 0 || nameBytes > 4096 || valueBytes > 4096)
        return;
    const uint64_t recordBytes = 8ull + nameBytes + valueBytes;
    if (context->count >= 32768
        || context->dataBytes > 8ull * 1024ull * 1024ull - recordBytes)
        return;
    if (context->write
        && (!DB_VitaWriteValue(context->file, nameBytes)
            || !DB_VitaWriteValue(context->file, valueBytes)
            || !DB_VitaWriteString(context->file, entry->name, nameBytes)
            || !DB_VitaWriteString(context->file, entry->value, valueBytes)))
        return;
    ++context->count;
    context->dataBytes += recordBytes;
}

static bool DB_DumpVitaLocalizeEntries(int file)
{
    VitaLocalizeDumpContext countContext{file, false, 0, 0};
    DB_EnumXAssets(ASSET_TYPE_LOCALIZE_ENTRY, DB_DumpVitaLocalize, &countContext, 0);
    const uint32_t magic = 0x5A544956; // "VITZ"
    const uint32_t version = 1;
    if (!DB_VitaWriteValue(file, magic) || !DB_VitaWriteValue(file, version)
        || !DB_VitaWriteValue(file, countContext.count)
        || !DB_VitaWriteValue(file, countContext.dataBytes))
        return false;
    VitaLocalizeDumpContext writeContext{file, true, 0, 0};
    DB_EnumXAssets(ASSET_TYPE_LOCALIZE_ENTRY, DB_DumpVitaLocalize, &writeContext, 0);
    return writeContext.count == countContext.count
        && writeContext.dataBytes == countContext.dataBytes;
}

// Menu definitions are parsed on the desktop from text and then populated
// with engine-owned pointers. Export only stable presentation/action fields;
// never serialize a native menuDef_t pointer or expression tree.
#pragma pack(push, 1)
struct VitaMenuRecordHeader
{
    uint32_t nameBytes;
    uint32_t fontBytes;
    uint32_t itemCount;
    uint32_t fullScreen;
    int32_t style;
    float rect[4];
    float foreColor[4];
    float backColor[4];
    float focusColor[4];
    float disableColor[4];
};

struct VitaMenuItemHeader
{
    uint32_t nameBytes;
    uint32_t textBytes;
    uint32_t actionBytes;
    uint32_t onAcceptBytes;
    int32_t type;
    int32_t alignment;
    int32_t textStyle;
    uint32_t flags;
    float rect[4];
    float textRect[4];
    float textScale;
};
#pragma pack(pop)

static_assert(sizeof(VitaMenuRecordHeader) == 100);
static_assert(sizeof(VitaMenuItemHeader) == 68);

struct VitaMenuDumpContext
{
    int file;
    bool write;
    uint32_t count;
    uint64_t bytes;
};

static bool DB_VitaBoundedString(const char *value, uint32_t *bytes)
{
    if (!bytes)
        return false;
    *bytes = 0;
    if (!value)
        return true;
    uint32_t length = 0;
    while (length < 4096 && value[length])
        ++length;
    if (length == 4096 && value[length])
        return false;
    *bytes = length;
    return true;
}

static bool DB_VitaWriteString(int file, const char *value, uint32_t bytes)
{
    return bytes == 0 || (value && DB_VitaWrite(file, value, bytes));
}

// StringTable assets are the source data behind the authored menu
// `tablelookup` actions.  Export their bounded row-major cells instead of the
// desktop pointer graph so Vita can evaluate the same weapon, attachment,
// perk, and class-template lookups locally.
#pragma pack(push, 1)
struct VitaStringTableRecordHeader
{
    uint32_t nameBytes;
    uint32_t rowCount;
    uint32_t columnCount;
};
#pragma pack(pop)

static_assert(sizeof(VitaStringTableRecordHeader) == 12);

struct VitaStringTableDumpContext
{
    int file;
    bool write;
    uint32_t count;
    uint64_t bytes;
};

static bool DB_VitaStringTableShape(const StringTable *table, const char *tableName,
                                    uint32_t *nameBytes, uint64_t *recordBytes)
{
    if (!table || !tableName || !*tableName || !nameBytes || !recordBytes
        || table->rowCount < 0 || table->rowCount > 4096
        || table->columnCount < 0 || table->columnCount > 4096)
        return false;
    if (static_cast<uint64_t>(table->rowCount)
            * static_cast<uint64_t>(table->columnCount)
        > 16ull * 1024ull * 1024ull)
        return false;
    if (!DB_VitaBoundedString(tableName, nameBytes)
        || *nameBytes == 0 || *nameBytes > 4096)
        return false;

    uint64_t bytes = sizeof(VitaStringTableRecordHeader) + *nameBytes;
    for (int32_t row = 0; row < table->rowCount; ++row)
    {
        for (int32_t column = 0; column < table->columnCount; ++column)
        {
            const uint64_t index = static_cast<uint64_t>(row)
                * static_cast<uint64_t>(table->columnCount)
                + static_cast<uint64_t>(column);
            const char *value = table->values ? table->values[index] : nullptr;
            uint32_t valueBytes = 0;
            if (!DB_VitaBoundedString(value, &valueBytes)
                || valueBytes > 4096)
                return false;
            bytes += sizeof(uint32_t) + valueBytes;
            if (bytes > 16ull * 1024ull * 1024ull)
                return false;
        }
    }
    *recordBytes = bytes;
    return true;
}

static void __cdecl DB_DumpVitaStringTable(XAssetHeader header, void *rawContext)
{
    auto *context = static_cast<VitaStringTableDumpContext *>(rawContext);
    const StringTable *table = header.stringTable;
    if (!context || !table)
        return;
    const char *tableName = DB_GetXAssetHeaderName(ASSET_TYPE_STRINGTABLE, &header);
    if (!tableName || !*tableName)
        tableName = table->name;
    uint32_t nameBytes = 0;
    uint64_t recordBytes = 0;
    if (!DB_VitaStringTableShape(table, tableName, &nameBytes, &recordBytes)
        || context->count >= 32768
        || context->bytes > 16ull * 1024ull * 1024ull - recordBytes)
        return;
    if (context->write)
    {
        VitaStringTableRecordHeader record{};
        record.nameBytes = nameBytes;
        record.rowCount = static_cast<uint32_t>(table->rowCount);
        record.columnCount = static_cast<uint32_t>(table->columnCount);
        if (!DB_VitaWrite(context->file, &record, sizeof(record))
            || !DB_VitaWriteString(context->file, tableName, nameBytes))
            return;
        for (int32_t row = 0; row < table->rowCount; ++row)
        {
            for (int32_t column = 0; column < table->columnCount; ++column)
            {
                const uint64_t index = static_cast<uint64_t>(row)
                    * static_cast<uint64_t>(table->columnCount)
                    + static_cast<uint64_t>(column);
                const char *value = table->values ? table->values[index] : nullptr;
                uint32_t valueBytes = 0;
                if (!DB_VitaBoundedString(value, &valueBytes)
                    || !DB_VitaWriteValue(context->file, valueBytes)
                    || !DB_VitaWriteString(context->file, value, valueBytes))
                    return;
            }
        }
    }
    ++context->count;
    context->bytes += recordBytes;
}

static bool DB_DumpVitaStringTables(int file)
{
    VitaStringTableDumpContext countContext{file, false, 0, 0};
    DB_EnumXAssets(ASSET_TYPE_STRINGTABLE, DB_DumpVitaStringTable, &countContext, 0);
    const uint32_t magic = 0x54544956; // "VITT"
    const uint32_t version = 1;
    if (!DB_VitaWriteValue(file, magic) || !DB_VitaWriteValue(file, version)
        || !DB_VitaWriteValue(file, countContext.count)
        || !DB_VitaWriteValue(file, countContext.bytes))
        return false;
    VitaStringTableDumpContext writeContext{file, true, 0, 0};
    DB_EnumXAssets(ASSET_TYPE_STRINGTABLE, DB_DumpVitaStringTable, &writeContext, 0);
    return writeContext.count == countContext.count
        && writeContext.bytes == countContext.bytes;
}

// A few retail scorebar expressions evaluate a live match clock.  That value
// is intentionally not serialized into VITM: it changes with the wall-clock
// moment at which the host oracle happens to dump its menu pool, making an
// otherwise identical package differ by one byte.  The Vita HUD owns the
// runtime score/time presentation, so omit this expression result from the
// static menu payload while retaining all authored labels and geometry.
static bool DB_VitaIsDynamicClockText(const char *value)
{
    if (!value)
        return false;
    const char *cursor = value;
    while (*cursor == ' ' || *cursor == '\t')
        ++cursor;
    uint32_t minutes = 0;
    while (*cursor >= '0' && *cursor <= '9')
    {
        ++minutes;
        ++cursor;
    }
    if (minutes == 0 || *cursor++ != ':')
        return false;
    for (int index = 0; index < 2; ++index)
    {
        if (*cursor < '0' || *cursor > '9')
            return false;
        ++cursor;
    }
    if (*cursor++ != '.')
        return false;
    uint32_t tenths = 0;
    while (*cursor >= '0' && *cursor <= '9')
    {
        ++tenths;
        ++cursor;
    }
    while (*cursor == ' ' || *cursor == '\t')
        ++cursor;
    return tenths > 0 && *cursor == '\0';
}

// COD4's menu parser often leaves item->text empty and stores the visible
// label in an expression or a dvar-backed value. Resolve only structurally
// valid, bounded expression trees while the Windows oracle owns the live UI
// context; the Vita package receives only the copied result, never pointers.
static const char *DB_VitaResolvedMenuText(const itemDef_s *item,
                                           char *scratch, size_t scratchBytes)
{
    if (!item || !scratch || scratchBytes < 2)
        return "";
    scratch[0] = '\0';
    const char *value = item->text;
    bool resolvedExpression = false;
    if (!value || !*value)
    {
        const statement_s &expression = item->textExp;
        bool safeExpression = expression.numEntries > 0 && expression.numEntries <= 32
            && expression.entries != nullptr;
        if (safeExpression)
        {
            for (int index = 0; index < expression.numEntries; ++index)
            {
                const expressionEntry *entry = expression.entries[index];
                if (!entry || (entry->type != 0 && entry->type != 1))
                {
                    safeExpression = false;
                    break;
                }
                if (entry->type == 0)
                {
                    const int operation = static_cast<int>(entry->data.op);
                    if (operation < 0 || operation >= NUM_OPERATORS)
                    {
                        safeExpression = false;
                        break;
                    }
                }
                else if (entry->data.operand.dataType == VAL_STRING)
                {
                    const char *string = entry->data.operand.internals.string;
                    uint32_t stringBytes = 0;
                    if (!string || !DB_VitaBoundedString(string, &stringBytes)
                        || stringBytes >= scratchBytes)
                    {
                        safeExpression = false;
                        break;
                    }
                }
                else if (entry->data.operand.dataType != VAL_INT
                         && entry->data.operand.dataType != VAL_FLOAT)
                {
                    safeExpression = false;
                    break;
                }
            }
        }
        if (safeExpression)
        {
            value = GetExpressionResultString(0, &expression);
            resolvedExpression = true;
        }
    }
    if (!value || !*value)
        return "";
    if (resolvedExpression && DB_VitaIsDynamicClockText(value))
        return "";
    std::strncpy(scratch, value, scratchBytes - 1);
    scratch[scratchBytes - 1] = '\0';
    return scratch;
}

static bool DB_VitaMenuShape(const menuDef_t *menu, const char *menuName,
                             uint32_t *recordBytes)
{
    if (!menu || !menuName || !*menuName || !recordBytes
        || menu->itemCount < 0 || menu->itemCount > 256
        || (menu->itemCount && !menu->items))
        return false;
    uint32_t nameBytes = 0;
    uint32_t fontBytes = 0;
    if (!DB_VitaBoundedString(menuName, &nameBytes)
        || !DB_VitaBoundedString(menu->font, &fontBytes))
        return false;
    uint64_t bytes = sizeof(VitaMenuRecordHeader) + nameBytes + fontBytes;
    for (int32_t index = 0; index < menu->itemCount; ++index)
    {
        const itemDef_s *item = menu->items[index];
        bytes += sizeof(VitaMenuItemHeader);
        if (!item)
            continue;
        uint32_t itemNameBytes = 0;
        uint32_t textBytes = 0;
        uint32_t actionBytes = 0;
        uint32_t onAcceptBytes = 0;
        char resolvedText[1024]{};
        if (!DB_VitaBoundedString(item->window.name, &itemNameBytes)
            || !DB_VitaBoundedString(DB_VitaResolvedMenuText(item, resolvedText,
                                                              sizeof(resolvedText)),
                                     &textBytes)
            || !DB_VitaBoundedString(item->action, &actionBytes)
            || !DB_VitaBoundedString(item->onAccept, &onAcceptBytes))
            return false;
        bytes += itemNameBytes + textBytes + actionBytes + onAcceptBytes;
    }
    if (bytes > 16ull * 1024ull * 1024ull)
        return false;
    *recordBytes = static_cast<uint32_t>(bytes);
    return true;
}

static void __cdecl DB_DumpVitaMenu(XAssetHeader header, void *rawContext)
{
    auto *context = static_cast<VitaMenuDumpContext *>(rawContext);
    if (!context || !header.menu)
        return;
    const char *menuName = DB_GetXAssetHeaderName(ASSET_TYPE_MENU, &header);
    if (!menuName || !*menuName)
        menuName = header.menu->window.name;
    uint32_t recordBytes = 0;
    if (!DB_VitaMenuShape(header.menu, menuName, &recordBytes)
        || context->bytes + recordBytes > 16ull * 1024ull * 1024ull)
        return;
    if (context->write)
    {
        const menuDef_t &menu = *header.menu;
        VitaMenuRecordHeader record{};
        DB_VitaBoundedString(menuName, &record.nameBytes);
        DB_VitaBoundedString(menu.font, &record.fontBytes);
        record.itemCount = static_cast<uint32_t>(menu.itemCount);
        record.fullScreen = menu.fullScreen ? 1u : 0u;
        record.style = menu.window.style;
        record.rect[0] = menu.window.rect.x;
        record.rect[1] = menu.window.rect.y;
        record.rect[2] = menu.window.rect.w;
        record.rect[3] = menu.window.rect.h;
        std::memcpy(record.foreColor, menu.window.foreColor, sizeof(record.foreColor));
        std::memcpy(record.backColor, menu.window.backColor, sizeof(record.backColor));
        std::memcpy(record.focusColor, menu.focusColor, sizeof(record.focusColor));
        std::memcpy(record.disableColor, menu.disableColor, sizeof(record.disableColor));
        if (!DB_VitaWrite(context->file, &record, sizeof(record))
            || !DB_VitaWriteString(context->file, menuName, record.nameBytes)
            || !DB_VitaWriteString(context->file, menu.font, record.fontBytes))
            return;
        for (int32_t index = 0; index < menu.itemCount; ++index)
        {
            const itemDef_s *item = menu.items[index];
            VitaMenuItemHeader itemRecord{};
            char resolvedText[1024]{};
            if (item)
            {
                DB_VitaBoundedString(item->window.name, &itemRecord.nameBytes);
                const char *text = DB_VitaResolvedMenuText(item, resolvedText,
                                                           sizeof(resolvedText));
                DB_VitaBoundedString(text, &itemRecord.textBytes);
                DB_VitaBoundedString(item->action, &itemRecord.actionBytes);
                DB_VitaBoundedString(item->onAccept, &itemRecord.onAcceptBytes);
                itemRecord.type = item->type;
                itemRecord.alignment = item->alignment;
                itemRecord.textStyle = item->textStyle;
                itemRecord.flags = static_cast<uint32_t>(item->itemFlags);
                itemRecord.rect[0] = item->window.rect.x;
                itemRecord.rect[1] = item->window.rect.y;
                itemRecord.rect[2] = item->window.rect.w;
                itemRecord.rect[3] = item->window.rect.h;
                itemRecord.textRect[0] = item->textRect[0].x;
                itemRecord.textRect[1] = item->textRect[0].y;
                itemRecord.textRect[2] = item->textRect[0].w;
                itemRecord.textRect[3] = item->textRect[0].h;
                itemRecord.textScale = item->textscale;
            }
            if (!DB_VitaWrite(context->file, &itemRecord, sizeof(itemRecord))
                || (item && !DB_VitaWriteString(context->file, item->window.name,
                                                 itemRecord.nameBytes))
                || (item && !DB_VitaWriteString(
                                 context->file,
                                 DB_VitaResolvedMenuText(item, resolvedText,
                                                         sizeof(resolvedText)),
                                 itemRecord.textBytes))
                || (item && !DB_VitaWriteString(context->file, item->action,
                                                 itemRecord.actionBytes))
                || (item && !DB_VitaWriteString(context->file, item->onAccept,
                                                 itemRecord.onAcceptBytes)))
                return;
        }
    }
    ++context->count;
    context->bytes += recordBytes;
}

static bool DB_DumpVitaMenus(int file)
{
    VitaMenuDumpContext countContext{file, false, 0, 0};
    DB_EnumXAssets(ASSET_TYPE_MENU, DB_DumpVitaMenu, &countContext, 0);
    const uint32_t magic = 0x4D544956; // "VITM"
    const uint32_t version = 1;
    if (!DB_VitaWriteValue(file, magic) || !DB_VitaWriteValue(file, version)
        || !DB_VitaWriteValue(file, countContext.count)
        || !DB_VitaWriteValue(file, countContext.bytes))
        return false;
    VitaMenuDumpContext writeContext{file, true, 0, 0};
    DB_EnumXAssets(ASSET_TYPE_MENU, DB_DumpVitaMenu, &writeContext, 0);
    return writeContext.count == countContext.count
        && writeContext.bytes == countContext.bytes;
}

#include <vita/vita_sound_alias_record.h>

struct VitaSoundAliasDumpContext {
    std::vector<kisak::vita::VitaSoundAliasRecord> records;
    bool valid = true;
};
static void DB_DumpVitaSoundAlias(XAssetHeader header, void* user) {
    auto& context = *static_cast<VitaSoundAliasDumpContext*>(user);
    const snd_alias_list_t* list = header.sound;
    if (!list || !list->aliasName || list->count <= 0 || !list->head) return;
    if (list->count > 256 || context.records.size() + list->count > kisak::vita::kVitaSoundAliasMaxRecords) {
        context.valid = false; return;
    }
    const auto text = [&context](char (&out)[128], const char* input) {
        if (!input) return;
        const size_t length = std::strlen(input);
        if (length >= sizeof(out)) { context.valid = false; return; }
        std::memcpy(out, input, length + 1);
    };
    for (int i = 0; i < list->count; ++i) {
        const snd_alias_t& alias = list->head[i];
        kisak::vita::VitaSoundAliasRecord record{};
        text(record.aliasName, list->aliasName);
        text(record.subtitle, alias.subtitle);
        text(record.secondaryAliasName, alias.secondaryAliasName);
        text(record.chainAliasName, alias.chainAliasName);
        record.sequence = 0; // selection history starts anew on each map
        record.flags = alias.flags; record.startDelay = alias.startDelay;
        record.volMin = alias.volMin; record.volMax = alias.volMax;
        record.pitchMin = alias.pitchMin; record.pitchMax = alias.pitchMax;
        record.distMin = alias.distMin; record.distMax = alias.distMax;
        record.slavePercentage = alias.slavePercentage; record.probability = alias.probability;
        record.lfePercentage = alias.lfePercentage; record.centerPercentage = alias.centerPercentage;
        record.envelopMin = alias.envelopMin; record.envelopMax = alias.envelopMax;
        record.envelopPercentage = alias.envelopPercentage;
        if (alias.soundFile) {
            record.soundType = alias.soundFile->type;
            record.sourceFileExists = alias.soundFile->exists;
            char name[256]{};
            Com_GetSoundFileName(&alias, name, sizeof(name));
            text(record.soundFile, name);
        }
        if (alias.volumeFalloffCurve) {
            text(record.curveName, alias.volumeFalloffCurve->filename);
            record.curveKnotCount = alias.volumeFalloffCurve->knotCount;
            if (record.curveKnotCount > 8) { context.valid = false; return; }
            std::memcpy(record.curveKnots, alias.volumeFalloffCurve->knots, sizeof(record.curveKnots));
        }
        if (alias.speakerMap) {
            text(record.speakerMapName, alias.speakerMap->name);
            record.speakerMapDefault = alias.speakerMap->isDefault;
            static_assert(sizeof(record.speakerMaps) == sizeof(alias.speakerMap->channelMaps));
            std::memcpy(record.speakerMaps, alias.speakerMap->channelMaps, sizeof(record.speakerMaps));
        }
        context.records.push_back(record);
    }
}
static bool DB_DumpVitaSoundAliases(int file) {
    VitaSoundAliasDumpContext context;
    DB_EnumXAssets(ASSET_TYPE_SOUND, DB_DumpVitaSoundAlias, &context, 0);
    if (!context.valid || context.records.empty()) return false;
    // Stable alias ordering; the original order of variants within a list is retained.
    std::stable_sort(context.records.begin(), context.records.end(), [](const auto& a, const auto& b) {
        return std::strcmp(a.aliasName, b.aliasName) < 0;
    });
    const uint32_t count = static_cast<uint32_t>(context.records.size());
    const uint64_t bytes = context.records.size() * sizeof(context.records[0]);
    return DB_VitaWriteValue(file, kisak::vita::kVitaSoundAliasMagic)
        && DB_VitaWriteValue(file, kisak::vita::kVitaSoundAliasVersion)
        && DB_VitaWriteValue(file, count) && DB_VitaWriteValue(file, bytes)
        && DB_VitaWrite(file, context.records.data(), static_cast<uint32_t>(bytes));
}

#pragma pack(push, 1)
struct VitaSoundRecordHeader
{
    uint32_t nameBytes;
    uint32_t sampleBytes;
    uint32_t rate;
    uint32_t bits;
    uint32_t channels;
    uint32_t samples;
    uint32_t format;
};
#pragma pack(pop)

static_assert(sizeof(VitaSoundRecordHeader) == 28);

struct VitaSoundDumpContext
{
    int file;
    bool write;
    uint32_t count;
    uint64_t bytes;
    std::unordered_set<std::string> names;
    uint64_t limit = 80ull * 1024ull * 1024ull;
    bool overflow = false;
};

static void DB_DumpVitaSoundRecord(const LoadedSound *sound, VitaSoundDumpContext *context)
{
    if (!context || !sound || !sound->name || !sound->sound.info.data_ptr
        || sound->sound.info.data_len == 0
        || sound->sound.info.data_len > 8u * 1024u * 1024u
        || sound->sound.info.format != 1 || sound->sound.info.bits != 16
        || (sound->sound.info.channels != 1 && sound->sound.info.channels != 2)
        || sound->sound.info.rate < 8000 || sound->sound.info.rate > 96000)
        return;
    uint32_t nameBytes = 0;
    if (!DB_VitaBoundedString(sound->name, &nameBytes) || nameBytes == 0)
        return;
    const uint64_t recordBytes = sizeof(VitaSoundRecordHeader) + nameBytes
        + static_cast<uint64_t>(sound->sound.info.data_len);
    if (context->names.count(sound->name)) return;
    if (recordBytes > 16ull * 1024ull * 1024ull
        || context->bytes + recordBytes > context->limit) {
        context->overflow = true;
        return;
    }
    context->names.emplace(sound->name);
    if (context->write)
    {
        VitaSoundRecordHeader record{};
        record.nameBytes = nameBytes;
        record.sampleBytes = sound->sound.info.data_len;
        record.rate = sound->sound.info.rate;
        record.bits = sound->sound.info.bits;
        record.channels = sound->sound.info.channels;
        // Miles reports `samples` as interleaved sample elements for some
        // stereo assets.  VITS defines this field as PCM frames so the Vita
        // mixer can advance one frame at a time without channel-dependent
        // over-read.
        const uint64_t frameCount = static_cast<uint64_t>(sound->sound.info.data_len)
            / (static_cast<uint64_t>(sound->sound.info.channels) * 2ull);
        if (frameCount == 0 || frameCount > UINT32_MAX)
            return;
        record.samples = static_cast<uint32_t>(frameCount);
        record.format = sound->sound.info.format;
        if (!DB_VitaWrite(context->file, &record, sizeof(record))
            || !DB_VitaWriteString(context->file, sound->name, nameBytes)
            || !DB_VitaWrite(context->file, sound->sound.info.data_ptr,
                             sound->sound.info.data_len))
            return;
    }
    ++context->count;
    // The VITS aggregate stores decoded PCM bytes only; record headers and
    // names are validated from the stream layout.
    context->bytes += static_cast<uint64_t>(sound->sound.info.data_len);
}

static void __cdecl DB_DumpVitaLoadedSound(XAssetHeader header, void *rawContext)
{
    auto *context = static_cast<VitaSoundDumpContext *>(rawContext);
    DB_DumpVitaSoundRecord(header.loadSnd, context);
}

// Weapon reload events resolve these aliases from the sound-alias pool rather
// than the ASSET_TYPE_LOADED_SOUND pool.  Capture their decoded PCM directly
// while the desktop oracle still owns the alias pointers; otherwise Vita would
// silently miss the authentic clipout/clipin/chamber cues.
static constexpr const char *kVitaWeaponSoundAliases[] = {
    "weap_m4carbine_clipout_plr", "weap_m4carbine_clipin_plr",
    "weap_m4carbine_chamber_close_plr", "weap_mp5_clipout_plr",
    "weap_mp5_clipin_plr", "weap_mp5_chamber_plr",
    "weap_usp45_clipout_plr", "weap_usp45_clipin_plr",
    "weap_usp45_chamber_plr",
};

// These decoded paths are the actual Vita presentation dependencies.  They
// are enumerated before the general loaded-sound pool so a bounded VITS
// stream cannot crowd out an exact fire/UI/footstep cue with unrelated retail
// records.
static constexpr const char *kVitaRequiredSoundPaths[] = {
    "foley/wpfoly_m4_reload_chamber.wav",
    "foley/wpfoly_m4_reload_clipin.wav",
    "foley/wpfoly_m4_reload_clipout.wav",
    "foley/wpfoly_mp5_reload_chamber_v3.wav",
    "foley/wpfoly_mp5_reload_clipin_v4.wav",
    "foley/wpfoly_mp5_reload_clipout_v4.wav",
    "foley/wpfoly_usp_reload_chamber_v1.wav",
    "foley/wpfoly_usp_reload_clipin_v1.wav",
    "foley/wpfoly_usp_reload_clipout_v2.wav",
    "footsteps/foot_metal04.wav",
    "footsteps/foot_wood02.wav",
    "user_interface/ui_over_v1.wav",
    "user_interface/ui_screen_in1.wav",
    "user_interface/ui_screen_out1.wav",
    "user_interface/ui_select_v1.wav",
    "weapons/colt45/weap_usp45_slst_y1b.wav",
    "weapons/m4/weap_m4_slmn_2c1.wav",
    "weapons/m4/weap_m4_slst_1c4.wav",
    "weapons/mp5/weap_mp5_silencer_slst_2d.wav",
    "weapons/mp5/weap_mp5_slst_4.wav",
};

static void DB_DumpVitaWeaponAliasSound(const char *aliasName,
                                        VitaSoundDumpContext *context)
{
    if (!aliasName || !context)
        return;
    const snd_alias_list_t *alias = Com_TryFindSoundAlias(aliasName);
    if (!alias || !alias->head || !alias->head->soundFile
        || alias->head->soundFile->type != 1)
        return;
    DB_DumpVitaSoundRecord(alias->head->soundFile->u.loadSnd, context);
}

static void __cdecl DB_DumpVitaRequiredLoadedSound(XAssetHeader header,
                                                    void *rawContext)
{
    auto *context = static_cast<VitaSoundDumpContext *>(rawContext);
    const LoadedSound *sound = header.loadSnd;
    if (!context || !sound || !sound->name)
        return;
    for (const char *required : kVitaRequiredSoundPaths) {
        if (std::strcmp(sound->name, required) == 0) {
            DB_DumpVitaSoundRecord(sound, context);
            return;
        }
    }
}

static bool DB_DumpVitaSounds(int file)
{
    VitaSoundDumpContext countContext{file, false, 0, 0, {}};
    for (const char *alias : kVitaWeaponSoundAliases)
        DB_DumpVitaWeaponAliasSound(alias, &countContext);
    DB_EnumXAssets(ASSET_TYPE_LOADED_SOUND, DB_DumpVitaRequiredLoadedSound,
                   &countContext, 0);
    DB_EnumXAssets(ASSET_TYPE_LOADED_SOUND, DB_DumpVitaLoadedSound, &countContext, 0);
    const uint32_t magic = 0x53544956; // "VITS"
    const uint32_t version = 1;
    if (!DB_VitaWriteValue(file, magic) || !DB_VitaWriteValue(file, version)
        || !DB_VitaWriteValue(file, countContext.count)
        || !DB_VitaWriteValue(file, countContext.bytes))
        return false;
    VitaSoundDumpContext writeContext{file, true, 0, 0, {}};
    for (const char *alias : kVitaWeaponSoundAliases)
        DB_DumpVitaWeaponAliasSound(alias, &writeContext);
    DB_EnumXAssets(ASSET_TYPE_LOADED_SOUND, DB_DumpVitaRequiredLoadedSound,
                   &writeContext, 0);
    DB_EnumXAssets(ASSET_TYPE_LOADED_SOUND, DB_DumpVitaLoadedSound, &writeContext, 0);
    return writeContext.count == countContext.count
        && writeContext.bytes == countContext.bytes;
}

static void DB_DumpVitaAliasPcm(XAssetHeader header, void* user) {
    const auto* list = header.sound;
    if (!list || !list->head || list->count < 1 || list->count > 256) return;
    for (int index = 0; index < list->count; ++index) {
        const auto* file = list->head[index].soundFile;
        if (file && file->type == SAT_LOADED && file->exists)
            DB_DumpVitaSoundRecord(file->u.loadSnd, static_cast<VitaSoundDumpContext*>(user));
    }
}

// Complete decoded source catalog for offline dependency selection. It is an
// exporter input, never a resident Vita allocation or an unfiltered pack.
static bool DB_DumpVitaSoundPool(int file) {
    VitaSoundDumpContext count{file, false, 0, 0, {}, 256ull << 20};
    DB_EnumXAssets(ASSET_TYPE_SOUND, DB_DumpVitaAliasPcm, &count, 0);
    DB_EnumXAssets(ASSET_TYPE_LOADED_SOUND, DB_DumpVitaLoadedSound, &count, 0);
    if (count.overflow || !DB_VitaWriteValue(file, uint32_t(0x53544956))
        || !DB_VitaWriteValue(file, uint32_t(1)) || !DB_VitaWriteValue(file, count.count)
        || !DB_VitaWriteValue(file, count.bytes)) return false;
    VitaSoundDumpContext write{file, true, 0, 0, {}, 256ull << 20};
    DB_EnumXAssets(ASSET_TYPE_SOUND, DB_DumpVitaAliasPcm, &write, 0);
    DB_EnumXAssets(ASSET_TYPE_LOADED_SOUND, DB_DumpVitaLoadedSound, &write, 0);
    return !write.overflow && write.count == count.count && write.bytes == count.bytes;
}

#pragma pack(push, 1)
struct VitaImageRecordHeader
{
    uint32_t nameBytes;
    uint32_t dataBytes;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t levelCount;
    uint32_t format;
    uint32_t flags;
};
#pragma pack(pop)

static_assert(sizeof(VitaImageRecordHeader) == 32);

struct VitaImageDumpContext
{
    int file;
    bool write;
    uint32_t count;
    uint64_t dataBytes;
};

struct VitaGeneratedImageCapture
{
    std::string name;
    std::vector<uint8_t> bytes;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levelCount = 1;
    uint32_t format = 0;
    uint32_t flags = 0;
};

static std::vector<VitaGeneratedImageCapture> g_vitaGeneratedImages;
static uint32_t g_vitaGeneratedCubeCalls;
static uint32_t g_vitaLoadedCubeCalls;
static uint32_t g_vitaReflectionImageNames;
static uint32_t g_vitaReflectionCaptureAttempts;
static uint32_t g_vitaReflectionCaptureSuccesses;

static bool DB_VitaGeneratedExportEnabled()
{
    const char *exportMode = std::getenv("KISAK_VITA_EXPORT");
    return exportMode && *exportMode;
}

static VitaGeneratedImageCapture *DB_VitaFindGeneratedImage(const char *name)
{
    if (!name || !*name)
        return nullptr;
    auto found = std::find_if(
        g_vitaGeneratedImages.begin(), g_vitaGeneratedImages.end(),
        [&](const VitaGeneratedImageCapture &capture) {
            return capture.name == name;
        });
    if (found == g_vitaGeneratedImages.end())
    {
        g_vitaGeneratedImages.push_back(VitaGeneratedImageCapture{});
        found = std::prev(g_vitaGeneratedImages.end());
    }
    return &*found;
}

void __cdecl DB_VitaCaptureGeneratedImage(const GfxImage *image,
                                          const uint8_t *pixels,
                                          int32_t width, int32_t height,
                                          uint32_t imageFormat)
{
    // Image_Generate2D is shared by ordinary PC images and generated render
    // targets.  Only retain the bounded BSP lightmap family while the explicit
    // exporter environment is active, so normal Windows runs do not acquire a
    // second copy of renderer-owned pixels.
    if (!DB_VitaGeneratedExportEnabled() || !image || !image->name || !pixels
        || width <= 0 || height <= 0
        || std::strncmp(image->name, "*lightmap", 9) != 0
        || (imageFormat != 50u && imageFormat != 21u))
        return;
    const uint64_t bytesPerPixel = imageFormat == 50u ? 1ull : 4ull;
    const uint64_t byteCount = static_cast<uint64_t>(width)
        * static_cast<uint64_t>(height) * bytesPerPixel;
    if (byteCount == 0 || byteCount > 8ull * 1024ull * 1024ull)
        return;
    VitaGeneratedImageCapture *captured =
        DB_VitaFindGeneratedImage(image->name);
    if (!captured)
        return;
    captured->name = image->name;
    captured->width = static_cast<uint32_t>(width);
    captured->height = static_cast<uint32_t>(height);
    captured->levelCount = 1;
    captured->format = imageFormat;
    captured->flags = 0;
    captured->bytes.assign(pixels, pixels + byteCount);
}

void __cdecl DB_VitaCaptureGeneratedImageCube(
    const GfxImage *image, const uint8_t *(*pixels)[15], int32_t edgeLength,
    uint32_t levelCount, uint32_t imageFormat)
{
    ++g_vitaGeneratedCubeCalls;
    // Reflection probes are generated from raw DiskGfxReflectionProbe data
    // and never appear as ordinary IWI-backed ASSET_TYPE_IMAGE records. Keep
    // the source face-major mip chain intact so VITI's cubemap reader can
    // upload the same six faces that Image_GenerateCube installed.
    static constexpr char kPrefix[] = "*reflection_probe";
    if (!DB_VitaGeneratedExportEnabled() || !image || !image->name || !pixels
        || std::strncmp(image->name, kPrefix, sizeof(kPrefix) - 1) != 0)
        return;
    ++g_vitaReflectionImageNames;
    if (edgeLength <= 0 || levelCount == 0 || levelCount > 15u
        || imageFormat != 21u)
        return;
    ++g_vitaReflectionCaptureAttempts;
    uint64_t faceBytes = 0;
    for (uint32_t level = 0; level < levelCount; ++level)
    {
        const int shiftedDimension = edgeLength >> static_cast<int>(level);
        const uint32_t dimension = shiftedDimension > 1
            ? static_cast<uint32_t>(shiftedDimension) : 1u;
        const uint64_t mipBytes = static_cast<uint64_t>(dimension)
            * dimension * 4ull;
        if (faceBytes > 8ull * 1024ull * 1024ull - mipBytes)
            return;
        faceBytes += mipBytes;
    }
    const uint64_t byteCount = faceBytes * 6ull;
    if (faceBytes == 0 || byteCount > 8ull * 1024ull * 1024ull)
        return;
    VitaGeneratedImageCapture *captured =
        DB_VitaFindGeneratedImage(image->name);
    if (!captured)
        return;
    captured->name = image->name;
    captured->width = static_cast<uint32_t>(edgeLength);
    captured->height = static_cast<uint32_t>(edgeLength);
    captured->levelCount = levelCount;
    captured->format = imageFormat;
    captured->flags = levelCount == 1u ? 7u : 5u;
    captured->bytes.resize(static_cast<size_t>(byteCount));
    size_t offset = 0;
    for (uint32_t face = 0; face < 6u; ++face)
    {
        for (uint32_t level = 0; level < levelCount; ++level)
        {
            const int shiftedDimension = edgeLength >> static_cast<int>(level);
            const uint32_t dimension = shiftedDimension > 1
                ? static_cast<uint32_t>(shiftedDimension) : 1u;
            const size_t mipBytes = static_cast<size_t>(dimension)
                * dimension * 4u;
            if (!pixels[face][level])
            {
                captured->bytes.clear();
                return;
            }
            std::memcpy(captured->bytes.data() + offset,
                        pixels[face][level], mipBytes);
            offset += mipBytes;
        }
    }
    ++g_vitaReflectionCaptureSuccesses;
}

void __cdecl DB_VitaCaptureLoadedImageCube(
    const GfxImage *image, const uint8_t *pixels, uint32_t payloadBytes,
    int32_t edgeLength, uint32_t levelCount, uint32_t imageFormat,
    uint32_t imageFlags)
{
    ++g_vitaLoadedCubeCalls;
    static constexpr char kPrefix[] = "*reflection_probe";
    if (!DB_VitaGeneratedExportEnabled() || !image || !image->name || !pixels
        || std::strncmp(image->name, kPrefix, sizeof(kPrefix) - 1) != 0)
        return;
    ++g_vitaReflectionImageNames;
    if ((imageFlags & 0x4u) == 0 || edgeLength <= 0 || levelCount == 0
        || levelCount > 15u || imageFormat != 21u)
        return;
    ++g_vitaReflectionCaptureAttempts;
    uint64_t faceBytes = 0;
    for (uint32_t level = 0; level < levelCount; ++level)
    {
        const int shiftedDimension = edgeLength >> static_cast<int>(level);
        const uint32_t dimension = shiftedDimension > 1
            ? static_cast<uint32_t>(shiftedDimension) : 1u;
        const uint64_t mipBytes = static_cast<uint64_t>(dimension)
            * dimension * 4ull;
        if (faceBytes > 8ull * 1024ull * 1024ull - mipBytes)
            return;
        faceBytes += mipBytes;
    }
    const uint64_t byteCount = faceBytes * 6ull;
    if (faceBytes == 0 || byteCount > 8ull * 1024ull * 1024ull
        || byteCount > payloadBytes)
        return;
    VitaGeneratedImageCapture *captured =
        DB_VitaFindGeneratedImage(image->name);
    if (!captured)
        return;
    captured->name = image->name;
    captured->width = static_cast<uint32_t>(edgeLength);
    captured->height = static_cast<uint32_t>(edgeLength);
    captured->levelCount = levelCount;
    captured->format = imageFormat;
    captured->flags = imageFlags;
    captured->bytes.assign(pixels, pixels + byteCount);
    ++g_vitaReflectionCaptureSuccesses;
}

static bool DB_DumpVitaCapturedImage(VitaImageDumpContext *context,
                                     const VitaGeneratedImageCapture &captured)
{
    if (!context || captured.name.empty() || captured.bytes.empty()
        || captured.bytes.size() > UINT32_MAX
        || context->dataBytes + captured.bytes.size() > 64ull * 1024ull * 1024ull)
        return false;
    const uint32_t nameBytes = static_cast<uint32_t>(captured.name.size());
    if (context->write)
    {
        VitaImageRecordHeader record{};
        record.nameBytes = nameBytes;
        record.dataBytes = static_cast<uint32_t>(captured.bytes.size());
        record.width = captured.width;
        record.height = captured.height;
        record.depth = 1;
        record.levelCount = captured.levelCount;
        record.format = captured.format;
        record.flags = captured.flags;
        if (!DB_VitaWrite(context->file, &record, sizeof(record))
            || !DB_VitaWriteString(context->file, captured.name.c_str(), nameBytes)
            || !DB_VitaWrite(context->file, captured.bytes.data(), captured.bytes.size()))
            return false;
    }
    ++context->count;
    context->dataBytes += captured.bytes.size();
    return true;
}

struct VitaImagePayload
{
    const uint8_t *data = nullptr;
    uint32_t bytes = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 0;
    uint32_t levelCount = 0;
    uint32_t format = 0;
    uint32_t flags = 0;
};

static bool DB_VitaIsSupportedImageFormat(uint32_t format)
{
    // DXT payloads are the normal retail IWI path.  The BSP loader also
    // synthesizes static lightmaps as L8/A8R8G8B8 images in the Windows
    // renderer; retain those pointer-free payloads so Vita can apply the
    // authored Shipment lighting without carrying native image objects.
    return format == 0x31545844u || format == 0x33545844u || format == 0x35545844u
        || format == 50u /* D3DFMT_L8 */
        || format == 21u /* D3DFMT_A8R8G8B8 */;
}

static bool DB_VitaImagePayload(const GfxImage *image, uint32_t *nameBytes,
                                VitaImagePayload *payload)
{
    if (!image || !image->name || !nameBytes || !payload)
        return false;
    if (!DB_VitaBoundedString(image->name, nameBytes) || *nameBytes == 0)
        return false;
    if (image->texture.loadDef)
    {
        const GfxImageLoadDef *loadDef = image->texture.loadDef;
        if (loadDef->resourceSize <= 0 || loadDef->resourceSize > 8 * 1024 * 1024
            || !loadDef->data || loadDef->levelCount == 0 || loadDef->levelCount > 16
            || loadDef->dimensions[0] <= 0 || loadDef->dimensions[0] > 4096
            || loadDef->dimensions[1] <= 0 || loadDef->dimensions[1] > 4096
            || loadDef->dimensions[2] != 1
            || !DB_VitaIsSupportedImageFormat(static_cast<uint32_t>(loadDef->format)))
            return false;
        payload->data = loadDef->data;
        payload->bytes = static_cast<uint32_t>(loadDef->resourceSize);
        payload->width = static_cast<uint32_t>(loadDef->dimensions[0]);
        payload->height = static_cast<uint32_t>(loadDef->dimensions[1]);
        payload->depth = static_cast<uint32_t>(loadDef->dimensions[2]);
        payload->levelCount = loadDef->levelCount;
        payload->format = static_cast<uint32_t>(loadDef->format);
        payload->flags = loadDef->flags;
        return true;
    }
    return false;
}

static void __cdecl DB_DumpVitaImage(XAssetHeader header, void *rawContext)
{
    auto *context = static_cast<VitaImageDumpContext *>(rawContext);
    const GfxImage *image = header.image;
    if (!context || !image)
        return;
    // BSP lightmaps are appended from the live GfxWorld below. Avoid a
    // duplicate if the generated image also happens to be visible through
    // the general ASSET_TYPE_IMAGE enumeration.
    if (image->name && std::strncmp(image->name, "*lightmap", 9) == 0)
        return;
    uint32_t nameBytes = 0;
    VitaImagePayload payload;
    if (!DB_VitaImagePayload(image, &nameBytes, &payload)
        || context->dataBytes + payload.bytes > 64ull * 1024ull * 1024ull)
        return;
    if (context->write)
    {
        VitaImageRecordHeader record{};
        record.nameBytes = nameBytes;
        record.dataBytes = payload.bytes;
        record.width = payload.width;
        record.height = payload.height;
        record.depth = payload.depth;
        record.levelCount = payload.levelCount;
        record.format = payload.format;
        record.flags = payload.flags;
        if (!DB_VitaWrite(context->file, &record, sizeof(record))
            || !DB_VitaWriteString(context->file, image->name, nameBytes)
            || !DB_VitaWrite(context->file, payload.data, payload.bytes))
            return;
    }
    ++context->count;
    context->dataBytes += payload.bytes;
}

static void DB_DumpVitaWorldLightmaps(VitaImageDumpContext *context)
{
    if (!context)
        return;
    if (!s_world.lightmaps || s_world.lightmapCount <= 0
        || s_world.lightmapCount > 31)
    {
        for (const VitaGeneratedImageCapture &captured : g_vitaGeneratedImages)
        {
            if (captured.name.rfind("*lightmap", 0) == 0)
                DB_DumpVitaCapturedImage(context, captured);
        }
        return;
    }
    for (int index = 0; index < s_world.lightmapCount; ++index)
    {
        // The BSP loader creates paired *lightmapN_primary (L8) and
        // *lightmapN_secondary (A8R8G8B8) images. Reuse the normal VITI writer
        // so the bounded reader and converter see the same record shape.
        const GfxImage *images[2] = {
            s_world.lightmaps[index].primary,
            s_world.lightmaps[index].secondary,
        };
        for (const GfxImage *image : images)
        {
            if (!image)
                continue;
            const auto captured = std::find_if(
                g_vitaGeneratedImages.begin(), g_vitaGeneratedImages.end(),
                [&](const VitaGeneratedImageCapture &value) {
                    return value.name == image->name;
                });
            if (captured == g_vitaGeneratedImages.end())
                continue;
            DB_DumpVitaCapturedImage(context, *captured);
        }
    }
}

static void DB_DumpVitaReflectionProbes(VitaImageDumpContext *context)
{
    if (!context)
        return;
    for (const VitaGeneratedImageCapture &captured : g_vitaGeneratedImages)
    {
        if (captured.name.rfind("*reflection_probe", 0) == 0)
            DB_DumpVitaCapturedImage(context, captured);
    }
}

static bool DB_DumpVitaImages(int file)
{
    VitaImageDumpContext countContext{file, false, 0, 0};
    DB_EnumXAssets(ASSET_TYPE_IMAGE, DB_DumpVitaImage, &countContext, 0);
    DB_DumpVitaWorldLightmaps(&countContext);
    DB_DumpVitaReflectionProbes(&countContext);
    const uint32_t magic = 0x49544956; // "VITI"
    const uint32_t version = 1;
    if (!DB_VitaWriteValue(file, magic) || !DB_VitaWriteValue(file, version)
        || !DB_VitaWriteValue(file, countContext.count)
        || !DB_VitaWriteValue(file, countContext.dataBytes))
        return false;
    VitaImageDumpContext writeContext{file, true, 0, 0};
    DB_EnumXAssets(ASSET_TYPE_IMAGE, DB_DumpVitaImage, &writeContext, 0);
    DB_DumpVitaWorldLightmaps(&writeContext);
    DB_DumpVitaReflectionProbes(&writeContext);
    Com_Printf(16, "Vita reflection image export: generatedCubes=%u loadedCubes=%u names=%u attempts=%u captured=%u records=%u\n",
               g_vitaGeneratedCubeCalls, g_vitaLoadedCubeCalls,
               g_vitaReflectionImageNames,
               g_vitaReflectionCaptureAttempts, g_vitaReflectionCaptureSuccesses,
               countContext.count);
    return writeContext.count == countContext.count
        && writeContext.dataBytes == countContext.dataBytes;
}

struct VitaMaterialDumpContext
{
    int file;
    bool write;
    uint32_t count;
    uint64_t bytes;
};

#pragma pack(push, 1)
struct VitaMaterialRecordHeader
{
    uint32_t nameBytes;
    uint32_t colorBytes;
    uint32_t normalBytes;
    uint32_t specularBytes;
    uint32_t techniqueSetBytes;
    uint32_t surfaceTypeBits;
    uint8_t gameFlags;
    uint8_t sortKey;
    uint8_t stateFlags;
    uint8_t textureCount;
    uint8_t colorSamplerState;
    uint8_t normalSamplerState;
    uint8_t specularSamplerState;
    // Material_SetMaterialDrawRegion derives this from the source technique
    // set. Region 3 is the shadow-only bucket and is not emitted by the
    // normal static-model camera pass.
    uint8_t cameraRegion;
    // OR of MaterialPass::customSamplerFlags for the authored lit techniques:
    // bit 0x02 is the primary lightmap and bit 0x04 is the secondary map.
    uint8_t lightmapSamplerFlags;
    // VITL v4 carries the first source lit-pass render-state pair.  These are
    // the exact GfxStateBits values selected by the desktop material loader;
    // the Vita side can decode alpha-test/blend/depth/cull behavior without
    // inferring it from a material-name substring.
    uint32_t litStateBits0;
    uint32_t litStateBits1;
    uint8_t litStateValid;
    // VITL v5 preserves TECHNIQUE_UNLIT separately for original HUD/text.
    uint32_t unlitStateBits0;
    uint32_t unlitStateBits1;
    uint8_t unlitStateValid;
    uint8_t colorSemantic;
};
#pragma pack(pop)

static_assert(sizeof(VitaMaterialRecordHeader) == 52);

struct VitaMaterialSource
{
    const Material *material = nullptr;
    const char *colorName = nullptr;
    const char *normalName = nullptr;
    const char *specularName = nullptr;
    const char *techniqueSetName = nullptr;
    uint32_t nameBytes = 0;
    uint32_t colorBytes = 0;
    uint32_t normalBytes = 0;
    uint32_t specularBytes = 0;
    uint32_t techniqueSetBytes = 0;
    uint8_t colorSamplerState = 0;
    uint8_t normalSamplerState = 0;
    uint8_t specularSamplerState = 0;
    uint8_t lightmapSamplerFlags = 0;
    uint32_t litStateBits0 = 0;
    uint32_t litStateBits1 = 0;
    uint8_t litStateValid = 0;
    uint32_t unlitStateBits0 = 0;
    uint32_t unlitStateBits1 = 0;
    uint8_t unlitStateValid = 0;
    uint8_t colorSemantic = TS_COLOR_MAP;
};

static bool DB_VitaMaterialUnlitStateBits(const Material *material,
                                         uint32_t *bits0, uint32_t *bits1)
{
    if (!material || !material->techniqueSet || !material->stateBitsTable)
        return false;
    const auto *set = material->techniqueSet->remappedTechniqueSet
        ? material->techniqueSet->remappedTechniqueSet : material->techniqueSet;
    const auto *technique = set->techniques[TECHNIQUE_UNLIT];
    const uint32_t entry = material->stateBitsEntry[TECHNIQUE_UNLIT];
    if (!technique || !technique->passCount || entry == 0xFFu
        || entry >= material->stateBitsCount)
        return false;
    *bits0 = material->stateBitsTable[entry].loadBits[0];
    *bits1 = material->stateBitsTable[entry].loadBits[1];
    return true;
}

static bool DB_VitaMaterialLitStateBits(const Material *material,
                                        uint32_t *stateBits0,
                                        uint32_t *stateBits1)
{
    if (!material || !material->techniqueSet || !material->stateBitsTable
        || material->stateBitsCount == 0 || !stateBits0 || !stateBits1)
        return false;
    // Material_OriginalRemapTechniqueSet may replace the desktop technique
    // set after the asset is loaded.  Use the same resolved set as the source
    // renderer and select its first available lit family.  The first pass is
    // the pass that owns the material blend/alpha/depth state; later passes
    // may add lightmap/shadow code samplers but do not change this base state.
    const MaterialTechniqueSet *techniqueSet =
        material->techniqueSet->remappedTechniqueSet
            ? material->techniqueSet->remappedTechniqueSet
            : material->techniqueSet;
    for (uint32_t techniqueType = TECHNIQUE_LIT_BEGIN;
         techniqueType < TECHNIQUE_LIT_END; ++techniqueType) {
        const MaterialTechnique *technique = techniqueSet->techniques[techniqueType];
        if (!technique || technique->passCount == 0)
            continue;
        const uint32_t entry = material->stateBitsEntry[techniqueType];
        if (entry == 0xFFu || entry >= material->stateBitsCount)
            continue;
        const GfxStateBits &bits = material->stateBitsTable[entry];
        *stateBits0 = bits.loadBits[0];
        *stateBits1 = bits.loadBits[1];
        return true;
    }
    return false;
}

static uint8_t DB_VitaMaterialLightmapSamplerFlags(const Material *material)
{
    if (!material || !material->techniqueSet)
        return 0;
    // Material_GetTechniqueSet returns the remapped set used by the desktop
    // renderer. Preserve the fallback for early-load materials that have not
    // installed a remap yet; this is still a source MaterialTechniqueSet,
    // never a Vita-side inference.
    const MaterialTechniqueSet *techniqueSet =
        material->techniqueSet->remappedTechniqueSet
            ? material->techniqueSet->remappedTechniqueSet
            : material->techniqueSet;
    uint8_t flags = 0;
    for (uint32_t techniqueType = TECHNIQUE_LIT_BEGIN;
         techniqueType < TECHNIQUE_LIT_END; ++techniqueType) {
        const MaterialTechnique *technique = techniqueSet->techniques[techniqueType];
        if (!technique || technique->passCount == 0)
            continue;
        const uint32_t passCount = std::min<uint32_t>(technique->passCount, 64u);
        for (uint32_t pass = 0; pass < passCount; ++pass)
            flags |= technique->passArray[pass].customSamplerFlags & 0x06u;
    }
    return flags;
}

static bool DB_VitaMaterialStrings(const Material *material,
                                   VitaMaterialSource *source)
{
    if (!material || !material->info.name || !source
        || !DB_VitaBoundedString(material->info.name, &source->nameBytes)
        || source->nameBytes == 0)
        return false;
    source->material = material;
    const auto CaptureImage = [&](uint8_t semantic, const char **name,
                                  uint32_t *bytes, uint8_t *sampler) {
        if (!name || !bytes || !sampler || !material->textureTable
            || material->textureCount > 64)
            return;
        for (uint32_t index = 0; index < material->textureCount; ++index)
        {
            const MaterialTextureDef &texture = material->textureTable[index];
            // Semantic 11 is the water/code-resource arm of the union. Only
            // the image-backed color/normal/specular slots are dereferenced.
            if (texture.semantic != semantic || !texture.u.image
                || !texture.u.image->name)
                continue;
            uint32_t candidateBytes = 0;
            if (!DB_VitaBoundedString(texture.u.image->name, &candidateBytes))
                continue;
            *name = texture.u.image->name;
            *bytes = candidateBytes;
            *sampler = texture.samplerState;
            return;
        }
    };
    // TS_COLOR_MAP/TS_NORMAL_MAP/TS_SPECULAR_MAP are the source material
    // semantic slots used by KisakCOD's world/model techniques.
    CaptureImage(2, &source->colorName, &source->colorBytes,
                 &source->colorSamplerState);
    if (!source->colorName) {
        CaptureImage(TS_2D, &source->colorName, &source->colorBytes,
                     &source->colorSamplerState);
        if (source->colorName)
            source->colorSemantic = TS_2D;
    }
    CaptureImage(5, &source->normalName, &source->normalBytes,
                 &source->normalSamplerState);
    CaptureImage(8, &source->specularName, &source->specularBytes,
                 &source->specularSamplerState);
    source->lightmapSamplerFlags =
        DB_VitaMaterialLightmapSamplerFlags(material);
    source->litStateValid = DB_VitaMaterialLitStateBits(
        material, &source->litStateBits0, &source->litStateBits1) ? 1u : 0u;
    source->unlitStateValid = DB_VitaMaterialUnlitStateBits(
        material, &source->unlitStateBits0, &source->unlitStateBits1) ? 1u : 0u;
    if (material->techniqueSet && material->techniqueSet->name) {
        uint32_t techniqueBytes = 0;
        if (DB_VitaBoundedString(material->techniqueSet->name, &techniqueBytes)) {
            source->techniqueSetBytes = techniqueBytes;
            source->techniqueSetName = material->techniqueSet->name;
        }
    }
    return true;
}

static void __cdecl DB_DumpVitaMaterial(XAssetHeader header, void *rawContext)
{
    auto *context = static_cast<VitaMaterialDumpContext *>(rawContext);
    const Material *material = header.material;
    if (!context || !material)
        return;
    VitaMaterialSource source;
    if (!DB_VitaMaterialStrings(material, &source))
        return;
    const uint64_t variableBytes = static_cast<uint64_t>(source.nameBytes)
        + source.colorBytes + source.normalBytes + source.specularBytes
        + source.techniqueSetBytes;
    const uint64_t recordBytes = sizeof(VitaMaterialRecordHeader) + variableBytes;
    if (context->bytes + recordBytes > 16ull * 1024ull * 1024ull)
        return;
    if (context->write)
    {
        VitaMaterialRecordHeader record{};
        record.nameBytes = source.nameBytes;
        record.colorBytes = source.colorBytes;
        record.normalBytes = source.normalBytes;
        record.specularBytes = source.specularBytes;
        record.techniqueSetBytes = source.techniqueSetBytes;
        record.surfaceTypeBits = material->info.surfaceTypeBits;
        record.gameFlags = material->info.gameFlags;
        record.sortKey = material->info.sortKey;
        record.stateFlags = material->stateFlags;
        record.textureCount = material->textureCount;
        record.colorSamplerState = source.colorSamplerState;
        record.normalSamplerState = source.normalSamplerState;
        record.specularSamplerState = source.specularSamplerState;
        record.cameraRegion = material->cameraRegion;
        record.lightmapSamplerFlags = source.lightmapSamplerFlags;
        record.litStateBits0 = source.litStateBits0;
        record.litStateBits1 = source.litStateBits1;
        record.litStateValid = source.litStateValid;
        record.unlitStateBits0 = source.unlitStateBits0;
        record.unlitStateBits1 = source.unlitStateBits1;
        record.unlitStateValid = source.unlitStateValid;
        record.colorSemantic = source.colorSemantic;
        if (!DB_VitaWriteValue(context->file, record)
            || !DB_VitaWriteString(context->file, material->info.name,
                                   source.nameBytes)
            || !DB_VitaWriteString(context->file, source.colorName,
                                   source.colorBytes)
            || !DB_VitaWriteString(context->file, source.normalName,
                                   source.normalBytes)
            || !DB_VitaWriteString(context->file, source.specularName,
                                   source.specularBytes)
            || !DB_VitaWriteString(context->file, source.techniqueSetName,
                                   source.techniqueSetBytes))
            return;
    }
    ++context->count;
    // The VITL aggregate covers only variable string payloads; fixed record
    // headers are accounted for by parsing the stream layout.
    context->bytes += variableBytes;
}

static bool DB_DumpVitaMaterials(int file)
{
    VitaMaterialDumpContext countContext{file, false, 0, 0};
    DB_EnumXAssets(ASSET_TYPE_MATERIAL, DB_DumpVitaMaterial, &countContext, 0);
    const uint32_t magic = 0x4C544956; // "VITL"
    const uint32_t version = 5;
    if (!DB_VitaWriteValue(file, magic) || !DB_VitaWriteValue(file, version)
        || !DB_VitaWriteValue(file, countContext.count)
        || !DB_VitaWriteValue(file, countContext.bytes))
        return false;
    VitaMaterialDumpContext writeContext{file, true, 0, 0};
    DB_EnumXAssets(ASSET_TYPE_MATERIAL, DB_DumpVitaMaterial, &writeContext, 0);
    return writeContext.count == countContext.count
        && writeContext.bytes == countContext.bytes;
}

#pragma pack(push, 1)
struct VitaFontRecordHeader
{
    uint32_t nameBytes;
    uint32_t imageBytes;
    uint32_t glyphCount;
    int32_t pixelHeight;
    uint32_t materialBytes;
    uint32_t glowMaterialBytes;
};
#pragma pack(pop)

static_assert(sizeof(VitaFontRecordHeader) == 24);

struct VitaFontDumpContext
{
    int file;
    bool write;
    uint32_t count;
    uint64_t bytes;
};

static bool DB_VitaFontStrings(const Font_s *font, uint32_t *nameBytes,
                               const char **imageName, uint32_t *imageBytes)
{
    if (!font || !nameBytes || !imageName || !imageBytes || !font->fontName
        || !DB_VitaBoundedString(font->fontName, nameBytes) || *nameBytes == 0
        || font->pixelHeight <= 0 || font->pixelHeight > 256
        || font->glyphCount <= 0 || font->glyphCount > 2048 || !font->glyphs)
        return false;
    *imageName = nullptr;
    *imageBytes = 0;
    if (font->material && font->material->textureTable
        && font->material->textureCount <= 64)
    {
        // Font materials do not consistently label their atlas as semantic 2
        // (the normal color-map slot).  All non-water texture semantics use
        // the GfxImage arm of MaterialTextureDefInfo; semantic 11 is the
        // only water/code-resource arm and must never be dereferenced here.
        // Preserve the first bounded image as the authored font atlas.
        for (uint32_t index = 0; index < font->material->textureCount; ++index)
        {
            const MaterialTextureDef &texture = font->material->textureTable[index];
            if (texture.semantic == 11 || !texture.u.image || !texture.u.image->name)
                continue;
            uint32_t candidateBytes = 0;
            if (!DB_VitaBoundedString(texture.u.image->name, &candidateBytes))
                continue;
            *imageName = texture.u.image->name;
            *imageBytes = candidateBytes;
            break;
        }
    }
    if (!DB_VitaBoundedString(font->fontName, nameBytes))
        return false;
    return true;
}

static void __cdecl DB_DumpVitaFont(XAssetHeader header, void *rawContext)
{
    auto *context = static_cast<VitaFontDumpContext *>(rawContext);
    const Font_s *font = header.font;
    if (!context || !font)
        return;
    uint32_t nameBytes = 0;
    uint32_t imageBytes = 0;
    const char *imageName = nullptr;
    if (!DB_VitaFontStrings(font, &nameBytes, &imageName, &imageBytes))
        return;
    uint32_t materialBytes = 0;
    uint32_t glowMaterialBytes = 0;
    if (!font->material
        || !DB_VitaBoundedString(font->material->info.name, &materialBytes)
        || materialBytes == 0
        || (font->glowMaterial && !DB_VitaBoundedString(
            font->glowMaterial->info.name, &glowMaterialBytes)))
        return;
    const uint64_t glyphBytes = static_cast<uint64_t>(font->glyphCount) * 24ull;
    const uint64_t recordBytes = static_cast<uint64_t>(nameBytes) + imageBytes
        + materialBytes + glowMaterialBytes + glyphBytes;
    if (context->bytes + recordBytes > 8ull * 1024ull * 1024ull)
        return;
    if (context->write)
    {
        VitaFontRecordHeader record{};
        record.nameBytes = nameBytes;
        record.imageBytes = imageBytes;
        record.glyphCount = static_cast<uint32_t>(font->glyphCount);
        record.pixelHeight = font->pixelHeight;
        record.materialBytes = materialBytes;
        record.glowMaterialBytes = glowMaterialBytes;
        if (!DB_VitaWrite(context->file, &record, sizeof(record))
            || !DB_VitaWriteString(context->file, font->fontName, nameBytes)
            || !DB_VitaWriteString(context->file, imageName, imageBytes)
            || !DB_VitaWriteString(context->file, font->material->info.name, materialBytes)
            || !DB_VitaWriteString(context->file, font->glowMaterial
                ? font->glowMaterial->info.name : nullptr, glowMaterialBytes))
            return;
        for (int32_t index = 0; index < font->glyphCount; ++index)
        {
            const Glyph &glyph = font->glyphs[index];
            if (!DB_VitaWriteValue(context->file, glyph.letter)
                || !DB_VitaWrite(context->file, &glyph.x0, sizeof(glyph.x0))
                || !DB_VitaWrite(context->file, &glyph.y0, sizeof(glyph.y0))
                || !DB_VitaWrite(context->file, &glyph.dx, sizeof(glyph.dx))
                || !DB_VitaWrite(context->file, &glyph.pixelWidth, sizeof(glyph.pixelWidth))
                || !DB_VitaWrite(context->file, &glyph.pixelHeight, sizeof(glyph.pixelHeight)))
                return;
            const uint8_t padding = 0;
            if (!DB_VitaWrite(context->file, &padding, sizeof(padding))
                || !DB_VitaWriteValue(context->file, glyph.s0)
                || !DB_VitaWriteValue(context->file, glyph.t0)
                || !DB_VitaWriteValue(context->file, glyph.s1)
                || !DB_VitaWriteValue(context->file, glyph.t1))
                return;
        }
    }
    ++context->count;
    context->bytes += recordBytes;
}

static bool DB_DumpVitaFonts(int file)
{
    VitaFontDumpContext countContext{file, false, 0, 0};
    DB_EnumXAssets(ASSET_TYPE_FONT, DB_DumpVitaFont, &countContext, 0);
    const uint32_t magic = 0x46544956; // "VITF"
    const uint32_t version = 2;
    if (!DB_VitaWriteValue(file, magic) || !DB_VitaWriteValue(file, version)
        || !DB_VitaWriteValue(file, countContext.count)
        || !DB_VitaWriteValue(file, countContext.bytes))
        return false;
    VitaFontDumpContext writeContext{file, true, 0, 0};
    DB_EnumXAssets(ASSET_TYPE_FONT, DB_DumpVitaFont, &writeContext, 0);
    return writeContext.count == countContext.count && writeContext.bytes == countContext.bytes;
}

// VFXA v2 graph records are intentionally independent from the desktop FX
// allocation graph.  The exporter below copies the authored values and exact
// asset names, but never writes an FxEffectDef/FxElemDef pointer.
#pragma pack(push, 1)
struct VitaEffectGraphEffectRecord
{
    uint32_t effectIndex;
    uint32_t flags;
    uint32_t msecLoopingLife;
    uint32_t elemDefCountLooping;
    uint32_t elemDefCountOneShot;
    uint32_t elemDefCountEmission;
    uint32_t elementOffset;
    uint32_t elementCount;
    uint32_t reserved;
};

struct VitaEffectGraphElementRecord
{
    uint32_t uints[20];
    float lightingFrac;
    float spawnRange[2];
    float fadeInRange[2];
    float fadeOutRange[2];
    float spawnFrustumCullRadius;
    int32_t spawnDelayMsec[2];
    int32_t lifeSpanMsec[2];
    float spawnOrigin[6];
    float spawnOffsetRadius[2];
    float spawnOffsetHeight[2];
    float spawnAngles[6];
    float angularVelocity[6];
    float initialRotation[2];
    float gravity[2];
    float reflectionFactor[2];
    uint32_t atlas[7];
    float collMins[3];
    float collMaxs[3];
    float emitDist[2];
    float emitDistVariance[2];
    int32_t spawnDef[2];
    int32_t trailScrollTimeMsec;
    int32_t trailRepeatDist;
    int32_t trailSplitDist;
    uint32_t trailPresent;
};
#pragma pack(pop)

static_assert(sizeof(VitaEffectGraphEffectRecord) == 36);
static_assert(sizeof(VitaEffectGraphElementRecord) == 332);

struct VitaFxStringTable
{
    std::vector<uint8_t> bytes{0};
    std::unordered_map<std::string, uint32_t> offsets{{"", 0}};

    bool Intern(const char *value, uint32_t maximum, uint32_t *offset)
    {
        if (!offset)
            return false;
        *offset = 0;
        if (!value || !*value)
            return true;
        uint32_t length = 0;
        if (!DB_VitaBoundedString(value, &length) || length == 0
            || length > maximum)
            return false;
        const std::string key(value, length);
        const auto existing = offsets.find(key);
        if (existing != offsets.end())
        {
            *offset = existing->second;
            return true;
        }
        if (bytes.size() > 1 * 1024 * 1024 - (key.size() + 1))
            return false;
        *offset = static_cast<uint32_t>(bytes.size());
        bytes.insert(bytes.end(), key.begin(), key.end());
        bytes.push_back(0);
        offsets.emplace(key, *offset);
        return true;
    }
};

static void __cdecl DB_VitaCollectImpactFx(XAssetHeader header,
                                           void *rawContext)
{
    auto *tables = static_cast<std::vector<const FxImpactTable *> *>(rawContext);
    // The cgame fastfile path requests only the canonical empty-name table.
    // The database also enumerates its synthetic "default" fallback asset;
    // that placeholder is not the authored impact closure and may contain
    // null slots by design.
    if (tables && header.impactFx && header.impactFx->name
        && !header.impactFx->name[0])
        tables->push_back(header.impactFx);
}

// VIFX is the pointer-free counterpart of the retail impactfx fastfile
// asset.  The 12 material classes and their 29 non-flesh/4 flesh effect
// slots are fixed by COD4's FxImpactTable ABI; only table/effect names cross
// the platform boundary.  Vita resolves them against its arena-owned VFXA
// graph after loading, so no desktop pointer is serialized.
static bool DB_DumpVitaImpactEffects(int file)
{
    constexpr uint32_t kEntryCount = 12;
    constexpr uint32_t kNonFleshCount = 29;
    constexpr uint32_t kFleshCount = 4;
    constexpr uint32_t kSlotsPerTable =
        kEntryCount * (kNonFleshCount + kFleshCount);
    std::vector<const FxImpactTable *> tables;
    DB_EnumXAssets(ASSET_TYPE_IMPACT_FX, DB_VitaCollectImpactFx, &tables, 0);
    if (tables.empty() || tables.size() > 4u)
        return false;
    std::sort(tables.begin(), tables.end(),
              [](const FxImpactTable *left, const FxImpactTable *right) {
                  const char *leftName = left && left->name ? left->name : "";
                  const char *rightName = right && right->name ? right->name : "";
                  return std::strcmp(leftName, rightName) < 0;
              });

    VitaFxStringTable strings;
    std::vector<std::array<uint32_t, 2>> tableRecords;
    std::vector<uint32_t> effectOffsets;
    tableRecords.reserve(tables.size());
    effectOffsets.reserve(tables.size() * kSlotsPerTable);
    bool hasDefault = false;
    for (uint32_t tableIndex = 0; tableIndex < tables.size(); ++tableIndex)
    {
        const FxImpactTable *table = tables[tableIndex];
        if (!table || !table->name || !table->table)
            return false;
        if (tableIndex && !std::strcmp(tables[tableIndex - 1]->name,
                                      table->name))
            return false;
        hasDefault = hasDefault || !*table->name;
        uint32_t tableNameOffset = 0;
        if (!strings.Intern(table->name, 1024, &tableNameOffset))
            return false;
        tableRecords.push_back({tableNameOffset,
                                tableIndex * kSlotsPerTable});
        for (uint32_t impactType = 0; impactType < kEntryCount; ++impactType)
        {
            const FxImpactEntry &entry = table->table[impactType];
            for (const FxEffectDef *effect : entry.nonflesh)
            {
                uint32_t offset = 0;
                if (effect && (!effect->name || !*effect->name
                    || !strings.Intern(effect->name, 1024, &offset)))
                    return false;
                effectOffsets.push_back(offset);
            }
            for (const FxEffectDef *effect : entry.flesh)
            {
                uint32_t offset = 0;
                if (effect && (!effect->name || !*effect->name
                    || !strings.Intern(effect->name, 1024, &offset)))
                    return false;
                effectOffsets.push_back(offset);
            }
        }
    }
    if (!hasDefault || effectOffsets.size() != tables.size() * kSlotsPerTable
        || strings.bytes.empty() || strings.bytes.size() > 1024u * 1024u)
        return false;

    const uint32_t magic = 0x58464956; // "VIFX"
    const uint32_t version = 1;
    const uint32_t tableCount = static_cast<uint32_t>(tables.size());
    const uint32_t stringBytes = static_cast<uint32_t>(strings.bytes.size());
    const uint32_t reserved = 0;
    if (!DB_VitaWriteValue(file, magic)
        || !DB_VitaWriteValue(file, version)
        || !DB_VitaWriteValue(file, tableCount)
        || !DB_VitaWriteValue(file, kEntryCount)
        || !DB_VitaWriteValue(file, kNonFleshCount)
        || !DB_VitaWriteValue(file, kFleshCount)
        || !DB_VitaWriteValue(file, stringBytes)
        || !DB_VitaWriteValue(file, reserved)
        || !DB_VitaWrite(file, tableRecords.data(),
                         static_cast<uint32_t>(tableRecords.size()
                                               * sizeof(tableRecords[0])))
        || !DB_VitaWrite(file, effectOffsets.data(),
                         static_cast<uint32_t>(effectOffsets.size()
                                               * sizeof(effectOffsets[0])))
        || !DB_VitaWrite(file, strings.bytes.data(), stringBytes))
        return false;
    Com_Printf(16, "Vita impact FX export: tables=%u slots=%u strings=%u\n",
               static_cast<unsigned>(tableCount),
               static_cast<unsigned>(effectOffsets.size()),
               static_cast<unsigned>(stringBytes));
    return true;
}

struct VitaFxGraphElementExport
{
    VitaEffectGraphElementRecord record{};
    std::vector<const char *> visualNames;
    const char *effectOnImpact = nullptr;
    const char *effectOnDeath = nullptr;
    const char *effectEmitted = nullptr;
    std::vector<FxElemVelStateSample> velocitySamples;
    std::vector<FxElemVisStateSample> visualSamples;
    std::vector<FxTrailVertex> trailVertices;
    std::vector<uint16_t> trailIndices;
};

struct VitaFxGraphEffectExport
{
    const FxEffectDef *source = nullptr;
    std::vector<VitaFxGraphElementExport> elements;
};

struct VitaFxGraphContext
{
    std::unordered_set<const FxEffectDef *> effects;
    std::unordered_set<const Material *> materials;
    std::unordered_set<const XModel *> models;
};

static void __cdecl DB_VitaCollectFxEffect(XAssetHeader header, void *rawContext)
{
    auto *context = static_cast<VitaFxGraphContext *>(rawContext);
    if (context && header.fx)
        context->effects.insert(header.fx);
}

static void __cdecl DB_VitaCollectFxMaterial(XAssetHeader header, void *rawContext)
{
    auto *context = static_cast<VitaFxGraphContext *>(rawContext);
    if (context && header.material)
        context->materials.insert(header.material);
}

static void __cdecl DB_VitaCollectFxModel(XAssetHeader header, void *rawContext)
{
    auto *context = static_cast<VitaFxGraphContext *>(rawContext);
    if (context && header.model)
        context->models.insert(header.model);
}

static bool DB_VitaFxFinite(const float *values, size_t count)
{
    if (!values)
        return false;
    for (size_t index = 0; index < count; ++index)
    {
        if (!std::isfinite(values[index]))
            return false;
    }
    return true;
}

static bool DB_VitaFxFiniteRange(const FxFloatRange &range)
{
    return DB_VitaFxFinite(&range.base, 2);
}

static bool DB_VitaFxEffectRefName(const FxEffectDefRef &reference,
                                   const VitaFxGraphContext &context,
                                   const char **name)
{
    if (!name)
        return false;
    *name = nullptr;
    const FxEffectDef *target = reference.handle;
    if (!target)
        return true;
    // Fastfile fixups resolve FxEffectDefRef to a live definition. Checking
    // membership before dereferencing also rejects a load-object string held
    // in the union instead of treating it as a definition pointer.
    if (context.effects.find(target) == context.effects.end()
        || !target->name || !*target->name)
        return false;
    uint32_t bytes = 0;
    if (!DB_VitaBoundedString(target->name, &bytes) || bytes > 2048)
        return false;
    *name = target->name;
    return true;
}

static bool DB_VitaFxMaterialName(const Material *material,
                                  const VitaFxGraphContext &context,
                                  std::vector<const char *> *names)
{
    if (!material || !names
        || context.materials.find(material) == context.materials.end()
        || !material->info.name || !*material->info.name)
        return false;
    uint32_t bytes = 0;
    if (!DB_VitaBoundedString(material->info.name, &bytes) || bytes > 2048)
        return false;
    names->push_back(material->info.name);
    return true;
}

static bool DB_VitaFxModelName(const XModel *model,
                               const VitaFxGraphContext &context,
                               std::vector<const char *> *names)
{
    if (!model || !names || context.models.find(model) == context.models.end()
        || !model->name || !*model->name)
        return false;
    uint32_t bytes = 0;
    if (!DB_VitaBoundedString(model->name, &bytes) || bytes > 2048)
        return false;
    names->push_back(model->name);
    return true;
}

static bool DB_VitaFxBuildElement(const FxElemDef &source,
                                  const VitaFxGraphContext &context,
                                  VitaFxGraphElementExport *output)
{
    if (!output || source.elemType > 10u || source.visualCount > 32u
        || source.velIntervalCount > 80u || source.visStateIntervalCount > 80u
        || !DB_VitaFxFiniteRange(source.spawnRange)
        || !DB_VitaFxFiniteRange(source.fadeInRange)
        || !DB_VitaFxFiniteRange(source.fadeOutRange)
        || !DB_VitaFxFinite(&source.spawnFrustumCullRadius, 1)
        || !DB_VitaFxFiniteRange(source.spawnOffsetRadius)
        || !DB_VitaFxFiniteRange(source.spawnOffsetHeight)
        || !DB_VitaFxFiniteRange(source.initialRotation)
        || !DB_VitaFxFiniteRange(source.gravity)
        || !DB_VitaFxFiniteRange(source.reflectionFactor)
        || !DB_VitaFxFinite(source.collMins, 3)
        || !DB_VitaFxFinite(source.collMaxs, 3)
        || !DB_VitaFxFiniteRange(source.emitDist)
        || !DB_VitaFxFiniteRange(source.emitDistVariance))
        return false;
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        if (!DB_VitaFxFiniteRange(source.spawnOrigin[axis])
            || !DB_VitaFxFiniteRange(source.spawnAngles[axis])
            || !DB_VitaFxFiniteRange(source.angularVelocity[axis]))
            return false;
    }

    *output = VitaFxGraphElementExport{};
    auto &record = output->record;
    record.uints[0] = static_cast<uint32_t>(source.flags);
    record.uints[1] = source.elemType;
    record.uints[2] = source.visualCount;
    record.uints[3] = source.velIntervalCount;
    record.uints[4] = source.visStateIntervalCount;
    record.lightingFrac = static_cast<float>(source.lightingFrac);
    std::memcpy(record.spawnRange, &source.spawnRange, sizeof(record.spawnRange));
    std::memcpy(record.fadeInRange, &source.fadeInRange, sizeof(record.fadeInRange));
    std::memcpy(record.fadeOutRange, &source.fadeOutRange, sizeof(record.fadeOutRange));
    record.spawnFrustumCullRadius = source.spawnFrustumCullRadius;
    record.spawnDelayMsec[0] = source.spawnDelayMsec.base;
    record.spawnDelayMsec[1] = source.spawnDelayMsec.amplitude;
    record.lifeSpanMsec[0] = source.lifeSpanMsec.base;
    record.lifeSpanMsec[1] = source.lifeSpanMsec.amplitude;
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        record.spawnOrigin[axis * 2] = source.spawnOrigin[axis].base;
        record.spawnOrigin[axis * 2 + 1] = source.spawnOrigin[axis].amplitude;
        record.spawnAngles[axis * 2] = source.spawnAngles[axis].base;
        record.spawnAngles[axis * 2 + 1] = source.spawnAngles[axis].amplitude;
        record.angularVelocity[axis * 2] = source.angularVelocity[axis].base;
        record.angularVelocity[axis * 2 + 1] = source.angularVelocity[axis].amplitude;
    }
    record.spawnOffsetRadius[0] = source.spawnOffsetRadius.base;
    record.spawnOffsetRadius[1] = source.spawnOffsetRadius.amplitude;
    record.spawnOffsetHeight[0] = source.spawnOffsetHeight.base;
    record.spawnOffsetHeight[1] = source.spawnOffsetHeight.amplitude;
    record.initialRotation[0] = source.initialRotation.base;
    record.initialRotation[1] = source.initialRotation.amplitude;
    record.gravity[0] = source.gravity.base;
    record.gravity[1] = source.gravity.amplitude;
    record.reflectionFactor[0] = source.reflectionFactor.base;
    record.reflectionFactor[1] = source.reflectionFactor.amplitude;
    record.atlas[0] = source.atlas.behavior;
    record.atlas[1] = source.atlas.index;
    record.atlas[2] = source.atlas.fps;
    record.atlas[3] = source.atlas.loopCount;
    record.atlas[4] = source.atlas.colIndexBits;
    record.atlas[5] = source.atlas.rowIndexBits;
    record.atlas[6] = static_cast<uint16_t>(source.atlas.entryCount);
    std::memcpy(record.collMins, source.collMins, sizeof(record.collMins));
    std::memcpy(record.collMaxs, source.collMaxs, sizeof(record.collMaxs));
    record.emitDist[0] = source.emitDist.base;
    record.emitDist[1] = source.emitDist.amplitude;
    record.emitDistVariance[0] = source.emitDistVariance.base;
    record.emitDistVariance[1] = source.emitDistVariance.amplitude;
    record.uints[18] = source.sortOrder;
    record.uints[19] = source.useItemClip;
    record.spawnDef[0] = source.spawn.looping.intervalMsec;
    record.spawnDef[1] = source.spawn.looping.count;

    if (source.velSamples)
    {
        const size_t count = static_cast<size_t>(source.velIntervalCount) + 1u;
        output->velocitySamples.resize(count);
        std::memcpy(output->velocitySamples.data(), source.velSamples,
                    count * sizeof(FxElemVelStateSample));
        for (const FxElemVelStateSample &sample : output->velocitySamples)
        {
            if (!DB_VitaFxFinite(reinterpret_cast<const float *>(&sample), 24))
                return false;
        }
    }
    if (source.visSamples)
    {
        const size_t count = static_cast<size_t>(source.visStateIntervalCount) + 1u;
        output->visualSamples.resize(count);
        std::memcpy(output->visualSamples.data(), source.visSamples,
                    count * sizeof(FxElemVisStateSample));
        for (const FxElemVisStateSample &sample : output->visualSamples)
        {
            if (!DB_VitaFxFinite(&sample.base.rotationDelta, 5)
                || !DB_VitaFxFinite(&sample.amplitude.rotationDelta, 5))
                return false;
        }
    }

    if (!DB_VitaFxEffectRefName(source.effectOnImpact, context,
                                &output->effectOnImpact)
        || !DB_VitaFxEffectRefName(source.effectOnDeath, context,
                                   &output->effectOnDeath)
        || !DB_VitaFxEffectRefName(source.effectEmitted, context,
                                   &output->effectEmitted))
        return false;

    const uint32_t visualCount = source.visualCount;
    switch (source.elemType)
    {
    case 5u:
        for (uint32_t index = 0; index < visualCount; ++index)
        {
            const FxElemVisuals *visuals = visualCount == 1
                ? &source.visuals.instance : source.visuals.array;
            if (!visuals || !DB_VitaFxModelName(visuals[index].model,
                                                context,
                                                &output->visualNames))
                return false;
        }
        break;
    case 8u:
        for (uint32_t index = 0; index < visualCount; ++index)
        {
            const FxElemVisuals *visuals = visualCount == 1
                ? &source.visuals.instance : source.visuals.array;
            if (!visuals || !visuals[index].soundName
                || !*visuals[index].soundName)
                return false;
            output->visualNames.push_back(visuals[index].soundName);
        }
        break;
    case 10u:
        for (uint32_t index = 0; index < visualCount; ++index)
        {
            const FxElemVisuals *visuals = visualCount == 1
                ? &source.visuals.instance : source.visuals.array;
            const char *name = nullptr;
            if (!visuals || !DB_VitaFxEffectRefName(
                                visuals[index].effectDef, context, &name)
                || !name)
                return false;
            output->visualNames.push_back(name);
        }
        break;
    case 6u:
    case 7u:
        // Light elements have no visual pointer in the retail layout.
        break;
    case 9u:
        if (visualCount > 16u || (visualCount && !source.visuals.markArray))
            return false;
        for (uint32_t index = 0; index < visualCount; ++index)
        {
            const FxElemMarkVisuals &mark = source.visuals.markArray[index];
            for (uint32_t materialIndex = 0; materialIndex < 2; ++materialIndex)
            {
                // VFXA v3 retains both decal material slots, including an
                // explicit empty slot.  v2 collapsed null entries and could
                // not reconstruct FxElemMarkVisuals without guessing.
                if (!mark.materials[materialIndex])
                    output->visualNames.push_back(nullptr);
                else if (!DB_VitaFxMaterialName(mark.materials[materialIndex],
                                                context,
                                                &output->visualNames))
                    return false;
            }
        }
        break;
    default:
        for (uint32_t index = 0; index < visualCount; ++index)
        {
            const FxElemVisuals *visuals = visualCount == 1
                ? &source.visuals.instance : source.visuals.array;
            if (!visuals || !DB_VitaFxMaterialName(visuals[index].material,
                                                   context,
                                                   &output->visualNames))
                return false;
        }
        break;
    }

    if (source.trailDef)
    {
        const FxTrailDef &trail = *source.trailDef;
        if (trail.vertCount < 0 || trail.vertCount > 64
            || trail.indCount < 0 || trail.indCount > 128
            || (trail.vertCount && !trail.verts)
            || (trail.indCount && !trail.inds))
            return false;
        if (trail.vertCount)
        {
            output->trailVertices.assign(trail.verts,
                                          trail.verts + trail.vertCount);
            for (const FxTrailVertex &vertex : output->trailVertices)
            {
                if (!DB_VitaFxFinite(vertex.pos, 2)
                    || !DB_VitaFxFinite(vertex.normal, 2)
                    || !DB_VitaFxFinite(&vertex.texCoord, 1))
                    return false;
            }
        }
        if (trail.indCount)
        {
            output->trailIndices.assign(trail.inds,
                                        trail.inds + trail.indCount);
            for (uint16_t index : output->trailIndices)
            {
                if (index >= output->trailVertices.size())
                    return false;
            }
        }
        record.trailScrollTimeMsec = trail.scrollTimeMsec;
        record.trailRepeatDist = trail.repeatDist;
        record.trailSplitDist = trail.splitDist;
        record.trailPresent = 1;
    }
    return true;
}

static bool DB_VitaFxBuildEffect(const FxEffectDef *source,
                                 const VitaFxGraphContext &context,
                                 VitaFxGraphEffectExport *output)
{
    if (!source || !output || !source->name || !*source->name
        || source->elemDefCountLooping < 0
        || source->elemDefCountOneShot < 0
        || source->elemDefCountEmission < 0)
        return false;
    const uint64_t elementCount = static_cast<uint64_t>(source->elemDefCountLooping)
        + static_cast<uint64_t>(source->elemDefCountOneShot)
        + static_cast<uint64_t>(source->elemDefCountEmission);
    if (elementCount > 32u || (elementCount && !source->elemDefs))
        return false;
    uint32_t nameBytes = 0;
    if (!DB_VitaBoundedString(source->name, &nameBytes) || nameBytes > 1024)
        return false;
    *output = VitaFxGraphEffectExport{};
    output->source = source;
    output->elements.reserve(static_cast<size_t>(elementCount));
    for (size_t index = 0; index < elementCount; ++index)
    {
        VitaFxGraphElementExport element;
        if (!DB_VitaFxBuildElement(source->elemDefs[index], context, &element))
            return false;
        output->elements.push_back(std::move(element));
    }
    return true;
}

static bool DB_DumpVitaEffects(int file)
{
    VitaFxGraphContext context;
    DB_EnumXAssets(ASSET_TYPE_FX, DB_VitaCollectFxEffect, &context, 0);
    DB_EnumXAssets(ASSET_TYPE_MATERIAL, DB_VitaCollectFxMaterial, &context, 0);
    DB_EnumXAssets(ASSET_TYPE_XMODEL, DB_VitaCollectFxModel, &context, 0);
    if (context.effects.empty() || context.effects.size() > 4096u)
        return false;

    std::vector<const FxEffectDef *> sources;
    sources.reserve(context.effects.size());
    DB_EnumXAssets(ASSET_TYPE_FX,
                   [](XAssetHeader header, void *rawContext) {
                       auto *values = static_cast<std::vector<const FxEffectDef *> *>(rawContext);
                       if (values && header.fx)
                           values->push_back(header.fx);
                   },
                   &sources, 0);
    std::vector<VitaFxGraphEffectExport> effects;
    effects.reserve(sources.size());
    for (const FxEffectDef *source : sources)
    {
        VitaFxGraphEffectExport effect;
        if (!DB_VitaFxBuildEffect(source, context, &effect))
        {
            Com_PrintError(1, "Vita FX graph export rejected authored effect '%s'\n",
                           source && source->name ? source->name : "<unnamed>");
            return false;
        }
        effects.push_back(std::move(effect));
    }

    VitaFxStringTable strings;
    std::vector<uint32_t> definitionNameOffsets;
    definitionNameOffsets.reserve(effects.size());
    for (const VitaFxGraphEffectExport &effect : effects)
    {
        uint32_t offset = 0;
        if (!strings.Intern(effect.source->name, 1024, &offset))
            return false;
        definitionNameOffsets.push_back(offset);
    }

    std::vector<VitaEffectGraphEffectRecord> effectRecords;
    std::vector<VitaEffectGraphElementRecord> elementRecords;
    std::vector<uint32_t> visualRecords;
    std::vector<FxElemVelStateSample> velocitySamples;
    std::vector<FxElemVisStateSample> visualSamples;
    std::vector<FxTrailVertex> trailVertices;
    std::vector<uint16_t> trailIndices;
    effectRecords.reserve(effects.size());
    for (uint32_t effectIndex = 0; effectIndex < effects.size(); ++effectIndex)
    {
        const VitaFxGraphEffectExport &effect = effects[effectIndex];
        const FxEffectDef &source = *effect.source;
        VitaEffectGraphEffectRecord effectRecord{};
        effectRecord.effectIndex = effectIndex;
        effectRecord.flags = static_cast<uint32_t>(source.flags);
        effectRecord.msecLoopingLife = static_cast<uint32_t>(source.msecLoopingLife);
        effectRecord.elemDefCountLooping = static_cast<uint32_t>(source.elemDefCountLooping);
        effectRecord.elemDefCountOneShot = static_cast<uint32_t>(source.elemDefCountOneShot);
        effectRecord.elemDefCountEmission = static_cast<uint32_t>(source.elemDefCountEmission);
        effectRecord.elementOffset = static_cast<uint32_t>(elementRecords.size());
        effectRecord.elementCount = static_cast<uint32_t>(effect.elements.size());
        for (const VitaFxGraphElementExport &element : effect.elements)
        {
            VitaEffectGraphElementRecord record = element.record;
            record.uints[5] = static_cast<uint32_t>(visualRecords.size());
            record.uints[6] = static_cast<uint32_t>(element.visualNames.size());
            record.uints[7] = static_cast<uint32_t>(velocitySamples.size());
            record.uints[8] = static_cast<uint32_t>(element.velocitySamples.size());
            record.uints[9] = static_cast<uint32_t>(visualSamples.size());
            record.uints[10] = static_cast<uint32_t>(element.visualSamples.size());
            record.uints[11] = static_cast<uint32_t>(trailVertices.size());
            record.uints[12] = static_cast<uint32_t>(element.trailVertices.size());
            record.uints[13] = static_cast<uint32_t>(trailIndices.size());
            record.uints[14] = static_cast<uint32_t>(element.trailIndices.size());
            const char *references[3] = {
                element.effectOnImpact, element.effectOnDeath,
                element.effectEmitted,
            };
            for (uint32_t referenceIndex = 0; referenceIndex < 3; ++referenceIndex)
            {
                if (!strings.Intern(references[referenceIndex], 2048,
                                    &record.uints[15 + referenceIndex]))
                    return false;
            }
            for (const char *visualName : element.visualNames)
            {
                uint32_t offset = 0;
                if (!strings.Intern(visualName, 2048, &offset))
                    return false;
                visualRecords.push_back(offset);
            }
            velocitySamples.insert(velocitySamples.end(),
                                   element.velocitySamples.begin(),
                                   element.velocitySamples.end());
            visualSamples.insert(visualSamples.end(),
                                 element.visualSamples.begin(),
                                 element.visualSamples.end());
            trailVertices.insert(trailVertices.end(),
                                 element.trailVertices.begin(),
                                 element.trailVertices.end());
            trailIndices.insert(trailIndices.end(),
                               element.trailIndices.begin(),
                               element.trailIndices.end());
            elementRecords.push_back(record);
        }
        effectRecords.push_back(effectRecord);
    }

    const uint64_t graphBytes = static_cast<uint64_t>(effectRecords.size()) * sizeof(VitaEffectGraphEffectRecord)
        + static_cast<uint64_t>(elementRecords.size()) * sizeof(VitaEffectGraphElementRecord)
        + static_cast<uint64_t>(visualRecords.size()) * sizeof(uint32_t)
        + static_cast<uint64_t>(velocitySamples.size()) * sizeof(FxElemVelStateSample)
        + static_cast<uint64_t>(visualSamples.size()) * sizeof(FxElemVisStateSample)
        + static_cast<uint64_t>(trailVertices.size()) * sizeof(FxTrailVertex)
        + static_cast<uint64_t>(trailIndices.size()) * sizeof(uint16_t);
    if (strings.bytes.empty() || strings.bytes.size() > 1 * 1024 * 1024
        || graphBytes > 4ull * 1024ull * 1024ull
        || effectRecords.size() > UINT32_MAX
        || elementRecords.size() > UINT32_MAX
        || visualRecords.size() > UINT32_MAX
        || velocitySamples.size() > UINT32_MAX
        || visualSamples.size() > UINT32_MAX
        || trailVertices.size() > UINT32_MAX
        || trailIndices.size() > UINT32_MAX)
        return false;

    const uint32_t magic = 0x41584656; // "VFXA"
    const uint32_t version = 3;
    const uint32_t mapIndex = UINT32_MAX; // all-source graph, merged per map offline
    const uint32_t placementCount = 0;
    const uint32_t loopSoundCount = 0;
    const uint32_t stringBytes = static_cast<uint32_t>(strings.bytes.size());
    const uint32_t graphByteCount = static_cast<uint32_t>(graphBytes);
    if (!DB_VitaWriteValue(file, magic) || !DB_VitaWriteValue(file, version)
        || !DB_VitaWriteValue(file, mapIndex)
        || !DB_VitaWriteValue(file, static_cast<uint32_t>(effects.size()))
        || !DB_VitaWriteValue(file, placementCount)
        || !DB_VitaWriteValue(file, loopSoundCount)
        || !DB_VitaWriteValue(file, stringBytes)
        || !DB_VitaWriteValue(file, static_cast<uint32_t>(effectRecords.size()))
        || !DB_VitaWriteValue(file, static_cast<uint32_t>(elementRecords.size()))
        || !DB_VitaWriteValue(file, static_cast<uint32_t>(visualRecords.size()))
        || !DB_VitaWriteValue(file, static_cast<uint32_t>(velocitySamples.size()))
        || !DB_VitaWriteValue(file, static_cast<uint32_t>(visualSamples.size()))
        || !DB_VitaWriteValue(file, static_cast<uint32_t>(trailVertices.size()))
        || !DB_VitaWriteValue(file, static_cast<uint32_t>(trailIndices.size()))
        || !DB_VitaWriteValue(file, graphByteCount)
        || !DB_VitaWriteValue(file, static_cast<uint32_t>(0)))
        return false;
    for (uint32_t offset : definitionNameOffsets)
    {
        if (!DB_VitaWriteValue(file, offset) || !DB_VitaWriteValue(file, offset))
            return false;
    }
    if (!DB_VitaWrite(file, strings.bytes.data(), stringBytes)
        || !DB_VitaWrite(file, effectRecords.data(),
                         static_cast<uint32_t>(effectRecords.size()
                                                * sizeof(effectRecords[0])))
        || !DB_VitaWrite(file, elementRecords.data(),
                         static_cast<uint32_t>(elementRecords.size()
                                                * sizeof(elementRecords[0])))
        || !DB_VitaWrite(file, visualRecords.data(),
                         static_cast<uint32_t>(visualRecords.size()
                                                * sizeof(visualRecords[0])))
        || !DB_VitaWrite(file, velocitySamples.data(),
                         static_cast<uint32_t>(velocitySamples.size()
                                                * sizeof(velocitySamples[0])))
        || !DB_VitaWrite(file, visualSamples.data(),
                         static_cast<uint32_t>(visualSamples.size()
                                                * sizeof(visualSamples[0])))
        || !DB_VitaWrite(file, trailVertices.data(),
                         static_cast<uint32_t>(trailVertices.size()
                                                * sizeof(trailVertices[0])))
        || !DB_VitaWrite(file, trailIndices.data(),
                         static_cast<uint32_t>(trailIndices.size()
                                                * sizeof(trailIndices[0]))))
        return false;
    Com_Printf(16, "Vita FX graph export: effects=%u elements=%u visuals=%u bytes=%u\n",
               static_cast<unsigned>(effects.size()),
               static_cast<unsigned>(elementRecords.size()),
               static_cast<unsigned>(visualRecords.size()),
               static_cast<unsigned>(graphByteCount));
    return true;
}

static void __cdecl DB_DumpVitaGeometry_f()
{
    const int worldFile = FS_FOpenFileWrite("vita_world.bin");
    const int lightGridFile = FS_FOpenFileWrite("vita_lightgrid.bin");
    const int primaryLightFile = FS_FOpenFileWrite("vita_primary_lights.bin");
    const int collisionFile = FS_FOpenFileWrite("vita_collision.bin");
    const int entityFile = FS_FOpenFileWrite("vita_map_ents.bin");
    if (worldFile < 0 || lightGridFile < 0 || primaryLightFile < 0
        || collisionFile < 0 || entityFile < 0)
    {
        if (worldFile >= 0)
            FS_FCloseFile(worldFile);
        if (lightGridFile >= 0)
            FS_FCloseFile(lightGridFile);
        if (primaryLightFile >= 0)
            FS_FCloseFile(primaryLightFile);
        if (collisionFile >= 0)
            FS_FCloseFile(collisionFile);
        if (entityFile >= 0)
            FS_FCloseFile(entityFile);
        Com_PrintError(1, "Could not open Vita geometry export files\n");
        return;
    }

    const bool worldOk = DB_DumpVitaWorldGeometry(worldFile);
    const bool lightGridOk = DB_DumpVitaLightGrid(lightGridFile);
    const bool primaryLightOk = DB_DumpVitaPrimaryLights(primaryLightFile);
    const bool collisionOk = DB_DumpVitaCollisionGeometry(collisionFile);
    const bool entityOk = DB_DumpVitaMapEntities(entityFile);
    FS_FCloseFile(worldFile);
    FS_FCloseFile(lightGridFile);
    FS_FCloseFile(primaryLightFile);
    FS_FCloseFile(collisionFile);
    FS_FCloseFile(entityFile);
    Com_Printf(16, "Vita geometry export: world=%s lightGrid=%s primaryLights=%s collision=%s entities=%s\n",
               worldOk ? "ok" : "failed", lightGridOk ? "ok" : "failed",
               primaryLightOk ? "ok" : "failed", collisionOk ? "ok" : "failed",
               entityOk ? "ok" : "failed");
}

static void __cdecl DB_DumpVitaAssets_f()
{
    // Map geometry becomes available before the asynchronous zone load has
    // necessarily published all FX/materials. Export one completed database
    // generation, never a timing-dependent prefix of its asset table.
    DB_SyncXAssets();
#ifdef KISAK_MP
    if (SV_Loaded()) {
        const int dependencyFile = FS_FOpenFileWrite("vita_source_fx_dependencies.csv");
        if (dependencyFile < 0) {
            Com_PrintError(1, "Could not write source FX dependency provenance\n");
            return;
        }
        FS_Printf(dependencyFile, "name,sourceDefault,elements\n");
        std::unordered_set<std::string> effects;
        DB_EnumXAssets(ASSET_TYPE_FX, [](XAssetHeader asset, void* context) {
            if (asset.fx && asset.fx->name)
                static_cast<std::unordered_set<std::string>*>(context)->insert(asset.fx->name);
        }, &effects, 0);
        uint32_t required = 0, missing = 0, defaults = 0;
        for (int index = 1; index < 100; ++index) {
            char name[1024]{};
            SV_GetConfigstring(1598 + index, name, sizeof(name));
            if (!name[0]) continue;
            ++required;
            if (!effects.count(name)) {
                ++missing;
                Com_PrintError(1, "Vita source FX dependency absent from completed PC database: %s\n", name);
            }
            // Ask the original PC database what this name resolves to;
            // preserve and report its actual default behavior for parity.
            const FxEffectDef* resolved = DB_FindXAssetHeader(ASSET_TYPE_FX, name).fx;
            const bool sourceDefault = resolved && DB_IsXAssetDefault(ASSET_TYPE_FX, name);
            const int elements = resolved ? resolved->elemDefCountLooping
                + resolved->elemDefCountOneShot + resolved->elemDefCountEmission : -1;
            if (sourceDefault) {
                ++defaults;
                Com_PrintWarning(16, "Vita source FX resolution: requested=%s resolved=%s default=%u elements=%d\n",
                    name, resolved && resolved->name ? resolved->name : "<null>",
                    static_cast<unsigned>(sourceDefault), elements);
            }
            FS_Printf(dependencyFile, "%s,%u,%d\n", name, static_cast<unsigned>(sourceDefault), elements);
        }
        FS_FCloseFile(dependencyFile);
        Com_Printf(16, "Vita server FX dependency audit: required=%u missingBeforeLookup=%u sourceDefaults=%u assets=%u\n",
                   required, missing, defaults, static_cast<unsigned>(effects.size()));
    }
#endif
    const int file = FS_FOpenFileWrite("vita_assets.csv");
    if (file < 0)
    {
        Com_PrintError(1, "Could not open vita_assets.csv for writing\n");
        return;
    }
    FS_Printf(file, "type,typeName,name\n");
    uint32_t total = 0;
    for (int32_t rawType = 0; rawType < ASSET_TYPE_COUNT; ++rawType)
    {
        VitaAssetDumpContext context{file, static_cast<XAssetType>(rawType), 0};
        DB_EnumXAssets(context.type, DB_DumpVitaAssetName, &context, 0);
        total += context.count;
    }
    FS_FCloseFile(file);
    Com_Printf(16, "Wrote %u Vita asset names to vita_assets.csv\n", total);
    DB_DumpVitaGeometry_f();
    const int weaponFile = FS_FOpenFileWrite("vita_weapons.bin");
    if (weaponFile >= 0)
    {
        const bool weaponOk = DB_DumpVitaWeapons(weaponFile);
        FS_FCloseFile(weaponFile);
        Com_Printf(16, "Vita weapon export: %s\n", weaponOk ? "ok" : "failed");
    }
    const int modelFile = FS_FOpenFileWrite("vita_models.bin");
    const int modelLodFile = FS_FOpenFileWrite("vita_model_lods.bin");
    if (modelFile >= 0 && modelLodFile >= 0)
    {
        const bool modelOk = DB_DumpVitaModels(modelFile, modelLodFile);
        FS_FCloseFile(modelFile);
        FS_FCloseFile(modelLodFile);
        Com_Printf(16, "Vita model/render-LOD export: %s\n",
                   modelOk ? "ok" : "failed");
    }
    else
    {
        if (modelFile >= 0)
            FS_FCloseFile(modelFile);
        if (modelLodFile >= 0)
            FS_FCloseFile(modelLodFile);
        Com_PrintError(1, "Could not open Vita model export files\n");
    }
    const int animationFile = FS_FOpenFileWrite("vita_animations.bin");
    if (animationFile >= 0)
    {
        const bool animationOk = DB_DumpVitaAnimations(animationFile);
        FS_FCloseFile(animationFile);
        Com_Printf(16, "Vita animation export: %s\n",
                   animationOk ? "ok" : "failed");
    }
    const int rawFile = FS_FOpenFileWrite("vita_rawfiles.bin");
    if (rawFile >= 0)
    {
        const bool rawOk = DB_DumpVitaRawFiles(rawFile);
        FS_FCloseFile(rawFile);
        Com_Printf(16, "Vita rawfile export: %s\n", rawOk ? "ok" : "failed");
    }
    const int fxGraphFile = FS_FOpenFileWrite("vita_fxdefs.bin");
    if (fxGraphFile >= 0)
    {
        const bool fxOk = DB_DumpVitaEffects(fxGraphFile);
        FS_FCloseFile(fxGraphFile);
        Com_Printf(16, "Vita FX graph export: %s\n", fxOk ? "ok" : "failed");
    }
    const int impactFxFile = FS_FOpenFileWrite("vita_impactfx.bin");
    if (impactFxFile >= 0)
    {
        const bool impactFxOk = DB_DumpVitaImpactEffects(impactFxFile);
        FS_FCloseFile(impactFxFile);
        Com_Printf(16, "Vita impact FX export: %s\n",
                   impactFxOk ? "ok" : "failed");
    }
    const int localizeFile = FS_FOpenFileWrite("vita_localize.bin");
    if (localizeFile >= 0)
    {
        const bool localizeOk = DB_DumpVitaLocalizeEntries(localizeFile);
        FS_FCloseFile(localizeFile);
        Com_Printf(16, "Vita localized-string export: %s\n",
                   localizeOk ? "ok" : "failed");
    }
    const int stringTableFile = FS_FOpenFileWrite("vita_stringtables.bin");
    if (stringTableFile >= 0)
    {
        const bool stringTableOk = DB_DumpVitaStringTables(stringTableFile);
        FS_FCloseFile(stringTableFile);
        Com_Printf(16, "Vita string-table export: %s\n",
                   stringTableOk ? "ok" : "failed");
    }
    const int menuFile = FS_FOpenFileWrite("vita_menus.bin");
    if (menuFile >= 0)
    {
        const bool menuOk = DB_DumpVitaMenus(menuFile);
        FS_FCloseFile(menuFile);
        Com_Printf(16, "Vita menu export: %s\n", menuOk ? "ok" : "failed");
    }
    const int soundFile = FS_FOpenFileWrite("vita_sounds.bin");
    if (soundFile >= 0)
    {
        const bool soundOk = DB_DumpVitaSounds(soundFile);
        FS_FCloseFile(soundFile);
        Com_Printf(16, "Vita sound export: %s\n", soundOk ? "ok" : "failed");
    }
    const int aliasFile = FS_FOpenFileWrite("vita_soundaliases.bin");
    if (aliasFile >= 0) {
        const bool aliasOk = DB_DumpVitaSoundAliases(aliasFile);
        FS_FCloseFile(aliasFile);
        Com_Printf(16, "Vita sound alias export: %s\n", aliasOk ? "ok" : "failed");
    }
    const int poolFile = FS_FOpenFileWrite("vita_soundpool.bin");
    if (poolFile >= 0) {
        const bool poolOk = DB_DumpVitaSoundPool(poolFile);
        FS_FCloseFile(poolFile);
        Com_Printf(16, "Vita complete sound pool export: %s\n", poolOk ? "ok" : "failed");
    }
    const int imageFile = FS_FOpenFileWrite("vita_images.bin");
    if (imageFile >= 0)
    {
        const bool imageOk = DB_DumpVitaImages(imageFile);
        FS_FCloseFile(imageFile);
        Com_Printf(16, "Vita image export: %s\n", imageOk ? "ok" : "failed");
    }
    const int materialFile = FS_FOpenFileWrite("vita_materials.bin");
    if (materialFile >= 0)
    {
        const bool materialOk = DB_DumpVitaMaterials(materialFile);
        FS_FCloseFile(materialFile);
        Com_Printf(16, "Vita material export: %s\n", materialOk ? "ok" : "failed");
    }
    const int fontFile = FS_FOpenFileWrite("vita_fonts.bin");
    if (fontFile >= 0)
    {
        const bool fontOk = DB_DumpVitaFonts(fontFile);
        FS_FCloseFile(fontFile);
        Com_Printf(16, "Vita font export: %s\n", fontOk ? "ok" : "failed");
    }
}

void __cdecl DB_CloneXAssetInternal(const XAsset *from, XAsset *to)
{
    uint32_t size; // [esp+0h] [ebp-4h]

    iassert(from->type == to->type);
    size = DB_GetXAssetTypeSize(from->type);
    iassert(size <= sizeof(XAssetSize));
    memcpy(to->header.data, from->header.data, size);
}

XAssetHeader __cdecl DB_FindXAssetDefaultHeaderInternal(XAssetType type)
{
    const char *XAssetName; // eax
    uint32_t assetEntryIndex; // [esp+8h] [ebp-Ch]
    const char *name; // [esp+Ch] [ebp-8h]
    XAssetEntryPoolEntry *assetEntry; // [esp+10h] [ebp-4h]

    name = g_defaultAssetName[type];
    for (assetEntryIndex = db_hashTable[DB_HashForName(name, type)]; ; assetEntryIndex = assetEntry->entry.nextHash)
    {
        if (!assetEntryIndex)
            return 0;
        assetEntry = &g_assetEntryPool[assetEntryIndex];
        if (assetEntry->entry.asset.type == type)
        {
            XAssetName = DB_GetXAssetName(&assetEntry->entry.asset);
            if (!I_stricmp(XAssetName, name))
                break;
        }
    }
    while (assetEntry->entry.nextOverride)
        assetEntry = &g_assetEntryPool[assetEntry->entry.nextOverride];
    return assetEntry->entry.asset.header;
}

BOOL __cdecl IsConfigFile(const char *name)
{
    iassert(name);
    return (strstr(name, ".cfg") != NULL);
}

void __cdecl PrintWaitedError(XAssetType type, const char *name, int32_t waitedMsec)
{
    if (waitedMsec > 100)
    {
        if (type == ASSET_TYPE_SOUND)
            Com_Printf(10, "Waited %i msec for missing asset \"%s\".\n", waitedMsec, name);
        else
            Com_PrintError(1, "Waited %i msec for missing asset \"%s\".\n", waitedMsec, name);
    }
    if (type != ASSET_TYPE_SOUND)
    {
        if (type == ASSET_TYPE_LOCALIZE_ENTRY)
        {
            if (loc_warnings->current.enabled)
            {
                if (loc_warningsAsErrors->current.enabled)
                {
                LABEL_15:
                    Com_PrintError(1, "Could not load %s \"%s\".\n", g_assetNames[type], name);
                    return;
                }
                Com_PrintWarning(10, "Could not load %s \"%s\".\n", g_assetNames[type], name);
            }
        }
        else if (type != ASSET_TYPE_RAWFILE || !IsConfigFile(name))
        {
            goto LABEL_15;
        }
    }
}

void __cdecl DB_Update()
{
    if (!Sys_IsMainThread())
        MyAssertHandler(".\\database\\db_registry.cpp", 2805, 0, "%s", "Sys_IsMainThread()");
    if (!Sys_IsDatabaseReady2())
    {
        if (Sys_IsDatabaseReady())
            DB_PostLoadXZone();
    }
}

void __cdecl DB_SetInitializing(bool inUse)
{
    g_initializing = inUse;
}

bool __cdecl DB_GetInitializing()
{
    return g_initializing;
}

bool __cdecl DB_IsXAssetDefault(XAssetType type, const char *name)
{
    const char *XAssetName; // eax
    uint32_t hash; // [esp+4h] [ebp-Ch]
    uint32_t assetEntryIndex; // [esp+8h] [ebp-8h]
    XAssetEntryPoolEntry *assetEntry; // [esp+Ch] [ebp-4h]

    hash = DB_HashForName(name, type);
    InterlockedIncrement(&db_hashCritSect.readCount);
    while (db_hashCritSect.writeCount)
        NET_Sleep(0);
    for (assetEntryIndex = db_hashTable[hash]; assetEntryIndex; assetEntryIndex = assetEntry->entry.nextHash)
    {
        assetEntry = &g_assetEntryPool[assetEntryIndex];
        if (assetEntry->entry.asset.type == type)
        {
            XAssetName = DB_GetXAssetName(&assetEntry->entry.asset);
            if (!I_stricmp(XAssetName, name))
            {
                if (db_hashCritSect.readCount <= 0)
                    MyAssertHandler(
                        "c:\\trees\\cod3\\src\\gfx_d3d\\../qcommon/threads_interlock.h",
                        76,
                        0,
                        "%s",
                        "critSect->readCount > 0");
                InterlockedDecrement(&db_hashCritSect.readCount);
                return assetEntry->entry.zoneIndex == 0;
            }
        }
    }
    if (!alwaysfails)
        MyAssertHandler(".\\database\\db_registry.cpp", 2849, 0, "unreachable");
    return 1;
}

int32_t __cdecl DB_GetAllXAssetOfType_FastFile(XAssetType type, XAssetHeader *assets, int32_t maxCount)
{
    uint32_t hash; // [esp+4h] [ebp-10h]
    uint32_t assetEntryIndex; // [esp+8h] [ebp-Ch]
    int32_t assetCount; // [esp+Ch] [ebp-8h]
    XAssetEntryPoolEntry *assetEntry; // [esp+10h] [ebp-4h]

    assetCount = 0;
    InterlockedIncrement(&db_hashCritSect.readCount);
    while (db_hashCritSect.writeCount)
        NET_Sleep(0);
    for (hash = 0; hash < 0x8000; ++hash)
    {
        for (assetEntryIndex = db_hashTable[hash]; assetEntryIndex; assetEntryIndex = assetEntry->entry.nextHash)
        {
            assetEntry = &g_assetEntryPool[assetEntryIndex];
            if (assetEntry->entry.asset.type == type)
            {
                if (assets)
                {
                    if (assetCount >= maxCount)
                        MyAssertHandler(".\\database\\db_registry.cpp", 2877, 0, "%s", "assetCount < maxCount");
                    assets[assetCount] = assetEntry->entry.asset.header;
                }
                ++assetCount;
            }
        }
    }
    if (db_hashCritSect.readCount <= 0)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\gfx_d3d\\../qcommon/threads_interlock.h",
            76,
            0,
            "%s",
            "critSect->readCount > 0");
    InterlockedDecrement(&db_hashCritSect.readCount);
    return assetCount;
}

void DB_SyncLostDevice()
{
    if (g_isRecoveringLostDevice)
    {
        if (g_mayRecoverLostAssets)
            MyAssertHandler(".\\database\\db_registry.cpp", 2945, 0, "%s", "!g_mayRecoverLostAssets");
        g_mayRecoverLostAssets = 1;
        do
            NET_Sleep(0x19u);
        while (g_isRecoveringLostDevice);
        if (g_mayRecoverLostAssets)
            MyAssertHandler(".\\database\\db_registry.cpp", 2951, 0, "%s", "!g_mayRecoverLostAssets");
    }
}

XAssetHeader __cdecl DB_AddXAsset(XAssetType type, XAssetHeader header)
{
    XAssetEntryPoolEntry *existingEntry; // [esp+0h] [ebp-14h]
    XAssetEntryPoolEntry newEntry; // [esp+4h] [ebp-10h] BYREF

    newEntry.entry.asset.type = type;
    newEntry.entry.asset.header = header;
    Sys_LockWrite(&db_hashCritSect);
    existingEntry = DB_LinkXAssetEntry(&newEntry, 0);
    Sys_UnlockWrite(&db_hashCritSect);
    DB_SyncLostDevice();
    return existingEntry->entry.asset.header;
}

XAssetEntryPoolEntry *__cdecl DB_LinkXAssetEntry(XAssetEntryPoolEntry *newEntry, int32_t allowOverride)
{
    int32_t v2; // edx
    const char *XAssetName; // eax
    //XAssetHeader v5; // edx
    XAssetEntryPoolEntry *existingEntry; // [esp+0h] [ebp-8ACh]
    uint32_t hash; // [esp+4h] [ebp-8A8h]
    uint32_t existingEntryIndex; // [esp+8h] [ebp-8A4h]
    XAssetEntryPoolEntry *overrideAssetEntry; // [esp+Ch] [ebp-8A0h]
    XAsset asset; // [esp+10h] [ebp-89Ch] BYREF
    int32_t isStubAsset; // [esp+18h] [ebp-894h]
    const char *name; // [esp+1Ch] [ebp-890h]
    uint8_t zoneIndex; // [esp+23h] [ebp-889h]
    XAssetType type; // [esp+24h] [ebp-888h]
    uint16_t *pOverrideAssetEntryIndex; // [esp+28h] [ebp-884h]
    XAssetSize assetSize; // [esp+2Ch] [ebp-880h] BYREF

    name = DB_GetXAssetName(&newEntry->entry.asset);
    v2 = *name;
    isStubAsset = v2 == ',';
    if (v2 == ',')
        ++name;
    type = newEntry->entry.asset.type;
    hash = DB_HashForName(name, type);
    existingEntry = NULL;
    for (existingEntryIndex = db_hashTable[hash]; existingEntryIndex; existingEntryIndex = existingEntry->entry.nextHash)
    {
        existingEntry = &g_assetEntryPool[existingEntryIndex];
        if (existingEntry->entry.asset.type == type)
        {
            XAssetName = DB_GetXAssetName(&existingEntry->entry.asset);
            if (!I_stricmp(XAssetName, name))
                break;
        }
    }
    if (allowOverride)
    {
        iassert(!isStubAsset);
    }
    else
    {
        if (isStubAsset)
        {
            if (!existingEntryIndex)
                return (XAssetEntryPoolEntry *)DB_CreateDefaultEntry(type, (char *)name);
            iassert(existingEntry);
            return existingEntry;
        }
        //v5.xmodelPieces = newEntry->entry.asset.header.xmodelPieces;
        asset.type = newEntry->entry.asset.type;
        asset.header = newEntry->entry.asset.header;
        newEntry = DB_AllocXAssetEntry(asset.type, g_zoneIndex);
        DB_CloneXAssetInternal(&asset, &newEntry->entry.asset);
    }
    if (!existingEntryIndex)
    {
        newEntry->entry.nextHash = db_hashTable[hash];
        db_hashTable[hash] = newEntry - g_assetEntryPool;
        return newEntry;
    }
    iassert(existingEntry);
    if (existingEntry->entry.zoneIndex)
    {
        iassert(existingEntry->entry.zoneIndex != newEntry->entry.zoneIndex);
        if (!*g_defaultAssetName[type] && type != ASSET_TYPE_RAWFILE && type != ASSET_TYPE_MAP_ENTS)
        {
            Sys_UnlockWrite(&db_hashCritSect);
            Com_Error(
                ERR_DROP,
                "Attempting to override asset '%s' from zone '%s' with zone '%s'",
                name,
                g_zones[existingEntry->entry.zoneIndex].name,
                g_zones[newEntry->entry.zoneIndex].name);
        }
        if (!DB_OverrideAsset(newEntry->entry.zoneIndex, existingEntry->entry.zoneIndex))
        {
            for (pOverrideAssetEntryIndex = &existingEntry->entry.nextOverride;
                *pOverrideAssetEntryIndex;
                pOverrideAssetEntryIndex = &overrideAssetEntry->entry.nextOverride)
            {
                overrideAssetEntry = &g_assetEntryPool[*pOverrideAssetEntryIndex];
                if (DB_OverrideAsset(newEntry->entry.zoneIndex, overrideAssetEntry->entry.zoneIndex))
                    break;
            }
            newEntry->entry.nextOverride = *pOverrideAssetEntryIndex;
            *pOverrideAssetEntryIndex = newEntry - g_assetEntryPool;
            return existingEntry;
        }
        goto LABEL_46;
    }
    iassert(g_defaultAssetName[type][0]);
    iassert(!existingEntry->entry.nextOverride);
    iassert(g_defaultAssetCount);
    if (!allowOverride)
    {
    LABEL_46:
        if (allowOverride)
        {
            if (!existingEntry->entry.zoneIndex)
                MyAssertHandler(".\\database\\db_registry.cpp", 3096, 0, "%s", "existingEntry->zoneIndex");
            if (existingEntry->entry.inuse)
            {
                varXAsset = &existingEntry->entry.asset;
                Mark_XAsset();
            }
            newEntry->entry.nextOverride = existingEntry->entry.nextOverride;
            existingEntry->entry.nextOverride = newEntry - g_assetEntryPool;
            asset.header.xmodelPieces = (XModelPieces *)&assetSize;
            asset.type = type;
            DB_CloneXAssetInternal(&existingEntry->entry.asset, &asset);
            zoneIndex = existingEntry->entry.zoneIndex;
            DB_CloneXAssetEntry(&newEntry->entry, &existingEntry->entry);
            DB_CloneXAssetInternal(&asset, &newEntry->entry.asset);
            newEntry->entry.zoneIndex = zoneIndex;
        }
        else
        {
            DB_DelayedCloneXAsset(&newEntry->entry);
        }
        return existingEntry;
    }
    --g_defaultAssetCount;
    if (existingEntry->entry.inuse)
    {
        varXAsset = &existingEntry->entry.asset;
        Mark_XAsset();
    }
    DB_CloneXAssetEntry(&newEntry->entry, &existingEntry->entry);
    DB_FreeXAssetEntry(newEntry);
    return existingEntry;
}

void __cdecl DB_FreeXAssetEntry(XAssetEntryPoolEntry *assetEntry)
{
    XAssetEntryPoolEntry *oldFreeHead; // [esp+4h] [ebp-4h]

    DB_FreeXAssetHeader(assetEntry->entry.asset.type, assetEntry->entry.asset.header);
    oldFreeHead = g_freeAssetEntryHead;
    g_freeAssetEntryHead = assetEntry;
    assetEntry->next = oldFreeHead;
}

void __cdecl DB_FreeXAssetHeader(XAssetType type, XAssetHeader header)
{
    DB_FreeXAssetHeaderHandler[type](DB_XAssetPool[type], header);
}

void __cdecl DB_CloneXAssetEntry(const XAssetEntry *from, XAssetEntry *to)
{
    iassert(from->asset.type == to->asset.type);
    DB_DynamicCloneXAsset(to->asset.header, from->asset.header, to->asset.type, to->zoneIndex == 0);
    DB_CloneXAssetInternal(&from->asset, &to->asset);
    to->zoneIndex = from->zoneIndex;
}

void(__cdecl *DB_DynamicCloneXAssetHandler[ASSET_TYPE_COUNT])(XAssetHeader, XAssetHeader, int) =
{
    NULL, // 0
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL, // 9

    NULL, // 10
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,

    NULL, // 20
    DB_DynamicCloneMenu,
    NULL,
    (void(*)(XAssetHeader, XAssetHeader, int))KISAK_NULLSUB,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,

    NULL,
    NULL,
    NULL,
};
void __cdecl DB_DynamicCloneXAsset(XAssetHeader from, XAssetHeader to, XAssetType type, int32_t fromDefault)
{
    if (DB_DynamicCloneXAssetHandler[type])
        DB_DynamicCloneXAssetHandler[type](from, to, fromDefault);
}

int32_t g_sync;
void __cdecl DB_DelayedCloneXAsset(XAssetEntry *newEntry)
{
    const char *XAssetTypeName; // eax
    const char *XAssetName; // [esp-4h] [ebp-8h]
    uint32_t i; // [esp+0h] [ebp-4h]

    if (g_sync)
    {
        DB_LinkXAssetEntry((XAssetEntryPoolEntry *)newEntry, 1);
    }
    else
    {
        if (g_copyInfoCount >= 0x800)
        {
            Com_Printf(0, "g_copyInfo exceeded\n"); // LWSS: if this hits, it means that PostLoadXZone isn't setting back to zero (prob not being called)
            for (i = 0; i < 0x800; ++i)
            {
                XAssetName = DB_GetXAssetName(&g_copyInfo[i]->asset);
                XAssetTypeName = DB_GetXAssetTypeName(g_copyInfo[i]->asset.type);
                Com_Printf(0, "%s: %s\n", XAssetTypeName, XAssetName);
            }
            Sys_Error("g_copyInfo exceeded");
        }
        g_copyInfo[g_copyInfoCount++] = newEntry;
    }
}

bool __cdecl DB_OverrideAsset(uint32_t newZoneIndex, uint32_t existingZoneIndex)
{
    if (!newZoneIndex)
        MyAssertHandler(".\\database\\db_registry.cpp", 2959, 0, "%s", "newZoneIndex");
    if (!existingZoneIndex)
        MyAssertHandler(".\\database\\db_registry.cpp", 2960, 0, "%s", "existingZoneIndex");
    return g_zones[newZoneIndex].flags >= g_zones[existingZoneIndex].flags;
}

void __cdecl DB_GetXAsset(XAssetType type, XAssetHeader header)
{
    uint32_t assetEntryIndex; // [esp+4h] [ebp-14h]
    XAsset asset; // [esp+8h] [ebp-10h] BYREF
    const char *name; // [esp+10h] [ebp-8h]
    XAssetEntry *assetEntry; // [esp+14h] [ebp-4h]

    asset.type = type;
    asset.header = header;
    name = DB_GetXAssetName(&asset);
    for (assetEntryIndex = db_hashTable[DB_HashForName(name, type)]; ; assetEntryIndex = assetEntry->nextHash)
    {
        if (!assetEntryIndex)
            MyAssertHandler(".\\database\\db_registry.cpp", 3163, 0, "%s", "assetEntryIndex");
        assetEntry = &g_assetEntryPool[assetEntryIndex].entry;
        if (assetEntry->asset.type == type && assetEntry->asset.header.xmodelPieces == header.xmodelPieces)
            break;
    }
    assetEntry->inuse = 1;
}

void DB_PostLoadXZone()
{
    uint32_t i; // [esp+0h] [ebp-8h]
    int32_t remoteScreenUpdateNesting; // [esp+4h] [ebp-4h]

    iassert(Sys_IsMainThread() || Sys_IsRenderThread());
    iassert(!g_loadingZone);
    iassert(!g_zoneInfoCount);

    if (!Sys_IsDatabaseReady2())
    {
        if (g_copyInfoCount)
        {
            remoteScreenUpdateNesting = 0;
            if (!Sys_IsMainThread()
                || (++g_mainThreadBlocked,
                    remoteScreenUpdateNesting = R_PopRemoteScreenUpdate(),
                    --g_mainThreadBlocked,
                    g_copyInfoCount))

            {
                DB_ArchiveAssets();
                Sys_LockWrite(&db_hashCritSect);
                for (i = 0; i < g_copyInfoCount; ++i)
                    DB_LinkXAssetEntry((XAssetEntryPoolEntry *)g_copyInfo[i], 1);
                g_copyInfoCount = 0;
                Sys_UnlockWrite(&db_hashCritSect);
                Material_DirtyTechniqueSetOverrides();
                Material_OverrideTechniqueSets();
                DB_UnarchiveAssets();
                if (Sys_IsMainThread())
                    R_PushRemoteScreenUpdate(remoteScreenUpdateNesting);
                Sys_DatabaseCompleted2();
            }
            else
            {
                R_PushRemoteScreenUpdate(remoteScreenUpdateNesting);
            }
        }
        else
        {
            DB_ExternalInitAssets();
            Sys_DatabaseCompleted2();
        }
    }
}

void __cdecl DB_UpdateDebugZone()
{
    XZoneInfo zoneInfo[2]; // [esp+0h] [ebp-18h] BYREF

    if (g_debugZoneName[0])
    {
        zoneInfo[0].name = 0;
        zoneInfo[0].allocFlags = 0;
        zoneInfo[1].name = g_debugZoneName;
        Com_SyncThreads();
        zoneInfo[0].freeFlags = 64;
        zoneInfo[1].allocFlags = 64;
        zoneInfo[1].freeFlags = 64;
        DB_LoadXAssets(zoneInfo, 2u, 1);
        CG_VisionSetMyChanges();
    }
}

void __cdecl DB_SyncXAssets()
{
    if (!Sys_IsMainThread())
        MyAssertHandler(".\\database\\db_registry.cpp", 3386, 0, "%s", "Sys_IsMainThread()");
    R_BeginRemoteScreenUpdate();
    Sys_SyncDatabase();
    R_EndRemoteScreenUpdate();
    DB_PostLoadXZone();
}

cmd_function_s DB_LoadZone_f_VAR;
cmd_function_s DB_DumpVitaAssets_f_VAR;
cmd_function_s DB_DumpVitaGeometry_f_VAR;
void __cdecl DB_LoadXAssets(XZoneInfo *zoneInfo, uint32_t zoneCount, int32_t sync)
{
    uint32_t j; // [esp+4h] [ebp-14h]
    uint32_t ja; // [esp+4h] [ebp-14h]
    bool unloadedZone; // [esp+Bh] [ebp-Dh]
    uint32_t zoneIndex; // [esp+Ch] [ebp-Ch]
    int32_t i; // [esp+10h] [ebp-8h]
    int32_t zoneFreeFlags; // [esp+14h] [ebp-4h]

    iassert(Sys_IsMainThread());
    iassert(zoneCount);

    if (!g_zoneInited)
    {
        g_zoneInited = 1;
        DB_Init();
        Cmd_AddCommandInternal("loadzone", DB_LoadZone_f, &DB_LoadZone_f_VAR);
        Cmd_AddCommandInternal("dump_vita_assets", DB_DumpVitaAssets_f,
                               &DB_DumpVitaAssets_f_VAR);
        Cmd_AddCommandInternal("dump_vita_geometry", DB_DumpVitaGeometry_f,
                               &DB_DumpVitaGeometry_f_VAR);
    }

    unloadedZone = 0;
    Material_ClearShaderUploadList();
    DB_SyncXAssets();
    
    iassert(!g_archiveBuf);

    for (j = 0; j < zoneCount; ++j)
    {
        zoneFreeFlags = zoneInfo[j].freeFlags;
        for (i = g_zoneCount - 1; i >= 0; --i)
        {
            zoneIndex = g_zoneHandles[i];
            if ((zoneFreeFlags & g_zones[zoneIndex].flags) != 0)
            {
                if (!unloadedZone)
                {
                    unloadedZone = 1;
                    DB_SyncExternalAssets();
                    DB_ArchiveAssets();
                    Sys_LockWrite(&db_hashCritSect);
                }
                DB_UnloadXZone(zoneIndex, 1);
            }
        }
    }
    if (unloadedZone)
    {
        DB_FreeUnusedResources();
        for (ja = 0; ja < zoneCount; ++ja)
        {
            DB_UnloadXAssetsMemoryForZone(zoneInfo[ja].freeFlags, 64);
            DB_UnloadXAssetsMemoryForZone(zoneInfo[ja].freeFlags, 32);
            DB_UnloadXAssetsMemoryForZone(zoneInfo[ja].freeFlags, 16);
            DB_UnloadXAssetsMemoryForZone(zoneInfo[ja].freeFlags, 8);
            DB_UnloadXAssetsMemoryForZone(zoneInfo[ja].freeFlags, 4);
            DB_UnloadXAssetsMemoryForZone(zoneInfo[ja].freeFlags, 1);
        }
        Sys_UnlockWrite(&db_hashCritSect);
        DB_UnarchiveAssets();
    }
    if (sync)
        DB_ArchiveAssets();
    g_sync = sync;
    DB_LoadXZone(zoneInfo, zoneCount);
    if (sync)
    {
        iassert(!g_copyInfoCount);
        Sys_SyncDatabase();
        DB_UnarchiveAssets();
    }
}

void DB_Init()
{
    for (XAssetType type = (XAssetType)0; type < ASSET_TYPE_COUNT; ++type)
        DB_InitPoolHeader(type);

    g_freeAssetEntryHead = g_assetEntryPool + 16;

    for (int32_t i = 1; i < 0x7FFF; ++i)
        g_assetEntryPool[i].next = &g_assetEntryPool[i + 1];

    g_assetEntryPool[0x7FFF].next = NULL;
}

void __cdecl DB_InitPoolHeader(XAssetType type)
{
    if (DB_XAssetPool[type])
        DB_InitPoolHeaderHandler[type](DB_XAssetPool[type], g_poolSize[type]);
}

void __cdecl DB_LoadXZone(XZoneInfo *zoneInfo, uint32_t zoneCount)
{
    uint32_t j; // [esp+0h] [ebp-Ch]
    char *zoneName; // [esp+4h] [ebp-8h]
    uint32_t zoneInfoCount; // [esp+8h] [ebp-4h]

    if (g_zoneCount == 32)
        Com_Error(ERR_DROP, "Max zone count exceeded");
    if (g_zoneInfoCount)
        MyAssertHandler(".\\database\\db_registry.cpp", 3240, 0, "%s", "!g_zoneInfoCount");
    if (g_loadingAssets)
        MyAssertHandler(".\\database\\db_registry.cpp", 3241, 0, "%s", "!g_loadingAssets");
    zoneInfoCount = 0;
    for (j = 0; j < zoneCount; ++j)
    {
        zoneName = (char *)zoneInfo[j].name;
        if (zoneName)
        {
            if (zoneInfoCount >= 8)
                MyAssertHandler(".\\database\\db_registry.cpp", 3249, 0, "%s", "zoneInfoCount < ARRAY_COUNT( g_zoneInfo )");
            I_strncpyz(g_zoneInfo[zoneInfoCount].name, zoneName, 64);
            Com_Printf(16, "Loading fastfile %s\n", g_zoneInfo[zoneInfoCount].name);
            g_zoneInfo[zoneInfoCount++].flags = zoneInfo[j].allocFlags;
            if (zoneInfoCount)
            {
                //g_loadingAssets = zoneInfoCount;
                Sys_WakeDatabase2();
                Sys_WakeDatabase();
                //g_zoneInfoCount = zoneInfoCount;
                Sys_NotifyDatabase();
            }
        }
    }
    if (zoneInfoCount)
    {
        g_loadingAssets = zoneInfoCount;
        Sys_WakeDatabase2();
        Sys_WakeDatabase();
        g_zoneInfoCount = zoneInfoCount;
        Sys_NotifyDatabase();
    }
}

void __cdecl DB_LoadZone_f()
{
    char *v0; // eax

    v0 = (char *)Cmd_Argv(1);
    I_strncpyz(g_debugZoneName, v0, 64);
    DB_UpdateDebugZone();
}

void __cdecl DB_InitThread()
{
    if (!Sys_SpawnDatabaseThread((void(__cdecl *)(uint32_t))DB_Thread))
        Sys_Error("Failed to create database thread");
}

void __cdecl  DB_Thread(uint32_t threadContext)
{
    jmp_buf *Value; // eax

    iassert(threadContext == THREAD_CONTEXT_DATABASE);
    Value = (jmp_buf *)Sys_GetValue(2);
    
    if (setjmp(*Value))
    {
        Profile_Recover(1);
#ifdef __llvm__ 
        __builtin_debugtrap();
#else
        __debugbreak();
#endif
        Com_ErrorAbort();
    }
    Profile_Guard(1);
    while (1)
    {
        Sys_WaitStartDatabase();
        DB_TryLoadXFile();
    }
}

void DB_TryLoadXFile()
{
    uint32_t j; // [esp+0h] [ebp-8h]
    uint32_t zoneInfoCount; // [esp+4h] [ebp-4h]

    if (g_zoneInfoCount)
    {
        zoneInfoCount = g_zoneInfoCount;
        g_zoneInfoCount = 0;
        if (g_loadingZone)
            MyAssertHandler(".\\database\\db_registry.cpp", 3764, 0, "%s", "!g_loadingZone");
        for (j = 0; j < zoneInfoCount; ++j)
        {
            if (!DB_TryLoadXFileInternal(g_zoneInfo[j].name, g_zoneInfo[j].flags))
                --g_loadingAssets;
        }
        if (g_loadingZone)
            MyAssertHandler(".\\database\\db_registry.cpp", 3772, 0, "%s", "!g_loadingZone");
        if (g_loadingAssets)
            MyAssertHandler(".\\database\\db_registry.cpp", 3773, 0, "%s", "!g_loadingAssets");
        Sys_LockWrite(&s_dbReorder.critSect);
        DB_EndReorderZone();
        Sys_UnlockWrite(&s_dbReorder.critSect);
        Sys_DatabaseCompleted();
    }
    else if (g_loadingAssets)
    {
        MyAssertHandler(".\\database\\db_registry.cpp", 3759, 0, "%s", "!g_loadingAssets");
    }
}

char __cdecl DB_NextZoneCsvToken(const char **parse, char *token, uint32_t tokenSize, bool allowNewLine)
{
    uint32_t used; // [esp+0h] [ebp-Ch]
    bool isSkippingLeadingSpaces; // [esp+7h] [ebp-5h]
    const char *scan; // [esp+8h] [ebp-4h]

    scan = *parse;
    if (**parse == 10)
    {
        if (!allowNewLine)
            return 0;
        while (*scan == 10)
            ++scan;
    }
    if (*scan)
    {
        if (*scan == 44)
            ++scan;
        used = 0;
        isSkippingLeadingSpaces = 1;
        while (*scan && *scan != 10 && *scan != 44)
        {
            if (!isSkippingLeadingSpaces || !isspace(*scan))
            {
                isSkippingLeadingSpaces = 0;
                if (used < tokenSize - 1)
                    token[used++] = *scan;
            }
            ++scan;
        }
        while (used && isspace(token[used - 1]))
            --used;
        token[used] = 0;
        *parse = scan;
        return 1;
    }
    else
    {
        *parse = scan;
        return 0;
    }
}

void __cdecl DB_AddReorderAsset(const char *typeString, const char *assetName)
{
    char *v2; // [esp+0h] [ebp-10h]
    DBReorderAssetEntry *entry; // [esp+4h] [ebp-Ch]
    DBReorderAssetEntry *entrya; // [esp+4h] [ebp-Ch]
    int32_t type; // [esp+8h] [ebp-8h]
    uint32_t entryIter; // [esp+Ch] [ebp-4h]

    for (type = 0; type < 33 && _stricmp(typeString, g_assetNames[type]); ++type)
        ;
    if (type == 23)
    {
        //if (strnicmp(assetName, "mp/", 3u))
        if (_strnicmp(assetName, "mp/", 3u))
            MyAssertHandler(
                ".\\database\\db_registry.cpp",
                1911,
                0,
                "%s",
                "!strnicmp( assetName, REORDER_WEAPON_PREFIX, REORDER_WEAPON_PREFIX_LEN )");
        assetName += 3;
    }
    for (entryIter = 0; entryIter < s_dbReorder.entryCount; ++entryIter)
    {
        entry = &s_dbReorder.entries[entryIter];
        if (entry->type == type && !_stricmp(entry->assetName, assetName) && !_stricmp(entry->typeString, typeString))
            return;
    }
    entrya = &s_dbReorder.entries[s_dbReorder.entryCount++];
    entrya->type = type;
    if (type >= 33)
        v2 = _strdup(typeString);
    else
        v2 = (char *)g_assetNames[type];
    entrya->typeString = v2;
    entrya->assetName = _strdup(assetName);
    if (entrya->type != 33 || I_stricmp(typeString, "ignore"))
    {
        if (entrya->type == 10 || entrya->type == 11)
            entrya->sequence = 1;
        else
            entrya->sequence = -1;
    }
    else
    {
        entrya->sequence = 0;
    }
}

void __cdecl DB_BeginReorderZone(const char *zoneName)
{
    char v1; // [esp+3h] [ebp-259h]
    char *v2; // [esp+8h] [ebp-254h]
    const char *v3; // [esp+Ch] [ebp-250h]
    char *from; // [esp+10h] [ebp-24Ch]
    DBReorderAssetEntry *entry; // [esp+14h] [ebp-248h]
    char assetType[32]; // [esp+18h] [ebp-244h] BYREF
    uint32_t size; // [esp+38h] [ebp-224h]
    void *file; // [esp+3Ch] [ebp-220h]
    int32_t success; // [esp+40h] [ebp-21Ch]
    char assetName[256]; // [esp+44h] [ebp-218h] BYREF
    char csvName[256]; // [esp+144h] [ebp-118h] BYREF
    const char *parse; // [esp+248h] [ebp-14h] BYREF
    char *csv; // [esp+24Ch] [ebp-10h]
    char *to; // [esp+250h] [ebp-Ch]
    DWORD read; // [esp+254h] [ebp-8h] BYREF
    DWORD entryIter; // [esp+258h] [ebp-4h]

    for (entryIter = 0; entryIter < s_dbReorder.entryCount; ++entryIter)
    {
        entry = &s_dbReorder.entries[entryIter];
        free((void *)entry->assetName);
        if (entry->type == 33)
            free((void *)entry->typeString);
    }
    s_dbReorder.entryCount = 0;
    s_dbReorder.sequence = 2;
    s_dbReorder.sequenceForIncludes = 3;
    s_dbReorder.alreadyFinished = 0;
    s_dbReorder.loadedSound = 0;
    s_dbReorder.loadedLocalization = 0;
    s_dbReorder.lastEntry = 0;
    v3 = zoneName;
    v2 = s_dbReorder.zoneName;
    do
    {
        v1 = *v3;
        *v2++ = *v3++;
    } while (v1);
    Sys_LockWrite(&s_dbReorder.critSect);
    Com_sprintf(csvName, 0x100u, "..\\share\\zone_source\\%s.csv", zoneName);
    file = CreateFileA(csvName, 0x80000000, 0, 0, 3u, 0, 0);
    if (file == (void *)-1)
    {
        Sys_UnlockWrite(&s_dbReorder.critSect);
    }
    else
    {
        size = GetFileSize(file, 0);
        csv = (char *)malloc(size + 1);
        success = ReadFile(file, csv, size, &read, 0);
        CloseHandle(file);
        if (success && read == size)
        {
            csv[size] = 0;
            to = csv;
            for (from = csv; *from; ++from)
            {
                if (*from != 13)
                    *to++ = *from;
            }
            *to = 0;
            for (parse = csv; DB_NextZoneCsvToken(&parse, assetType, 0x20u, 1); ++parse)
            {
                if (DB_NextZoneCsvToken(&parse, assetName, 0x100u, 0) && assetType[0] && assetName[0])
                    DB_AddReorderAsset(assetType, assetName);
                while (*parse && *parse != 10)
                    ++parse;
                if (!*parse)
                    break;
            }
            free(csv);
            Sys_UnlockWrite(&s_dbReorder.critSect);
        }
        else
        {
            free(csv);
            Sys_UnlockWrite(&s_dbReorder.critSect);
        }
    }
}

char __cdecl DB_ShouldLoadFromModDir(const char *zoneName)
{
    const char *strPos; // [esp+0h] [ebp-8h]
    int32_t i; // [esp+4h] [ebp-4h]

    if (!zoneName)
        MyAssertHandler(".\\database\\db_registry.cpp", 3523, 0, "%s", "zoneName");
    if (com_sv_running->current.enabled)
        return 1;
    if (!fs_numServerReferencedFFs)
        return 1;
    if (!I_stricmp(zoneName, "mod"))
        return 1;
    for (i = 0; ; ++i)
    {
        if (i >= fs_numServerReferencedFFs)
            return 0;
        if (!fs_serverReferencedFFNames[i])
            MyAssertHandler(".\\database\\db_registry.cpp", 3536, 0, "%s", "fs_serverReferencedFFNames[i]");
        strPos = I_stristr(fs_serverReferencedFFNames[i], zoneName);
        if (strPos)
            break;
    }
    if (strPos <= fs_serverReferencedFFNames[i])
        return 0;
    if (!I_strncmp(fs_serverReferencedFFNames[i], "mods", 4))
        MyAssertHandler(
            ".\\database\\db_registry.cpp",
            3543,
            0,
            "%s",
            "I_strncmp( fs_serverReferencedFFNames[i], MODS_BASE_PATH, MODS_BASE_PATH_LENGTH )");
    return 1;
}

int32_t __cdecl DB_TryLoadXFileInternal(char *zoneName, int32_t zoneFlags)
{
    const char *v3; // eax
    uint32_t v4; // eax
    //uint8_t v5; // [esp+0h] [ebp-11Ch]
    uint32_t startWaitingTime; // [esp+4h] [ebp-118h]
    XZone *zone; // [esp+8h] [ebp-114h]
    char filename[256]; // [esp+Ch] [ebp-110h] BYREF
    bool modZone; // [esp+113h] [ebp-9h]
    uint32_t i; // [esp+114h] [ebp-8h]
    void *zoneFile; // [esp+118h] [ebp-4h]

    Com_Printf(0, "Trying to load file %s with flags %x\n", zoneName, zoneFlags);

    modZone = 0;
    iassert(!g_zoneInfoCount);
    if (I_stricmp(zoneName, "mp_patch"))
    {
        if (*(_BYTE *)fs_gameDirVar->current.integer && DB_ShouldLoadFromModDir(zoneName))
        {
            DB_BuildOSPath_Mod(zoneName, 256, filename);
            zoneFile = CreateFileA(filename, 0x80000000, 1u, 0, 3u, 0x60000000u, 0);
            modZone = (char *)zoneFile + 1 != 0;
        }
        else
        {
            zoneFile = (void *)-1;
        }
        if (zoneFile == (void *)-1)
        {
            DB_BuildOSPath(zoneName, 256, filename);
            zoneFile = CreateFileA(filename, 0x80000000, 1u, 0, 3u, 0x60000000u, 0);
        }
    }
    else
    {
        zoneFile = CreateFileA("update:\\mp_patch.ff", 0x80000000, 0, 0, 3u, 0x60000000u, 0);
        if (zoneFile == (void *)-1)
        {
            Com_Printf(16, "Loading mp_patch.ff from disc, not from the update drive\n");
            DB_BuildOSPath(zoneName, 256, filename);
            zoneFile = CreateFileA(filename, 0x80000000, 0, 0, 3u, 0x60000000u, 0);
            if (zoneFile == (void *)-1)
            {
                Com_PrintWarning(10, "WARNING: Could not find zone '%s'\n", filename);
                return 0;
            }
        }
    }
    if (zoneFile == (void *)-1)
    {
        v3 = strstr(filename, "_load");
        if (v3)
        {
            Com_PrintWarning(10, "WARNING: Could not find zone '%s'\n", filename);
        }
        else
        {
            Sys_DatabaseCompleted();
            Com_Error(ERR_DROP, "ERROR: Could not find zone '%s'\n", filename);
        }
        return 0;
    }
    else
    {
        g_zoneIndex = 0;
        for (i = 1; i < 0x21; ++i)
        {
            if (!g_zones[i].name[0])
            {
                g_zoneIndex = i;
                break;
            }
        }

        if (!g_zoneIndex)
            MyAssertHandler(".\\database\\db_registry.cpp", 3667, 0, "%s", "g_zoneIndex");
        if (!*zoneName)
            MyAssertHandler(".\\database\\db_registry.cpp", 3668, 0, "%s", "zoneName[0]");
        zone = &g_zones[g_zoneIndex];
        memset(zone, 0, sizeof(XZone));
        //v5 = g_zoneIndex;
        if (g_zoneIndex != (uint8_t)g_zoneIndex)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\qcommon\\../universal/assertive.h",
                281,
                0,
                "i == static_cast< Type >( i )\n\t%i, %i",
                g_zoneIndex,
                (uint8_t)g_zoneIndex);
        g_zoneHandles[g_zoneCount] = g_zoneIndex;
        if (zone->name[0])
            MyAssertHandler(".\\database\\db_registry.cpp", 3674, 0, "%s", "!zone->name[0]");
        I_strncpyz(zone->name, zoneName, 64);
        zone->flags = zoneFlags;
        zone->fileSize = GetFileSize(zoneFile, 0);
        zone->modZone = modZone;
        if (g_loadingZone)
            MyAssertHandler(".\\database\\db_registry.cpp", 3683, 0, "%s", "!g_loadingZone");
        if ((_S1 & 1) == 0)
        {
            _S1 |= 1u;
            zone_reorder = Dvar_RegisterString(
                "zone_reorder",
                "",
                DVAR_NOFLAG,
                "Set to the name of the fast file you want to reorder");
        }
        if (!_stricmp(zone_reorder->current.string, zoneName))
            DB_BeginReorderZone(zoneName);
        ++g_zoneCount;
        g_loadingZone = 1;
        while (g_isRecoveringLostDevice)
            NET_Sleep(25);
        g_mayRecoverLostAssets = 0;
        g_zoneAllocType = DB_GetZoneAllocType(zoneFlags);
        if (g_zoneAllocType == 1 && g_initializing)
        {
            startWaitingTime = Sys_Milliseconds();
            Com_Printf(0, "Waiting for $init to finish.  There may be assets missing from code_post_gfx.\n");
            while (g_initializing)
                DB_Sleep(1);
            v4 = Sys_Milliseconds();
            Com_Printf(16, "Waited %d ms for $init to finish.\n", v4 - startWaitingTime);
        }
        PMem_BeginAlloc(zone->name, g_zoneAllocType);
        zone->allocType = g_zoneAllocType;
        DB_ResetZoneSize((zoneFlags & 8) != 0);
        if (!zone)
            MyAssertHandler(".\\database\\db_registry.cpp", 3718, 0, "%s", "zone");
        DB_LoadXFile(filename, zoneFile, zone->name, &zone->mem, 0, g_fileBuf, g_zoneAllocType);
        DB_LoadXFileInternal();
        PMem_EndAlloc(zone->name, g_zoneAllocType);
        if (!g_loadingZone)
            MyAssertHandler(".\\database\\db_registry.cpp", 3725, 0, "%s", "g_loadingZone");
        g_loadingZone = 0;
        g_mayRecoverLostAssets = 1;
        return 1;
    }
}

void __cdecl DB_BuildOSPath(const char *zoneName, uint32_t size, char *filename)
{
    char *v3; // eax
    char *Language; // [esp-8h] [ebp-8h]

    Language = Win_GetLanguage();
    v3 = (fs_basepath && fs_basepath->current.string && *fs_basepath->current.string)
        ? (char *)fs_basepath->current.string : Sys_DefaultInstallPath();
    Com_sprintf(filename, size, "%s\\zone\\%s\\%s.ff", v3, Language, zoneName);
}

int32_t __cdecl DB_GetZoneAllocType(int32_t zoneFlags)
{
    int32_t result; // eax

    switch (zoneFlags)
    {
    case 1:
    case 4:
    case 16:
    case 32:
    case 64:
        result = 1;
        break;
    default:
        result = 0;
        break;
    }
    return result;
}

void __cdecl DB_UnloadXZone(uint32_t zoneIndex, bool createDefault)
{
    uint32_t hash; // [esp+4h] [ebp-28h]
    uint16_t *pAssetEntryIndex; // [esp+8h] [ebp-24h]
    XAssetEntryPoolEntry *overrideAssetEntry; // [esp+Ch] [ebp-20h]
    XAsset asset; // [esp+14h] [ebp-18h] BYREF
    const char *name; // [esp+1Ch] [ebp-10h]
    XAssetEntry *assetEntry; // [esp+20h] [ebp-Ch]
    uint16_t *pOverrideAssetEntryIndex; // [esp+24h] [ebp-8h]
    uint32_t overrideAssetEntryIndex; // [esp+28h] [ebp-4h]

    iassert(zoneIndex);
    hash = 0;

    // KISAKTODO: would be nice
#if 0
    //Com_Printf(16, "Unloading assets from fastfile '%s' ", g_zoneNames[zoneIndex]) // KISAKTODO: would be nice
    Com_Printf(16, "Unloading assets from fastfile '%i' ", zoneIndex);
    
    if (createDefault)
    {
        Com_Printf(16, "and creating default assets stubs\n");
    }
    else
    {
        Com_Printf(16, "and deleting all assets\n");
    }
#endif

LABEL_4:
    if (hash < 0x8000)
    {
        pAssetEntryIndex = &db_hashTable[hash];
        while (1)
        {
            while (1)
            {
                if (!*pAssetEntryIndex)
                {
                    ++hash;
                    goto LABEL_4;
                }
                assetEntry = &g_assetEntryPool[*pAssetEntryIndex].entry;
                if (assetEntry->zoneIndex == zoneIndex)
                    break;
            LABEL_24:
                pOverrideAssetEntryIndex = &assetEntry->nextOverride;
                while (*pOverrideAssetEntryIndex)
                {
                    overrideAssetEntry = &g_assetEntryPool[*pOverrideAssetEntryIndex];
                    iassert(!overrideAssetEntry->entry.inuse);
                    if (overrideAssetEntry->entry.zoneIndex == zoneIndex)
                    {
                        DB_RemoveXAsset(&overrideAssetEntry->entry.asset);
                        *pOverrideAssetEntryIndex = overrideAssetEntry->entry.nextOverride;
                        DB_FreeXAssetEntry(overrideAssetEntry);
                    }
                    else
                    {
                        pOverrideAssetEntryIndex = &overrideAssetEntry->entry.nextOverride;
                    }
                }
                pAssetEntryIndex = &assetEntry->nextHash;
            }
            if (assetEntry->inuse && createDefault)
            {
                varXAsset = &assetEntry->asset;
                Mark_XAsset();
            }
            DB_RemoveXAsset(&assetEntry->asset);
            overrideAssetEntryIndex = assetEntry->nextOverride;
            if (overrideAssetEntryIndex)
            {
                overrideAssetEntry = &g_assetEntryPool[overrideAssetEntryIndex];
                DB_CloneXAssetEntry(&overrideAssetEntry->entry, assetEntry);
                assetEntry->nextOverride = overrideAssetEntry->entry.nextOverride;
                DB_FreeXAssetEntry(overrideAssetEntry);
                goto LABEL_24;
            }
            if (createDefault)
            {
                asset.type = assetEntry->asset.type;
                asset.header = DB_FindXAssetDefaultHeaderInternal(asset.type);
                if (asset.header.xmodelPieces)
                {
                    ++g_defaultAssetCount;
                    assetEntry->zoneIndex = 0;
                    name = DB_GetXAssetName(&assetEntry->asset);
                    DB_CloneXAssetInternal(&asset, &assetEntry->asset);
                    DB_SetXAssetName(&assetEntry->asset, name);
                    goto LABEL_24;
                }
                
                iassert(!assetEntry->nextOverride);
                *pAssetEntryIndex = assetEntry->nextHash;
                DB_FreeXAssetEntry((XAssetEntryPoolEntry *)assetEntry);
                if (*g_defaultAssetName[asset.type])
                {
                    Sys_UnlockWrite(&db_hashCritSect);
                    asset.header = DB_FindXAssetDefaultHeaderInternal(asset.type);
                    Sys_Error("Could not load default asset for asset type '%s'", g_assetNames[asset.type]);
                }
            }
            else
            {
                iassert(!assetEntry->nextOverride);
                *pAssetEntryIndex = assetEntry->nextHash;
                DB_FreeXAssetEntry((XAssetEntryPoolEntry *)assetEntry);
            }
        }
    }
}

void(__cdecl *DB_RemoveXAssetHandler[ASSET_TYPE_COUNT])(XAssetHeader) =
{
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  (void(*)(XAssetHeader)) & Material_ReleaseTechniqueSet,
  (void(*)(XAssetHeader)) & Image_Free,
  NULL,
  NULL,
  &DB_RemoveLoadedSound,
  &DB_RemoveClipMap,
  &DB_RemoveClipMap,
  &DB_RemoveComWorld,
  NULL,
  NULL,
  NULL,
  &DB_RemoveGfxWorld,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL
}; // idb

void __cdecl DB_RemoveXAsset(XAsset *asset)
{
    if (DB_RemoveXAssetHandler[asset->type])
        DB_RemoveXAssetHandler[asset->type](asset->header);
}

void __cdecl DB_ReleaseXAssets()
{
    uint32_t hash; // [esp+0h] [ebp-Ch]
    uint32_t assetEntryIndex; // [esp+4h] [ebp-8h]

    if (!Sys_IsMainThread())
        MyAssertHandler(".\\database\\db_registry.cpp", 3998, 0, "%s", "Sys_IsMainThread()");
    Sys_SyncDatabase();
    for (hash = 0; hash < 0x8000; ++hash)
    {
        for (assetEntryIndex = db_hashTable[hash];
            assetEntryIndex;
            assetEntryIndex = g_assetEntryPool[assetEntryIndex].entry.nextHash)
        {
            g_assetEntryPool[assetEntryIndex].entry.inuse = 0;
        }
    }
}

void __cdecl DB_ShutdownXAssets()
{
    int32_t i; // [esp+0h] [ebp-4h]
    int32_t ia; // [esp+0h] [ebp-4h]

    DB_SyncXAssets();
    DB_SyncExternalAssets();
    iassert(!db_hashCritSect.writeCount);
    Sys_LockWrite(&db_hashCritSect);
    for (i = g_zoneCount - 1; i >= 0; --i)
        DB_UnloadXZone(g_zoneHandles[i], 0);
    DB_FreeDefaultEntries();
    DB_FreeUnusedResources();
    for (ia = g_zoneCount - 1; ia >= 0; --ia)
        DB_UnloadXZoneMemory(&g_zones[g_zoneHandles[ia]]);
    g_zoneCount = 0;
    Sys_UnlockWrite(&db_hashCritSect);
}

void __cdecl DB_FreeXZoneMemory(XZoneMemory *zoneMem)
{
    uint32_t blockIndex; // [esp+0h] [ebp-4h]

    DB_ReleaseGeometryBuffers(zoneMem);
    for (blockIndex = 0; blockIndex < 9; ++blockIndex)
    {
        zoneMem->blocks[blockIndex].data = 0;
        zoneMem->blocks[blockIndex].size = 0;
    }
}

void __cdecl DB_UnloadXZoneMemory(XZone *zone)
{
    DB_FreeXZoneMemory(&zone->mem);
    Com_Printf(16, "Unloaded fastfile %s\n", zone->name);
    PMem_Free(zone->name, zone->allocType);
    zone->name[0] = 0;
}

void DB_FreeDefaultEntries()
{
    uint32_t nextAssetEntryIndex; // [esp+0h] [ebp-10h]
    uint32_t hash; // [esp+4h] [ebp-Ch]
    uint32_t assetEntryIndex; // [esp+8h] [ebp-8h]
    XAssetEntryPoolEntry *assetEntry; // [esp+Ch] [ebp-4h]

    for (hash = 0; hash < 0x8000; ++hash)
    {
        for (assetEntryIndex = db_hashTable[hash]; assetEntryIndex; assetEntryIndex = nextAssetEntryIndex)
        {
            assetEntry = &g_assetEntryPool[assetEntryIndex];
            nextAssetEntryIndex = assetEntry->entry.nextHash;
            if (assetEntry->entry.zoneIndex)
                MyAssertHandler(".\\database\\db_registry.cpp", 3950, 0, "%s", "!assetEntry->zoneIndex");
            if (assetEntry->entry.nextOverride)
                MyAssertHandler(".\\database\\db_registry.cpp", 3951, 0, "%s", "!assetEntry->nextOverride");
            if (!g_defaultAssetCount)
                MyAssertHandler(".\\database\\db_registry.cpp", 3952, 0, "%s", "g_defaultAssetCount");
            --g_defaultAssetCount;
            DB_FreeXAssetEntry(assetEntry);
        }
        db_hashTable[hash] = 0;
    }
    if (g_defaultAssetCount)
        MyAssertHandler(".\\database\\db_registry.cpp", 3959, 0, "%s", "!g_defaultAssetCount");
}

void __cdecl DB_UnloadXAssetsMemoryForZone(int32_t zoneFreeFlags, int32_t zoneFreeBit)
{
    int32_t sortedIndex; // [esp+0h] [ebp-8h]
    XZone *zone; // [esp+4h] [ebp-4h]

    if ((zoneFreeBit & zoneFreeFlags) != 0)
    {
        for (sortedIndex = g_zoneCount - 1; sortedIndex >= 0; --sortedIndex)
        {
            zone = &g_zones[g_zoneHandles[sortedIndex]];
            if ((zoneFreeBit & zone->flags) != 0)
                DB_UnloadXAssetsMemory(zone, sortedIndex);
        }
    }
}

void __cdecl DB_UnloadXAssetsMemory(XZone *zone, int32_t sortedIndex)
{
    DB_UnloadXZoneMemory(zone);
    --g_zoneCount;
    while (sortedIndex < g_zoneCount)
    {
        //g_zoneHandles[sortedIndex] = *(_BYTE *)(sortedIndex + 19939261);
        g_zoneHandles[sortedIndex] = g_zoneHandles[sortedIndex + 1];
        ++sortedIndex;
    }
}

void __cdecl DB_ReplaceModel(const char *original, const char *replacement)
{
    DB_ReplaceXAsset(ASSET_TYPE_XMODEL, original, replacement);
}

void __cdecl DB_ReplaceXAsset(XAssetType type, const char *original, const char *replacement)
{
    const char *originalName; // [esp+8h] [ebp-14h]
    XAsset replacementAsset; // [esp+Ch] [ebp-10h] BYREF
    XAsset originalAsset; // [esp+14h] [ebp-8h] BYREF

    originalAsset.type = type;
    originalAsset.header = DB_FindXAssetHeader(type, original);
    originalName = DB_GetXAssetName(&originalAsset);
    replacementAsset.type = type;
    replacementAsset.header = DB_FindXAssetHeader(type, replacement);
    DB_CloneXAsset(&replacementAsset, &originalAsset);
    DB_SetXAssetName(&originalAsset, originalName);
}

void __cdecl DB_CloneXAsset(const XAsset *from, XAsset *to)
{
    if (from->type != to->type)
        MyAssertHandler(".\\database\\db_registry.cpp", 2504, 0, "%s", "from->type == to->type");
    DB_DynamicCloneXAsset(to->header, from->header, to->type, 0);
    DB_CloneXAssetInternal(from, to);
}

void DB_SyncExternalAssets()
{
#ifndef DEDICATED
    R_SyncRenderThread();
    RB_UnbindAllImages();
    R_ShutdownStreams();
    RB_ClearPixelShader();
    RB_ClearVertexShader();
    RB_ClearVertexDecl();
#endif
}

void DB_ArchiveAssets()
{
    if (!g_archiveBuf)
    {
        g_archiveBuf = 1;
        R_SyncRenderThread();
        R_ClearAllStaticModelCacheRefs();
        DB_SaveSounds();
        DB_SaveDObjs();
    }
}

void DB_FreeUnusedResources()
{
    uint32_t hash; // [esp+0h] [ebp-18h]
    uint32_t hasha; // [esp+0h] [ebp-18h]
    uint16_t *pAssetEntryIndex; // [esp+4h] [ebp-14h]
    uint32_t assetEntryIndex; // [esp+8h] [ebp-10h]
    const char *newName; // [esp+Ch] [ebp-Ch]
    char *name; // [esp+10h] [ebp-8h]
    XAssetEntryPoolEntry *assetEntry; // [esp+14h] [ebp-4h]

    SL_TransferSystem(4u, 8u);
    for (hash = 0; hash < 0x8000; ++hash)
    {
        for (assetEntryIndex = db_hashTable[hash];
            assetEntryIndex;
            assetEntryIndex = g_assetEntryPool[assetEntryIndex].entry.nextHash)
        {
            if (g_assetEntryPool[assetEntryIndex].entry.zoneIndex)
            {
                varXAsset = &g_assetEntryPool[assetEntryIndex].entry.asset;
                Mark_XAsset();
            }
        }
    }
    for (hasha = 0; hasha < 0x8000; ++hasha)
    {
        //pAssetEntryIndex = (uint16_t *)(2 * hasha + 17442712);
        pAssetEntryIndex = &db_hashTable[hasha];
        while (*pAssetEntryIndex)
        {
            assetEntry = &g_assetEntryPool[*pAssetEntryIndex];
            if (assetEntry->entry.zoneIndex)
            {
                pAssetEntryIndex = &assetEntry->entry.nextHash;
            }
            else if (assetEntry->entry.inuse)
            {
                name = (char *)DB_GetXAssetName(&assetEntry->entry.asset);
                newName = SL_ConvertToString(SL_GetString(name, 4));
                DB_SetXAssetName(&assetEntry->entry.asset, newName);
                pAssetEntryIndex = &assetEntry->entry.nextHash;
            }
            else
            {
                if (assetEntry->entry.nextOverride)
                    MyAssertHandler(".\\database\\db_registry.cpp", 4200, 0, "%s", "!assetEntry->nextOverride");
                *pAssetEntryIndex = assetEntry->entry.nextHash;
                if (!g_defaultAssetCount)
                    MyAssertHandler(".\\database\\db_registry.cpp", 4202, 0, "%s", "g_defaultAssetCount");
                --g_defaultAssetCount;
                DB_FreeXAssetEntry(assetEntry);
            }
        }
    }
    SL_ShutdownSystem(8);
}

void DB_ExternalInitAssets()
{
    Material_DirtyTechniqueSetOverrides();
    BG_FillInAllWeaponItems();
}

void DB_UnarchiveAssets()
{
    iassert(g_archiveBuf);
    g_archiveBuf = 0;
    DB_LoadSounds();
    DB_LoadDObjs();
    DB_ExternalInitAssets();
    iassert(Sys_IsMainThread() || Sys_IsRenderThread());

    if (Sys_IsMainThread())
        R_ReleaseThreadOwnership();
}

void __cdecl DB_Cleanup()
{
    Sys_SyncDatabase();
    iassert(!g_archiveBuf);
}

int32_t __cdecl DB_FileSize(const char *zoneName, int32_t isMod)
{
    char filename[260]; // [esp+0h] [ebp-110h] BYREF
    int32_t size; // [esp+108h] [ebp-8h]
    void *zoneFile; // [esp+10Ch] [ebp-4h]

    if (isMod)
        DB_BuildOSPath_Mod(zoneName, 0x100u, filename);
    else
        DB_BuildOSPath(zoneName, 0x100u, filename);
    zoneFile = CreateFileA(filename, 0x80000000, 1u, 0, 3u, 0x60000000u, 0);
    if (zoneFile == (void *)-1)
        return 0;
    size = GetFileSize(zoneFile, 0);
    CloseHandle(zoneFile);
    return size;
}

void __cdecl Load_GetCurrentZoneHandle(uint8_t *handle)
{
    //uint8_t v1; // [esp+0h] [ebp-4h]

    iassert(g_loadingZone);
    //v1 = g_zoneIndex;
    //if (g_zoneIndex != g_zoneIndex)
    //    MyAssertHandler(
    //        "c:\\trees\\cod3\\src\\qcommon\\../universal/assertive.h",
    //        281,
    //        0,
    //        "i == static_cast< Type >( i )\n\t%i, %i",
    //        g_zoneIndex,
    //        g_zoneIndex);
    *handle = g_zoneIndex;
}
