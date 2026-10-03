#ifdef NDEBUG
#undef NDEBUG
#endif
#define DR_WAV_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#include <dr_libs/dr_wav.h>
#include <dr_libs/dr_mp3.h>
#include <vita/vita_cpu_audio.h>
#include <vita/vita_cpu_audio_index.h>
#include <vita/vita_cpu_profile.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <vector>
#include <array>
#include <cstring>
using namespace kisak::vita;
namespace {
VitaCpuPcmResult Read(uint32_t channel,uint32_t generation,int16_t* samples,uint32_t* frames,bool* looped) {
    const auto until=VitaCpuNow()+2000000;
    for(;;) {
        const auto result=VitaCpuAudio_Read(channel,generation,samples,frames,looped);
        if(result!=VitaCpuPcmResult::Pending)return result;
        assert(VitaCpuNow()<until);sceKernelDelayThread(1000);
    }
}
}
int main() {
    const std::filesystem::path root="cpu-audio-fixtures";
    std::filesystem::create_directories(root/"sound");
    std::vector<int16_t> input(20000);for(unsigned i=0;i<input.size();++i)input[i]=int16_t(i%9000);
    drwav_data_format format{drwav_container_riff,DR_WAVE_FORMAT_PCM,1,48000,16};drwav wav;
    const auto wave=root/"sound/test.wav";
    assert(drwav_init_file_write(&wav,wave.string().c_str(),&format,nullptr));
    assert(drwav_write_pcm_frames(&wav,input.size(),input.data())==input.size());drwav_uninit(&wav);
    VitaCpuAudioIndexRecord entry{};std::strcpy(entry.path,"sound/test.wav");entry.channels=1;entry.rate=48000;entry.frames=input.size();entry.fileBytes=std::filesystem::file_size(wave);
    std::array<char,4096> bytes{};std::ifstream file(wave,std::ios::binary);file.read(bytes.data(),bytes.size());entry.prefixCrc=VitaCpuAudioCrc(bytes.data(),bytes.size());
    file.seekg(entry.fileBytes-bytes.size());file.read(bytes.data(),bytes.size());entry.tailCrc=VitaCpuAudioCrc(bytes.data(),bytes.size());file.close();
    const auto index=root/"index.bin";
    {VitaCpuAudioIndexHeader header{kCpuAudioIndexMagic,1,1,sizeof(entry),VitaCpuAudioCrc(&entry,sizeof(entry))};
     std::ofstream out(index,std::ios::binary);out.write(reinterpret_cast<const char*>(&header),sizeof(header));out.write(reinterpret_cast<const char*>(&entry),sizeof(entry));}
    CpuTestFailNextThreadStart();assert(!VitaCpuAudio_Start());assert(CpuTestLiveThreads()==0);
    assert(VitaCpuAudio_Start());assert(CpuTestLiveThreads()==1);
    std::array<int16_t,8192*2> pcm;uint32_t frames=0;bool looped=false;
    auto stream=VitaCpuAudio_Open(0,"sound/test.wav",false);assert(stream.valid && stream.frames==input.size());
    assert(VitaCpuAudio_Begin(0,stream.generation,0,false));
    size_t consumed=0;
    while(Read(0,stream.generation,pcm.data(),&frames,&looped)==VitaCpuPcmResult::Data) {
        assert(!looped);assert(std::equal(pcm.begin(),pcm.begin()+frames,input.begin()+consumed));consumed+=frames;
    }
    assert(consumed==input.size());
    // Old close commands and full stale PCM queues cannot affect a replacement.
    for(unsigned i=0;i<100;++i) {
        const auto previous=stream;
        stream=VitaCpuAudio_Open(0,"sound/test.wav",false);assert(stream.valid && stream.generation!=previous.generation);
        VitaCpuAudio_Close(0,previous.generation);
        assert(VitaCpuAudio_Begin(0,stream.generation,8000,true));
        assert(Read(0,stream.generation,pcm.data(),&frames,&looped)==VitaCpuPcmResult::Data);
        assert(std::equal(pcm.begin(),pcm.begin()+frames,input.begin()+8000));
        // Delay the consumer and let the worker fill its bounded queue.
        if(i==0){sceKernelDelayThread(40000);assert(VitaCpuAudio_ReadStatus().queuedBlocks<=8);}
    }
    VitaCpuAudio_Drain();assert(VitaCpuAudio_ReadStatus().queuedBlocks==0);assert(VitaCpuAudio_ReadStatus().activeStreams==0);
    // Changed/truncated files fail asynchronously and never yield old PCM.
    {std::ofstream truncated(wave,std::ios::binary|std::ios::trunc);truncated<<"bad";}
    stream=VitaCpuAudio_Open(1,"sound/test.wav",false);assert(stream.valid);
    assert(VitaCpuAudio_Begin(1,stream.generation,0,false));
    assert(Read(1,stream.generation,pcm.data(),&frames,&looped)==VitaCpuPcmResult::Error);
    assert(VitaCpuAudio_ReadStatus().failures>0);
    VitaCpuAudio_Stop();assert(CpuTestLiveThreads()==0);
    std::filesystem::remove(wave);std::filesystem::remove(index);
    std::filesystem::remove(root/"sound");std::filesystem::remove(root);
}
