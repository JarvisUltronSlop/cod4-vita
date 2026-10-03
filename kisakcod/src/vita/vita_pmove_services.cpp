#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cstdio>

#include <bgame/bg_public.h>
#include <bgame/bg_local.h>
#include <bgame/bg_weapon_def_vita.h>
#include <universal/com_math.h>
#include <vita/vita_pmove_services.h>
#include <vita/vita_script_services.h>
#include <vita/vita_shipment_collision.h>
#include <qcommon/qcommon.h>

#ifndef KISAK_VITA_PROTOTYPE_BRIDGE
#define KISAK_VITA_PROTOTYPE_BRIDGE 0
#endif

// These globals are owned by the full KisakCOD BG objects when that archive
// is linked.  The weak definitions below keep the narrower engine/probe
// targets linkable; all accesses in the bridge intentionally go through the
// resolved symbol so the bootstrap uses the real source globals.
extern bgs_t* bgs;
extern animScriptData_t* globalScriptData;
extern WeaponDef* bg_weaponDefs[128];
extern uint32_t bg_lastParsedWeaponIndex;

namespace {

const kisak::vita::ShipmentCollision* g_collision = nullptr;
VitaPmoveWeaponConfig g_weaponConfig{};

// The full multiplayer Pmove object graph is linked into the bootstrap so
// that weapon timing and animation conditions come from KisakCOD itself.  The
// desktop server normally fills these globals while starting a level; the
// compact Vita bootstrap has no desktop G_InitGame call, so provide the same
// narrow state boundary here before the first authoritative frame.
//
// Keep the animation data inside bgs_t.  A zeroed script table is intentional:
// it makes BG_AnimScriptAnimation/BG_AnimScriptEvent fail closed until the
// Vita raw-file loader supplies playeranim.script, while still preserving the
// shared condition storage used by BG_AnimUpdatePlayerStateConditions.
#if KISAK_VITA_PROTOTYPE_BRIDGE
static bgs_t g_vitaAnimationState{};
#endif
static bool g_vitaAnimationStateReady = false;

#if KISAK_VITA_PROTOTYPE_BRIDGE
// bg_weapons.cpp uses the pointer-bearing 0x878-byte WeaponDef layout.  The
// compact Vita config only carries the fields Pmove consumes, so these
// records are populated at the documented fast-file offsets and exposed
// through the real bg_weaponDefs table.  This avoids calling the desktop
// weapon parser while retaining the source BG_GetWeaponDef call sites.
constexpr uint32_t kVitaWeaponDefBytes = 0x878;
// Slots 1..3 are the authored compact firearm set; slot 4 is the source
// frag-grenade offhand record used by PM_Weapon_CheckForOffHand.  Keep the
// record in the same 0x878-byte shape as desktop WeaponDef so the shared
// bgame code can dereference it without a Vita-only weapon state machine.
alignas(8) static uint8_t g_vitaWeaponDefStorage[5][kVitaWeaponDefBytes]{};
static bool g_vitaWeaponDefsReady = false;

constexpr uint32_t kWeaponPlayerAnimType = 296;
constexpr uint32_t kWeaponType = 300;
constexpr uint32_t kWeaponClass = 304;
constexpr uint32_t kWeaponPenetrateType = 308;
constexpr uint32_t kWeaponImpactType = 312;
constexpr uint32_t kWeaponInventoryType = 316;
constexpr uint32_t kWeaponFireType = 320;
constexpr uint32_t kWeaponOffhandClass = 324;
constexpr uint32_t kWeaponStartAmmo = 800;
constexpr uint32_t kWeaponAmmoIndex = 808;
constexpr uint32_t kWeaponClipIndex = 816;
constexpr uint32_t kWeaponMaxAmmo = 820;
constexpr uint32_t kWeaponClipSize = 824;
constexpr uint32_t kWeaponDamage = 844;
constexpr uint32_t kWeaponPlayerDamage = 848;
constexpr uint32_t kWeaponMeleeDamage = 852;
constexpr uint32_t kWeaponFireDelay = 860;
constexpr uint32_t kWeaponMeleeDelay = 864;
constexpr uint32_t kWeaponMeleeChargeDelay = 868;
constexpr uint32_t kWeaponDetonateDelay = 872;
constexpr uint32_t kWeaponFireTime = 876;
constexpr uint32_t kWeaponRechamberTime = 880;
constexpr uint32_t kWeaponHoldFireTime = 888;
constexpr uint32_t kWeaponDetonateTime = 892;
constexpr uint32_t kWeaponMeleeTime = 896;
constexpr uint32_t kWeaponMeleeChargeTime = 900;
constexpr uint32_t kWeaponReloadTime = 904;
constexpr uint32_t kWeaponReloadEmptyTime = 912;
constexpr uint32_t kWeaponReloadStartTime = 920;
constexpr uint32_t kWeaponReloadEndTime = 928;
constexpr uint32_t kWeaponDropTime = 932;
constexpr uint32_t kWeaponRaiseTime = 936;
constexpr uint32_t kWeaponQuickDropTime = 948;
constexpr uint32_t kWeaponQuickRaiseTime = 952;
constexpr uint32_t kWeaponSprintInTime = 968;
constexpr uint32_t kWeaponSprintLoopTime = 972;
constexpr uint32_t kWeaponSprintOutTime = 976;
constexpr uint32_t kWeaponFuseTime = 1004;
constexpr uint32_t kWeaponRequireLockon = 1012;
constexpr uint32_t kWeaponNoAdsWhenMagEmpty = 1016;
constexpr uint32_t kWeaponMoveSpeedScale = 1048;
constexpr uint32_t kWeaponAdsMoveSpeedScale = 1052;
constexpr uint32_t kWeaponSprintDurationScale = 1056;
constexpr uint32_t kWeaponAdsZoomFov = 1060;
constexpr uint32_t kWeaponAdsZoomInFrac = 1064;
constexpr uint32_t kWeaponAdsZoomOutFrac = 1068;
constexpr uint32_t kWeaponHipSpreadStandMin = 1104;
constexpr uint32_t kWeaponHipSpreadDuckedMin = 1108;
constexpr uint32_t kWeaponHipSpreadProneMin = 1112;
constexpr uint32_t kWeaponHipSpreadStandMax = 1116;
constexpr uint32_t kWeaponHipSpreadDuckedMax = 1120;
constexpr uint32_t kWeaponHipSpreadProneMax = 1124;
constexpr uint32_t kWeaponHipSpreadDecayRate = 1128;
constexpr uint32_t kWeaponHipSpreadFireAdd = 1132;
constexpr uint32_t kWeaponAdsTransInTime = 1156;
constexpr uint32_t kWeaponAdsTransOutTime = 1160;
constexpr uint32_t kWeaponAdsAimPitch = 1740;
constexpr uint32_t kWeaponAimDownSight = 1260;
constexpr uint32_t kWeaponRechamberWhileAds = 1264;
constexpr uint32_t kWeaponClipOnly = 1280;
constexpr uint32_t kWeaponAdsFireOnly = 1284;
constexpr uint32_t kWeaponSegmentedReload = 1328;
constexpr uint32_t kWeaponBlocksProne = 1356;
constexpr uint32_t kWeaponExplosionRadius = 1364;
constexpr uint32_t kWeaponExplosionRadiusMin = 1368;
constexpr uint32_t kWeaponExplosionInnerDamage = 1372;
constexpr uint32_t kWeaponExplosionOuterDamage = 1376;
constexpr uint32_t kWeaponHasDetonator = 1448;
constexpr uint32_t kWeaponHoldButtonToThrow = 1460;
constexpr uint32_t kWeaponFreezeMovementWhenFiring = 1464;
constexpr uint32_t kWeaponPositionReloadTransTime = 1936;
// WeaponDef::fOOPosAnimLength is derived by BG_SetupTransitionTimes.  The
// compact fallback record bypasses that desktop parser, so write the derived
// ADS rates explicitly for the shared PM_UpdateAimDownSightLerp path.
constexpr uint32_t kWeaponOOPosAnimLength = 2040;

template <typename T>
void WriteVitaWeaponField(uint32_t slot, uint32_t offset, T value) {
    if (slot > 4 || offset + sizeof(T) > kVitaWeaponDefBytes)
        return;
    std::memcpy(g_vitaWeaponDefStorage[slot] + offset, &value, sizeof(T));
}

void WriteVitaWeaponPointer(uint32_t slot, uint32_t offset, const void* value) {
    // Vita user pointers are 32-bit, matching the recovered WeaponDef layout.
    const uintptr_t pointer = reinterpret_cast<uintptr_t>(value);
    const uint32_t narrowed = static_cast<uint32_t>(pointer);
    WriteVitaWeaponField(slot, offset, narrowed);
}

void InitializeVitaWeaponRecord(uint32_t slot) {
    if (slot > 4)
        return;
    std::memset(g_vitaWeaponDefStorage[slot], 0, kVitaWeaponDefBytes);
    static const char* const names[] = {
        "none", "m4_mp", "mp5_mp", "usp_mp", "frag_grenade_mp"
    };
    static const char* const ammoNames[] = {
        "none", "556 NATO", "9mm", ".45 ACP", "frag_grenade"
    };
    static const char* const clipNames[] = {
        "none", "m4_clip", "mp5_clip", "usp_clip", "frag_grenade_clip"
    };
    WriteVitaWeaponPointer(slot, 0, names[slot]);
    WriteVitaWeaponPointer(slot, 4, names[slot]);
    WriteVitaWeaponPointer(slot, 212, names[slot]);
    WriteVitaWeaponPointer(slot, kWeaponAmmoIndex - 4, ammoNames[slot]);
    WriteVitaWeaponPointer(slot, kWeaponClipIndex - 4, clipNames[slot]);

    if (slot == 4) {
        // This is the source offhand record, not a second hand-authored
        // grenade implementation.  PM_Weapon_CheckForOffHand selects it by
        // offhandClass and then runs the normal source prepare/hold/throw
        // states (15..20), including the authored weapon animation slots.
        WriteVitaWeaponField(slot, kWeaponPlayerAnimType, int32_t{0});
        WriteVitaWeaponField(slot, kWeaponType, int32_t{WEAPTYPE_GRENADE});
        WriteVitaWeaponField(slot, kWeaponClass, int32_t{WEAPCLASS_GRENADE});
        WriteVitaWeaponField(slot, kWeaponPenetrateType, int32_t{PENETRATE_TYPE_NONE});
        WriteVitaWeaponField(slot, kWeaponImpactType, int32_t{IMPACT_TYPE_GRENADE_EXPLODE});
        WriteVitaWeaponField(slot, kWeaponInventoryType, int32_t{WEAPINVENTORY_OFFHAND});
        WriteVitaWeaponField(slot, kWeaponFireType, int32_t{WEAPON_FIRETYPE_FULLAUTO});
        WriteVitaWeaponField(slot, kWeaponOffhandClass,
                             int32_t{OFFHAND_CLASS_FRAG_GRENADE});
        WriteVitaWeaponField(slot, kWeaponStartAmmo, int32_t{2});
        WriteVitaWeaponField(slot, kWeaponAmmoIndex, int32_t{4});
        WriteVitaWeaponField(slot, kWeaponClipIndex, int32_t{4});
        WriteVitaWeaponField(slot, kWeaponMaxAmmo, int32_t{2});
        WriteVitaWeaponField(slot, kWeaponClipSize, int32_t{1});
        WriteVitaWeaponField(slot, kWeaponDamage, int32_t{100});
        WriteVitaWeaponField(slot, kWeaponPlayerDamage, int32_t{100});
        WriteVitaWeaponField(slot, kWeaponFireDelay, int32_t{100});
        WriteVitaWeaponField(slot, kWeaponDetonateDelay, int32_t{0});
        WriteVitaWeaponField(slot, kWeaponFireTime, int32_t{100});
        WriteVitaWeaponField(slot, kWeaponHoldFireTime, int32_t{300});
        WriteVitaWeaponField(slot, kWeaponDetonateTime, int32_t{100});
        WriteVitaWeaponField(slot, kWeaponFuseTime, int32_t{1800});
        WriteVitaWeaponField(slot, kWeaponDropTime, int32_t{100});
        WriteVitaWeaponField(slot, kWeaponRaiseTime, int32_t{100});
        WriteVitaWeaponField(slot, kWeaponQuickDropTime, int32_t{100});
        WriteVitaWeaponField(slot, kWeaponQuickRaiseTime, int32_t{100});
        WriteVitaWeaponField(slot, kWeaponExplosionRadius, int32_t{180});
        WriteVitaWeaponField(slot, kWeaponExplosionRadiusMin, int32_t{48});
        WriteVitaWeaponField(slot, kWeaponExplosionInnerDamage, int32_t{100});
        WriteVitaWeaponField(slot, kWeaponExplosionOuterDamage, int32_t{30});
        WriteVitaWeaponField(slot, kWeaponHasDetonator, int32_t{0});
        WriteVitaWeaponField(slot, kWeaponHoldButtonToThrow, int32_t{0});
        WriteVitaWeaponField(slot, kWeaponRequireLockon, int32_t{0});
        WriteVitaWeaponField(slot, kWeaponNoAdsWhenMagEmpty, int32_t{1});
        WriteVitaWeaponField(slot, kWeaponAimDownSight, int32_t{0});
        WriteVitaWeaponField(slot, kWeaponOOPosAnimLength, 1.0f / 300.0f);
        WriteVitaWeaponField(slot, kWeaponOOPosAnimLength + sizeof(float),
                             1.0f / 500.0f);
        return;
    }

    const int32_t weaponClass = slot == 3 ? WEAPCLASS_PISTOL
        : (slot == 2 ? WEAPCLASS_SMG : WEAPCLASS_RIFLE);
    const int32_t clipSize = slot == 3 ? 12 : 30;
    WriteVitaWeaponField(slot, kWeaponPlayerAnimType, int32_t{0});
    WriteVitaWeaponField(slot, kWeaponType, int32_t{WEAPTYPE_BULLET});
    WriteVitaWeaponField(slot, kWeaponClass, weaponClass);
    WriteVitaWeaponField(slot, kWeaponPenetrateType, int32_t{PENETRATE_TYPE_SMALL});
    WriteVitaWeaponField(slot, kWeaponImpactType, int32_t{IMPACT_TYPE_BULLET_SMALL});
    WriteVitaWeaponField(slot, kWeaponInventoryType, int32_t{WEAPINVENTORY_PRIMARY});
    WriteVitaWeaponField(slot, kWeaponOffhandClass, int32_t{OFFHAND_CLASS_NONE});
    WriteVitaWeaponField(slot, kWeaponStartAmmo, int32_t{90});
    WriteVitaWeaponField(slot, kWeaponAmmoIndex, int32_t{static_cast<int32_t>(slot)});
    WriteVitaWeaponField(slot, kWeaponClipIndex, int32_t{static_cast<int32_t>(slot)});
    WriteVitaWeaponField(slot, kWeaponMaxAmmo, int32_t{999});
    WriteVitaWeaponField(slot, kWeaponClipSize, clipSize);
    WriteVitaWeaponField(slot, kWeaponDamage, int32_t{40});
    WriteVitaWeaponField(slot, kWeaponPlayerDamage, int32_t{40});
    // PM_Weapon_CheckForMelee only needs these source scalars to enter the
    // melee init/fire/end state machine.  The authored player animation and
    // event paths remain in bg_weapons.cpp; the Vita renderer consumes the
    // resulting weapAnim values rather than inventing clip transforms.
    WriteVitaWeaponField(slot, kWeaponMeleeDamage, int32_t{40});
    WriteVitaWeaponField(slot, kWeaponFireDelay, int32_t{100});
    WriteVitaWeaponField(slot, kWeaponMeleeDelay, int32_t{0});
    WriteVitaWeaponField(slot, kWeaponMeleeChargeDelay, int32_t{0});
    WriteVitaWeaponField(slot, kWeaponDetonateDelay, int32_t{0});
    WriteVitaWeaponField(slot, kWeaponFireTime, int32_t{100});
    WriteVitaWeaponField(slot, kWeaponRechamberTime, int32_t{0});
    WriteVitaWeaponField(slot, kWeaponHoldFireTime, int32_t{0});
    WriteVitaWeaponField(slot, kWeaponDetonateTime, int32_t{0});
    WriteVitaWeaponField(slot, kWeaponMeleeTime, int32_t{350});
    WriteVitaWeaponField(slot, kWeaponMeleeChargeTime, int32_t{0});
    WriteVitaWeaponField(slot, kWeaponReloadTime, int32_t{1400});
    WriteVitaWeaponField(slot, kWeaponReloadEmptyTime, int32_t{1800});
    WriteVitaWeaponField(slot, kWeaponReloadStartTime, int32_t{350});
    WriteVitaWeaponField(slot, kWeaponReloadEndTime, int32_t{350});
    WriteVitaWeaponField(slot, kWeaponDropTime, int32_t{250});
    WriteVitaWeaponField(slot, kWeaponRaiseTime, int32_t{250});
    WriteVitaWeaponField(slot, kWeaponQuickDropTime, int32_t{100});
    WriteVitaWeaponField(slot, kWeaponQuickRaiseTime, int32_t{100});
    WriteVitaWeaponField(slot, kWeaponSprintInTime, int32_t{250});
    WriteVitaWeaponField(slot, kWeaponSprintLoopTime, int32_t{100});
    WriteVitaWeaponField(slot, kWeaponSprintOutTime, int32_t{250});
    WriteVitaWeaponField(slot, kWeaponFuseTime, int32_t{0});
    WriteVitaWeaponField(slot, kWeaponRequireLockon, int32_t{0});
    WriteVitaWeaponField(slot, kWeaponNoAdsWhenMagEmpty, int32_t{0});
    WriteVitaWeaponField(slot, kWeaponSprintDurationScale, 1.0f);
    WriteVitaWeaponField(slot, kWeaponAdsZoomFov, 40.0f);
    WriteVitaWeaponField(slot, kWeaponAdsZoomInFrac, 0.0f);
    WriteVitaWeaponField(slot, kWeaponAdsZoomOutFrac, 0.0f);
    WriteVitaWeaponField(slot, kWeaponHipSpreadStandMin, 2.0f);
    WriteVitaWeaponField(slot, kWeaponHipSpreadDuckedMin, 2.0f);
    WriteVitaWeaponField(slot, kWeaponHipSpreadProneMin, 1.0f);
    WriteVitaWeaponField(slot, kWeaponHipSpreadStandMax, 8.0f);
    WriteVitaWeaponField(slot, kWeaponHipSpreadDuckedMax, 8.0f);
    WriteVitaWeaponField(slot, kWeaponHipSpreadProneMax, 6.0f);
    WriteVitaWeaponField(slot, kWeaponHipSpreadDecayRate, 5.0f);
    WriteVitaWeaponField(slot, kWeaponHipSpreadFireAdd, 1.0f);
    WriteVitaWeaponField(slot, kWeaponAdsAimPitch, 0.0f);
    WriteVitaWeaponField(slot, kWeaponOOPosAnimLength, 1.0f / 300.0f);
    WriteVitaWeaponField(slot, kWeaponOOPosAnimLength + sizeof(float),
                         1.0f / 500.0f);
    WriteVitaWeaponField(slot, kWeaponAimDownSight, int32_t{1});
    WriteVitaWeaponField(slot, kWeaponRechamberWhileAds, int32_t{1});
    WriteVitaWeaponField(slot, kWeaponClipOnly, int32_t{0});
    WriteVitaWeaponField(slot, kWeaponAdsFireOnly, int32_t{0});
    WriteVitaWeaponField(slot, kWeaponSegmentedReload, int32_t{0});
    WriteVitaWeaponField(slot, kWeaponBlocksProne, int32_t{0});
    WriteVitaWeaponField(slot, kWeaponHasDetonator, int32_t{0});
    WriteVitaWeaponField(slot, kWeaponHoldButtonToThrow, int32_t{0});
    WriteVitaWeaponField(slot, kWeaponPositionReloadTransTime, int32_t{0});
}

void EnsureVitaWeaponDefs() {
    if (g_vitaWeaponDefsReady)
        return;

    // A fully initialized desktop table wins for the authored firearms.  If
    // the compact bootstrap is the only provider, install the same bounded
    // records used by the source BG callers.  In either case, ensure the
    // frag offhand record exists at index 4 so BUTTON_FRAG can take the real
    // source offhand state path.
    const bool primaryTableReady = bg_lastParsedWeaponIndex >= 3
        && bg_weaponDefs[1] && bg_weaponDefs[2] && bg_weaponDefs[3];
    if (!primaryTableReady) {
        for (uint32_t slot = 0; slot <= 3; ++slot) {
            InitializeVitaWeaponRecord(slot);
            bg_weaponDefs[slot] = reinterpret_cast<WeaponDef*>(
                g_vitaWeaponDefStorage[slot]);
        }
        bg_lastParsedWeaponIndex = 3;
    }
    if (bg_lastParsedWeaponIndex < 4 || !bg_weaponDefs[4]) {
        InitializeVitaWeaponRecord(4);
        bg_weaponDefs[4] = reinterpret_cast<WeaponDef*>(
            g_vitaWeaponDefStorage[4]);
        bg_lastParsedWeaponIndex = std::max<uint32_t>(bg_lastParsedWeaponIndex, 4);
    }
    g_vitaWeaponDefsReady = true;
}

void UpdateVitaWeaponConfigRecord(const VitaPmoveWeaponConfig& config) {
    EnsureVitaWeaponDefs();
    if (!g_vitaWeaponDefsReady || config.weapon < 1 || config.weapon > 3)
        return;
    const uint32_t slot = config.weapon;
    WriteVitaWeaponField(slot, kWeaponFireType,
                         static_cast<int32_t>(config.fireType));
    // Keep the pointer-bearing WeaponDef scratch record source-complete for
    // BG callers that inspect inventory, damage, and weapon timing.  Pmove
    // currently consumes only the movement fields below, but populating the
    // rest here means Vita follows the exact VITP/KisakCOD record instead of
    // silently retaining bridge defaults after a weapon swap.
    WriteVitaWeaponField(slot, kWeaponStartAmmo, config.startAmmo);
    WriteVitaWeaponField(slot, kWeaponMaxAmmo, config.maxAmmo);
    WriteVitaWeaponField(slot, kWeaponClipSize, config.clipSize);
    WriteVitaWeaponField(slot, kWeaponDamage, config.damage);
    WriteVitaWeaponField(slot, kWeaponPlayerDamage, config.playerDamage);
    WriteVitaWeaponField(slot, kWeaponFireDelay, config.fireDelay);
    WriteVitaWeaponField(slot, kWeaponFireTime, config.fireTime);
    WriteVitaWeaponField(slot, kWeaponRechamberTime, config.rechamberTime);
    WriteVitaWeaponField(slot, kWeaponReloadTime, config.reloadTime);
    WriteVitaWeaponField(slot, kWeaponReloadEmptyTime, config.reloadEmptyTime);
    WriteVitaWeaponField(slot, kWeaponMoveSpeedScale, config.moveSpeedScale);
    WriteVitaWeaponField(slot, kWeaponAdsMoveSpeedScale, config.adsMoveSpeedScale);
    WriteVitaWeaponField(slot, kWeaponSprintDurationScale,
                         config.sprintDurationScale);
    WriteVitaWeaponField(slot, kWeaponAdsTransInTime, config.adsTransInTime);
    WriteVitaWeaponField(slot, kWeaponAdsTransOutTime, config.adsTransOutTime);
    WriteVitaWeaponField(slot, kWeaponOOPosAnimLength,
                         1.0f / static_cast<float>(std::max(1, config.adsTransInTime)));
    WriteVitaWeaponField(slot, kWeaponOOPosAnimLength + sizeof(float),
                         1.0f / static_cast<float>(std::max(1, config.adsTransOutTime)));
    WriteVitaWeaponField(slot, kWeaponFreezeMovementWhenFiring,
                         config.freezeMovementWhenFiring);
}
#else
static bool g_vitaWeaponDefsReady = false;

void EnsureVitaWeaponDefs() {
    // This narrow Pmove TU intentionally sees only bg_weapon_def_vita.h.
    // Identity and schema validation belongs to VitaAssetRegistry before the
    // source BG table is populated; never widen this seam or infer names from
    // the 0x878-byte storage here.
    g_vitaWeaponDefsReady = bg_lastParsedWeaponIndex >= 4;
    for (uint32_t index = 0; g_vitaWeaponDefsReady && index <= 4; ++index)
        g_vitaWeaponDefsReady &= bg_weaponDefs[index] != nullptr;
}

void UpdateVitaWeaponConfigRecord(const VitaPmoveWeaponConfig&) {
    // The source-owned build accepts WeaponDef changes only by mounting a
    // new typed VITP generation. Runtime writes here would recreate the
    // prototype ownership boundary that the release link is meant to remove.
    EnsureVitaWeaponDefs();
}
#endif

void EnsureVitaMovementDvars();

void EnsureVitaSharedPmoveState() {
    EnsureVitaMovementDvars();
#if KISAK_VITA_PROTOTYPE_BRIDGE
    if (!bgs) {
        bgs = &g_vitaAnimationState;
        for (uint32_t client = 0; client < MAX_CLIENTS; ++client)
            bgs->clientinfo[client].clientNum = static_cast<int32_t>(client);
    }
    if (!globalScriptData)
        globalScriptData = &bgs->animScriptData;
    if (!g_vitaAnimationStateReady) {
        g_vitaAnimationStateReady = true;
        kisak::vita::VitaScriptServicesTrace(
            "vita_pmove_animation_state", "initialized=1 source_conditions=1 script_fail_closed=1");
    }
#else
    g_vitaAnimationStateReady = bgs && globalScriptData;
#endif
    EnsureVitaWeaponDefs();
}

// The shared movement code expects registered dvars.  These values are the
// COD4 defaults needed by the portable movement path; the later Vita dvar
// registry will replace the pointers without changing bgame call sites.
dvar_t g_floatOne{};
dvar_t g_floatZero{};
dvar_t g_floatNinety{};
dvar_t g_floatPitchUp{};
dvar_t g_floatPitchDown{};
dvar_t g_floatFriction{};
dvar_t g_floatStopSpeed{};
dvar_t g_floatSprintTime{};
dvar_t g_floatJumpHeight{};
dvar_t g_floatJumpStepSize{};
dvar_t g_floatJumpLadderPushVel{};
dvar_t g_floatJumpSpreadAdd{};
dvar_t g_floatLadderYawCap{};
dvar_t g_floatProneYawCap{};
dvar_t g_floatBobMax{};
dvar_t g_floatInertiaMax{};
dvar_t g_floatInertiaAngle{};
dvar_t g_floatMeleeRange{};
dvar_t g_floatMeleeWidth{};
dvar_t g_floatMeleeHeight{};
dvar_t g_floatMeleeChargeFriction{};
dvar_t g_floatMoveThreshold{};
dvar_t g_floatFootstepsThreshold{};
dvar_t g_floatDmgtimerMax{};
dvar_t g_floatDmgtimerMinScale{};
dvar_t g_intDmgtimerFlinchTime{};
dvar_t g_intDmgtimerStumbleTime{};
dvar_t g_intFoliageFastInterval{};
dvar_t g_intFoliageSlowInterval{};
dvar_t g_intFoliageResetInterval{};
dvar_t g_floatFoliageMinSpeed{};
dvar_t g_floatFoliageMaxSpeed{};
dvar_t g_floatFallDamageMinHeight{};
dvar_t g_floatFallDamageMaxHeight{};
dvar_t g_floatAimSpreadMoveSpeedThreshold{};
dvar_t g_floatBreathHoldTime{};
dvar_t g_floatBreathGaspTime{};
dvar_t g_floatBreathFireDelay{};
dvar_t g_floatBreathGaspScale{};
dvar_t g_floatBreathHoldLerp{};
dvar_t g_floatBreathGaspLerp{};
dvar_t g_floatBreathSoundLerp{};
dvar_t g_floatBreathSoundDelay{};
dvar_t g_floatMoveFactorOnTorso{};
dvar_t g_floatDmgtimerTimePerPoint{};
dvar_t g_floatViewKickScale{};
dvar_t g_floatViewKickMax{};
dvar_t g_floatViewKickMin{};
dvar_t g_floatViewKickRandom{};
dvar_t g_floatBurstFireCooldown{};
dvar_t g_floatPerkSpreadMultiplier{};
dvar_t g_floatPerkExtraBreath{};
dvar_t g_floatPerkReloadMultiplier{};
dvar_t g_floatPerkRateMultiplier{};
dvar_t g_floatPerkSprintMultiplier{};
dvar_t g_intAdsExitDelay{};
dvar_t g_boolFalse{};
dvar_t g_boolTrue{};
dvar_t g_intZero{};
dvar_t g_floatLegYawTolerance{};
dvar_t g_floatSwingSpeed{};
dvar_t g_vecBobAmplitudeSprinting{};
dvar_t g_vecBobAmplitudeStanding{};
dvar_t g_vecBobAmplitudeDucked{};
dvar_t g_vecBobAmplitudeProne{};
dvar_t g_jumpSlowdownEnable{};
dvar_t g_floatBackSpeedScale{};
dvar_t g_floatStrafeSpeedScale{};
dvar_t g_floatStrafeAnimCosAngle{};
dvar_t g_floatSprintSpeedScale{};
dvar_t g_floatSprintStrafeSpeedScale{};
dvar_t g_floatSprintRechargePause{};
dvar_t g_floatSprintCameraBob{};
dvar_t g_intSprintForwardMinimum{};
dvar_t g_intOne{};
dvar_t g_intHundred{};
dvar_t g_intTwoHundred{};
dvar_t g_enabledFalse{};

struct VitaDvarInit {
    VitaDvarInit() {
        g_floatOne.current.value = 1.0f;
        g_floatZero.current.value = 0.0f;
        g_floatNinety.current.value = 90.0f;
        g_floatPitchUp.current.value = 85.0f;
        g_floatPitchDown.current.value = 85.0f;
        g_floatFriction.current.value = 5.5f;
        g_floatStopSpeed.current.value = 100.0f;
        g_floatSprintTime.current.value = 4.0f;
        g_floatJumpHeight.current.value = 39.0f;
        g_floatJumpStepSize.current.value = 18.0f;
        g_floatJumpLadderPushVel.current.value = 128.0f;
        g_floatJumpSpreadAdd.current.value = 64.0f;
        g_floatLadderYawCap.current.value = 100.0f;
        g_floatProneYawCap.current.value = 85.0f;
        g_floatBobMax.current.value = 8.0f;
        g_floatInertiaMax.current.value = 50.0f;
        g_floatInertiaAngle.current.value = 0.0f;
        g_floatMeleeRange.current.value = 64.0f;
        g_floatMeleeWidth.current.value = 10.0f;
        g_floatMeleeHeight.current.value = 10.0f;
        g_floatMeleeChargeFriction.current.value = 1200.0f;
        g_floatMoveThreshold.current.value = 10.0f;
        g_floatFootstepsThreshold.current.value = 0.0f;
        g_floatDmgtimerMax.current.value = 1000.0f;
        g_floatDmgtimerMinScale.current.value = 0.0f;
        g_intDmgtimerFlinchTime.current.integer = 500;
        g_intDmgtimerStumbleTime.current.integer = 500;
        g_intFoliageFastInterval.current.integer = 500;
        g_intFoliageSlowInterval.current.integer = 1500;
        g_intFoliageResetInterval.current.integer = 500;
        g_floatFoliageMinSpeed.current.value = 40.0f;
        g_floatFoliageMaxSpeed.current.value = 180.0f;
        g_floatFallDamageMinHeight.current.value = 128.0f;
        g_floatFallDamageMaxHeight.current.value = 300.0f;
        g_floatAimSpreadMoveSpeedThreshold.current.value = 11.0f;
        g_floatBreathHoldTime.current.value = 4.5f;
        g_floatBreathGaspTime.current.value = 1.0f;
        g_floatBreathFireDelay.current.value = 0.0f;
        g_floatBreathGaspScale.current.value = 4.5f;
        g_floatBreathHoldLerp.current.value = 1.0f;
        g_floatBreathGaspLerp.current.value = 6.0f;
        g_floatBreathSoundLerp.current.value = 2.0f;
        g_floatBreathSoundDelay.current.value = 1.0f;
        g_floatMoveFactorOnTorso.current.value = 0.0f;
        g_floatDmgtimerTimePerPoint.current.value = 100.0f;
        g_floatViewKickScale.current.value = 0.2f;
        g_floatViewKickMax.current.value = 90.0f;
        g_floatViewKickMin.current.value = 5.0f;
        g_floatViewKickRandom.current.value = 0.4f;
        g_floatBurstFireCooldown.current.value = 0.2f;
        g_floatPerkSpreadMultiplier.current.value = 0.65f;
        g_floatPerkExtraBreath.current.value = 5.0f;
        g_floatPerkReloadMultiplier.current.value = 0.5f;
        g_floatPerkRateMultiplier.current.value = 0.75f;
        g_floatPerkSprintMultiplier.current.value = 2.0f;
        g_intAdsExitDelay.current.integer = 0;
        g_boolFalse.current.enabled = false;
        g_boolFalse.current.integer = 0;
        g_boolTrue.current.enabled = true;
        g_boolTrue.current.integer = 1;
        g_intZero.current.integer = 0;
        g_floatLegYawTolerance.current.value = 20.0f;
        g_floatSwingSpeed.current.value = 0.2f;
        // The multiplayer source registers these as vec2 dvars.  The
        // movement/viewmodel code reads the second component for vertical
        // bob, so initialize both components to the authored COD4 values.
        g_vecBobAmplitudeSprinting.current.vector[0] = 0.02f;
        g_vecBobAmplitudeSprinting.current.vector[1] = 0.014f;
        g_vecBobAmplitudeStanding.current.vector[0] = 0.007f;
        g_vecBobAmplitudeStanding.current.vector[1] = 0.007f;
        g_vecBobAmplitudeDucked.current.vector[0] = 0.0075f;
        g_vecBobAmplitudeDucked.current.vector[1] = 0.0075f;
        g_vecBobAmplitudeProne.current.vector[0] = 0.02f;
        g_vecBobAmplitudeProne.current.vector[1] = 0.005f;
        g_jumpSlowdownEnable.current.enabled = true;
        g_jumpSlowdownEnable.current.integer = 1;
        g_floatBackSpeedScale.current.value = 0.7f;
        g_floatStrafeSpeedScale.current.value = 0.8f;
        g_floatStrafeAnimCosAngle.current.value = 0.5f;
        g_floatSprintSpeedScale.current.value = 1.5f;
        g_floatSprintStrafeSpeedScale.current.value = 0.667f;
        g_floatSprintRechargePause.current.value = 0.0f;
        g_floatSprintCameraBob.current.value = 0.5f;
        g_intSprintForwardMinimum.current.integer = 105;
        g_intOne.current.integer = 1;
        g_intHundred.current.integer = 100;
        g_intTwoHundred.current.integer = 200;
        g_enabledFalse.current.enabled = false;
    }
} g_dvarInit;

// The desktop BG archive owns these pointer variables, but the Vita slice
// does not run the desktop dvar registration pass. Point the source-owned
// variables at COD4-default records before the first Pmove call so sprint,
// friction, stance transitions, and movement scaling all use the source
// values. This also covers the narrow probe build where the weak Vita
// definitions are selected.
void EnsureVitaMovementDvars() {
    static bool initialized = false;
    if (initialized)
        return;
    initialized = true;
    friction = &g_floatFriction;
    stopspeed = &g_floatStopSpeed;
    player_backSpeedScale = &g_floatBackSpeedScale;
    player_strafeSpeedScale = &g_floatStrafeSpeedScale;
    player_strafeAnimCosAngle = &g_floatStrafeAnimCosAngle;
    player_sprintForwardMinimum = &g_intSprintForwardMinimum;
    player_sprintSpeedScale = &g_floatSprintSpeedScale;
    player_sprintTime = &g_floatSprintTime;
    player_sprintMinTime = &g_floatOne;
    player_sprintRechargePause = &g_floatSprintRechargePause;
    player_sprintStrafeSpeedScale = &g_floatSprintStrafeSpeedScale;
    player_sprintCameraBob = &g_floatSprintCameraBob;
    jump_height = &g_floatJumpHeight;
    jump_stepSize = &g_floatJumpStepSize;
    jump_slowdownEnable = &g_jumpSlowdownEnable;
    jump_ladderPushVel = &g_floatJumpLadderPushVel;
    jump_spreadAdd = &g_floatJumpSpreadAdd;
    bg_ladder_yawcap = &g_floatLadderYawCap;
    bg_prone_yawcap = &g_floatProneYawCap;
    bg_bobMax = &g_floatBobMax;
    inertiaMax = &g_floatInertiaMax;
    inertiaAngle = &g_floatInertiaAngle;
    player_meleeRange = &g_floatMeleeRange;
    player_meleeWidth = &g_floatMeleeWidth;
    player_meleeHeight = &g_floatMeleeHeight;
    player_meleeChargeFriction = &g_floatMeleeChargeFriction;
    player_moveThreshhold = &g_floatMoveThreshold;
    player_footstepsThreshhold = &g_floatFootstepsThreshold;
    player_dmgtimer_maxTime = &g_floatDmgtimerMax;
    player_dmgtimer_minScale = &g_floatDmgtimerMinScale;
    player_dmgtimer_flinchTime = &g_intDmgtimerFlinchTime;
    player_dmgtimer_stumbleTime = &g_intDmgtimerStumbleTime;
    bg_foliagesnd_fastinterval = &g_intFoliageFastInterval;
    bg_foliagesnd_slowinterval = &g_intFoliageSlowInterval;
    bg_foliagesnd_resetinterval = &g_intFoliageResetInterval;
    bg_foliagesnd_minspeed = &g_floatFoliageMinSpeed;
    bg_foliagesnd_maxspeed = &g_floatFoliageMaxSpeed;
    bg_fallDamageMinHeight = &g_floatFallDamageMinHeight;
    bg_fallDamageMaxHeight = &g_floatFallDamageMaxHeight;
    bg_aimSpreadMoveSpeedThreshold = &g_floatAimSpreadMoveSpeedThreshold;
    player_breath_hold_time = &g_floatBreathHoldTime;
    player_breath_gasp_time = &g_floatBreathGaspTime;
    player_breath_fire_delay = &g_floatBreathFireDelay;
    player_breath_gasp_scale = &g_floatBreathGaspScale;
    player_breath_hold_lerp = &g_floatBreathHoldLerp;
    player_breath_gasp_lerp = &g_floatBreathGaspLerp;
    player_breath_snd_lerp = &g_floatBreathSoundLerp;
    player_breath_snd_delay = &g_floatBreathSoundDelay;
    player_move_factor_on_torso = &g_floatMoveFactorOnTorso;
    player_dmgtimer_timePerPoint = &g_floatDmgtimerTimePerPoint;
    player_scopeExitOnDamage = &g_boolFalse;
    player_adsExitDelay = &g_intAdsExitDelay;
    player_sustainAmmo = &g_boolFalse;
    player_debugHealth = &g_boolFalse;
    player_burstFireCooldown = &g_floatBurstFireCooldown;
    bg_viewKickScale = &g_floatViewKickScale;
    bg_viewKickMax = &g_floatViewKickMax;
    bg_viewKickMin = &g_floatViewKickMin;
    bg_viewKickRandom = &g_floatViewKickRandom;
    perk_weapSpreadMultiplier = &g_floatPerkSpreadMultiplier;
    perk_extraBreath = &g_floatPerkExtraBreath;
    perk_weapReloadMultiplier = &g_floatPerkReloadMultiplier;
    perk_weapRateMultiplier = &g_floatPerkRateMultiplier;
    perk_sprintMultiplier = &g_floatPerkSprintMultiplier;
    player_view_pitch_up = &g_floatPitchUp;
    player_view_pitch_down = &g_floatPitchDown;
    player_spectateSpeedScale = &g_floatOne;
    player_turnAnims = &g_enabledFalse;
    inertiaDebug = &g_enabledFalse;
    xanim_debug = &g_enabledFalse;
#if defined(KISAK_MP)
    animscript_debug = &g_enabledFalse;
    anim_debugSpeeds = &g_enabledFalse;
#endif
    bg_legYawTolerance = &g_floatLegYawTolerance;
    bg_swingSpeed = &g_floatSwingSpeed;
    bg_bobAmplitudeSprinting = &g_vecBobAmplitudeSprinting;
    bg_bobAmplitudeStanding = &g_vecBobAmplitudeStanding;
    bg_bobAmplitudeDucked = &g_vecBobAmplitudeDucked;
    bg_bobAmplitudeProne = &g_vecBobAmplitudeProne;
    kisak::vita::VitaScriptServicesTrace(
        "vita_pmove_dvars",
        "friction=5.500 stopspeed=100 sprintMinForward=105 sprintScale=1.500 sprintTime=4.000");
}

void FillTrace(trace_t* result, const float* start, const float* mins,
               const float* maxs, const float* end, int contentMask) {
    if (!result)
        return;
    *result = trace_t{};
    result->fraction = 1.0f;
    result->hitType = TRACE_HITTYPE_NONE;
    result->hitId = ENTITYNUM_WORLD;
    if (!g_collision || !start || !mins || !maxs || !end)
        return;
    kisak::vita::ShipmentCollisionTrace collisionTrace{};
    if (!g_collision->TraceCapsule(start, mins, maxs, end,
                                   static_cast<uint32_t>(contentMask), &collisionTrace))
        return;
    static unsigned collisionTraceCount = 0;
    if (collisionTraceCount++ < 256) {
        char detail[256];
        std::snprintf(detail, sizeof(detail),
                      "fraction=%.3f start=%u all=%u contents=%u normal=%.2f,%.2f,%.2f startPos=%.1f,%.1f,%.1f endPos=%.1f,%.1f,%.1f walkable=%u",
                      collisionTrace.fraction,
                      static_cast<unsigned>(collisionTrace.startSolid),
                      static_cast<unsigned>(collisionTrace.allSolid),
                      static_cast<unsigned>(collisionTrace.contents),
                      collisionTrace.normal[0], collisionTrace.normal[1],
                      collisionTrace.normal[2], start[0], start[1], start[2],
                      end[0], end[1], end[2],
                      static_cast<unsigned>(collisionTrace.normal[2] >= 0.7f));
        kisak::vita::VitaScriptServicesTrace("trace_result", detail);
    }
    result->fraction = collisionTrace.fraction;
    result->normal[0] = collisionTrace.normal[0];
    result->normal[1] = collisionTrace.normal[1];
    result->normal[2] = collisionTrace.normal[2];
    result->contents = static_cast<int>(collisionTrace.contents);
    result->surfaceFlags = 0;
    result->walkable = result->normal[2] >= 0.7f;
    result->startsolid = collisionTrace.startSolid;
    result->allsolid = collisionTrace.allSolid;
    // The shared Trace_GetEntityHitId treats TRACE_HITTYPE_NONE as a miss,
    // even when the collision service reports a solid brush.  Mark world
    // geometry as a dynamic brush so PM_GroundTrace can retain ENTITYNUM_WORLD
    // and slide movement can register the contact.
    result->hitType = TRACE_HITTYPE_DYNENT_BRUSH;
    result->hitId = ENTITYNUM_WORLD;
}

} // namespace

// Dvar symbols consumed by bg_pmove.  They remain exported as pointers so the
// same compiled movement source can later be wired to the real Vita dvar
// registry.
#define VITA_DVAR_FLOAT(name, storage) extern __attribute__((weak)) const dvar_t* name = &storage
#define VITA_DVAR_INT(name, storage) extern __attribute__((weak)) const dvar_t* name = &storage
VITA_DVAR_FLOAT(bg_fallDamageMaxHeight, g_floatNinety);
VITA_DVAR_FLOAT(bg_fallDamageMinHeight, g_floatNinety);
VITA_DVAR_FLOAT(bg_aimSpreadMoveSpeedThreshold, g_floatAimSpreadMoveSpeedThreshold);
VITA_DVAR_INT(bg_foliagesnd_fastinterval, g_intTwoHundred);
VITA_DVAR_FLOAT(bg_foliagesnd_maxspeed, g_floatNinety);
VITA_DVAR_FLOAT(bg_foliagesnd_minspeed, g_floatZero);
VITA_DVAR_INT(bg_foliagesnd_resetinterval, g_intTwoHundred);
VITA_DVAR_INT(bg_foliagesnd_slowinterval, g_intHundred);
VITA_DVAR_FLOAT(bg_ladder_yawcap, g_floatNinety);
VITA_DVAR_FLOAT(bg_prone_yawcap, g_floatNinety);
VITA_DVAR_FLOAT(bg_bobMax, g_floatBobMax);
VITA_DVAR_FLOAT(bg_legYawTolerance, g_floatLegYawTolerance);
VITA_DVAR_FLOAT(bg_swingSpeed, g_floatSwingSpeed);
VITA_DVAR_FLOAT(bg_bobAmplitudeSprinting, g_vecBobAmplitudeSprinting);
VITA_DVAR_FLOAT(bg_bobAmplitudeStanding, g_vecBobAmplitudeStanding);
VITA_DVAR_FLOAT(bg_bobAmplitudeDucked, g_vecBobAmplitudeDucked);
VITA_DVAR_FLOAT(bg_bobAmplitudeProne, g_vecBobAmplitudeProne);
VITA_DVAR_FLOAT(friction, g_floatFriction);
VITA_DVAR_FLOAT(bg_viewKickScale, g_floatViewKickScale);
VITA_DVAR_FLOAT(bg_viewKickMax, g_floatViewKickMax);
VITA_DVAR_FLOAT(bg_viewKickMin, g_floatViewKickMin);
VITA_DVAR_FLOAT(bg_viewKickRandom, g_floatViewKickRandom);
VITA_DVAR_FLOAT(perk_weapSpreadMultiplier, g_floatPerkSpreadMultiplier);
VITA_DVAR_FLOAT(perk_extraBreath, g_floatPerkExtraBreath);
VITA_DVAR_FLOAT(perk_weapReloadMultiplier, g_floatPerkReloadMultiplier);
VITA_DVAR_FLOAT(perk_weapRateMultiplier, g_floatPerkRateMultiplier);
VITA_DVAR_FLOAT(perk_sprintMultiplier, g_floatPerkSprintMultiplier);
VITA_DVAR_FLOAT(inertiaAngle, g_floatOne);
VITA_DVAR_FLOAT(inertiaDebug, g_floatZero);
VITA_DVAR_FLOAT(inertiaMax, g_floatNinety);
VITA_DVAR_FLOAT(player_backSpeedScale, g_floatOne);
VITA_DVAR_FLOAT(player_debugHealth, g_enabledFalse);
VITA_DVAR_FLOAT(player_breath_fire_delay, g_floatBreathFireDelay);
VITA_DVAR_FLOAT(player_breath_gasp_lerp, g_floatBreathGaspLerp);
VITA_DVAR_FLOAT(player_breath_gasp_scale, g_floatBreathGaspScale);
VITA_DVAR_FLOAT(player_breath_gasp_time, g_floatBreathGaspTime);
VITA_DVAR_FLOAT(player_breath_hold_lerp, g_floatBreathHoldLerp);
VITA_DVAR_FLOAT(player_breath_hold_time, g_floatBreathHoldTime);
VITA_DVAR_FLOAT(player_breath_snd_delay, g_floatBreathSoundDelay);
VITA_DVAR_FLOAT(player_breath_snd_lerp, g_floatBreathSoundLerp);
VITA_DVAR_FLOAT(player_move_factor_on_torso, g_floatMoveFactorOnTorso);
VITA_DVAR_INT(player_dmgtimer_flinchTime, g_intHundred);
VITA_DVAR_FLOAT(player_dmgtimer_maxTime, g_floatNinety);
VITA_DVAR_FLOAT(player_dmgtimer_minScale, g_floatOne);
VITA_DVAR_FLOAT(player_dmgtimer_timePerPoint, g_floatDmgtimerTimePerPoint);
VITA_DVAR_INT(player_dmgtimer_stumbleTime, g_intHundred);
VITA_DVAR_FLOAT(player_footstepsThreshhold, g_floatZero);
VITA_DVAR_FLOAT(player_meleeChargeFriction, g_floatNinety);
VITA_DVAR_FLOAT(player_meleeHeight, g_floatMeleeHeight);
VITA_DVAR_FLOAT(player_meleeRange, g_floatMeleeRange);
VITA_DVAR_FLOAT(player_meleeWidth, g_floatMeleeWidth);
VITA_DVAR_FLOAT(player_moveThreshhold, g_floatZero);
VITA_DVAR_INT(player_adsExitDelay, g_intZero);
VITA_DVAR_FLOAT(player_burstFireCooldown, g_floatBurstFireCooldown);
VITA_DVAR_FLOAT(player_scopeExitOnDamage, g_floatZero);
VITA_DVAR_FLOAT(player_sustainAmmo, g_floatZero);
VITA_DVAR_FLOAT(player_spectateSpeedScale, g_floatOne);
VITA_DVAR_FLOAT(player_sprintCameraBob, g_floatOne);
VITA_DVAR_INT(player_sprintForwardMinimum, g_intOne);
VITA_DVAR_FLOAT(player_sprintMinTime, g_floatOne);
VITA_DVAR_FLOAT(player_sprintRechargePause, g_floatOne);
VITA_DVAR_FLOAT(player_sprintSpeedScale, g_floatOne);
VITA_DVAR_FLOAT(player_sprintStrafeSpeedScale, g_floatOne);
VITA_DVAR_FLOAT(player_sprintTime, g_floatSprintTime);
VITA_DVAR_FLOAT(player_strafeAnimCosAngle, g_floatOne);
VITA_DVAR_FLOAT(player_strafeSpeedScale, g_floatOne);
VITA_DVAR_FLOAT(player_turnAnims, g_floatZero);
VITA_DVAR_FLOAT(player_view_pitch_down, g_floatPitchDown);
VITA_DVAR_FLOAT(player_view_pitch_up, g_floatPitchUp);
VITA_DVAR_FLOAT(stopspeed, g_floatStopSpeed);
VITA_DVAR_FLOAT(xanim_debug, g_floatZero);
#if defined(KISAK_MP)
VITA_DVAR_FLOAT(animscript_debug, g_enabledFalse);
VITA_DVAR_FLOAT(anim_debugSpeeds, g_enabledFalse);
#endif
#undef VITA_DVAR_FLOAT
#undef VITA_DVAR_INT

// Pmove only needs the shared animation-state pointers and weapon table as a
// read-only service hook.  Weak definitions keep the narrow engine/probe
// targets linkable; the full BG archive owns the same symbols in the Vita
// bootstrap and wins at link time.
__attribute__((weak)) bgs_t* bgs = nullptr;
__attribute__((weak)) animScriptData_t* globalScriptData = nullptr;
__attribute__((weak)) WeaponDef* bg_weaponDefs[128] = {};
__attribute__((weak)) uint32_t bg_lastParsedWeaponIndex = 0;

void __cdecl track_static_alloc_internal(void*, int, const char*, int) {}

static dvar_t g_registeredDvar{};
__attribute__((weak)) const dvar_t* __cdecl Dvar_RegisterBool(const char*, bool value,
                                        uint16_t, const char*) {
    g_registeredDvar.current.enabled = value;
    g_registeredDvar.current.integer = value ? 1 : 0;
    return &g_registeredDvar;
}
__attribute__((weak)) const dvar_t* __cdecl Dvar_RegisterFloat(const char*, float value,
                                         DvarLimits, uint16_t, const char*) {
    g_registeredDvar.current.value = value;
    return &g_registeredDvar;
}

extern "C" void VitaPmove_SetCollision(const kisak::vita::ShipmentCollision* collision) {
    g_collision = collision;
}

extern "C" void VitaPmove_SetWeaponConfig(const VitaPmoveWeaponConfig* config) {
    if (config && config->weapon >= 1 && config->weapon <= 3) {
        g_weaponConfig = *config;
        g_weaponConfig.moveSpeedScale = std::clamp(g_weaponConfig.moveSpeedScale,
                                                    0.1f, 2.0f);
        g_weaponConfig.adsMoveSpeedScale = std::clamp(g_weaponConfig.adsMoveSpeedScale,
                                                       0.1f, 2.0f);
        g_weaponConfig.sprintDurationScale = std::clamp(
            g_weaponConfig.sprintDurationScale, 0.1f, 4.0f);
        g_weaponConfig.adsTransInTime = std::clamp(g_weaponConfig.adsTransInTime,
                                                    1, 2000);
        g_weaponConfig.adsTransOutTime = std::clamp(g_weaponConfig.adsTransOutTime,
                                                     1, 2000);
        g_weaponConfig.freezeMovementWhenFiring =
            g_weaponConfig.freezeMovementWhenFiring != 0 ? 1 : 0;
        UpdateVitaWeaponConfigRecord(g_weaponConfig);
    }
}

extern "C" bool VitaPmove_Run(playerState_s* state,
                               const usercmd_s* command,
                               const usercmd_s* oldCommand) {
    if (!state || !command)
        return false;
    static unsigned pmoveTraceCount = 0;
    const bool trace = pmoveTraceCount < 24;
    if (trace) {
        char detail[96];
        std::snprintf(detail, sizeof(detail), "call=%u cmd=%d state=%d",
                      pmoveTraceCount, command->serverTime, state->commandTime);
        kisak::vita::VitaScriptServicesTrace("pmove_enter", detail);
    }
    ++pmoveTraceCount;
    pmove_t move{};
    move.ps = state;
    move.cmd = *command;
    if (oldCommand)
        move.oldcmd = *oldCommand;
    move.tracemask = 0x2810011;
    move.mins[0] = playerMins[0];
    move.mins[1] = playerMins[1];
    move.mins[2] = playerMins[2];
    move.maxs[0] = playerMaxs[0];
    move.maxs[1] = playerMaxs[1];
    move.maxs[2] = playerMaxs[2];
    EnsureVitaSharedPmoveState();
    if (pmoveTraceCount == 1 && state->weapon != WP_NONE) {
        const WeaponDef* sourceWeapon = BG_GetWeaponDef(state->weapon);
        const float sprintScale = sourceWeapon
            ? sourceWeapon->sprintDurationScale : 0.0f;
        char detail[192]{};
        std::snprintf(detail, sizeof(detail),
                      "weapon=%u maxSprint=%d sprintScale=%.3f speed=%d flags=%d",
                      static_cast<unsigned>(state->weapon),
                      BG_GetMaxSprintTime(state),
                      sprintScale,
                      state->speed, state->pm_flags);
        kisak::vita::VitaScriptServicesTrace("pmove_config", detail);
    }
    // The portable trace service is installed in both source handler slots,
    // but the weak server callback can be discarded by the Vita linker when
    // the full game_mp server is not linked.  Use the strong CG callback;
    // FillTrace still consults the same Shipment collision service.
    move.handler = 0;
    Pmove(&move);
    static unsigned pmoveResultCount = 0;
    if (pmoveResultCount < 160) {
        char detail[224];
        std::snprintf(detail, sizeof(detail),
                      "call=%u cmd=%d state=%d origin=%.2f,%.2f,%.2f vel=%.2f,%.2f,%.2f ground=%d flags=%d weapon=%d",
                      pmoveResultCount, command->serverTime, state->commandTime,
                      state->origin[0], state->origin[1], state->origin[2],
                      state->velocity[0], state->velocity[1], state->velocity[2],
                      state->groundEntityNum, state->pm_flags, state->weapon);
        kisak::vita::VitaScriptServicesTrace("pmove_result", detail);
    }
    ++pmoveResultCount;
    if (trace) {
        char detail[96];
        std::snprintf(detail, sizeof(detail), "cmd=%d state=%d",
                      command->serverTime, state->commandTime);
        kisak::vita::VitaScriptServicesTrace("pmove_exit", detail);
    }
    return true;
}

__attribute__((weak)) void __cdecl CG_TraceCapsule(trace_t* result, const float* start, const float* mins,
                             const float* maxs, const float* end, int passEntityNum,
                             int contentMask) {
    (void)passEntityNum;
    static unsigned traceCount = 0;
    if (traceCount++ < 16)
        kisak::vita::VitaScriptServicesTrace("trace_capsule", "cg_trace");
    FillTrace(result, start, mins, maxs, end, contentMask);
}

__attribute__((weak)) void __cdecl G_TraceCapsule(trace_t* result, const float* start,
                                                  const float* mins, const float* maxs,
                                                  const float* end, int passEntityNum,
                                                  int contentMask) {
    (void)passEntityNum;
    static unsigned traceCount = 0;
    if (traceCount++ < 16)
        kisak::vita::VitaScriptServicesTrace("trace_capsule", "g_trace");
    FillTrace(result, start, mins, maxs, end, contentMask);
}

__attribute__((weak)) void __cdecl G_PlayerEvent(int, int) {}

__attribute__((weak)) uint16_t __cdecl Trace_GetEntityHitId(const trace_t* result) {
    return result ? result->hitId : ENTITYNUM_WORLD;
}

__attribute__((weak)) void __cdecl SV_SetupIgnoreEntParams(IgnoreEntParams*, int) {}

__attribute__((weak)) void __cdecl SV_Trace(trace_t* result, const float* start, const float* mins,
                      const float* maxs, const float* end, const IgnoreEntParams*,
                      int contentMask, int, uint8_t*, int) {
    FillTrace(result, start, mins, maxs, end, contentMask);
}

__attribute__((weak)) int32_t __cdecl G_IsServerGameSystem(int32_t) { return 1; }

__attribute__((weak)) void __cdecl BG_AddPredictableEventToPlayerstate(entity_event_t event, uint32_t parm,
                                                  playerState_s* ps) {
    if (!ps)
        return;
    const int32_t slot = ps->eventSequence & 3;
    ps->events[slot] = static_cast<int32_t>(event);
    ps->eventParms[slot] = parm;
    ++ps->eventSequence;
}

__attribute__((weak)) int32_t __cdecl BG_AnimScriptAnimation(playerState_s*, aistateEnum_t,
                                       scriptAnimMoveTypes_t, int32_t) { return 0; }
__attribute__((weak)) int32_t __cdecl BG_AnimScriptEvent(playerState_s*, scriptAnimEventTypes_t,
                                   int32_t, int32_t) { return 0; }
__attribute__((weak)) int32_t __cdecl BG_PlayAnim(playerState_s*, int32_t, animBodyPart_t,
                            int32_t, int32_t, int32_t, int32_t) { return 0; }
__attribute__((weak)) void __cdecl BG_AnimUpdatePlayerStateConditions(pmove_t*) {}
__attribute__((weak)) void __cdecl BG_SetConditionValue(uint32_t, uint32_t, uint64_t) {}
__attribute__((weak)) void __cdecl BG_CheckThread() {}
__attribute__((weak)) char __cdecl BG_CheckProne(int32_t passEntityNum, const float* origin,
                           float size, float height, float yaw,
                           float* torsoPitch, float* waistPitch,
                           bool alreadyProne, bool onGround,
                           bool groundWalkable, uint8_t handler,
                           proneCheckType_t proneCheckType,
                           float proneFeetDistance) {
    if (!origin || size <= 0.0f || height <= 0.0f || proneFeetDistance <= 0.0f
        || (onGround && !groundWalkable))
        return 0;
    if (torsoPitch)
        *torsoPitch = 0.0f;
    if (waistPitch)
        *waistPitch = 0.0f;

    // Keep the original Pmove stance decision, but replace the desktop
    // collision callback with the same pointer-free Vita trace service used
    // by slide movement.  This is intentionally conservative: a prone
    // request is accepted only when the compact body fits, has support, and
    // has the required forward clearance.
    const uint8_t safeHandler = handler < 2 ? handler : 1;
    const auto traceFunc = pmoveHandlers[safeHandler].trace;
    if (!traceFunc)
        return 0;
    const int traceMask = proneCheckType ? 0x820011 : 0x810011;
    trace_t trace{};
    float mins[3] = {-size, -size, 0.0f};
    float maxs[3] = {size, size, std::max(18.0f, height * 0.45f)};
    float start[3] = {origin[0], origin[1], origin[2]};
    float end[3] = {origin[0], origin[1], origin[2] + 1.0f};

    if (!alreadyProne) {
        float standingMaxs[3] = {size, size, height};
        float standingEnd[3] = {origin[0], origin[1], origin[2] + 10.0f};
        traceFunc(&trace, start, mins, standingMaxs, standingEnd,
                  passEntityNum, traceMask);
        if (trace.allsolid || trace.startsolid)
            return 0;
    }

    traceFunc(&trace, start, mins, maxs, end, passEntityNum, traceMask);
    if (trace.allsolid || trace.startsolid)
        return 0;

    // A grounded player must still have a walkable support plane below the
    // compact prone body. Airborne transitions are allowed, matching the
    // original helper's deferred landing behavior.
    if (onGround) {
        float floorStart[3] = {origin[0], origin[1], origin[2] + 2.0f};
        float floorEnd[3] = {origin[0], origin[1], origin[2] - 32.0f};
        float pointMins[3] = {0.0f, 0.0f, 0.0f};
        float pointMaxs[3] = {0.0f, 0.0f, 0.0f};
        traceFunc(&trace, floorStart, pointMins, pointMaxs, floorEnd,
                  passEntityNum, traceMask);
        if (trace.fraction >= 1.0f || !trace.walkable)
            return 0;
    }

    float angles[3] = {0.0f, yaw - 180.0f, 0.0f};
    float forward[3]{};
    AngleVectors(angles, forward, nullptr, nullptr);
    const float clearance = std::max(0.0f, proneFeetDistance - 6.0f);
    float clearanceEnd[3] = {
        origin[0] + forward[0] * clearance,
        origin[1] + forward[1] * clearance,
        origin[2],
    };
    traceFunc(&trace, start, mins, maxs, clearanceEnd,
              passEntityNum, traceMask);
    if (trace.allsolid || trace.startsolid || trace.fraction < 1.0f)
        return 0;
    return 1;
}
__attribute__((weak)) int32_t __cdecl BG_GetMaxSprintTime(const playerState_s*) {
    return static_cast<int32_t>(std::lround(
        std::clamp(g_weaponConfig.sprintDurationScale, 0.1f, 4.0f) * 4000.0f));
}

static shellshock_parms_t g_shellshock{};
__attribute__((weak)) shellshock_parms_t* __cdecl BG_GetShellshockParms(uint32_t) { return &g_shellshock; }

static WeaponDef g_weapon{};
__attribute__((weak)) WeaponDef* __cdecl BG_GetWeaponDef(uint32_t weapon) {
    g_weapon.moveSpeedScale = g_weaponConfig.weapon == weapon
        ? g_weaponConfig.moveSpeedScale : 1.0f;
    g_weapon.adsMoveSpeedScale = g_weaponConfig.weapon == weapon
        ? g_weaponConfig.adsMoveSpeedScale : 1.0f;
    g_weapon.freezeMovementWhenFiring = g_weaponConfig.weapon == weapon
        ? g_weaponConfig.freezeMovementWhenFiring : 0;
    // VITP fireType values use the same full-auto/single-shot/burst ordering
    // as the shared weapon enums. Keep a conservative automatic fallback for
    // diagnostic callers that have not supplied a package record yet.
    g_weapon.fireType = g_weaponConfig.weapon == weapon
        ? static_cast<int32_t>(g_weaponConfig.fireType)
        : (weapon == 3 ? WEAPON_FIRETYPE_SINGLESHOT : WEAPON_FIRETYPE_FULLAUTO);
    return &g_weapon;
}
__attribute__((weak)) bool __cdecl BG_WeaponBlocksProne(uint32_t) { return false; }
__attribute__((weak)) bool __cdecl BG_UsingSniperScope(playerState_s*) { return false; }
__attribute__((weak)) int32_t __cdecl PM_WeaponAmmoAvailable(playerState_s* ps) {
    if (!ps || ps->weapon >= 128)
        return 0;
    return ps->ammoclip[ps->weapon] > 0 || ps->ammo[ps->weapon] > 0;
}
__attribute__((weak)) void __cdecl PM_AdjustAimSpreadScale(pmove_t*, pml_t*) {}
__attribute__((weak)) void __cdecl PM_UpdateAimDownSightFlag(pmove_t* pm, pml_t*) {
    if (!pm || !pm->ps)
        return;
    playerState_s* ps = pm->ps;
    if ((pm->cmd.buttons & BUTTON_ADS) != 0
        && ps->weaponstate != WEAPON_RELOADING
        && ps->weaponstate != WEAPON_RELOAD_START
        && ps->weaponstate != WEAPON_RELOAD_END)
        ps->pm_flags |= PMF_SIGHT_AIMING;
    else
        ps->pm_flags &= ~PMF_SIGHT_AIMING;
}
__attribute__((weak)) void __cdecl PM_UpdateAimDownSightLerp(pmove_t* pm, pml_t* pml) {
    if (!pm || !pm->ps)
        return;
    const float target = (pm->ps->pm_flags & PMF_SIGHT_AIMING) != 0 ? 1.0f : 0.0f;
    const int32_t transitionMs = target > 0.5f
        ? g_weaponConfig.adsTransInTime : g_weaponConfig.adsTransOutTime;
    const float step = pml
        ? std::max(0.01f, static_cast<float>(pml->msec)
            / static_cast<float>(std::max(1, transitionMs)))
        : 0.05f;
    if (pm->ps->fWeaponPosFrac < target)
        pm->ps->fWeaponPosFrac = std::min(target, pm->ps->fWeaponPosFrac + step);
    else if (pm->ps->fWeaponPosFrac > target)
        pm->ps->fWeaponPosFrac = std::max(target, pm->ps->fWeaponPosFrac - step);
}
__attribute__((weak)) void __cdecl PM_ExitAimDownSight(playerState_s* ps) {
    if (ps)
        ps->pm_flags &= ~PMF_SIGHT_AIMING;
}
__attribute__((weak)) int32_t __cdecl PM_InteruptWeaponWithProneMove(playerState_s*) { return 0; }
__attribute__((weak)) void __cdecl PM_ResetWeaponState(playerState_s*) {}
__attribute__((weak)) void __cdecl PM_Weapon(pmove_t* pm, pml_t*) {
    if (!pm || !pm->ps)
        return;
    playerState_s* ps = pm->ps;
    const bool attackHeld = (pm->cmd.buttons & BUTTON_ATTACK) != 0;
    const bool attackPressed = attackHeld
        && (pm->oldcmd.buttons & BUTTON_ATTACK) == 0;
    const bool singleShot = BG_GetWeaponDef(ps->weapon)->fireType
        == WEAPON_FIRETYPE_SINGLESHOT;
    if ((pm->cmd.buttons & BUTTON_RELOAD) != 0) {
        ps->weaponstate = WEAPON_RELOADING;
        ps->weaponTime = 1;
    } else if ((singleShot ? attackPressed : attackHeld)
               && PM_WeaponAmmoAvailable(ps)) {
        ps->weaponstate = WEAPON_FIRING;
        ++ps->weaponShotCount;
        ps->weaponTime = 1;
    } else {
        ps->weaponstate = WEAPON_READY;
        ps->weaponTime = 0;
    }
}

__attribute__((weak)) void __cdecl Mantle_Check(pmove_t*, pml_t*) {}
__attribute__((weak)) void __cdecl Mantle_Move(pmove_t*, playerState_s*, pml_t*) {}
__attribute__((weak)) void __cdecl Mantle_CapView(playerState_s*) {}
__attribute__((weak)) void __cdecl Mantle_ClearHint(playerState_s* ps) {
    if (ps)
        ps->cursorHint = 0;
}

__attribute__((weak)) void __cdecl AddLeanToPosition(float*, float, float, float, float) {}
__attribute__((weak)) double __cdecl UnGetLeanFraction(float fraction) { return fraction; }
void __cdecl Sys_SnapVector(float* value) {
    if (!value)
        return;
    value[0] = std::floor(value[0] + 0.5f);
    value[1] = std::floor(value[1] + 0.5f);
    value[2] = std::floor(value[2] + 0.5f);
}

__attribute__((weak)) void __cdecl AngleVectors(const float* angles, float* forward, float* right, float* up) {
    if (!angles)
        return;
    constexpr float kDegToRad = 0.01745329251994329577f;
    const float pitch = angles[0] * kDegToRad;
    const float yaw = angles[1] * kDegToRad;
    const float roll = angles[2] * kDegToRad;
    const float sp = std::sin(pitch), cp = std::cos(pitch);
    const float sy = std::sin(yaw), cy = std::cos(yaw);
    const float sr = std::sin(roll), cr = std::cos(roll);
    if (forward) {
        forward[0] = cp * cy;
        forward[1] = cp * sy;
        forward[2] = -sp;
    }
    if (right) {
        right[0] = -sr * sp * cy + cr * sy;
        right[1] = -sr * sp * sy + -cr * cy;
        right[2] = -sr * cp;
    }
    if (up) {
        up[0] = cr * sp * cy + -sr * sy;
        up[1] = cr * sp * sy + sr * cy;
        up[2] = cr * cp;
    }
}

void __cdecl Com_Printf(int channel, const char* format, ...) {
    char message[2048]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format ? format : "", args);
    va_end(args);
    Com_PrintMessage(channel, message, 0);
}

void __cdecl Com_DPrintf(int channel, const char* format, ...) {
#if KISAK_VITA_PROFILE
    char message[2048]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format ? format : "", args);
    va_end(args);
    Com_PrintMessage(channel, message, 0);
#else
    (void)channel;
    (void)format;
#endif
}

void __cdecl MyAssertHandler(const char* file, int line, int,
                             const char* format, ...) {
    char message[1024]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format ? format : "", args);
    va_end(args);
    // A failed source invariant must stop the active source root. Continuing
    // past it previously turned a useful assertion into a later guest fault.
    Com_Error(ERR_DROP, "source assertion %s:%d: %s", file ? file : "?", line, message);
}
__attribute__((weak)) int __cdecl I_stricmp(const char* lhs, const char* rhs) {
    if (lhs == rhs)
        return 0;
    if (!lhs)
        return -1;
    if (!rhs)
        return 1;
    while (*lhs && *rhs) {
        const unsigned char a = static_cast<unsigned char>(*lhs++);
        const unsigned char b = static_cast<unsigned char>(*rhs++);
        const int lowerA = std::tolower(a);
        const int lowerB = std::tolower(b);
        if (lowerA != lowerB)
            return lowerA < lowerB ? -1 : 1;
    }
    return *lhs ? 1 : (*rhs ? -1 : 0);
}
struct DObj_s;
__attribute__((weak)) void __cdecl DObjSetControlTagAngles(
    DObj_s*, int*, uint32_t, float*) {}
__attribute__((weak)) void __cdecl DObjSetLocalTag(
    DObj_s*, int*, uint32_t, const float*, const float*) {}
__attribute__((weak)) char* __cdecl va(const char*, ...) {
    static char buffer[1024];
    buffer[0] = '\0';
    return buffer;
}
