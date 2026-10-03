#!/usr/bin/env python3
"""Regression tests for source-authored map environment extraction."""

from __future__ import annotations

import importlib.util
import struct
import tempfile
from pathlib import Path


MODULE = Path(__file__).resolve().parents[1] / "vita_environment.py"
SPEC = importlib.util.spec_from_file_location("vita_environment", MODULE)
assert SPEC and SPEC.loader
vita_environment = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(vita_environment)


def rawfile_bundle(records: dict[str, str]) -> bytes:
    encoded = [(name.encode("utf-8"), value.encode("utf-8"))
               for name, value in records.items()]
    declared = sum(len(payload) for _, payload in encoded)
    output = bytearray(struct.pack("<3IQ", 0x52544956, 1, len(encoded), declared))
    for name, payload in encoded:
        output.extend(struct.pack("<2I", len(name), len(payload)))
        output.extend(name)
        output.extend(payload)
    return bytes(output)


def test_source_environment_is_serialized() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        rawfiles = root / "vita_rawfiles.bin"
        sidecar = root / "environment.bin"
        rawfiles.write_bytes(rawfile_bundle({
            "maps/createart/mp_farm_art.gsc":
                "setExpFog(300, 1400, 0.5, 0.5, 0.5, 0);",
            "maps/mp/mp_farm.gsc":
                'setdvar("r_specularcolorscale", "1.85");',
            "maps/mp/mp_farm_fx.gsc":
                'loadfx("weather/rain_mp_farm"); loadfx("weather/lightning_mp_farm");',
            "vision/mp_farm.vision":
                'r_glow "1"\n'
                'r_glowRadius0 "6.5"\n'
                'r_glowRadius1 "0"\n'
                'r_glowBloomCutoff "0.5"\n'
                'r_glowBloomDesaturation "0.25"\n'
                'r_glowBloomIntensity0 "1.1"\n'
                'r_glowBloomIntensity1 "0"\n'
                'r_glowSkyBleedIntensity0 "0.1"\n'
                'r_glowSkyBleedIntensity1 "0"\n'
                'r_filmLightTint "1 0.9 0.8"\n'
                'r_filmDarkTint "0.2 0.3 0.4"\n'
                'r_filmContrast "1.15"\n'
                'r_filmBrightness "0.03"\n'
                'r_filmDesaturation "0.1"\n',
        }))
        report = vita_environment.build_environment_sidecar(
            rawfiles, sidecar, 5, "farm")
        assert report["mapIndex"] == 5
        assert report["weatherEffect"] == "weather/rain_mp_farm"
        assert report["bytes"] == vita_environment.HEADER.size + len("vision/mp_farm.vision") + len(
            "weather/rain_mp_farm")
        data = sidecar.read_bytes()
        assert len(data) == report["bytes"]
        unpacked = vita_environment.HEADER.unpack_from(data)
        assert unpacked[0:4] == (b"VENV", 3, 5,
                                 vita_environment.HAS_FOG
                                 | vita_environment.HAS_VISION
                                 | vita_environment.HAS_RAIN
                                 | vita_environment.HAS_LIGHTNING
                                 | vita_environment.HAS_CLIENT_FX
                                 | vita_environment.HAS_GLOW)
        assert unpacked[4:7] == (0.5, 0.5, 0.5)
        assert unpacked[7] == 300.0
        assert abs(unpacked[8] - 0.69314718 / 1400.0) < 1e-8
        assert all(abs(actual - expected) < 1e-5 for actual, expected in zip(
            unpacked[21:25], (1.0, 0.18, 0.72, 0.0)))
        assert report["glow"]["enabled"] is True
        assert all(abs(actual - expected) < 1e-5 for actual, expected in zip(
            unpacked[29:38], (1.0, 6.5, 0.0, 0.5, 0.25, 1.1, 0.0, 0.1, 0.0)))
        assert abs(unpacked[38] - 1.85) < 1e-5


def test_map_without_fx_has_no_weather() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        rawfiles = root / "vita_rawfiles.bin"
        sidecar = root / "environment.bin"
        rawfiles.write_bytes(rawfile_bundle({
            "maps/createart/mp_shipment_art.gsc":
                "setExpFog(0, 1000, 0.1, 0.2, 0.3, 0);",
        }))
        report = vita_environment.build_environment_sidecar(
            rawfiles, sidecar, 0, "shipment")
        assert report["flags"] & vita_environment.HAS_FOG
        assert not report["flags"] & vita_environment.HAS_RAIN
        values = vita_environment.HEADER.unpack_from(sidecar.read_bytes())
        assert values[21:25] == (0.0, 0.0, 0.0, 0.0)
        assert values[38] == 1.0


def test_commented_fog_is_not_serialized() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        rawfiles = root / "vita_rawfiles.bin"
        sidecar = root / "environment.bin"
        rawfiles.write_bytes(rawfile_bundle({
            "maps/createart/mp_cargoship_art.gsc":
                "//setExpFog(300, 1400, 0.5, 0.5, 0.5, 0);\n"
                "level.fognearplane = 300;",
        }))
        report = vita_environment.build_environment_sidecar(
            rawfiles, sidecar, 10, "cargoship")
        assert not report["flags"] & vita_environment.HAS_FOG
        values = vita_environment.HEADER.unpack_from(sidecar.read_bytes())
        assert values[4:11] == (0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0)


if __name__ == "__main__":
    test_source_environment_is_serialized()
    test_map_without_fx_has_no_weather()
    print("vita environment tests: ok")
