#include "vita_audio.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
int32_t SampleAt(const VitaAudioVoice& voice, uint32_t frame, uint32_t channel) {
    return voice.samples[static_cast<size_t>(frame) * voice.channels
                         + (voice.channels == 1 ? 0 : channel)];
}

int32_t InterpolatedSample(const VitaAudioVoice& voice, uint64_t cursor, uint32_t channel) {
    const uint32_t frame = static_cast<uint32_t>(cursor >> 16);
    const uint32_t next = frame + 1 < voice.frames ? frame + 1 : (voice.loop ? 0 : frame);
    const uint32_t fraction = static_cast<uint32_t>(cursor & 65535);
    const int64_t first = SampleAt(voice, frame, channel);
    const int64_t second = SampleAt(voice, next, channel);
    return static_cast<int32_t>((first * (65536 - fraction) + second * fraction) >> 16);
}

void MixVoicesIntoBuffer(VitaAudioDevice* device) {
    std::memset(device->mix, 0, sizeof(device->mix));
    for (auto& voice : device->voices) {
        if (!voice.active || !voice.samples || !voice.frames)
            continue;
        const uint64_t end = static_cast<uint64_t>(voice.frames) << 16;
        const uint64_t step = (static_cast<uint64_t>(voice.rate) << 16) / 48000;
        for (uint32_t frame = 0; frame < device->frames; ++frame) {
            if (voice.cursorQ16 >= end) {
                if (!voice.loop) break;
                voice.cursorQ16 %= end;
            }
            device->mix[frame * 2] += static_cast<int32_t>(
                (static_cast<int64_t>(InterpolatedSample(voice, voice.cursorQ16, 0)) * voice.gainLeftQ15) >> 15);
            device->mix[frame * 2 + 1] += static_cast<int32_t>(
                (static_cast<int64_t>(InterpolatedSample(voice, voice.cursorQ16, 1)) * voice.gainRightQ15) >> 15);
            voice.cursorQ16 += step;
        }
        if (voice.cursorQ16 >= end) {
            if (voice.loop) voice.cursorQ16 %= end;
            else voice = {};
        }
    }
    for (uint32_t i = 0; i < device->frames * 2; ++i)
        device->silence[i] = static_cast<int16_t>(std::clamp(device->mix[i], -32768, 32767));
}
} // namespace

bool VitaAudio_MixVoices(VitaAudioDevice* device) {
    if (!device || device->frames == 0 || device->frames > 1024) return false;
    MixVoicesIntoBuffer(device);
    return true;
}

bool VitaAudio_StartVoice(VitaAudioDevice* device, const int16_t* samples,
                         uint32_t frames, uint32_t channels, uint32_t rate,
                         const VitaAudioPlayParams& params) {
    if (!device || !samples || frames == 0 || (channels != 1 && channels != 2)
        || rate < 8000 || rate > 96000 || !std::isfinite(params.volume) || !std::isfinite(params.pan))
        return false;
    VitaAudioVoice* selected = nullptr;
    for (auto& voice : device->voices) {
        if (!voice.active) { selected = &voice; break; }
        if (!selected || voice.priority < selected->priority
            || (voice.priority == selected->priority
                && static_cast<int32_t>(voice.sequence - selected->sequence) < 0))
            selected = &voice;
    }
    if (selected->active && selected->priority > params.priority) return false;
    *selected = {};
    selected->samples = samples;
    selected->frames = frames;
    selected->channels = channels;
    selected->rate = rate;
    selected->loop = params.loop;
    selected->priority = params.priority;
    const float volume = std::clamp(params.volume, 0.0f, 1.0f);
    const float pan = std::clamp(params.pan, -1.0f, 1.0f);
    selected->gainLeftQ15 = static_cast<int32_t>(32768 * volume * (1 - std::max(0.0f, pan)));
    selected->gainRightQ15 = static_cast<int32_t>(32768 * volume * (1 + std::min(0.0f, pan)));
    selected->sequence = device->nextSequence++;
    selected->active = true;
    device->voicesStarted.fetch_add(1, std::memory_order_relaxed);
    return true;
}
