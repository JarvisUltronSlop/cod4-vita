#include <vita/vita_audio.h>
#include <cstdio>
#include <cstdlib>

static void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
    VitaAudioDevice device;
    device.frames = 4;
    const int16_t ramp[] = {1000, 3000};
    Require(VitaAudio_StartVoice(&device, ramp, 2, 1, 24000), "admit mono");
    Require(VitaAudio_MixVoices(&device), "mix mono");
    Require(device.silence[0] == 1000 && device.silence[2] == 2000
        && device.silence[4] == 3000 && device.silence[6] == 3000
        && !device.voices[0].active, "resampling and one-shot retirement");
    VitaAudioPlayParams loop;
    loop.loop = true;
    loop.pan = 1;
    Require(VitaAudio_StartVoice(&device, ramp, 2, 1, 24000, loop), "admit loop");
    Require(VitaAudio_MixVoices(&device), "mix loop");
    Require(device.silence[0] == 0 && device.silence[1] == 1000
        && device.silence[7] == 2000 && device.voices[0].active, "panning and interpolated loop boundary");
    Require(VitaAudio_MixVoices(&device) && device.silence[1] == 1000, "seamless next buffer");
    VitaAudioDevice full;
    full.frames = 64;
    const int16_t loud[] = {24000, -24000};
    loop.priority = 200;
    loop.pan = 0;
    for (unsigned i = 0; i < kVitaAudioMaxVoices; ++i)
        Require(VitaAudio_StartVoice(&full, loud, 2, 1, 48000, loop), "fill priority voices");
    VitaAudioPlayParams low;
    low.priority = 10;
    Require(!VitaAudio_StartVoice(&full, ramp, 2, 1, 48000, low), "low priority must not evict critical audio");
    Require(VitaAudio_MixVoices(&full) && full.silence[0] == 32767
        && full.silence[2] == -32768, "signed saturation");
    loop.priority = 220;
    Require(VitaAudio_StartVoice(&full, ramp, 2, 1, 48000, loop)
        && full.voices[0].samples == ramp, "higher priority evicts oldest peer");
    Require(!VitaAudio_StartVoice(&full, nullptr, 2, 1, 48000)
        && !VitaAudio_StartVoice(&full, ramp, 2, 3, 48000)
        && !VitaAudio_StartVoice(&full, ramp, 2, 1, 0), "invalid PCM rejected");
    return 0;
}
