#pragma once
#include <cstdint>
namespace kisak::vita {
struct VitaCpuStreamInfo {uint32_t channels=0,rate=0,generation=0;uint64_t frames=0;bool valid=false;};
struct VitaCpuAudioStatus {uint64_t decodedFrames=0,pendingReads=0,failures=0;uint32_t queuedBlocks=0,activeStreams=0,queueHighWater=0;uint64_t underruns=0;};
enum class VitaCpuPcmResult {Pending,Data,End,Error};
bool VitaCpuAudio_Start();
void VitaCpuAudio_Stop();
VitaCpuStreamInfo VitaCpuAudio_Open(uint32_t channel,const char* relativePath,bool mp3);
bool VitaCpuAudio_Begin(uint32_t channel,uint32_t generation,uint64_t frame,bool loop);
VitaCpuPcmResult VitaCpuAudio_Read(uint32_t channel,uint32_t generation,int16_t* samples,uint32_t* frames,bool* looped);
void VitaCpuAudio_Close(uint32_t channel,uint32_t generation);
void VitaCpuAudio_Drain();
VitaCpuAudioStatus VitaCpuAudio_ReadStatus();
void VitaCpuAudio_ReportUnderrun(uint32_t channel);
}
