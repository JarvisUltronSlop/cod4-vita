#include <vita/vita_input.h>
#include <vita/vita_cpu_replay.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>

#include <vita/vita_script_services.h>

namespace {

constexpr float kCommandAngleUnit = 0.0054931640625f; // 360 / 65536
VitaInputSample g_sourceSample{};
bool g_sourceSamplePublished = false;
bool g_sourceSamplePending = false;

int8_t ApplyDeadzone(uint8_t value, int16_t deadzone, bool invert) {
    const int centered = static_cast<int>(value) - 128;
    if (std::abs(centered) <= deadzone)
        return 0;
    const float scaled = static_cast<float>(centered) * 127.0f
        / static_cast<float>(std::max(1, 127 - deadzone));
    const int result = static_cast<int>(std::lround(std::clamp(scaled, -127.0f, 127.0f)));
    return static_cast<int8_t>(invert ? -result : result);
}

int8_t ScaleLook(int8_t value, float sensitivity) {
    const float scaled = static_cast<float>(value) * std::max(0.1f, sensitivity);
    return static_cast<int8_t>(std::lround(std::clamp(scaled, -127.0f, 127.0f)));
}

enum class RearZone : uint8_t {
    UpperLeft,
    UpperRight,
    LowerLeft,
    LowerRight,
};

// Vita rear-touch coordinates are 1920x1088.  Deliberately leave an outer
// border and a cross-shaped centre gutter dead: fingers resting while holding
// the console must not cook a grenade, melee, or leave sprint latched.
bool BackTouchDown(const SceTouchData& touch, RearZone zone) {
    constexpr int kOuterMarginX = 96;
    constexpr int kOuterMarginY = 64;
    constexpr int kLeftMaximum = 864;
    constexpr int kRightMinimum = 1056;
    constexpr int kUpperMaximum = 480;
    constexpr int kLowerMinimum = 608;
    for (uint32_t i = 0; i < touch.reportNum; ++i) {
        const int x = touch.report[i].x;
        const int y = touch.report[i].y;
        if (x < kOuterMarginX || x > 1920 - kOuterMarginX
            || y < kOuterMarginY || y > 1088 - kOuterMarginY)
            continue;
        const bool left = x <= kLeftMaximum;
        const bool right = x >= kRightMinimum;
        const bool upper = y <= kUpperMaximum;
        const bool lower = y >= kLowerMinimum;
        if ((zone == RearZone::UpperLeft && left && upper)
            || (zone == RearZone::UpperRight && right && upper)
            || (zone == RearZone::LowerLeft && left && lower)
            || (zone == RearZone::LowerRight && right && lower))
            return true;
    }
    return false;
}

} // namespace

void VitaInput_Initialize(VitaInputDevice* device) {
    if (!device)
        return;
    *device = VitaInputDevice{};
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
}

void VitaInput_SetViewAngles(VitaInputDevice* device, const float angles[3]) {
    if (!device || !angles)
        return;
    device->viewPitch = std::clamp(angles[0], -89.0f, 89.0f);
    device->viewYaw = std::fmod(angles[1], 360.0f);
    if (device->viewYaw < 0.0f)
        device->viewYaw += 360.0f;
}

void VitaInput_ApplyRearTouch(VitaInputDevice* device, bool enabled,
                              VitaInputSample* sample) {
    if (!device || !sample)
        return;
    sample->rearFragPressed = false;
    sample->rearSpecialPressed = false;
    sample->rearFragHeld = false;
    sample->rearSpecialHeld = false;
    sample->rearMeleePressed = false;
    sample->rearSprintHeld = false;
    if (enabled) {
        const auto admitted = [device](RearZone zone) {
            const uint8_t bit = static_cast<uint8_t>(1u << static_cast<unsigned>(zone));
            const bool down = BackTouchDown(device->backTouch, zone);
            if (!down) device->blockedRearZones &= ~bit;
            return down && (device->blockedRearZones & bit) == 0;
        };
        const bool rearFragDown = admitted(RearZone::UpperLeft);
        const bool rearSpecialDown = admitted(RearZone::UpperRight);
        const bool rearSprintDown = admitted(RearZone::LowerLeft);
        const bool rearMeleeDown = admitted(RearZone::LowerRight);
        sample->rearFragPressed = rearFragDown && !device->previousRearFrag;
        sample->rearSpecialPressed = rearSpecialDown && !device->previousRearSpecial;
        sample->rearFragHeld = rearFragDown;
        sample->rearSpecialHeld = rearSpecialDown;
        sample->rearMeleePressed = rearMeleeDown && !device->previousRearMelee;
        sample->rearSprintHeld = rearSprintDown;
        if (sample->rearFragHeld)
            sample->buttons |= BUTTON_FRAG;
        if (sample->rearSpecialHeld)
            sample->buttons |= BUTTON_SMOKE;
        if (sample->rearSprintHeld)
            sample->buttons |= BUTTON_SPRINT;
        if (sample->rearMeleePressed)
            sample->buttons |= BUTTON_MELEE;
        device->previousRearFrag = rearFragDown;
        device->previousRearSpecial = rearSpecialDown;
        device->previousRearMelee = rearMeleeDown;
        device->previousRearSprint = rearSprintDown;
    } else {
        device->blockedRearZones = 15;
        device->previousRearFrag = false;
        device->previousRearSpecial = false;
        device->previousRearMelee = false;
        device->previousRearSprint = false;
    }
}

void VitaInput_Poll(VitaInputDevice* device, const VitaInputConfig* config, VitaInputSample* sample) {
    if (!device || !sample)
        return;
    const VitaInputConfig fallback{};
    if (!config)
        config = &fallback;
    const uint64_t pollUsec = sceKernelGetProcessTimeWide();
    const uint64_t pollDelta = device->previousPollUsec && pollUsec >= device->previousPollUsec
        ? pollUsec - device->previousPollUsec : 33333;
    device->previousPollUsec = pollUsec;
    const float lookTimeScale = static_cast<float>(std::min<uint64_t>(pollDelta, 100000)) / 33333.0f;
    // Read the sampled pad rather than peeking it.  Vita3K's keyboard-backed
    // controller implementation publishes the current sample through the
    // read API; some releases leave PeekBufferPositive at the neutral state
    // even while the host mapping is held.  On hardware both APIs expose the
    // same one-sample queue, so consuming the poll keeps edge detection
    // deterministic without changing the Vita control contract.
    sceCtrlReadBufferPositive(0, &device->pad, 1);
    sceTouchPeek(SCE_TOUCH_PORT_BACK, &device->backTouch, 1);

    const uint32_t buttons = device->pad.buttons;
    static bool reportedFirstSample = false;
    if (!reportedFirstSample) {
        char detail[160]{};
        std::snprintf(detail, sizeof(detail),
                      "buttons=%u lx=%u ly=%u rx=%u ry=%u",
                      static_cast<unsigned>(buttons),
                      static_cast<unsigned>(device->pad.lx),
                      static_cast<unsigned>(device->pad.ly),
                      static_cast<unsigned>(device->pad.rx),
                      static_cast<unsigned>(device->pad.ry));
        kisak::vita::VitaScriptServicesTrace("vita_input_sample", "controller",
                                             detail);
        reportedFirstSample = true;
    }
    if (buttons != device->previousButtons) {
        // Keep the diagnostic edge-only so a held key cannot flood the
        // emulator log.  This is also useful when validating Vita3K's
        // keyboard/controller mapping against the real SceCtrl sample path.
        sceClibPrintf("[vita-input] buttons=%u previous=%u\\n",
                      static_cast<unsigned>(buttons),
                      static_cast<unsigned>(device->previousButtons));
        char detail[96]{};
        std::snprintf(detail, sizeof(detail), "buttons=%u previous=%u",
                      static_cast<unsigned>(buttons),
                      static_cast<unsigned>(device->previousButtons));
        kisak::vita::VitaScriptServicesTrace("vita_input_edge", "controller",
                                             detail);
    }
    const bool analogActive = device->pad.lx < 112 || device->pad.lx > 144
        || device->pad.ly < 112 || device->pad.ly > 144
        || device->pad.rx < 112 || device->pad.rx > 144
        || device->pad.ry < 112 || device->pad.ry > 144;
    static bool reportedAnalogActive = false;
    if (analogActive != reportedAnalogActive) {
        char detail[128]{};
        std::snprintf(detail, sizeof(detail), "active=%u buttons=%u lx=%u ly=%u rx=%u ry=%u",
                      static_cast<unsigned>(analogActive), static_cast<unsigned>(buttons),
                      static_cast<unsigned>(device->pad.lx), static_cast<unsigned>(device->pad.ly),
                      static_cast<unsigned>(device->pad.rx), static_cast<unsigned>(device->pad.ry));
        kisak::vita::VitaScriptServicesTrace("vita_input_analog", "controller", detail);
        reportedAnalogActive = analogActive;
    }
    const auto Pressed = [buttons, previous = device->previousButtons](uint32_t mask) {
        return (buttons & mask) != 0 && (previous & mask) == 0;
    };
    sample->forwardmove = ApplyDeadzone(device->pad.ly, config->deadzone, true);
    sample->rightmove = ApplyDeadzone(device->pad.lx, config->deadzone, false);
    // COD4 uses negative yaw for right and negative pitch for up. Pad axes
    // increase right/down, so reverse X and preserve Y unless Invert Y is on.
    sample->lookX = ScaleLook(ApplyDeadzone(device->pad.rx, config->lookDeadzone, true),
                              config->lookSensitivity);
    sample->lookY = ScaleLook(ApplyDeadzone(device->pad.ry, config->lookDeadzone, config->invertY),
                              config->lookSensitivity);
    // usercmd_s angles are absolute 16-bit turn units, not per-frame deltas.
    // Accumulate the sampled stick motion here while retaining the existing
    // shared PM_UpdateViewAngles/Pmove implementation unchanged.
    sample->lookDegrees[0] = static_cast<float>(sample->lookY) * 8.0f * kCommandAngleUnit * lookTimeScale;
    sample->lookDegrees[1] = static_cast<float>(sample->lookX) * 8.0f * kCommandAngleUnit * lookTimeScale;
    device->viewPitch = std::clamp(
        device->viewPitch + sample->lookDegrees[0],
        -89.0f, 89.0f);
    device->viewYaw = std::fmod(
        device->viewYaw + sample->lookDegrees[1],
        360.0f);
    if (device->viewYaw < 0.0f)
        device->viewYaw += 360.0f;
    sample->viewAngles[0] = static_cast<int32_t>(std::lround(
        device->viewPitch / kCommandAngleUnit));
    sample->viewAngles[1] = static_cast<int32_t>(std::lround(
        device->viewYaw / kCommandAngleUnit));
    sample->buttons = 0;
    if (buttons & SCE_CTRL_RTRIGGER) sample->buttons |= BUTTON_ATTACK;
    if (buttons & SCE_CTRL_LTRIGGER) sample->buttons |= BUTTON_ADS;
    if (buttons & SCE_CTRL_CROSS) sample->buttons |= BUTTON_JUMP;
    if (buttons & SCE_CTRL_SQUARE) sample->buttons |= BUTTON_RELOAD;
    if (buttons & SCE_CTRL_CIRCLE) {
        if (!(device->previousButtons & SCE_CTRL_CIRCLE)) device->circleDownUsec = pollUsec;
        device->circleHoldFrames = static_cast<uint16_t>(
            std::min<uint32_t>(device->circleHoldFrames + 1u, 0xFFFFu));
        // COD4's stance bind distinguishes a short crouch tap from a held
        // prone request. Keep the distinction in BUTTON_* so shared Pmove
        // handles the transition and collision rather than a Vita shim.
        if (pollUsec - device->circleDownUsec >= 400000)
            sample->buttons |= BUTTON_PRONE;
        else
            sample->buttons |= BUTTON_CROUCH;
    } else {
        device->circleHoldFrames = 0;
    }
    if (buttons & SCE_CTRL_UP) sample->buttons |= config->dpadFallbackProfile
        ? BUTTON_SPRINT : BUTTON_USE;
    if (buttons & SCE_CTRL_DOWN) sample->buttons |= config->dpadFallbackProfile
        ? BUTTON_MELEE : BUTTON_PRONE;
    if (!config->dpadFallbackProfile && (buttons & SCE_CTRL_LEFT))
        sample->buttons |= BUTTON_LEAN_LEFT;
    if (!config->dpadFallbackProfile && (buttons & SCE_CTRL_RIGHT))
        sample->buttons |= BUTTON_LEAN_RIGHT;
    sample->weaponCyclePressed = (buttons & SCE_CTRL_TRIANGLE)
        && !(device->previousButtons & SCE_CTRL_TRIANGLE);
    sample->menuUpPressed = Pressed(SCE_CTRL_UP);
    sample->menuDownPressed = Pressed(SCE_CTRL_DOWN);
    sample->menuLeftPressed = Pressed(SCE_CTRL_LEFT);
    sample->menuRightPressed = Pressed(SCE_CTRL_RIGHT);
    sample->menuAcceptPressed = Pressed(SCE_CTRL_CROSS);
    sample->menuBackPressed = Pressed(SCE_CTRL_CIRCLE);
    sample->startHeld = (buttons & SCE_CTRL_START) != 0;
    sample->selectHeld = (buttons & SCE_CTRL_SELECT) != 0;
    const bool previousStartHeld =
        (device->previousButtons & SCE_CTRL_START) != 0;
    const bool previousSelectHeld =
        (device->previousButtons & SCE_CTRL_SELECT) != 0;
    const bool chordHeld = sample->startHeld && sample->selectHeld;
    const bool previousChordHeld = previousStartHeld && previousSelectHeld;
    sample->performanceTogglePressed = chordHeld && !previousChordHeld;
    sample->startPressed = Pressed(SCE_CTRL_START) && !sample->selectHeld;
    sample->selectPressed = Pressed(SCE_CTRL_SELECT) && !sample->startHeld;
    VitaInput_ApplyRearTouch(device, config->rearTouchEnabled, sample);
    device->previousButtons = buttons;
}

void VitaInput_ApplyUsercmd(const VitaInputSample* sample, int32_t serverTime, usercmd_s* command) {
    if (!sample || !command)
        return;
    std::memset(command, 0, sizeof(*command));
    command->serverTime = serverTime;
    command->buttons = static_cast<int32_t>(sample->buttons);
    command->forwardmove = static_cast<char>(sample->forwardmove);
    command->rightmove = static_cast<char>(sample->rightmove);
    command->angles[0] = sample->viewAngles[0];
    command->angles[1] = sample->viewAngles[1];
}

void VitaInput_PublishSourceSample(const VitaInputSample* sample) {
    if (!sample) {
        VitaInput_ClearSourceSample();
        return;
    }
#if KISAK_VITA_CPU_RUNTIME
    // At most two ordinary 60 Hz samples feed a 30 Hz source command.
    // Preserve relative motion and short actions until CL_Input consumes them.
    VitaInputSample merged = *sample;
    if (g_sourceSamplePending) {
        merged.lookDegrees[0] += g_sourceSample.lookDegrees[0];
        merged.lookDegrees[1] += g_sourceSample.lookDegrees[1];
        merged.buttons |= g_sourceSample.buttons;
        merged.weaponCyclePressed |= g_sourceSample.weaponCyclePressed;
        merged.rearFragPressed |= g_sourceSample.rearFragPressed;
        merged.rearSpecialPressed |= g_sourceSample.rearSpecialPressed;
        merged.rearMeleePressed |= g_sourceSample.rearMeleePressed;
    }
    g_sourceSample = merged;
#else
    g_sourceSample = *sample;
#endif
    g_sourceSamplePending = true;
    g_sourceSamplePublished = true;
}

bool VitaInput_ReadSourceSample(VitaInputSample* sample) {
#if KISAK_VITA_CPU_PROFILE
    if(kisak::vita::VitaCpuReplay_Input(sample))return true;
#endif
    if (!sample || !g_sourceSamplePublished)
        return false;
    *sample = g_sourceSample;
    g_sourceSamplePending = false;
    g_sourceSample.lookDegrees[0] = g_sourceSample.lookDegrees[1] = 0.0f;
    g_sourceSample.weaponCyclePressed = false;
    g_sourceSample.rearFragPressed = false;
    g_sourceSample.rearSpecialPressed = false;
    g_sourceSample.rearMeleePressed = false;
    g_sourceSample.buttons &= ~BUTTON_MELEE;
    return true;
}

void VitaInput_SuppressRearUntilRelease(VitaInputDevice* device) {
    if (device) device->blockedRearZones = 15;
}

void VitaInput_ClearSourceSample() {
    g_sourceSample = {};
    g_sourceSamplePublished = false;
    g_sourceSamplePending = false;
}

void IN_ShowSystemCursor(BOOL) {}
void IN_Frame() {}
#if defined(__GNUC__)
__attribute__((weak))
#endif
bool IN_IsTalkKeyHeld() { return false; }
