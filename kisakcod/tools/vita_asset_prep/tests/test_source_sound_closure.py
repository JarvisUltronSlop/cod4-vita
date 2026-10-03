"""Executable corruption, dependency, and variant-order checks for VSAL."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import source_sound_closure as sound


def test_original_console_and_composed_faction_aliases_are_selected(tmp_path):
    source = tmp_path / "source"
    export = tmp_path / "export"
    for directory in ("cgame_mp", "client", "ui_mp"):
        (source / directory).mkdir(parents=True)
    export.mkdir()
    (source / "cgame_mp/cg_main_mp.cpp").write_text('CG_RegisterSurfaceTypeSounds("step");')
    (source / "client/cl_console.cpp").write_text('SND_PlayLocalSoundAliasByName(0, "ui_pulse_text_type");')
    (source / "ui_mp/ui_main_mp.cpp").write_text('play("ui_confirm");')
    (export / "vita_rawfiles.bin").write_bytes(b'"RU_1mc_" "freeforall"')
    names = ("UI_PULSE_TEXT_TYPE", "ui_confirm", "ru_1mc_freeforall", "step_metal",
             "ru_1mc_unreferenced", "ab_1mc_freeforall", "unused")
    selected = sound.source_seeds(source, export, {name: [] for name in names})
    assert selected == {"UI_PULSE_TEXT_TYPE", "ui_confirm", "ru_1mc_freeforall", "step_metal"}


def record(name, secondary="", chain="", volume=0.5):
    raw = bytearray(sound.RECORD_BYTES)
    for index, text in enumerate((name, "", secondary, chain, "example.wav", "default", "default")):
        raw[index * 128:index * 128 + len(text)] = text.encode()
    struct.pack_into("<3i4I", raw, 896, 0, 0, 30, 1, 1, 2, 1)
    struct.pack_into("<13f", raw, 924, volume, volume, 1, 1, 1, 256, 0, 1, 0, 0, 0, 0, 0)
    struct.pack_into("<4f", raw, 976, 0, 1, 1, 0)
    return bytes(raw)


class SourceSoundTests(unittest.TestCase):
    def test_variants_and_cycles_preserve_source_order(self):
        aliases = {"first": [record("first", "second", volume=0.25), record("first", volume=0.75)],
                   "second": [record("second", chain="first")], "unused": [record("unused")]}
        parsed = sound.parse_aliases(sound.build_aliases(aliases))
        self.assertEqual(parsed, aliases)
        self.assertEqual(sound.dependency_closure(parsed, {"first"}), {"first", "second"})

    def test_missing_secondary_is_an_error(self):
        with self.assertRaisesRegex(ValueError, "missing alias dependency"):
            sound.dependency_closure({"a": [record("a", "missing")]}, {"a"})

    def test_corrupt_catalogs_are_rejected(self):
        wire = sound.build_aliases({"a": [record("a")]})
        corruptions = [wire[:-1], wire + b"x"]
        for offset, data in ((4, struct.pack("<I", 9)), (20, b"a" * 128),
                             (20 + 924, struct.pack("<f", float("nan"))),
                             (20 + 1040, struct.pack("<i", 7))):
            damaged = bytearray(wire)
            damaged[offset:offset + len(data)] = data
            corruptions.append(bytes(damaged))
        for damaged in corruptions:
            with self.assertRaises(ValueError):
                sound.parse_aliases(damaged)

    def test_path_normalization_does_not_escape_install(self):
        self.assertEqual(sound.normalized_path("Music\\Track.MP3"), "music/track.mp3")
        for path in ("../file", "a/../../file", "ux0:data/file", "/absolute"):
            with self.assertRaises(ValueError):
                sound.normalized_path(path)


if __name__ == "__main__":
    unittest.main()
