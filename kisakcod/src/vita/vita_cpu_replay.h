#pragma once
#include <cstdint>
struct playerState_s;
struct usercmd_s;
struct VitaInputSample;
namespace kisak::vita {
// Source-owned validation only. This observes authored results; it never
// implements replacement gameplay or consumes RNG to form a checksum.
void VitaCpuReplay_Enable(bool enabled, bool completed=true);
bool VitaCpuReplay_Enabled();
void VitaCpuReplay_State(const char* owner,const playerState_s& state,uint32_t tick);
void VitaCpuReplay_ServerWorld(uint32_t tick);
void VitaCpuReplay_Command(const usercmd_s& command,uint32_t number);
void VitaCpuReplay_Event(const char* owner,uint32_t entity,uint32_t event,uint32_t parameter);
// Explicit correctness harness, available once per process for a clean start.
// It uses the same timestamped input tape and virtual source clock in both
// builds. Ordinary play/CPU diagnostics never use this clock or cadence.
bool VitaCpuReplay_BeginTape();
bool VitaCpuReplay_TapeActive();
bool VitaCpuReplay_TapeComplete();
bool VitaCpuReplay_NextFrame(uint32_t& elapsedMs);
bool VitaCpuReplay_Input(VitaInputSample* sample);
bool VitaCpuReplay_Clock(uint32_t* milliseconds);
void VitaCpuReplay_EndTape(bool complete);
void VitaCpuReplay_SourceInitialize();
}
