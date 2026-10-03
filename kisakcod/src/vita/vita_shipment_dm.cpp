#include <vita/vita_shipment_dm.h>
#include <vita/vita_net_compat.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace kisak::vita {
namespace {

constexpr uint32_t kSnapshotMagic = 0x31534D44; // "DMS1"
constexpr uint32_t kMatchMagic = 0x314D4D44; // "DMM1"
constexpr uint32_t kCommandMagic = 0x31434D44; // "DMC1"

#pragma pack(push, 1)
struct ShipmentSnapshotPacket {
    uint32_t magic;
    int32_t serverTime;
    uint32_t clientNum;
    uint32_t team;
    uint32_t flags;
    int32_t health;
    int32_t score;
    int32_t deaths;
    uint32_t weaponSlot;
    int32_t weapAnim;
    int32_t clipAmmo;
    int32_t reserveAmmo;
    int32_t nextFireTime;
    int32_t reloadStartTime;
    int32_t reloadEndTime;
    int32_t respawnTime;
    int32_t weaponState;
    int32_t grenadeCount;
    float origin[3];
    float velocity[3];
    float angles[3];
    float viewHeight;
    float weaponPosFrac;
};
#pragma pack(pop)

static_assert(sizeof(ShipmentSnapshotPacket) == 116);

#pragma pack(push, 1)
// Keep match metadata off the player packet so the existing DMS1 payload and
// source snapshot bridge stay byte-for-byte stable.  This is the bounded
// equivalent of the gametype/server-config state that the retail client reads
// separately from playerstate deltas.
struct ShipmentMatchPacket {
    uint32_t magic;
    int32_t serverTime;
    uint8_t mode;
    uint8_t phase;
    uint16_t reserved;
    int32_t elapsedMsec;
    int32_t timeLimitMsec;
    int32_t scoreLimit;
    int32_t killScore;
    uint32_t winnerClientNum;
};
#pragma pack(pop)

static_assert(sizeof(ShipmentMatchPacket) == 32);

#pragma pack(push, 1)
struct ShipmentCommandPacket {
    uint32_t magic;
    uint32_t clientNum;
    usercmd_s command;
};
#pragma pack(pop)

static_assert(sizeof(ShipmentCommandPacket) == 40);

constexpr int32_t kWeaponFireDelay = 100;
constexpr int32_t kReloadDuration = 1600;
constexpr int32_t kRespawnDelay = 1000;
constexpr uint32_t kWeaponFireTypeFullAuto = 0;
constexpr uint32_t kWeaponFireTypeSingle = 1;
constexpr uint32_t kWeaponFireTypeBurst2 = 2;
constexpr uint32_t kWeaponFireTypeBurst3 = 3;
constexpr uint32_t kWeaponFireTypeBurst4 = 4;
constexpr uint32_t kSnapshotFlagAds = 1u << 2;
constexpr uint32_t kSnapshotFlagFiring = 1u << 3;
constexpr uint32_t kSnapshotFlagReloading = 1u << 4;
constexpr uint32_t kSnapshotFlagProne = 1u << 5;
constexpr uint32_t kSnapshotFlagCrouched = 1u << 6;
constexpr uint32_t kSnapshotFlagSprinting = 1u << 7;
constexpr uint32_t kSnapshotFlagJumping = 1u << 8;
constexpr uint32_t kSnapshotFlagMelee = 1u << 9;
constexpr uint32_t kSnapshotFlagGrenade = 1u << 10;

// The retail registerTimeLimitDvar convention uses zero for an unlimited
// playlist.  Keep the start-relative clock monotonic in that case instead of
// feeding zero as the upper bound to std::clamp (which would freeze elapsed
// time and make a valid source registration look like a waiting match).
int32_t ElapsedSince(int32_t serverTime, int32_t startServerTime,
                     int32_t timeLimitMsec) {
    const int64_t raw = std::max<int64_t>(
        0, static_cast<int64_t>(serverTime)
            - static_cast<int64_t>(startServerTime));
    const int64_t bounded = timeLimitMsec > 0
        ? std::min<int64_t>(raw, timeLimitMsec) : raw;
    return static_cast<int32_t>(std::clamp<int64_t>(
        bounded, 0, static_cast<int64_t>(INT32_MAX)));
}

void Copy3(float destination[3], const float source[3]) {
    std::memcpy(destination, source, sizeof(float) * 3);
}

float Dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void Normalize3(float value[3]) {
    const float length = std::sqrt(Dot3(value, value));
    if (length > 0.0001f) {
        value[0] /= length;
        value[1] /= length;
        value[2] /= length;
    }
}

bool IsSourceReloadState(int32_t state) {
    return state >= WEAPON_RELOADING && state <= WEAPON_RELOAD_END;
}

// Returns the first normalized intersection of a ray segment with an AABB.
// Keeping this local to the Vita server avoids exposing pointer-bearing
// entity types through the package boundary.
bool RayBoxFraction(const float start[3], const float direction[3],
                    const float mins[3], const float maxs[3], float* fraction) {
    if (!start || !direction || !mins || !maxs || !fraction)
        return false;
    float enter = 0.0f;
    float exit = 1.0f;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(direction[axis]) < 0.000001f) {
            if (start[axis] < mins[axis] || start[axis] > maxs[axis])
                return false;
            continue;
        }
        const float inverse = 1.0f / direction[axis];
        float nearHit = (mins[axis] - start[axis]) * inverse;
        float farHit = (maxs[axis] - start[axis]) * inverse;
        if (nearHit > farHit)
            std::swap(nearHit, farHit);
        enter = std::max(enter, nearHit);
        exit = std::min(exit, farHit);
        if (enter > exit)
            return false;
    }
    *fraction = std::clamp(enter, 0.0f, 1.0f);
    return true;
}

} // namespace

void ShipmentDmServer::Reset() {
    clients_.clear();
    events_.clear();
    connectedClients_ = 0;
    nextSpawn_ = 0;
    lastServerTime_ = 0;
    matchStartServerTime_ = 0;
    match_ = ShipmentDmMatchState{};
    matchStarted_ = false;
    initialized_ = false;
    rules_ = ShipmentDmRules{};
    collision_ = nullptr;
    entities_ = nullptr;
    weapons_ = nullptr;
    VitaPmove_SetCollision(nullptr);
    VitaLoopback_Reset();
}

bool ShipmentDmServer::Initialize(const ShipmentCollision* collision,
                                  const ShipmentMapEntities* entities,
                                  const ShipmentWeaponTable* weapons,
                                  uint32_t maxClients,
                                  const ShipmentDmRules* rules) {
    Reset();
    if (!collision || !entities || !collision->Loaded() || !entities->Loaded()
        || entities->DeathmatchSpawns().empty())
        return false;
    collision_ = collision;
    entities_ = entities;
    weapons_ = weapons;
    if (rules)
        rules_ = *rules;
    rules_.timeLimitMsec = std::clamp(rules_.timeLimitMsec, 0,
                                      1440 * 60 * 1000);
    rules_.scoreLimit = std::clamp(rules_.scoreLimit, 0, 5000);
    rules_.killScore = std::clamp(rules_.killScore, 0, 1000);
    match_.timeLimitMsec = rules_.timeLimitMsec;
    match_.scoreLimit = rules_.scoreLimit;
    match_.killScore = rules_.killScore;
    VitaPmove_SetCollision(collision_);
    clients_.resize(std::max(1u, std::min(maxClients, 8u)));
    for (uint32_t i = 0; i < clients_.size(); ++i)
        clients_[i].state.clientNum = i;
    initialized_ = true;
    return true;
}

uint32_t ShipmentDmServer::ConnectLocalClient() {
    if (!initialized_)
        return UINT32_MAX;
    for (uint32_t i = 0; i < clients_.size(); ++i) {
        ClientSlot& slot = clients_[i];
        if (slot.state.connected)
            continue;
        slot.state = ShipmentDmPlayerState{};
        slot.state.clientNum = i;
        slot.nativeState = playerState_s{};
        slot.oldCommand = usercmd_s{};
        slot.shotSequence = 0;
        slot.hasCommand = false;
        slot.state.connected = true;
        // dm.gsc explicitly describes Deathmatch as having no gameplay or
        // scoring teams.  Keep every local client on TEAM_FREE; the source
        // allies/axis definitions only select the available character and
        // weapon assets, not friendly-fire filtering or score ownership.
        slot.state.team = rules_.teamBased ? (i & 1u ? 2u : 1u) : 0u;
        Spawn(&slot, lastServerTime_, false);
        ++connectedClients_;
        return i;
    }
    return UINT32_MAX;
}

bool ShipmentDmServer::SubmitUsercmd(uint32_t clientNum, const usercmd_s& command) {
    if (!initialized_ || clientNum >= clients_.size() || !clients_[clientNum].state.connected)
        return false;
    clients_[clientNum].pending = command;
    clients_[clientNum].hasCommand = true;
    return true;
}

void ShipmentDmServer::Spawn(ClientSlot* slot, int32_t serverTime, bool respawn) {
    if (!slot || !entities_)
        return;
    const auto& spawns = entities_->DeathmatchSpawns();
    if (spawns.empty())
        return;
    const ShipmentSpawnPoint& spawnPoint = spawns[nextSpawn_++ % spawns.size()];
    Copy3(slot->state.origin, spawnPoint.origin);
    Copy3(slot->state.angles, spawnPoint.angles);
    std::memset(slot->state.velocity, 0, sizeof(slot->state.velocity));
    slot->state.alive = true;
    slot->state.health = 100;
    // The first authored loadout in this vertical slice is the source MP5;
    // keeping the authoritative server slot at 2 makes the viewmodel, ammo
    // rules, animations, and HUD all exercise the same KisakCOD weapon path.
    slot->state.weaponSlot = 2;
    // Reset the compact inventory on every spawn/respawn, then keep each
    // authored slot independent while the player is alive.
    for (uint32_t weaponSlot = 1; weaponSlot <= 3; ++weaponSlot) {
        const ShipmentWeaponDefinition* weapon = weapons_
            ? weapons_->ForSlot(weaponSlot) : nullptr;
        const int32_t clipSize = weapon && weapon->clipSize > 0
            ? weapon->clipSize : (weaponSlot == 3 ? 12 : 30);
        const int32_t reserve = weapon && weapon->maxAmmo > clipSize
            ? weapon->maxAmmo - clipSize : 90;
        slot->weaponClipAmmo[weaponSlot] = clipSize;
        slot->weaponReserveAmmo[weaponSlot] = std::max(0, reserve);
    }
    const int32_t clipSize = slot->weaponClipAmmo[slot->state.weaponSlot];
    const int32_t reserve = slot->weaponReserveAmmo[slot->state.weaponSlot];
    slot->state.clipAmmo = clipSize;
    slot->state.reserveAmmo = std::max(0, reserve);
    slot->state.weapAnim = 0;
    slot->state.nextFireTime = serverTime;
    slot->state.reloadStartTime = 0;
    slot->state.reloadEndTime = 0;
    slot->state.respawnTime = 0;
    slot->state.ads = false;
    slot->state.firing = false;
    slot->state.reloading = false;
    slot->state.prone = false;
    slot->state.crouched = false;
    slot->state.sprinting = false;
    slot->state.jumping = false;
    slot->state.melee = false;
    slot->state.grenade = false;
    slot->state.weaponState = WEAPON_READY;
    slot->state.grenadeCount = 2;
    slot->state.weaponPosFrac = 0.0f;
    slot->state.viewHeight = 60.0f;
    slot->state.muzzleFlashEndTime = 0;
    slot->nativeState = playerState_s{};
    slot->nativeState.commandTime = serverTime;
    slot->nativeState.pm_type = PM_NORMAL;
    slot->nativeState.otherFlags = POF_PLAYER;
    slot->nativeState.clientNum = static_cast<int32_t>(slot->state.clientNum);
    Copy3(slot->nativeState.origin, slot->state.origin);
    Copy3(slot->nativeState.viewangles, slot->state.angles);
    slot->nativeState.viewHeightTarget = 60;
    slot->nativeState.viewHeightCurrent = 60.0f;
    slot->nativeState.groundEntityNum = ENTITYNUM_NONE;
    slot->nativeState.speed = 190;
    slot->nativeState.gravity = 800;
    slot->nativeState.weapon = 2;
    slot->nativeState.weaponstate = WEAPON_READY;
    slot->nativeState.offHandIndex = 4;
    slot->nativeState.offhandSecondary = PLAYER_OFFHAND_SECONDARY_SMOKE;
    slot->nativeState.weapAnim = 0;
    slot->nativeState.weapons[0] = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4);
    for (uint32_t weaponSlot = 1; weaponSlot <= 3; ++weaponSlot) {
        slot->nativeState.ammoclip[weaponSlot] = slot->weaponClipAmmo[weaponSlot];
        slot->nativeState.ammo[weaponSlot] = slot->weaponReserveAmmo[weaponSlot];
    }
    slot->nativeState.ammoclip[4] = slot->state.grenadeCount;
    slot->nativeState.ammo[4] = 0;
    slot->oldCommand = usercmd_s{};
    slot->oldCommand.serverTime = serverTime;
    slot->shotSequence = 0;
    slot->burstShotsRemaining = 0;
    slot->grenadeActive = false;
    slot->grenadeSpawnTime = 0;
    std::memset(slot->grenadeOrigin, 0, sizeof(slot->grenadeOrigin));
    std::memset(slot->grenadeVelocity, 0, sizeof(slot->grenadeVelocity));
    slot->pending = usercmd_s{};
    slot->pending.serverTime = serverTime;
    slot->hasCommand = false;
    events_.push_back({respawn ? ShipmentDmEventType::Respawn : ShipmentDmEventType::Spawn,
                       slot->state.clientNum, serverTime});
}

bool ShipmentDmServer::ApplyDamage(uint32_t targetClientNum, uint32_t attackerClientNum,
                                   int32_t damage, int32_t serverTime,
                                   uint32_t weapon, const float* point,
                                   const float* direction) {
    if (!initialized_ || damage <= 0 || targetClientNum >= clients_.size()
        || !clients_[targetClientNum].state.connected
        || !clients_[targetClientNum].state.alive
        || match_.phase == ShipmentDmMatchPhase::Complete)
        return false;
    // Events are authored against the monotonic server clock.  A late local
    // command must not move a death/respawn backwards relative to the last
    // authoritative tick.
    const int32_t eventTime = std::max(0, std::max(serverTime, lastServerTime_));
    ClientSlot& target = clients_[targetClientNum];
    target.state.health = std::max(0, target.state.health - damage);
    ShipmentDmEvent damageEvent{};
    damageEvent.type = ShipmentDmEventType::Damage;
    damageEvent.clientNum = targetClientNum;
    damageEvent.serverTime = eventTime;
    damageEvent.attackerClientNum = attackerClientNum;
    damageEvent.damage = damage;
    damageEvent.weapon = weapon;
    if (point)
        std::memcpy(damageEvent.point, point, sizeof(damageEvent.point));
    if (direction)
        std::memcpy(damageEvent.direction, direction, sizeof(damageEvent.direction));
    events_.push_back(damageEvent);
    if (target.state.health > 0)
        return true;

    target.state.alive = false;
    ++target.state.deaths;
    target.state.respawnTime = eventTime + kRespawnDelay;
    ShipmentDmEvent deathEvent = damageEvent;
    deathEvent.type = ShipmentDmEventType::Death;
    events_.push_back(deathEvent);
    if (attackerClientNum < clients_.size() && attackerClientNum != targetClientNum
        && clients_[attackerClientNum].state.connected) {
        ClientSlot& attacker = clients_[attackerClientNum];
        // dm.gsc registers the authored `kill` score event as 5.  This is
        // the scoreboard value (and the value checked by the DM score-limit
        // callback), distinct from the rank-XP/profile plumbing.
        const int32_t awardedScore = attacker.state.score + rules_.killScore;
        // A zero score limit is the source convention for an unlimited
        // playlist. Do not clamp every kill back to zero in that case; the
        // intermission scoreboard still needs the accumulated DM score.
        attacker.state.score = match_.scoreLimit > 0
            ? std::min(match_.scoreLimit, awardedScore) : awardedScore;
        events_.push_back({ShipmentDmEventType::Kill, attackerClientNum, eventTime});
        if (match_.phase == ShipmentDmMatchPhase::Active
            && match_.scoreLimit > 0
            && attacker.state.score >= match_.scoreLimit) {
            match_.phase = ShipmentDmMatchPhase::Complete;
            match_.elapsedMsec = ElapsedSince(
                eventTime, matchStartServerTime_, match_.timeLimitMsec);
            match_.winnerClientNum = attackerClientNum;
        }
    }
    return true;
}

void ShipmentDmServer::HandleWeapon(ClientSlot* slot, int32_t serverTime,
                                     bool attackPressedEdge, bool sourceFired) {
    if (!slot || !slot->state.alive)
        return;
    const ShipmentWeaponDefinition* weapon = weapons_ ? weapons_->ForSlot(slot->state.weaponSlot) : nullptr;
    const int32_t clipSize = weapon && weapon->clipSize > 0 ? weapon->clipSize : 30;
    const int32_t fireDelay = weapon && weapon->fireDelay > 0
        ? weapon->fireDelay : kWeaponFireDelay;
    const int32_t reloadDuration = weapon && weapon->reloadTime > 0
        ? weapon->reloadTime : kReloadDuration;
    const int32_t reloadEmptyDuration = weapon && weapon->reloadEmptyTime > 0
        ? weapon->reloadEmptyTime : reloadDuration;
    const usercmd_s& command = slot->pending;
    const int32_t sourceState = static_cast<int32_t>(slot->nativeState.weaponstate);
    const bool sourceMelee = sourceState >= WEAPON_MELEE_INIT
        && sourceState <= WEAPON_MELEE_END;
    const bool sourceOffhand = sourceState >= WEAPON_OFFHAND_INIT
        && sourceState <= WEAPON_OFFHAND_END;
    const bool sourceSprint = sourceState >= WEAPON_SPRINT_RAISE
        && sourceState <= WEAPON_SPRINT_DROP;
    const bool sourceReload = IsSourceReloadState(sourceState);
    if (sourceMelee || sourceOffhand || sourceSprint || sourceReload) {
        // These transitions are wholly owned by bg_weapons.cpp.  Do not let
        // the compact firearm fallback overwrite the authored source
        // weapAnim while the reload/melee/offhand/sprint state machine is
        // active.  The source reload timers/ammo are copied back by
        // SimulatePlayer; this branch only keeps the legacy mirror from
        // starting a second, parallel reload animation.
        slot->state.firing = false;
        slot->state.reloading = sourceReload;
        slot->state.weapAnim = slot->nativeState.weapAnim;
        return;
    }
    slot->state.firing = false;
    slot->state.reloading = slot->state.reloadEndTime != 0;
    if (slot->state.reloadEndTime != 0) {
        if (serverTime >= slot->state.reloadEndTime) {
            const int32_t needed = std::max(0, clipSize - slot->state.clipAmmo);
            const int32_t loaded = std::min(needed, slot->state.reserveAmmo);
            slot->state.clipAmmo += loaded;
            slot->state.reserveAmmo -= loaded;
            slot->state.reloadStartTime = 0;
            slot->state.reloadEndTime = 0;
            slot->state.reloading = false;
            slot->state.weapAnim = 0;
            slot->nativeState.weapAnim = 0;
            events_.push_back({ShipmentDmEventType::Reload, slot->state.clientNum, serverTime});
        }
        return;
    }
    if ((command.buttons & BUTTON_RELOAD) && slot->state.clipAmmo < clipSize
        && slot->state.reserveAmmo > 0) {
        slot->burstShotsRemaining = 0;
        const int32_t duration = slot->state.clipAmmo == 0
            ? reloadEmptyDuration : reloadDuration;
        slot->state.reloadStartTime = serverTime;
        slot->state.reloadEndTime = serverTime + duration;
        slot->state.reloading = true;
        // PM_Weapon_SetReloadingState uses 13/14 for reload/reload-empty;
        // CG_WeaponRunXModelAnims maps those source values to slots 9/10.
        slot->state.weapAnim = slot->state.clipAmmo == 0 ? 14 : 13;
        slot->nativeState.weapAnim = slot->state.weapAnim;
        return;
    }
    // Fire is a one-frame source animation event. Keep reload state latched
    // above until completion, then return idle to source slot 1.
    if (!sourceFired) {
        slot->state.weapAnim = 0;
        slot->nativeState.weapAnim = 0;
    }
    const uint32_t fireType = weapon ? weapon->fireType : kWeaponFireTypeFullAuto;
    uint8_t burstSize = 0;
    if (fireType == kWeaponFireTypeBurst2)
        burstSize = 2;
    else if (fireType == kWeaponFireTypeBurst3)
        burstSize = 3;
    else if (fireType == kWeaponFireTypeBurst4)
        burstSize = 4;
    if (burstSize != 0 && attackPressedEdge)
        slot->burstShotsRemaining = burstSize;

    bool wantsFire = (command.buttons & BUTTON_ATTACK) != 0;
    if (sourceFired)
        wantsFire = true;
    if (fireType == kWeaponFireTypeSingle)
        wantsFire = attackPressedEdge;
    else if (burstSize != 0)
        wantsFire = slot->burstShotsRemaining != 0;
    if (wantsFire && (sourceFired || serverTime >= slot->state.nextFireTime)) {
        if (sourceFired || slot->state.clipAmmo > 0) {
            if (!sourceFired) {
                --slot->state.clipAmmo;
                slot->state.nextFireTime = serverTime + fireDelay;
            }
            slot->state.firing = true;
            const bool clipEmpty = slot->state.clipAmmo == 0;
            // PM_Weapon_SetFPSFireAnim uses 2/3 for hip fire/last shot and
            // 5/6 for ADS fire/last shot. The renderer applies the exact
            // source WeaponRunXModelAnims switch to these values.
            slot->state.weapAnim = sourceFired ? slot->nativeState.weapAnim
                : (slot->state.ads ? (clipEmpty ? 6 : 5)
                                   : (clipEmpty ? 3 : 2));
            slot->nativeState.weapAnim = slot->state.weapAnim;
            slot->state.muzzleFlashEndTime = serverTime + 65;
            if (burstSize != 0 && slot->burstShotsRemaining != 0)
                --slot->burstShotsRemaining;
            // Keep the native player view authoritative while applying a
            // bounded weapon-specific kick. The deterministic sequence makes
            // fixed PC/Vita input traces reproducible without inventing a new
            // spread model outside the exported weapon scalars.
            ++slot->shotSequence;
            float shotPoint[3] = {0.0f, 0.0f, 0.0f};
            float shotDirection[3] = {0.0f, 0.0f, 1.0f};
            ResolveHitscan(*slot, weapon, serverTime, shotPoint, shotDirection);
            const float sway = weapon && weapon->adsSwayMaxAngle > 0.0f
                ? weapon->adsSwayMaxAngle : 5.0f;
            const float recoil = std::clamp(sway * (slot->state.ads ? 0.035f : 0.06f),
                                            0.12f, 1.25f);
            uint32_t seed = slot->shotSequence * 1664525u + 1013904223u;
            const float lateral = (static_cast<float>((seed >> 16) & 0x7FFu) / 1023.5f - 1.0f)
                * recoil * 0.35f;
            slot->nativeState.viewangles[0] = std::clamp(
                slot->nativeState.viewangles[0] - recoil, -89.0f, 89.0f);
            slot->nativeState.viewangles[1] += lateral;
            ShipmentDmEvent fireEvent{};
            fireEvent.type = ShipmentDmEventType::Fire;
            fireEvent.clientNum = slot->state.clientNum;
            fireEvent.serverTime = serverTime;
            fireEvent.weapon = slot->state.weaponSlot;
            std::memcpy(fireEvent.point, shotPoint, sizeof(fireEvent.point));
            std::memcpy(fireEvent.direction, shotDirection,
                        sizeof(fireEvent.direction));
            events_.push_back(fireEvent);
        } else if (slot->state.reserveAmmo > 0) {
            slot->burstShotsRemaining = 0;
            slot->state.reloadStartTime = serverTime;
            slot->state.reloadEndTime = serverTime + reloadEmptyDuration;
            slot->state.reloading = true;
            slot->state.weapAnim = 14;
            slot->nativeState.weapAnim = 14;
        }
    }
}

void ShipmentDmServer::ResolveHitscan(const ClientSlot& shooter,
                                      const ShipmentWeaponDefinition* weapon,
                                      int32_t serverTime, float* shotPoint,
                                      float* shotDirection) {
    if (!collision_ || !shooter.state.alive)
        return;

    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kRange = 8192.0f;
    const float pitch = shooter.nativeState.viewangles[0] * kPi / 180.0f;
    const float yaw = shooter.nativeState.viewangles[1] * kPi / 180.0f;
    float forward[3] = {
        std::cos(pitch) * std::cos(yaw),
        std::cos(pitch) * std::sin(yaw),
        -std::sin(pitch),
    };
    Normalize3(forward);

    if (shotDirection)
        std::memcpy(shotDirection, forward, sizeof(forward));
    float right[3] = {std::sin(yaw), -std::cos(yaw), 0.0f};
    float up[3] = {
        std::sin(pitch) * std::cos(yaw),
        std::sin(pitch) * std::sin(yaw),
        std::cos(pitch),
    };
    Normalize3(right);
    Normalize3(up);

    // Use the exported hip-spread scalar as the source of truth. A tiny
    // deterministic sequence keeps PC/Vita traces reproducible without a
    // floating-point RNG or per-platform library dependency.
    const float spreadDegrees = weapon && weapon->hipSpreadStandMax > 0.0f
        ? weapon->hipSpreadStandMax : 0.5f;
    const float spreadScale = shooter.state.ads ? 0.25f : 1.0f;
    uint32_t seed = shooter.shotSequence * 747796405u
        + (shooter.state.clientNum + 1u) * 2891336453u;
    seed ^= seed >> 16;
    const float horizontal = (static_cast<float>(seed & 0xFFFFu) / 32767.5f - 1.0f);
    seed = seed * 1664525u + 1013904223u;
    const float vertical = (static_cast<float>((seed >> 8) & 0xFFFFu) / 32767.5f - 1.0f);
    const float tangent = std::tan(spreadDegrees * spreadScale * kPi / 180.0f);
    forward[0] += right[0] * horizontal * tangent + up[0] * vertical * tangent;
    forward[1] += right[1] * horizontal * tangent + up[1] * vertical * tangent;
    forward[2] += right[2] * horizontal * tangent + up[2] * vertical * tangent;
    Normalize3(forward);

    float start[3] = {
        shooter.nativeState.origin[0],
        shooter.nativeState.origin[1],
        shooter.nativeState.origin[2] + std::clamp(shooter.state.viewHeight, 8.0f, 60.0f),
    };
    float end[3] = {
        start[0] + forward[0] * kRange,
        start[1] + forward[1] * kRange,
        start[2] + forward[2] * kRange,
    };
    const float rayDelta[3] = {
        end[0] - start[0], end[1] - start[1], end[2] - start[2],
    };
    ShipmentCollisionTrace worldTrace{};
    const bool worldHit = collision_->TraceSphere(start, end, 0.0f, 0xFFFFFFFFu, &worldTrace);
    const float worldFraction = worldHit ? std::clamp(worldTrace.fraction, 0.0f, 1.0f) : 1.0f;

    // Carry the first collision point with the fire event even when there is
    // no opposing client.  The renderer uses this bounded point for a short
    // tracer/impact cue, while damage remains entirely server-authoritative.
    if (shotPoint) {
        for (uint32_t axis = 0; axis < 3; ++axis)
            shotPoint[axis] = start[axis] + rayDelta[axis] * worldFraction;
    }

    uint32_t targetClient = UINT32_MAX;
    float targetFraction = worldFraction;
    for (const ClientSlot& candidate : clients_) {
        if (!candidate.state.connected || !candidate.state.alive
            || candidate.state.clientNum == shooter.state.clientNum)
            continue;
        const float mins[3] = {
            candidate.state.origin[0] - 16.0f,
            candidate.state.origin[1] - 16.0f,
            candidate.state.origin[2],
        };
        const float maxs[3] = {
            candidate.state.origin[0] + 16.0f,
            candidate.state.origin[1] + 16.0f,
            candidate.state.origin[2] + 72.0f,
        };
        float fraction = 1.0f;
        if (RayBoxFraction(start, rayDelta, mins, maxs, &fraction)
            && fraction <= targetFraction + 0.0001f) {
            targetFraction = fraction;
            targetClient = candidate.state.clientNum;
        }
    }
    if (shotPoint) {
        for (uint32_t axis = 0; axis < 3; ++axis)
            shotPoint[axis] = start[axis] + rayDelta[axis] * targetFraction;
    }
    if (targetClient == UINT32_MAX)
        return;

    const int32_t damage = std::clamp(
        weapon && weapon->playerDamage > 0 ? weapon->playerDamage
            : (weapon && weapon->damage > 0 ? weapon->damage : 10),
        1, 200);
    // Report the resolved impact point to the authoritative damage event.
    // The renderer and later hit-feedback consumers use this point for the
    // short tracer/impact cue; passing the muzzle start here made every hit
    // appear to originate at the shooter even after a target intersection.
    ApplyDamage(targetClient, shooter.state.clientNum, damage, serverTime,
                shooter.state.weaponSlot, shotPoint, forward);
}

void ShipmentDmServer::ResolveMelee(const ClientSlot& shooter, int32_t serverTime,
                                    float* impactPoint, float* impactDirection) {
    if (!shooter.state.alive)
        return;

    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kRange = 96.0f;
    const float pitch = shooter.nativeState.viewangles[0] * kPi / 180.0f;
    const float yaw = shooter.nativeState.viewangles[1] * kPi / 180.0f;
    float forward[3] = {
        std::cos(pitch) * std::cos(yaw),
        std::cos(pitch) * std::sin(yaw),
        -std::sin(pitch),
    };
    Normalize3(forward);
    if (impactDirection)
        std::memcpy(impactDirection, forward, sizeof(forward));

    float start[3] = {
        shooter.nativeState.origin[0], shooter.nativeState.origin[1],
        shooter.nativeState.origin[2]
            + std::clamp(shooter.state.viewHeight, 8.0f, 60.0f),
    };
    float end[3] = {
        start[0] + forward[0] * kRange,
        start[1] + forward[1] * kRange,
        start[2] + forward[2] * kRange,
    };
    const float rayDelta[3] = {
        end[0] - start[0], end[1] - start[1], end[2] - start[2],
    };
    ShipmentCollisionTrace worldTrace{};
    const bool worldHit = collision_
        && collision_->TraceSphere(start, end, 4.0f, 0xFFFFFFFFu, &worldTrace);
    float hitFraction = worldHit ? std::clamp(worldTrace.fraction, 0.0f, 1.0f) : 1.0f;
    uint32_t targetClient = UINT32_MAX;
    for (const ClientSlot& candidate : clients_) {
        if (!candidate.state.connected || !candidate.state.alive
            || candidate.state.clientNum == shooter.state.clientNum)
            continue;
        const float mins[3] = {
            candidate.state.origin[0] - 20.0f,
            candidate.state.origin[1] - 20.0f,
            candidate.state.origin[2] - 4.0f,
        };
        const float maxs[3] = {
            candidate.state.origin[0] + 20.0f,
            candidate.state.origin[1] + 20.0f,
            candidate.state.origin[2] + 76.0f,
        };
        float fraction = 1.0f;
        if (RayBoxFraction(start, rayDelta, mins, maxs, &fraction)
            && fraction <= hitFraction + 0.0001f) {
            hitFraction = fraction;
            targetClient = candidate.state.clientNum;
        }
    }
    if (impactPoint) {
        for (uint32_t axis = 0; axis < 3; ++axis)
            impactPoint[axis] = start[axis] + rayDelta[axis] * hitFraction;
    }
    if (targetClient != UINT32_MAX) {
        // This mirrors the default knife/melee scalar populated in the source
        // WeaponDef fallback record.  The compact Shipment table has no
        // separate melee damage field, so the shared source value is retained
        // here instead of deriving damage from the firearm playerDamage.
        ApplyDamage(targetClient, shooter.state.clientNum, 40, serverTime,
                    shooter.state.weaponSlot, impactPoint, forward);
    }
}

void ShipmentDmServer::SimulateGrenades(int32_t serverTime) {
    if (!collision_)
        return;
    const int32_t frameMsec = std::clamp(serverTime - lastServerTime_, 1, 100);
    const float frameSeconds = static_cast<float>(frameMsec) * 0.001f;
    constexpr float kGravity = 800.0f;
    constexpr float kRadius = 180.0f;
    constexpr float kInnerRadius = 48.0f;
    constexpr int32_t kFuseMsec = 1800;

    for (uint32_t clientIndex = 0; clientIndex < clients_.size(); ++clientIndex) {
        ClientSlot& grenade = clients_[clientIndex];
        if (!grenade.grenadeActive)
            continue;
        if (!grenade.state.connected || !grenade.state.alive) {
            grenade.grenadeActive = false;
            continue;
        }

        const float start[3] = {
            grenade.grenadeOrigin[0], grenade.grenadeOrigin[1], grenade.grenadeOrigin[2],
        };
        float end[3] = {
            start[0] + grenade.grenadeVelocity[0] * frameSeconds,
            start[1] + grenade.grenadeVelocity[1] * frameSeconds,
            start[2] + grenade.grenadeVelocity[2] * frameSeconds,
        };
        ShipmentCollisionTrace trace{};
        const bool hit = collision_->TraceSphere(start, end, 4.0f,
                                                  0xFFFFFFFFu, &trace);
        bool resting = false;
        if (hit && trace.fraction < 1.0f) {
            const float fraction = std::clamp(trace.fraction, 0.0f, 1.0f);
            for (uint32_t axis = 0; axis < 3; ++axis)
                grenade.grenadeOrigin[axis] = start[axis]
                    + (end[axis] - start[axis]) * fraction;
            const float velocityIntoSurface = Dot3(grenade.grenadeVelocity,
                                                   trace.normal);
            if (velocityIntoSurface < 0.0f) {
                for (uint32_t axis = 0; axis < 3; ++axis)
                    grenade.grenadeVelocity[axis] -=
                        1.85f * velocityIntoSurface * trace.normal[axis];
            }
            for (float& component : grenade.grenadeVelocity)
                component *= 0.45f;
            resting = trace.normal[2] >= 0.55f
                && Dot3(grenade.grenadeVelocity, grenade.grenadeVelocity) < 3600.0f;
        } else {
            std::memcpy(grenade.grenadeOrigin, end, sizeof(end));
        }
        grenade.grenadeVelocity[2] -= kGravity * frameSeconds;

        const int32_t age = serverTime - grenade.grenadeSpawnTime;
        if (age < 300)
            continue;
        if (age < kFuseMsec && (!resting || Dot3(grenade.grenadeVelocity,
                                                 grenade.grenadeVelocity) >= 3600.0f))
            continue;

        float explosionPoint[3] = {
            grenade.grenadeOrigin[0], grenade.grenadeOrigin[1], grenade.grenadeOrigin[2],
        };
        ShipmentDmEvent explosion{};
        explosion.type = ShipmentDmEventType::Grenade;
        explosion.clientNum = grenade.state.clientNum;
        explosion.attackerClientNum = grenade.state.clientNum;
        explosion.serverTime = serverTime;
        explosion.weapon = 4;
        explosion.action = 2;
        std::memcpy(explosion.point, explosionPoint, sizeof(explosionPoint));
        explosion.direction[0] = 0.0f;
        explosion.direction[1] = 0.0f;
        explosion.direction[2] = 1.0f;
        events_.push_back(explosion);

        for (ClientSlot& target : clients_) {
            if (!target.state.connected || !target.state.alive)
                continue;
            const float delta[3] = {
                target.state.origin[0] - explosionPoint[0],
                target.state.origin[1] - explosionPoint[1],
                target.state.origin[2] + 36.0f - explosionPoint[2],
            };
            const float distance = std::sqrt(Dot3(delta, delta));
            if (distance > kRadius)
                continue;
            const float falloff = distance <= kInnerRadius ? 1.0f
                : std::clamp((kRadius - distance) / (kRadius - kInnerRadius),
                             0.0f, 1.0f);
            const int32_t damage = std::clamp(
                static_cast<int32_t>(std::lround(30.0f + 70.0f * falloff)),
                1, 100);
            ApplyDamage(target.state.clientNum, grenade.state.clientNum, damage,
                        serverTime, 4, explosionPoint, explosion.direction);
        }
        grenade.grenadeActive = false;
        grenade.grenadeSpawnTime = 0;
        std::memset(grenade.grenadeVelocity, 0, sizeof(grenade.grenadeVelocity));
    }
}

void ShipmentDmServer::PublishSnapshot(const ClientSlot& slot, int32_t serverTime) {
    ShipmentSnapshotPacket packet{};
    packet.magic = kSnapshotMagic;
    packet.serverTime = serverTime;
    packet.clientNum = slot.state.clientNum;
    packet.team = slot.state.team;
    packet.flags = (slot.state.connected ? 1u : 0u) | (slot.state.alive ? 2u : 0u)
        | (slot.state.ads ? kSnapshotFlagAds : 0u)
        | (slot.state.firing ? kSnapshotFlagFiring : 0u)
        | (slot.state.reloading ? kSnapshotFlagReloading : 0u)
        | (slot.state.prone ? kSnapshotFlagProne : 0u)
        | (slot.state.crouched ? kSnapshotFlagCrouched : 0u)
        | (slot.state.sprinting ? kSnapshotFlagSprinting : 0u)
        | (slot.state.jumping ? kSnapshotFlagJumping : 0u)
        | (slot.state.melee ? kSnapshotFlagMelee : 0u)
        | (slot.state.grenade ? kSnapshotFlagGrenade : 0u);
    packet.health = slot.state.health;
    packet.score = slot.state.score;
    packet.deaths = slot.state.deaths;
    packet.weaponSlot = slot.state.weaponSlot;
    packet.weapAnim = slot.state.weapAnim;
    packet.clipAmmo = slot.state.clipAmmo;
    packet.reserveAmmo = slot.state.reserveAmmo;
    packet.nextFireTime = slot.state.nextFireTime;
    packet.reloadStartTime = slot.state.reloadStartTime;
    packet.reloadEndTime = slot.state.reloadEndTime;
    packet.respawnTime = slot.state.respawnTime;
    packet.weaponState = slot.state.weaponState;
    packet.grenadeCount = std::clamp(slot.state.grenadeCount, 0, 2);
    Copy3(packet.origin, slot.state.origin);
    Copy3(packet.velocity, slot.state.velocity);
    Copy3(packet.angles, slot.state.angles);
    packet.viewHeight = slot.state.viewHeight;
    packet.weaponPosFrac = std::clamp(slot.state.weaponPosFrac, 0.0f, 1.0f);
    VitaLoopback_SendSnapshot(slot.state.clientNum, &packet, sizeof(packet));
}

void ShipmentDmServer::PublishMatchState(int32_t serverTime) {
    ShipmentMatchPacket packet{};
    packet.magic = kMatchMagic;
    packet.serverTime = serverTime;
    packet.mode = static_cast<uint8_t>(match_.mode);
    packet.phase = static_cast<uint8_t>(match_.phase);
    packet.elapsedMsec = ElapsedSince(match_.elapsedMsec, 0,
                                      match_.timeLimitMsec);
    packet.timeLimitMsec = std::max(0, match_.timeLimitMsec);
    packet.scoreLimit = std::max(0, match_.scoreLimit);
    packet.killScore = std::max(0, match_.killScore);
    packet.winnerClientNum = match_.winnerClientNum;
    for (const ClientSlot& slot : clients_) {
        if (slot.state.connected)
            VitaLoopback_SendMatchState(slot.state.clientNum, &packet,
                                        sizeof(packet));
    }
}

void ShipmentDmServer::SimulatePlayer(ClientSlot* slot, int32_t serverTime) {
    if (!slot || !slot->state.alive)
        return;
    usercmd_s command = slot->pending;
    if (command.serverTime <= slot->nativeState.commandTime)
        command.serverTime = serverTime;
    if (command.weapon < 1 || command.weapon > 3)
        command.weapon = static_cast<uint8_t>(slot->state.weaponSlot ? slot->state.weaponSlot : 1);
    if (command.weapon != slot->state.weaponSlot) {
        const uint32_t previousWeapon = slot->state.weaponSlot;
        if (previousWeapon >= 1 && previousWeapon <= 3) {
            slot->weaponClipAmmo[previousWeapon] = slot->state.clipAmmo;
            slot->weaponReserveAmmo[previousWeapon] = slot->state.reserveAmmo;
        }
        slot->state.weaponSlot = command.weapon;
        slot->nativeState.weapon = command.weapon;
        slot->state.clipAmmo = slot->weaponClipAmmo[slot->state.weaponSlot];
        slot->state.reserveAmmo = slot->weaponReserveAmmo[slot->state.weaponSlot];
        slot->state.reloadStartTime = 0;
        slot->state.reloadEndTime = 0;
        slot->burstShotsRemaining = 0;
        slot->state.weapAnim = 0;
        slot->nativeState.weapAnim = 0;
    }
    const bool attackPressedEdge = (command.buttons & BUTTON_ATTACK) != 0
        && (slot->oldCommand.buttons & BUTTON_ATTACK) == 0;
    const bool fragPressedEdge = (command.buttons & BUTTON_FRAG) != 0
        && (slot->oldCommand.buttons & BUTTON_FRAG) == 0;
    const bool wasSprinting = slot->state.sprinting;
    const bool wasJumping = slot->state.jumping;
    const bool wasProne = slot->state.prone;
    const bool wasCrouched = slot->state.crouched;
    const int32_t oldWeaponState = slot->state.weaponState;
    const bool wasSourceReload = IsSourceReloadState(oldWeaponState);
    const int32_t grenadesBefore = slot->state.grenadeCount;
    if (slot->state.grenadeCount <= 0)
        command.buttons &= ~BUTTON_FRAG;
    if ((command.buttons & BUTTON_FRAG) != 0)
        command.offHandIndex = 4;
    else if (slot->state.grenadeCount > 0)
        command.offHandIndex = 4;
    slot->pending = command;
    slot->nativeState.weapon = command.weapon;
    slot->nativeState.pm_type = PM_NORMAL;
    slot->nativeState.otherFlags = POF_PLAYER;
    slot->nativeState.offHandIndex = slot->state.grenadeCount > 0 ? 4 : 0;
    slot->nativeState.weapons[0] = (1u << 1) | (1u << 2) | (1u << 3)
        | (slot->state.grenadeCount > 0 ? (1u << 4) : 0u);
    slot->nativeState.ammoclip[4] = slot->state.grenadeCount;
    slot->nativeState.ammo[4] = 0;
    slot->nativeState.ammoclip[slot->state.weaponSlot] = slot->state.clipAmmo;
    slot->nativeState.ammo[slot->state.weaponSlot] = slot->state.reserveAmmo;
    const int32_t sourceClipBefore = slot->nativeState.ammoclip[slot->state.weaponSlot];
    const ShipmentWeaponDefinition* movementWeapon = weapons_
        ? weapons_->ForSlot(slot->state.weaponSlot) : nullptr;
    VitaPmoveWeaponConfig movementConfig{};
    movementConfig.weapon = slot->state.weaponSlot;
    if (movementWeapon) {
        movementConfig.fireType = movementWeapon->fireType;
        movementConfig.startAmmo = movementWeapon->startAmmo;
        movementConfig.maxAmmo = movementWeapon->maxAmmo;
        movementConfig.clipSize = movementWeapon->clipSize;
        movementConfig.damage = movementWeapon->damage;
        movementConfig.playerDamage = movementWeapon->playerDamage;
        movementConfig.fireDelay = movementWeapon->fireDelay;
        movementConfig.fireTime = movementWeapon->fireTime;
        movementConfig.rechamberTime = movementWeapon->rechamberTime;
        movementConfig.reloadTime = movementWeapon->reloadTime;
        movementConfig.reloadEmptyTime = movementWeapon->reloadEmptyTime;
        movementConfig.moveSpeedScale = movementWeapon->moveSpeedScale;
        movementConfig.adsMoveSpeedScale = movementWeapon->adsMoveSpeedScale;
        movementConfig.sprintDurationScale = movementWeapon->sprintDurationScale;
        movementConfig.adsTransInTime = movementWeapon->adsTransInTime;
        movementConfig.adsTransOutTime = movementWeapon->adsTransOutTime;
        movementConfig.freezeMovementWhenFiring = movementWeapon->freezeMovementWhenFiring;
    }
    VitaPmove_SetWeaponConfig(&movementConfig);
    VitaPmove_Run(&slot->nativeState, &command, &slot->oldCommand);
    const int32_t sourceClipAfter = slot->nativeState.ammoclip[slot->state.weaponSlot];
    const bool sourceFired = sourceClipAfter < sourceClipBefore;
    // The source off-hand state owns grenade inventory consumption.  The
    // authored state can advance past WEAPON_OFFHAND in the same Pmove call,
    // so key the compact projectile spawn off the actual clip delta as well
    // as the visible throw state below.  This keeps the Vita projectile and
    // explosion path attached to PM_Weapon instead of guessing from input.
    const int32_t sourceGrenadesAfter = slot->nativeState.ammoclip[4];
    const bool sourceGrenadeConsumed = sourceGrenadesAfter < grenadesBefore;
    const int32_t sourceWeaponState = static_cast<int32_t>(
        slot->nativeState.weaponstate);
    const bool sourceReload = IsSourceReloadState(sourceWeaponState);
    slot->oldCommand = command;
    Copy3(slot->state.origin, slot->nativeState.origin);
    Copy3(slot->state.velocity, slot->nativeState.velocity);
    Copy3(slot->state.angles, slot->nativeState.viewangles);
    slot->state.ads = (slot->nativeState.pm_flags & PMF_SIGHT_AIMING) != 0;
    slot->state.prone = (slot->nativeState.pm_flags & PMF_PRONE) != 0;
    slot->state.crouched = (slot->nativeState.pm_flags & PMF_DUCKED) != 0;
    slot->state.sprinting = (slot->nativeState.pm_flags & PMF_SPRINTING) != 0;
    slot->state.jumping = (slot->nativeState.pm_flags & PMF_JUMPING) != 0;
    slot->state.weaponState = sourceWeaponState;
    slot->state.melee = slot->state.weaponState >= WEAPON_MELEE_INIT
        && slot->state.weaponState <= WEAPON_MELEE_END;
    slot->state.grenade = slot->state.weaponState >= WEAPON_OFFHAND_INIT
        && slot->state.weaponState <= WEAPON_OFFHAND_END;
    if (sourceReload) {
        // Keep the compact HUD/audio clock aligned with the source reload
        // states.  The source playerstate owns the actual magazine transfer;
        // these timestamps only describe the same authored animation window
        // to the Vita presentation layer.
        slot->state.reloading = true;
        if (!wasSourceReload) {
            const bool emptyReload = sourceClipBefore <= 0;
            const int32_t duration = movementWeapon
                ? (emptyReload ? movementWeapon->reloadEmptyTime
                               : movementWeapon->reloadTime)
                : (emptyReload ? kReloadDuration : kReloadDuration);
            slot->state.reloadStartTime = serverTime;
            slot->state.reloadEndTime = serverTime + std::max(1, duration);
        }
    } else if (wasSourceReload) {
        slot->state.reloading = false;
        slot->state.reloadStartTime = 0;
        slot->state.reloadEndTime = 0;
    }
    // Source Pmove is authoritative for ammo consumption (including the
    // offhand clip).  Keep the compact per-slot mirrors synchronized before
    // the renderer or custom hit resolver sees the frame.
    slot->state.clipAmmo = std::max(0, slot->nativeState.ammoclip[slot->state.weaponSlot]);
    slot->state.reserveAmmo = std::max(0, slot->nativeState.ammo[slot->state.weaponSlot]);
    slot->state.grenadeCount = std::clamp(slot->nativeState.ammoclip[4], 0, 2);
    slot->weaponClipAmmo[slot->state.weaponSlot] = slot->state.clipAmmo;
    slot->weaponReserveAmmo[slot->state.weaponSlot] = slot->state.reserveAmmo;
    slot->state.weapAnim = slot->nativeState.weapAnim;
    slot->state.weaponPosFrac = std::clamp(slot->nativeState.fWeaponPosFrac,
                                           0.0f, 1.0f);
    slot->state.viewHeight = std::clamp(slot->nativeState.viewHeightCurrent, 8.0f, 60.0f);
    if (slot->state.jumping && !wasJumping) {
        ShipmentDmEvent event{};
        event.type = ShipmentDmEventType::Jump;
        event.clientNum = slot->state.clientNum;
        event.serverTime = serverTime;
        event.action = 1;
        events_.push_back(event);
    }
    if (slot->state.sprinting != wasSprinting) {
        ShipmentDmEvent event{};
        event.type = ShipmentDmEventType::Sprint;
        event.clientNum = slot->state.clientNum;
        event.serverTime = serverTime;
        event.action = slot->state.sprinting ? 1u : 2u;
        events_.push_back(event);
    }
    if (slot->state.prone != wasProne || slot->state.crouched != wasCrouched) {
        ShipmentDmEvent event{};
        event.type = ShipmentDmEventType::Stance;
        event.clientNum = slot->state.clientNum;
        event.serverTime = serverTime;
        event.action = 1;
        event.point[0] = slot->state.prone ? 1.0f
            : (slot->state.crouched ? 2.0f : 0.0f);
        events_.push_back(event);
    }
    if (slot->state.weaponState == WEAPON_MELEE_FIRE
        && oldWeaponState != WEAPON_MELEE_FIRE) {
        float impactPoint[3] = {0.0f, 0.0f, 0.0f};
        float impactDirection[3] = {0.0f, 0.0f, 1.0f};
        ResolveMelee(*slot, serverTime, impactPoint, impactDirection);
        ShipmentDmEvent event{};
        event.type = ShipmentDmEventType::Melee;
        event.clientNum = slot->state.clientNum;
        event.serverTime = serverTime;
        event.attackerClientNum = slot->state.clientNum;
        event.weapon = slot->state.weaponSlot;
        event.action = 1;
        std::memcpy(event.point, impactPoint, sizeof(event.point));
        std::memcpy(event.direction, impactDirection, sizeof(event.direction));
        events_.push_back(event);
    }
    const auto SpawnGrenade = [&](bool sourceConsumed) {
        if (slot->grenadeActive || grenadesBefore <= 0)
            return;
        if (!sourceConsumed) {
            // The reduced engine/probe targets retain a weak PM_Weapon. Keep
            // their gameplay contract useful without replacing the source
            // path in the bootstrap: consume the same offhand inventory unit
            // and let the next source-capable frame continue from the
            // compact state.
            slot->state.grenadeCount = std::max(0, slot->state.grenadeCount - 1);
            slot->nativeState.ammoclip[4] = slot->state.grenadeCount;
        }
        // PM_Weapon_OffHand has already consumed one source clip unit when
        // sourceConsumed is true. Spawn the bounded projectile only at the
        // source throw state so the authored prepare/hold/throw animation
        // sequence stays visible.
        const float pitch = slot->nativeState.viewangles[0]
            * 3.14159265358979323846f / 180.0f;
        const float yaw = slot->nativeState.viewangles[1]
            * 3.14159265358979323846f / 180.0f;
        float direction[3] = {
            std::cos(pitch) * std::cos(yaw),
            std::cos(pitch) * std::sin(yaw),
            -std::sin(pitch),
        };
        Normalize3(direction);
        slot->grenadeActive = true;
        slot->grenadeSpawnTime = serverTime;
        slot->grenadeOrigin[0] = slot->nativeState.origin[0] + direction[0] * 12.0f;
        slot->grenadeOrigin[1] = slot->nativeState.origin[1] + direction[1] * 12.0f;
        slot->grenadeOrigin[2] = slot->nativeState.origin[2]
            + std::clamp(slot->state.viewHeight, 8.0f, 60.0f)
            + direction[2] * 12.0f;
        slot->grenadeVelocity[0] = direction[0] * 700.0f;
        slot->grenadeVelocity[1] = direction[1] * 700.0f;
        slot->grenadeVelocity[2] = direction[2] * 700.0f + 180.0f;
        ShipmentDmEvent event{};
        event.type = ShipmentDmEventType::Grenade;
        event.clientNum = slot->state.clientNum;
        event.serverTime = serverTime;
        event.attackerClientNum = slot->state.clientNum;
        event.weapon = 4;
        event.action = 1;
        std::memcpy(event.point, slot->grenadeOrigin, sizeof(event.point));
        std::memcpy(event.direction, direction, sizeof(event.direction));
        events_.push_back(event);
    };
    if (sourceGrenadeConsumed
        || (slot->state.weaponState == WEAPON_OFFHAND
            && oldWeaponState != WEAPON_OFFHAND))
        SpawnGrenade(true);
    else if (fragPressedEdge && grenadesBefore > 0
             && slot->state.weaponState < WEAPON_OFFHAND_INIT)
        SpawnGrenade(false);
    HandleWeapon(slot, serverTime, attackPressedEdge, sourceFired);
    Copy3(slot->state.angles, slot->nativeState.viewangles);
}

void ShipmentDmServer::Tick(int32_t serverTime) {
    if (!initialized_)
        return;
    const int32_t effectiveServerTime = std::max(serverTime, lastServerTime_);

    // The first authoritative tick is the DM start boundary.  Connecting a
    // client only allocates/spawns it; the retail gametype clock starts once
    // the match is actually running, which keeps the loading route from
    // consuming the ten-minute limit.
    if (!matchStarted_ && connectedClients_ != 0) {
        matchStarted_ = true;
        matchStartServerTime_ = effectiveServerTime;
        match_.phase = ShipmentDmMatchPhase::Active;
        match_.elapsedMsec = 0;
        match_.winnerClientNum = UINT32_MAX;
    }
    if (matchStarted_) {
        const int32_t elapsed = ElapsedSince(
            effectiveServerTime, matchStartServerTime_, match_.timeLimitMsec);
        match_.elapsedMsec = elapsed;
        if (match_.phase == ShipmentDmMatchPhase::Active
            && match_.timeLimitMsec > 0
            && elapsed >= match_.timeLimitMsec) {
            match_.phase = ShipmentDmMatchPhase::Complete;
            // At the time limit the source scoreboard winner is the highest
            // DM score.  A tie intentionally reports no winner so the
            // presentation layer can show the normal tie/intermission state.
            int32_t bestScore = -1;
            uint32_t winner = UINT32_MAX;
            bool tied = false;
            for (const ClientSlot& slot : clients_) {
                if (!slot.state.connected)
                    continue;
                if (slot.state.score > bestScore) {
                    bestScore = slot.state.score;
                    winner = slot.state.clientNum;
                    tied = false;
                } else if (slot.state.score == bestScore) {
                    tied = true;
                }
            }
            match_.winnerClientNum = tied ? UINT32_MAX : winner;
        }
    }
    VitaPmove_SetCollision(collision_);

    // Keep the local client/server boundary packet-shaped.  Several input
    // frames may arrive before a server tick; the newest valid command wins,
    // matching the usual snapshot server behavior while retaining bounded
    // queue memory on Vita.
    for (ClientSlot& slot : clients_) {
        if (!slot.state.connected)
            continue;
        ShipmentCommandPacket packet{};
        uint32_t length = 0;
        while (VitaLoopback_ReceiveCommand(slot.state.clientNum, &packet, sizeof(packet), &length)) {
            if (length != sizeof(packet) || packet.magic != kCommandMagic
                || packet.clientNum != slot.state.clientNum)
                continue;
            slot.pending = packet.command;
            slot.hasCommand = true;
        }
    }
    // Projectiles are advanced once per authoritative server tick, outside
    // the player loop, so a thrown frag continues to simulate while the
    // owner changes stance, swaps weapons, or dies.
    if (match_.phase != ShipmentDmMatchPhase::Active) {
        // Preserve the last authoritative player snapshot for an intermission
        // frame, but stop gameplay mutations once the DM has ended.
        PublishMatchState(effectiveServerTime);
        for (const ClientSlot& slot : clients_) {
            if (slot.state.connected)
                PublishSnapshot(slot, effectiveServerTime);
        }
        lastServerTime_ = effectiveServerTime;
        return;
    }
    SimulateGrenades(effectiveServerTime);
    for (ClientSlot& slot : clients_) {
        if (!slot.state.connected)
            continue;
        if (!slot.state.alive) {
            if (slot.state.respawnTime != 0
                && effectiveServerTime >= slot.state.respawnTime)
                Spawn(&slot, effectiveServerTime, true);
            else
                slot.hasCommand = false;
            PublishSnapshot(slot, effectiveServerTime);
            continue;
        }
        if (!slot.hasCommand) {
            slot.pending = usercmd_s{};
            slot.pending.serverTime = effectiveServerTime;
        }
        SimulatePlayer(&slot, effectiveServerTime);
        PublishSnapshot(slot, effectiveServerTime);
        slot.hasCommand = false;
    }
    PublishMatchState(effectiveServerTime);
    lastServerTime_ = effectiveServerTime;
}

const ShipmentDmPlayerState* ShipmentDmServer::Player(uint32_t clientNum) const {
    if (clientNum >= clients_.size() || !clients_[clientNum].state.connected)
        return nullptr;
    return &clients_[clientNum].state;
}

bool ShipmentDmServer::CopyGrenade(uint32_t clientNum,
                                    ShipmentDmProjectileState* output) const {
    if (!output || clientNum >= clients_.size()
        || !clients_[clientNum].state.connected)
        return false;
    const ClientSlot& slot = clients_[clientNum];
    *output = ShipmentDmProjectileState{};
    output->active = slot.grenadeActive;
    output->ownerClientNum = slot.state.clientNum;
    output->spawnTime = slot.grenadeSpawnTime;
    std::memcpy(output->origin, slot.grenadeOrigin, sizeof(output->origin));
    std::memcpy(output->velocity, slot.grenadeVelocity, sizeof(output->velocity));
    return true;
}

bool ShipmentDmClient::Connect(ShipmentDmServer* server) {
    Disconnect();
    if (!server)
        return false;
    const uint32_t client = server->ConnectLocalClient();
    if (client == UINT32_MAX)
        return false;
    server_ = server;
    clientNum_ = client;
    snapshot_ = ShipmentDmPlayerState{};
    match_ = ShipmentDmMatchState{};
    hasSnapshot_ = false;
    hasMatch_ = false;
    return true;
}

void ShipmentDmClient::Disconnect() {
    server_ = nullptr;
    clientNum_ = 0;
    snapshot_ = ShipmentDmPlayerState{};
    match_ = ShipmentDmMatchState{};
    hasSnapshot_ = false;
    hasMatch_ = false;
}

bool ShipmentDmClient::Frame(const usercmd_s& command) {
    if (!server_)
        return false;
    ShipmentCommandPacket packet{};
    packet.magic = kCommandMagic;
    packet.clientNum = clientNum_;
    packet.command = command;
    return VitaLoopback_SendCommand(clientNum_, &packet, sizeof(packet));
}

const ShipmentDmPlayerState* ShipmentDmClient::Snapshot() const {
    const_cast<ShipmentDmClient*>(this)->PollSnapshot();
    return server_ && hasSnapshot_ ? &snapshot_ : nullptr;
}

bool ShipmentDmClient::CopyMatchState(ShipmentDmMatchState* output) const {
    const_cast<ShipmentDmClient*>(this)->PollSnapshot();
    if (!output || !server_ || !hasMatch_)
        return false;
    *output = match_;
    return true;
}

void ShipmentDmClient::PollSnapshot() {
    if (!server_)
        return;
    ShipmentMatchPacket matchPacket{};
    uint32_t matchLength = 0;
    while (VitaLoopback_ReceiveMatchState(clientNum_, &matchPacket,
                                          sizeof(matchPacket), &matchLength)) {
        if (matchLength != sizeof(matchPacket) || matchPacket.magic != kMatchMagic)
            continue;
        if (matchPacket.mode != static_cast<uint8_t>(ShipmentDmGameMode::Deathmatch)
            || matchPacket.phase > static_cast<uint8_t>(ShipmentDmMatchPhase::Complete))
            continue;
        match_.mode = static_cast<ShipmentDmGameMode>(matchPacket.mode);
        match_.phase = static_cast<ShipmentDmMatchPhase>(matchPacket.phase);
        match_.timeLimitMsec = std::max(0, matchPacket.timeLimitMsec);
        match_.scoreLimit = std::max(0, matchPacket.scoreLimit);
        match_.killScore = std::max(0, matchPacket.killScore);
        match_.elapsedMsec = ElapsedSince(matchPacket.elapsedMsec, 0,
                                          match_.timeLimitMsec);
        match_.winnerClientNum = matchPacket.winnerClientNum;
        hasMatch_ = true;
    }
    ShipmentSnapshotPacket packet{};
    uint32_t length = 0;
    while (VitaLoopback_ReceiveSnapshot(clientNum_, &packet, sizeof(packet), &length)) {
        if (length != sizeof(packet) || packet.magic != kSnapshotMagic
            || packet.clientNum != clientNum_)
            continue;
        snapshot_.clientNum = packet.clientNum;
        snapshot_.team = packet.team;
        snapshot_.connected = (packet.flags & 1u) != 0;
        snapshot_.alive = (packet.flags & 2u) != 0;
        snapshot_.health = packet.health;
        snapshot_.score = packet.score;
        snapshot_.deaths = packet.deaths;
        snapshot_.weaponSlot = packet.weaponSlot;
        snapshot_.weapAnim = packet.weapAnim;
        snapshot_.clipAmmo = packet.clipAmmo;
        snapshot_.reserveAmmo = packet.reserveAmmo;
        snapshot_.nextFireTime = packet.nextFireTime;
        snapshot_.reloadStartTime = packet.reloadStartTime;
        snapshot_.reloadEndTime = packet.reloadEndTime;
        snapshot_.respawnTime = packet.respawnTime;
        snapshot_.weaponState = packet.weaponState;
        snapshot_.grenadeCount = std::clamp(packet.grenadeCount, 0, 2);
        snapshot_.ads = (packet.flags & kSnapshotFlagAds) != 0;
        snapshot_.firing = (packet.flags & kSnapshotFlagFiring) != 0;
        snapshot_.reloading = (packet.flags & kSnapshotFlagReloading) != 0;
        snapshot_.prone = (packet.flags & kSnapshotFlagProne) != 0;
        snapshot_.crouched = (packet.flags & kSnapshotFlagCrouched) != 0;
        snapshot_.sprinting = (packet.flags & kSnapshotFlagSprinting) != 0;
        snapshot_.jumping = (packet.flags & kSnapshotFlagJumping) != 0;
        snapshot_.melee = (packet.flags & kSnapshotFlagMelee) != 0;
        snapshot_.grenade = (packet.flags & kSnapshotFlagGrenade) != 0;
        snapshot_.weaponPosFrac = std::clamp(packet.weaponPosFrac, 0.0f, 1.0f);
        snapshot_.viewHeight = std::clamp(packet.viewHeight, 8.0f, 60.0f);
        snapshot_.muzzleFlashEndTime = snapshot_.firing ? packet.serverTime + 65 : 0;
        Copy3(snapshot_.origin, packet.origin);
        Copy3(snapshot_.velocity, packet.velocity);
        Copy3(snapshot_.angles, packet.angles);
        hasSnapshot_ = true;
    }
}

} // namespace kisak::vita
