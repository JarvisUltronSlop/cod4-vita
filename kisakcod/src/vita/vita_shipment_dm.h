#pragma once

#include <cstdint>
#include <vector>

#include <qcommon/msg_mp.h>

#include <vita/vita_shipment_collision.h>
#include <vita/vita_shipment_entities.h>
#include <vita/vita_shipment_weapons.h>
#include <vita/vita_pmove_services.h>
#include <vita/vita_dm_rules.h>

namespace kisak::vita {

// The bounded Shipment server intentionally exposes one source ruleset for
// now. Keeping the mode and match clock explicit at the server boundary makes
// the HUD consume authoritative DM state instead of assuming a generic lobby
// timer while additional retail playlists remain disabled in the frontend.
enum class ShipmentDmGameMode : uint8_t {
    Deathmatch = 0,
};

enum class ShipmentDmMatchPhase : uint8_t {
    Waiting = 0,
    Active,
    Complete,
};

// Match metadata is server-owned and intentionally separate from a player
// snapshot.  The desktop DM script keeps the score limit/time limit at the
// gametype level; exposing the same values here lets the Vita HUD, scoreboard,
// and future network transport consume one authoritative state.
struct ShipmentDmMatchState {
    ShipmentDmGameMode mode = ShipmentDmGameMode::Deathmatch;
    ShipmentDmMatchPhase phase = ShipmentDmMatchPhase::Waiting;
    int32_t elapsedMsec = 0;
    int32_t timeLimitMsec = kShipmentDmTimeLimitMsec;
    int32_t scoreLimit = kShipmentDmScoreLimit;
    int32_t killScore = kShipmentDmKillScore;
    uint32_t winnerClientNum = UINT32_MAX;
};

enum class ShipmentDmEventType : uint8_t {
    Spawn,
    Fire,
    Reload,
    Respawn,
    Damage,
    Death,
    Kill,
    // Keep the original event ordinals stable; these source-action events
    // are appended so older script consumers still decode Spawn..Kill.
    Jump,
    Sprint,
    Stance,
    Melee,
    Grenade,
};

struct ShipmentDmEvent {
    ShipmentDmEventType type = ShipmentDmEventType::Spawn;
    uint32_t clientNum = 0;
    int32_t serverTime = 0;
    uint32_t attackerClientNum = UINT32_MAX;
    int32_t damage = 0;
    uint32_t weapon = 0;
    // Source action discriminator used by the bounded Vita event bridge:
    // 1=start/throw, 2=finish/explode, 0=state notification.
    uint32_t action = 0;
    float point[3] = {0.0f, 0.0f, 0.0f};
    float direction[3] = {0.0f, 0.0f, 1.0f};
};

// A compact server-owned state used until the full desktop entity snapshot
// transport is replaced by Vita-native services.  It deliberately mirrors the
// MP rules that matter to a local Shipment slice: spawn, movement, ammo,
// reload, fire cadence, and event delivery.
struct ShipmentDmPlayerState {
    uint32_t clientNum = 0;
    uint32_t team = 0;
    bool connected = false;
    bool alive = false;
    float origin[3] = {0.0f, 0.0f, 0.0f};
    float velocity[3] = {0.0f, 0.0f, 0.0f};
    float angles[3] = {0.0f, 0.0f, 0.0f};
    int32_t health = 100;
    int32_t score = 0;
    int32_t deaths = 0;
    uint32_t weaponSlot = 0;
    // Mirrors playerState_s::weapAnim. This is the authored source animation
    // command consumed by CG_WeaponRunXModelAnims, not a Vita clip id.
    int32_t weapAnim = 0;
    int32_t clipAmmo = 30;
    int32_t reserveAmmo = 90;
    int32_t nextFireTime = 0;
    int32_t reloadStartTime = 0;
    int32_t reloadEndTime = 0;
    int32_t respawnTime = 0;
    bool ads = false;
    bool firing = false;
    bool reloading = false;
    bool prone = false;
    bool crouched = false;
    bool sprinting = false;
    bool jumping = false;
    bool melee = false;
    bool grenade = false;
    // Mirrors playerState_s::weaponstate so the renderer can select the
    // authored source animation slot for melee/offhand/sprint transitions.
    int32_t weaponState = 0;
    int32_t grenadeCount = 2;
    // Shared Pmove's fWeaponPosFrac is the source of truth for the ADS
    // transition.  Keep the fractional value alongside the compact flags so
    // the Vita camera/viewmodel can follow CG_GetViewFov instead of snapping
    // between hip and zoom as a boolean.
    float weaponPosFrac = 0.0f;
    float viewHeight = 60.0f;
    int32_t muzzleFlashEndTime = 0;
};

// Authoritative projectile state exposed to the presentation bridge.  The
// server remains the owner of the grenade simulation; exposing a compact copy
// lets both the scalar Vita3K renderer and the accelerated Vita renderer draw
// the same in-flight grenade without re-simulating a second, divergent arc.
struct ShipmentDmProjectileState {
    bool active = false;
    uint32_t ownerClientNum = UINT32_MAX;
    int32_t spawnTime = 0;
    float origin[3] = {0.0f, 0.0f, 0.0f};
    float velocity[3] = {0.0f, 0.0f, 0.0f};
};

class ShipmentDmServer {
public:
    bool Initialize(const ShipmentCollision* collision,
                    const ShipmentMapEntities* entities,
                    const ShipmentWeaponTable* weapons = nullptr,
                    uint32_t maxClients = 1,
                    const ShipmentDmRules* rules = nullptr);
    void Reset();

    uint32_t ConnectLocalClient();
    bool SubmitUsercmd(uint32_t clientNum, const usercmd_s& command);
    bool ApplyDamage(uint32_t targetClientNum, uint32_t attackerClientNum,
                     int32_t damage, int32_t serverTime, uint32_t weapon = 0,
                     const float* point = nullptr, const float* direction = nullptr);
    void Tick(int32_t serverTime);

    const ShipmentDmPlayerState* Player(uint32_t clientNum) const;
    bool CopyGrenade(uint32_t clientNum, ShipmentDmProjectileState* output) const;
    const std::vector<ShipmentDmEvent>& Events() const { return events_; }
    void ClearEvents() { events_.clear(); }
    uint32_t ConnectedClients() const { return connectedClients_; }
    bool Ready() const { return initialized_; }
    ShipmentDmGameMode GameMode() const { return match_.mode; }
    int32_t MatchTimeLimitMsec() const { return match_.timeLimitMsec; }
    int32_t MatchScoreLimit() const { return match_.scoreLimit; }
    int32_t MatchKillScore() const { return match_.killScore; }
    // MatchServerTime is deliberately start-relative for the HUD timer;
    // player snapshots/events retain the absolute simulation timestamp.
    int32_t MatchServerTime() const { return match_.elapsedMsec; }
    ShipmentDmMatchPhase MatchPhase() const { return match_.phase; }
    bool MatchActive() const { return match_.phase == ShipmentDmMatchPhase::Active; }
    bool MatchFinished() const { return match_.phase == ShipmentDmMatchPhase::Complete; }
    uint32_t MatchWinnerClientNum() const { return match_.winnerClientNum; }
    const ShipmentDmMatchState& Match() const { return match_; }

private:
    struct ClientSlot {
        ShipmentDmPlayerState state{};
        playerState_s nativeState{};
        usercmd_s pending{};
        usercmd_s oldCommand{};
        // COD4 keeps ammunition per carried weapon.  Keep the bounded Vita
        // slice's three authored slots independent so swapping cannot mint a
        // fresh magazine/reserve pair.
        int32_t weaponClipAmmo[4] = {};
        int32_t weaponReserveAmmo[4] = {};
        uint32_t shotSequence = 0;
        uint8_t burstShotsRemaining = 0;
        bool grenadeActive = false;
        int32_t grenadeSpawnTime = 0;
        float grenadeOrigin[3] = {0.0f, 0.0f, 0.0f};
        float grenadeVelocity[3] = {0.0f, 0.0f, 0.0f};
        bool hasCommand = false;
    };

    void Spawn(ClientSlot* slot, int32_t serverTime, bool respawn);
    void SimulatePlayer(ClientSlot* slot, int32_t serverTime);
    void HandleWeapon(ClientSlot* slot, int32_t serverTime, bool attackPressedEdge,
                      bool sourceFired);
    void ResolveMelee(const ClientSlot& shooter, int32_t serverTime,
                      float* impactPoint, float* impactDirection);
    void SimulateGrenades(int32_t serverTime);
    void ResolveHitscan(const ClientSlot& shooter, const ShipmentWeaponDefinition* weapon,
                       int32_t serverTime, float* shotPoint, float* shotDirection);
    void PublishMatchState(int32_t serverTime);
    void PublishSnapshot(const ClientSlot& slot, int32_t serverTime);

    const ShipmentCollision* collision_ = nullptr;
    const ShipmentMapEntities* entities_ = nullptr;
    const ShipmentWeaponTable* weapons_ = nullptr;
    std::vector<ClientSlot> clients_;
    std::vector<ShipmentDmEvent> events_;
    uint32_t connectedClients_ = 0;
    uint32_t nextSpawn_ = 0;
    int32_t lastServerTime_ = 0;
    int32_t matchStartServerTime_ = 0;
    ShipmentDmMatchState match_{};
    bool matchStarted_ = false;
    bool initialized_ = false;
    ShipmentDmRules rules_{};
};

// The client owns only input and a last acknowledged snapshot.  The server
// remains authoritative even for the offline one-process slice, so the same
// usercmd path is ready for later Vita UDP transport without changing input
// semantics.
class ShipmentDmClient {
public:
    bool Connect(ShipmentDmServer* server);
    void Disconnect();
    bool Frame(const usercmd_s& command);
    const ShipmentDmPlayerState* Snapshot() const;
    // Poll and copy the latest authoritative DM match metadata delivered on
    // the local client lane.  The caller owns the destination, and the
    // server remains the fallback only before the first match packet arrives.
    bool CopyMatchState(ShipmentDmMatchState* output) const;
    uint32_t ClientNum() const { return clientNum_; }
    bool Connected() const { return server_ != nullptr; }

private:
    void PollSnapshot();

    ShipmentDmServer* server_ = nullptr;
    uint32_t clientNum_ = 0;
    ShipmentDmPlayerState snapshot_{};
    ShipmentDmMatchState match_{};
    bool hasSnapshot_ = false;
    bool hasMatch_ = false;
};

} // namespace kisak::vita
