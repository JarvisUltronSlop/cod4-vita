#include "vita_audio.h"
#include "vita_script_services.h"

#include <psp2/audioout.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr/mutex.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <new>
#include <cstdio>

struct VitaAudioThreadState {
    SceUID thread = -1;
    SceUID mutex = -1;
    std::atomic<bool> running{false};
};

namespace {
void TracePauseFailure(const char* stage, int result) {
    char detail[32]{};
    std::snprintf(detail, sizeof(detail), "result=0x%08x", static_cast<unsigned>(result));
    kisak::vita::VitaScriptServicesTrace("audio_pause_failed", stage, detail);
}
bool Lock(VitaAudioDevice* device) {
    return device && device->threadState
        && sceKernelLockMutex(device->threadState->mutex, 1, nullptr) >= 0;
}
void Unlock(VitaAudioDevice* device) {
    sceKernelUnlockMutex(device->threadState->mutex, 1);
}

int OutputThread(SceSize size, void* args) {
    if (size != sizeof(VitaAudioDevice*)) return -1;
    auto* device = *static_cast<VitaAudioDevice**>(args);
    auto* state = device->threadState;
    uint64_t previous = 0;
    const uint64_t bufferUsec = static_cast<uint64_t>(device->frames) * 1000000 / 48000;
    while (state->running.load(std::memory_order_acquire)) {
        if (!Lock(device)) break;
        const bool mixed = VitaAudio_MixVoices(device);
        Unlock(device);
        if (!mixed) break;
        // The blocking port owns the cadence. Rendering never supplies audio
        // buffers, and no source globals or GPU calls run on this thread.
        if (sceAudioOutOutput(device->port, device->silence) < 0) {
            device->submitFailures.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        const uint64_t now = sceKernelGetProcessTimeWide();
        if (previous && now > previous + bufferUsec * 2)
            device->lateBuffers.fetch_add(1, std::memory_order_relaxed);
        previous = now;
        device->buffersSubmitted.fetch_add(1, std::memory_order_relaxed);
    }
    state->running.store(false, std::memory_order_release);
    return 0;
}

bool OpenOutputPort(VitaAudioDevice* device) {
    device->port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN,
        static_cast<int>(device->frames), 48000, SCE_AUDIO_OUT_MODE_STEREO);
    if (device->port < 0) { device->port = -1; return false; }
    int volume[2] = {SCE_AUDIO_OUT_MAX_VOL, SCE_AUDIO_OUT_MAX_VOL};
    if (sceAudioOutSetVolume(device->port, static_cast<SceAudioOutChannelFlag>(
        SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), volume) < 0) {
        sceAudioOutReleasePort(device->port);
        device->port = -1;
        return false;
    }
    auto* state = device->threadState;
    state->thread = sceKernelCreateThread("cod4-audio", OutputThread,
        0x10000100, 0x10000, 0, 0, nullptr);
    if (state->thread < 0) {
        sceAudioOutReleasePort(device->port);
        device->port = -1;
        return false;
    }
    state->running.store(true, std::memory_order_release);
    if (sceKernelStartThread(state->thread, sizeof(device), &device) < 0) {
        state->running.store(false, std::memory_order_release);
        sceKernelDeleteThread(state->thread);
        state->thread = -1;
        sceAudioOutReleasePort(device->port);
        device->port = -1;
        return false;
    }
    return true;
}
} // namespace

bool VitaAudio_Initialize(VitaAudioDevice* device, uint32_t frames) {
    if (!device || frames == 0 || frames > 1024 || (frames & 63u) != 0
        || device->threadState || device->port >= 0) return false;
    device->frames = frames;
    device->threadState = new (std::nothrow) VitaAudioThreadState;
    if (!device->threadState) return false;
    device->threadState->mutex = sceKernelCreateMutex("cod4-voices", 0, 0, nullptr);
    if (device->threadState->mutex < 0 || !OpenOutputPort(device)) {
        VitaAudio_Shutdown(device);
        return false;
    }
    return true;
}

bool VitaAudio_Pause(VitaAudioDevice* device) {
    if (!device) return false;
    if (auto* state = device->threadState) {
        state->running.store(false, std::memory_order_release);
        if (state->thread >= 0) {
            // Joining acknowledges the last sample access before its storage
            // or port is released. Never forcibly terminate the mixer.
            const int joined = sceKernelWaitThreadEnd(state->thread, nullptr, nullptr);
            if (joined < 0) {
                TracePauseFailure("wait_thread", joined);
                return false;
            }
            sceKernelDeleteThread(state->thread);
            state->thread = -1;
        }
    }
    if (device->port >= 0) {
        // The last Output call can leave samples queued after the worker exits.
        // A null output drains that buffer before releasing the hardware port
        // and handing it to the source sound driver.
        const int drained = sceAudioOutOutput(device->port, nullptr);
        if (drained < 0) {
            TracePauseFailure("drain", drained);
            return false;
        }
        const int released = sceAudioOutReleasePort(device->port);
        if (released < 0) {
            TracePauseFailure("release_port", released);
            return false;
        }
        device->port = -1;
    }
    return true;
}

bool VitaAudio_Resume(VitaAudioDevice* device) {
    if (!device || !device->threadState) return false;
    if (device->port >= 0) return device->threadState->running.load(std::memory_order_acquire);
    return OpenOutputPort(device);
}

void VitaAudio_StopAll(VitaAudioDevice* device) {
    if (!device) return;
    const bool locked = device->threadState && Lock(device);
    if (device->threadState && !locked) return;
    for (auto& voice : device->voices) voice = {};
    if (locked) Unlock(device);
}

void VitaAudio_Shutdown(VitaAudioDevice* device) {
    if (!device || !VitaAudio_Pause(device)) return;
    if (device->threadState) {
        if (device->threadState->mutex >= 0) sceKernelDeleteMutex(device->threadState->mutex);
        delete device->threadState;
        device->threadState = nullptr;
    }
    VitaAudio_StopAll(device);
}

bool VitaAudio_SubmitStereoS16(VitaAudioDevice* device, const int16_t* samples, uint32_t frames) {
    // Retained as a health query for existing diagnostics. All PCM enters via
    // the voice pool; the main thread must not compete for the hardware port.
    return device && !samples && frames == device->frames && device->port >= 0
        && device->threadState && device->threadState->running.load(std::memory_order_acquire);
}

bool VitaAudio_PlayPcm(VitaAudioDevice* device, const int16_t* samples,
                     uint32_t frames, uint32_t channels, uint32_t rate,
                     const VitaAudioPlayParams& params) {
    if (!device || device->port < 0 || !Lock(device)) return false;
    const bool admitted = VitaAudio_StartVoice(device, samples, frames, channels, rate, params);
    Unlock(device);
    return admitted;
}
