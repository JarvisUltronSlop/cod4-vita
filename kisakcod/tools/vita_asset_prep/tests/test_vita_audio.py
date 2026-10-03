#!/usr/bin/env python3
"""Regression checks for the bounded Vita PCM mixer seam."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
AUDIO_HEADER = ROOT / "src" / "vita" / "vita_audio.h"
AUDIO_SOURCE = ROOT / "src" / "vita" / "vita_audio.cpp"
RUNTIME_SMOKE = ROOT / "src" / "vita" / "vita_runtime_smoke.cpp"
SOUND_BUNDLE = ROOT / "src" / "vita" / "vita_sound_bundle.cpp"
VITA_CMAKE = ROOT / "scripts" / "platform" / "vita" / "CMakeLists.txt"


def main() -> int:
    header = AUDIO_HEADER.read_text(encoding="utf-8")
    source = AUDIO_SOURCE.read_text(encoding="utf-8")
    mixer = AUDIO_SOURCE.with_name("vita_audio_mixer.cpp").read_text(encoding="utf-8")
    smoke = RUNTIME_SMOKE.read_text(encoding="utf-8")
    sound_bundle = SOUND_BUNDLE.read_text(encoding="utf-8")
    cmake = VITA_CMAKE.read_text(encoding="utf-8")
    assert "constexpr uint32_t kVitaAudioMaxVoices = 24;" in header
    assert "bool VitaAudio_MixVoices(VitaAudioDevice *device);" in header
    assert "void MixVoicesIntoBuffer(VitaAudioDevice* device)" in mixer
    assert "bool VitaAudio_MixVoices(VitaAudioDevice* device)" in mixer
    assert "const bool mixed = VitaAudio_MixVoices(device);" in source
    assert "sceAudioOutSetVolume" in source
    assert "sceKernelCreateThread" in source
    assert "sceKernelWaitThreadEnd" in source
    assert "sceAudioOutReleasePort(device->port);" in source
    assert "if (!device)" in source
    assert "rate < 8000 || rate > 96000" in mixer
    assert "channels != 1 && channels != 2" in mixer
    assert "VitaAudio_MixVoices(&mixer)" in smoke
    assert "mixer.silence[2] != 2000" in smoke
    assert "mixer.silence[0] != 32767" in smoke
    assert "for (uint32_t index = 0; index < database.ChunkCount(); ++index)" in sound_bundle
    assert "combined.push_back(std::move(sound))" in sound_bundle
    assert "Primary map chunks are enumerated before the common overlay" in sound_bundle
    runtime_link = cmake.split("target_link_libraries(KisakCOD-vita-runtime-smoke", 1)[1]
    assert "SceCommonDialog_stub" in runtime_link.split(")", 1)[0]
    assert "SceAppMgr_stub" in runtime_link.split(")", 1)[0]
    print("vita audio tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
