#include <vita/vita_app_state.h>

namespace kisak::vita {

void VitaAppLifecycle::Reset() {
    ownership_ = {};
    lastError_ = nullptr;
}

bool VitaAppLifecycle::IsLegalTransition(VitaAppState from, VitaAppState to) {
    if (from == to)
        return true;
    if (to == VitaAppState::Fatal)
        return true;
    switch (from) {
    case VitaAppState::Boot:
        return to == VitaAppState::Frontend;
    case VitaAppState::Frontend:
        return to == VitaAppState::Loading
            || to == VitaAppState::Suspended;
    case VitaAppState::Loading:
        return to == VitaAppState::InGame || to == VitaAppState::Frontend
            || to == VitaAppState::Suspended;
    case VitaAppState::InGame:
        return to == VitaAppState::Paused || to == VitaAppState::Frontend
            || to == VitaAppState::Loading || to == VitaAppState::Suspended;
    case VitaAppState::Paused:
        return to == VitaAppState::InGame || to == VitaAppState::Frontend
            || to == VitaAppState::Suspended;
    case VitaAppState::Suspended:
        // Resume() validates the saved pre-suspend state separately.
        return false;
    case VitaAppState::Fatal:
        return to == VitaAppState::Frontend;
    }
    return false;
}

bool VitaAppLifecycle::Commit(VitaAppState next, uint32_t resources) {
    if (!IsLegalTransition(ownership_.state, next)) {
        lastError_ = "illegal state transition";
        return false;
    }
    if (next == VitaAppState::InGame
        && (resources & (VitaOwnMapArena | VitaOwnMapGpu))
            != (VitaOwnMapArena | VitaOwnMapGpu)) {
        lastError_ = "in-game state requires complete map ownership";
        return false;
    }
    if (next == VitaAppState::Frontend
        && (resources & VitaOwnFrontendArena) == 0) {
        lastError_ = "frontend state requires frontend arena";
        return false;
    }
    ownership_.state = next;
    ownership_.resources = resources;
    ++ownership_.generation;
    lastError_ = nullptr;
    return true;
}

bool VitaAppLifecycle::Suspend(uint32_t retainedResources) {
    if (ownership_.state == VitaAppState::Suspended
        || ownership_.state == VitaAppState::Fatal) {
        lastError_ = "state cannot be suspended";
        return false;
    }
    ownership_.resumeState = ownership_.state;
    ownership_.state = VitaAppState::Suspended;
    // Audio output and menu video are always paused/closed before this commit.
    ownership_.resources = retainedResources
        & ~(VitaOwnAudioPool | VitaOwnMenuMedia);
    ++ownership_.generation;
    lastError_ = nullptr;
    return true;
}

bool VitaAppLifecycle::Resume(uint32_t resources) {
    if (ownership_.state != VitaAppState::Suspended) {
        lastError_ = "resume requested outside suspended state";
        return false;
    }
    const VitaAppState target = ownership_.resumeState;
    if (target == VitaAppState::Suspended || target == VitaAppState::Fatal
        || target == VitaAppState::Boot) {
        lastError_ = "invalid saved resume state";
        return false;
    }
    ownership_.state = target;
    ownership_.resources = resources;
    ++ownership_.generation;
    lastError_ = nullptr;
    return true;
}

const char* VitaAppLifecycle::StateName(VitaAppState state) {
    switch (state) {
    case VitaAppState::Boot: return "boot";
    case VitaAppState::Frontend: return "frontend";
    case VitaAppState::Loading: return "loading";
    case VitaAppState::InGame: return "in-game";
    case VitaAppState::Paused: return "paused";
    case VitaAppState::Suspended: return "suspended";
    case VitaAppState::Fatal: return "fatal";
    }
    return "unknown";
}

} // namespace kisak::vita
