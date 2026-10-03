#pragma once
#include <cstdint>
namespace kisak::vita {
// Renderer-owned controls; display callbacks only transfer bounded records.
void VitaDisplayProbe_SetPhase(uint32_t phase);
void VitaDisplayProbe_Drain();
void VitaDisplayProbe_Finish(); // Outside timed test: retire callbacks before log flush.
}
