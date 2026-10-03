#pragma once

#include <cstdint>

#include <vita/vita_source_platform.h>
#include <vita/vita_source_registry.h>
#include <vita/vita_shipment_dm.h>

namespace kisak::vita {

class ShipmentWeaponTable;
class ShipmentCollision;
class ShipmentMapEntities;
class VitaAnimationBundle;
class VitaEffectBundle;
class VitaImpactFxBundle;
class VitaImageBundle;
class VitaFontBundle;
class VitaMaterialBundle;
class VitaModelBundle;
class VitaRawFileBundle;
class VitaStringTableBundle;
class VitaSoundBundle;

enum class VitaSourceSessionState : uint8_t {
    Cold = 0,
    Initialized,
    Loading,
    Connecting,
    Active,
    Paused,
    Failed,
};

struct VitaSourceSessionStatus {
    VitaSourceSessionState state = VitaSourceSessionState::Cold;
    char mapName[32] = {};
    char error[192] = {};
    uint32_t serverTicks = 0;
    uint32_t loopbackPackets = 0;
    uint32_t connectionElapsedMs = 0;
    int32_t clientConnectionState = 0;
    int32_t serverClientState = 0;
    uint32_t connectPacketCount = 0;
    uint32_t serverMessageSequence = 0;
    uint32_t frameServerTicks = 0, backlogMs = 0;
    uint64_t clFrameUsec = 0, cgameUsec = 0;
    uint64_t serverUsec = 0;
    uint64_t clientUsec = 0;
    uint64_t loopbackUsec = 0;
    uint64_t presentationUsec = 0;
    uint64_t audioUsec = 0;
    uint64_t runtimeScriptErrors = 0;
    uint64_t completedClientFrames = 0;
    bool initialPlayerSpawned = false;
    char handModelName[64]{};
};

// Owns one source server + one source client. The caller owns map assets,
// renderer and audio, and must invoke every method from the main thread.
class VitaSourceSession {
public:
    static constexpr uint32_t kServerTickMs = 50; // original sv_fps=20
    static constexpr uint32_t kConnectionTimeoutMs = 30000;
    // An ordinary hitch is simulated in full. This is an error boundary for
    // an unreported minute-long platform interruption, never a time clamp.
    static constexpr uint32_t kMaximumRunningFrameMs = 60000;

    bool Initialize(VitaAssetRegistry* registry,
                    const VitaSourcePlatformHooks& services,
                    uint32_t worldChecksum,
                    const char* fastfileNames,
                    const char* fastfileChecksums);
    bool MountSourceModelAssets(const VitaImageBundle& images,
                                const VitaMaterialBundle& materials,
                                const VitaModelBundle& models);
    bool MountSourceData(const VitaRawFileBundle& rawfiles,
                         const VitaStringTableBundle& stringTables);
    bool MountSourceSounds(const VitaSoundBundle& sounds);
    bool MountSourceFonts(const VitaFontBundle& fonts);
    // Borrows immutable compressed arrays. The caller must keep this bundle
    // unchanged and alive until Shutdown() and the registry's map reset.
    bool MountSourceAnimations(const VitaAnimationBundle& animations);
    bool MountSourceEffects(const VitaEffectBundle& effects,
                            const VitaImpactFxBundle& impacts);
    bool MountSourceWeapons(const ShipmentWeaponTable& weapons);
    bool MountMapRoots(const char* mapName,
                       const ShipmentCollision& collision,
                       const ShipmentMapEntities& entities,
                       uint32_t checksum);
    bool StartMap(const char* mapName);
    // The source owner can service server deadlines between client updates.
    // elapsedMs advances the same monotonic source clock in both cases.
    bool Frame(uint32_t elapsedMs, bool advanceClient = true);
    bool SetPaused(bool paused);
    bool SetScoreboardVisible(bool visible);
    bool CopyPlayerPresentation(ShipmentDmPlayerState* output) const;
    bool CopyMatchPresentation(ShipmentDmMatchState* output) const;
    bool CopyGrenadePresentation(ShipmentDmProjectileState* output) const;
    void Stop();
    void Shutdown();
    bool DiagnosticInputComplete() const;
    // GPU-test setup only, on the source owner after original FFA spawn.
    // Uses the first authored deathmatch spawn and original teleport protocol.
    bool PlaceGpuDiagnosticPlayer(const ShipmentMapEntities& entities);
    bool GpuDiagnosticPlayerReady(const ShipmentMapEntities& entities) const;

    const VitaSourceSessionStatus& Status() const { return status_; }
    bool SoundActive() const { return sourceSoundInitialized_; }
    bool Active() const {
        return status_.state == VitaSourceSessionState::Active;
    }

private:
    uint32_t PumpLoopback();
    void UpdatePresentation();
    void AdvanceInitialJoin();
    bool SourceBoundarySucceeded(const char* stage);
    bool Fail(const char* message);
    static bool NormalizeMapName(const char* input, char output[32]);

    VitaAssetRegistry* registry_ = nullptr;
    VitaSourceSessionStatus status_{};
    uint32_t tickAccumulatorMs_ = 0;
    uint32_t clientAccumulatorMs_ = 0;
    uint32_t cachedFragIndex_ = 0;
    uint32_t cachedWeaponCount_ = UINT32_MAX;
    uint64_t runtimeErrorBaseline_ = 0;
    uint32_t initialJoinElapsedMs_ = 0;
    uint8_t initialJoinStage_ = 0;
    bool serverRootInitialized_ = false;
    bool clientRootInitialized_ = false;
    bool sourceStringsInitialized_ = false;
    bool sourceDvarsInitialized_ = false;
    bool sourceScriptInitialized_ = false;
    bool sourceXAnimInitialized_ = false;
    bool sourceDObjInitialized_ = false;
    bool sourceDataMounted_ = false;
    bool sourceSoundInitialized_ = false;
    bool sourceModelAssetsMounted_ = false;
    bool sourceFontsMounted_ = false;
    bool sourceAnimationsMounted_ = false;
    bool sourceEffectsMounted_ = false;
    bool sourceWeaponsMounted_ = false;
    bool sourceMapRootsMounted_ = false;
    char mountedRootMap_[32] = {};
    ShipmentDmPlayerState playerPresentation_{};
    ShipmentDmMatchState matchPresentation_{};
    ShipmentDmProjectileState grenadePresentation_{};
    weaponstate_t previousWeaponState_ = WEAPON_READY;
    uint32_t previousWeaponShotCount_ = 0;
};

const char* VitaSourceSessionStateName(VitaSourceSessionState state);

} // namespace kisak::vita
