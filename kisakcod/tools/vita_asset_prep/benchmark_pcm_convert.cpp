// Use the port's vendored decoder and its s16/8192-frame stream format.
#define DR_MP3_IMPLEMENTATION
#define DR_WAV_IMPLEMENTATION
#define DR_MP3_NO_SIMD
#include <dr_libs/dr_mp3.h>
#include <dr_libs/dr_wav.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
int main(int argc, char** argv) {
    if (argc!=3) return 2;
    drmp3 mp3{};
    if (!drmp3_init_file(&mp3,argv[1],nullptr)) return 3;
    if (mp3.channels<1 || mp3.channels>2) return 4;
    drwav_data_format format{drwav_container_riff,DR_WAVE_FORMAT_PCM,mp3.channels,mp3.sampleRate,16};
    drwav wav{};
    if (!drwav_init_file_write(&wav,argv[2],&format,nullptr)) return 5;
    int16_t pcm[8192*2]; uint64_t frames=0;
    for (;;) {
        const auto n=drmp3_read_pcm_frames_s16(&mp3,8192,pcm);
        if (!n) break;
        if (drwav_write_pcm_frames(&wav,n,pcm)!=n) return 6;
        frames+=n;
    }
    const auto rate=mp3.sampleRate, channels=mp3.channels;
    drwav_uninit(&wav); drmp3_uninit(&mp3);
    // Verify every written sample against a fresh decode, not just a WAV header.
    if (!drmp3_init_file(&mp3,argv[1],nullptr) || !drwav_init_file(&wav,argv[2],nullptr)) return 7;
    if (wav.channels!=channels || wav.sampleRate!=rate || wav.totalPCMFrameCount!=frames) return 8;
    int16_t check[8192*2]; uint64_t verified=0;
    for (;;) {
        const auto n=drmp3_read_pcm_frames_s16(&mp3,8192,pcm);
        if (drwav_read_pcm_frames_s16(&wav,8192,check)!=n) return 9;
        if (!n) break;
        if (std::memcmp(pcm,check,n*channels*sizeof(int16_t))) return 10;
        verified+=n;
    }
    drwav_uninit(&wav); drmp3_uninit(&mp3);
    if (!frames || verified!=frames) return 11;
    std::printf("{\"frames\":%llu,\"sampleRate\":%u,\"channels\":%u,\"bits\":16,\"verifiedAllSamples\":true}\n",
                (unsigned long long)frames,rate,channels);
}
