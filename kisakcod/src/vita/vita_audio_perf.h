#pragma once
#include <cstdint>
struct VitaAudioStreamPerf {
    uint64_t readUs=0, readBytes=0, decodeUs=0, frames=0;
    uint32_t readCalls=0, refillCalls=0, processedBuffers=0, uploads=0, stoppedQueued=0;
    uint32_t nameHash=0, rate=0, channels=0;
    bool active=false, eligible=false, pcm=false, suppressed=false;
};
struct VitaAudioPerf {
    uint64_t readUs=0, readBytes=0, decodeUs=0, frames=0, uploadUs=0, queueUs=0;
    uint32_t activeStreams=0;
    uint32_t mode=0, errors=0, eligibleStreams=0, pcmStreams=0, suppressedStreams=0, updateCalls=0;
    VitaAudioStreamPerf streams[13]{};
};
// Main-thread stream work only; decodeUs includes readUs.
void VitaAudioPerf_Reset();
VitaAudioPerf VitaAudioPerf_Read();
