#pragma once

#include <cstdint>
#include <atomic>

constexpr uint32_t kVitaAudioMaxVoices = 24;

struct VitaAudioThreadState;

struct VitaAudioPlayParams
{
    bool loop = false;
    uint8_t priority = 128;
    float volume = 1.0f;
    float pan = 0.0f; // -1 left, +1 right; attenuation is applied by the caller.
};

struct VitaAudioVoice
{
    const int16_t* samples = nullptr;
    uint32_t frames = 0;
    uint32_t channels = 0;
    uint32_t rate = 0;
    uint64_t cursorQ16 = 0;
    uint32_t sequence = 0;
    int32_t gainLeftQ15 = 32768;
    int32_t gainRightQ15 = 32768;
    uint8_t priority = 128;
    bool loop = false;
    bool active = false;
};

struct VitaAudioDevice
{
    int port = -1;
    uint32_t frames = 1024;
    int16_t silence[1024 * 2]{};
    int32_t mix[1024 * 2]{};
    VitaAudioVoice voices[kVitaAudioMaxVoices]{};
    uint32_t nextSequence = 1;
    // Lightweight counters make the port observable without logging from
    // the audio callback. They are also useful for emulator/hardware smoke
    // tests because a successful port open alone does not prove that a PCM
    // voice was submitted to the device.
    std::atomic<uint32_t> voicesStarted{0};
    std::atomic<uint32_t> buffersSubmitted{0};
    std::atomic<uint32_t> submitFailures{0};
    std::atomic<uint32_t> lateBuffers{0};
    VitaAudioThreadState* threadState = nullptr;
};

bool VitaAudio_Initialize(VitaAudioDevice *device, uint32_t frames = 1024);
void VitaAudio_Shutdown(VitaAudioDevice *device);
// Release/reopen the hardware port while retaining bounded resident voices.
// Suspend therefore cannot leave an output handle stale, and resume does not
// rebuild the sound registry or allocate a second voice pool.
bool VitaAudio_Pause(VitaAudioDevice* device);
bool VitaAudio_Resume(VitaAudioDevice* device);
bool VitaAudio_SubmitStereoS16(VitaAudioDevice *device, const int16_t *samples, uint32_t frames);

// Mixes the currently active voices into device->silence without touching the
// Vita audio port. Only the output worker calls this on a running device;
// standalone tests can exercise the mixer without an output port.
bool VitaAudio_MixVoices(VitaAudioDevice *device);
bool VitaAudio_StartVoice(VitaAudioDevice* device, const int16_t* samples,
                         uint32_t frames, uint32_t channels, uint32_t rate,
                         const VitaAudioPlayParams& params = {});

// Starts one bounded decoded-PCM voice. Voices reference samples owned by the
// loaded Vita sound bundle; the fixed mixer never allocates or copies them.
bool VitaAudio_PlayPcm(VitaAudioDevice *device, const int16_t *samples,
                       uint32_t frames, uint32_t channels, uint32_t rate,
                       const VitaAudioPlayParams& params = {});

// Drop every active voice without reallocating the fixed pool. Source map
// teardown and suspend transitions use this before releasing bundle-owned
// PCM storage.
void VitaAudio_StopAll(VitaAudioDevice* device);
