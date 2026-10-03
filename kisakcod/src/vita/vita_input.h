#pragma once

#include <cstdint>

#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include <bgame/bg_local.h>
#include <qcommon/msg_mp.h>

struct VitaInputDevice
{
    SceCtrlData pad{};
    SceTouchData backTouch{};
    uint32_t previousButtons = 0;
    bool previousRearFrag = false;
    bool previousRearSpecial = false;
    bool previousRearMelee = false;
    bool previousRearSprint = false;
    uint16_t circleHoldFrames = 0;
    uint64_t circleDownUsec = 0;
    uint64_t previousPollUsec = 0;
    uint8_t blockedRearZones = 0;
    // Pmove consumes quantized absolute view angles in usercmd_s. Keep the
    // device-side camera in degrees so stick deltas accumulate before being
    // quantized for the shared COD4 command path.
    float viewPitch = 0.0f;
    float viewYaw = 0.0f;
};

struct VitaInputConfig
{
    int16_t deadzone = 18;
    int16_t lookDeadzone = 18;
    float lookSensitivity = 1.0f;
    bool invertY = false;
    bool rearTouchEnabled = true;
    bool dpadFallbackProfile = false;
};

struct VitaInputSample
{
    int8_t forwardmove = 0;
    int8_t rightmove = 0;
    int8_t lookX = 0;
    int8_t lookY = 0;
    // Relative controller motion for the original client angle accumulator.
    // Source prediction adds playerState.delta_angles when constructing view
    // angles; feeding those final angles back into the accumulator repeats
    // the spawn/teleport offset on every frame.
    float lookDegrees[2] = {0.0f, 0.0f};
    int32_t viewAngles[2] = {0, 0};
    uint32_t buttons = 0;
    bool weaponCyclePressed = false;
    bool rearFragPressed = false;
    bool rearSpecialPressed = false;
    bool rearFragHeld = false;
    bool rearSpecialHeld = false;
    bool rearMeleePressed = false;
    bool rearSprintHeld = false;
    bool selectPressed = false;
    bool selectHeld = false;
    // Menu navigation is kept separate from gameplay BUTTON_* bits so the
    // frontend can consume edge events without changing usercmd semantics.
    bool menuUpPressed = false;
    bool menuDownPressed = false;
    bool menuLeftPressed = false;
    bool menuRightPressed = false;
    bool menuAcceptPressed = false;
    bool menuBackPressed = false;
    bool startPressed = false;
    bool startHeld = false;
    // Edge of the two-button chord used by the in-game diagnostics.  Keeping
    // it in the platform sample prevents the same physical edge from also
    // opening Pause or toggling the scoreboard.
    bool performanceTogglePressed = false;
};

void VitaInput_Initialize(VitaInputDevice* device);
void VitaInput_SetViewAngles(VitaInputDevice* device, const float angles[3]);
// Apply rear-touch actions separately from the physical pad edge state.  This
// keeps a held rear-right touch from retriggering melee on alternating frames.
void VitaInput_ApplyRearTouch(VitaInputDevice* device, bool enabled,
                              VitaInputSample* sample);
void VitaInput_Poll(VitaInputDevice* device, const VitaInputConfig* config, VitaInputSample* sample);
void VitaInput_ApplyUsercmd(const VitaInputSample* sample, int32_t serverTime, usercmd_s* command);

// The application owns the one physical controller poll. Publish that sample
// to the source client before CL_Frame so CL_CreateCmd never consumes a
// second pad sample (and therefore never loses an edge to the frontend).
// These calls are main-thread only, matching the source client contract.
void VitaInput_PublishSourceSample(const VitaInputSample* sample);
bool VitaInput_ReadSourceSample(VitaInputSample* sample);
void VitaInput_ClearSourceSample();
// Prevent touches already held in menus or across suspension from becoming
// fresh gameplay actions when control returns. Each zone rearms on release.
void VitaInput_SuppressRearUntilRelease(VitaInputDevice* device);

// Win32 input entry points retained for the shared MP client code.  The Vita
// implementation polls in CL_CreateCmd and keeps cursor/talk hooks inert.
void IN_ShowSystemCursor(BOOL show);
void IN_Frame();
bool IN_IsTalkKeyHeld();
