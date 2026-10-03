#include "vita_platform_events.h"
#include <psp2/power.h>
#include <psp2/appmgr.h>
#include <psp2/kernel/threadmgr/callback.h>

int VitaPlatformEvents::PowerCallback(int, int, int flags, void* context) {
    auto* self = static_cast<VitaPlatformEvents*>(context);
    self->pending_ |= static_cast<uint32_t>(flags);
    return 0;
}
bool VitaPlatformEvents::Initialize() {
    if (callback_ >= 0) return true;
    callback_ = sceKernelCreateCallback("cod4-power", 0, PowerCallback, this);
    if (callback_ < 0) return false;
    if (scePowerRegisterCallback(callback_) < 0) {
        sceKernelDeleteCallback(callback_);
        callback_ = -1;
        return false;
    }
    return true;
}
VitaPlatformEvents::~VitaPlatformEvents() {
    if (callback_ >= 0) {
        scePowerUnregisterCallback(callback_);
        sceKernelDeleteCallback(callback_);
    }
}
VitaPlatformEventSample VitaPlatformEvents::Poll() {
    sceKernelCheckCallback();
    VitaPlatformEventSample result;
    result.available = callback_ >= 0;
    result.suspend = (pending_ & (SCE_POWER_CB_SYSTEM_SUSPEND | SCE_POWER_CB_APP_SUSPEND)) != 0;
    result.resume = (pending_ & (SCE_POWER_CB_SYSTEM_RESUME | SCE_POWER_CB_APP_RESUME)) != 0;
    pending_ = 0;
    SceAppMgrAppState state{};
    if (_sceAppMgrGetAppState(&state, sizeof(state), 1) >= 0) {
        const bool overlaid = state.isSystemUiOverlaid != 0;
        result.suspend |= overlaid && !overlay_;
        result.resume |= !overlaid && overlay_;
        overlay_ = overlaid;
        for (uint32_t i = 0; i < state.systemEventNum && i < 16; ++i) {
            SceAppMgrSystemEvent event{};
            if (sceAppMgrReceiveSystemEvent(&event) < 0) break;
            result.resume |= event.systemEvent == SCE_APPMGR_SYSTEMEVENT_ON_RESUME;
        }
    }
    return result;
}
