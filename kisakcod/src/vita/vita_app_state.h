#pragma once

#include <cstdint>

namespace kisak::vita {

enum class VitaAppState : uint8_t {
    Boot = 0,
    Frontend,
    Loading,
    InGame,
    Paused,
    Suspended,
    Fatal,
};

enum VitaOwnedResource : uint32_t {
    VitaOwnNone = 0,
    VitaOwnFrontendArena = 1u << 0,
    VitaOwnMapArena = 1u << 1,
    VitaOwnTransientArena = 1u << 2,
    VitaOwnAudioPool = 1u << 3,
    VitaOwnFrontendGpu = 1u << 4,
    VitaOwnMapGpu = 1u << 5,
    VitaOwnIoWorker = 1u << 6,
    VitaOwnAssetWorker = 1u << 7,
    VitaOwnMenuMedia = 1u << 8,
};

struct VitaAppOwnership {
    VitaAppState state = VitaAppState::Boot;
    VitaAppState resumeState = VitaAppState::Boot;
    uint32_t resources = VitaOwnNone;
    uint32_t generation = 0;
};

// Records the state/resource transaction after the owning subsystem has
// entered successfully. Invalid jumps are rejected, preventing a partial map
// or frontend set from being presented as a live state.
class VitaAppLifecycle {
public:
    void Reset();
    bool Commit(VitaAppState next, uint32_t resources);
    bool Suspend(uint32_t retainedResources);
    bool Resume(uint32_t resources);
    const VitaAppOwnership& Current() const { return ownership_; }
    const char* LastError() const { return lastError_; }

    static bool IsLegalTransition(VitaAppState from, VitaAppState to);
    static const char* StateName(VitaAppState state);

private:
    VitaAppOwnership ownership_{};
    const char* lastError_ = nullptr;
};

} // namespace kisak::vita
