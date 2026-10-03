#!/usr/bin/env python3
"""Regression tests for the pointer-free VFXA map effect closure."""

from __future__ import annotations

import importlib.util
import struct
import tempfile
from pathlib import Path


MODULE = Path(__file__).resolve().parents[1] / "vita_effects.py"
SPEC = importlib.util.spec_from_file_location("vita_effects", MODULE)
assert SPEC and SPEC.loader
vita_effects = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(vita_effects)


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


def test_vfxa_preserves_source_definitions_and_placements() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        rawfiles = root / "vita_rawfiles.bin"
        sidecar = root / "effects.bin"
        rawfiles.write_bytes(rawfile_bundle({
            "maps/mp/mp_farm_fx.gsc":
                'level._effect[ "rain_heavy_mist" ] = loadfx( "weather/rain_mp_farm" );\n'
                'level._effect[ "dust" ] = loadfx( "explosions/grenadeExp_dirt_1" );',
            "maps/createfx/mp_farm_fx.gsc":
                'ent = maps\\mp\\_utility::createOneshotEffect( "rain_heavy_mist" );\n'
                'ent.v[ "origin" ] = ( 1, 2, 3 );\n'
                'ent.v[ "angles" ] = ( 270, 0, 0 );\n'
                'ent.v[ "fxid" ] = "rain_heavy_mist";\n'
                'ent.v[ "delay" ] = -15;\n'
                'ent = maps\\mp\\_createfx::createLoopSound();\n'
                'ent.v[ "origin" ] = ( -4, 5, 6 );\n'
                'ent.v[ "angles" ] = ( 270, 0, 0 );\n'
                'ent.v[ "soundalias" ] = "emt_rain_metal";',
        }))
        report = vita_effects.build_effect_sidecar(rawfiles, sidecar, 5, "farm")
        assert report["definitions"] == 2
        assert report["placements"] == 1
        assert report["loopSounds"] == 1
        assert report["malformedRecords"] == 0
        info = vita_effects.inspect_effect_sidecar(sidecar)
        assert info["mapIndex"] == 5
        assert info["stringBytes"] > len("rain_heavy_mist")

        data = sidecar.read_bytes()
        header = vita_effects.HEADER.unpack_from(data)
        assert header[:3] == (b"VFXA", 1, 5)
        assert header[3:6] == (2, 1, 1)


def test_vfxa_rejects_non_finite_source_values() -> None:
    rawfiles = {
        "maps/mp/mp_farm_fx.gsc":
            b'level._effect[ "rain" ] = loadfx( "weather/rain" );',
        "maps/createfx/mp_farm_fx.gsc":
            b'ent = createOneshotEffect( "rain" );\n'
            b'ent.v[ "origin" ] = ( nan, 0, 0 );',
    }
    try:
        vita_effects._build_from_rawfiles(rawfiles, 5, "farm")
    except ValueError:
        raise AssertionError("malformed createfx records should be skipped")


def test_vfxa_v2_embeds_exact_element_graph() -> None:
    rawfiles = {
        "maps/mp/mp_farm_fx.gsc":
            b'level._effect[ "rain" ] = loadfx( "weather/rain_mp_farm" );',
        "maps/createfx/mp_farm_fx.gsc":
            b'ent = createOneshotEffect( "rain" );\n'
            b'ent.v[ "origin" ] = ( 1, 2, 3 );',
    }
    element = {
        "flags": 0x400,
        "elemType": 0,
        "visualCount": 1,
        "velIntervalCount": 0,
        "visStateIntervalCount": 0,
        "spawnRange": (1.0, 2.0),
        "visuals": ["gfx_rain_spray"],
        "velocitySamples": [tuple(0.25 for _ in range(24))],
        "visualSamples": [
            (255, 240, 220, 200, 0.0, 0.0, 8.0, 8.0, 1.0,
             255, 240, 220, 200, 0.0, 0.0, 1.0, 1.0, 0.5)
        ],
        "spawnDef": (100, 3),
    }
    graph_source = {
        "path": "vita_fxdefs.bin",
        "definitions": [("weather/rain_mp_farm", "weather/rain_mp_farm")],
        "graphs": {0: {
            "flags": 1,
            "msecLoopingLife": 1000,
            "elemDefCountLooping": 0,
            "elemDefCountOneShot": 1,
            "elemDefCountEmission": 0,
            "elements": [element],
        }},
    }
    payload, report = vita_effects._build_from_rawfiles(
        rawfiles, 5, "farm", graph_source)
    assert report["version"] == 3
    assert report["graphMatched"] == 1
    with tempfile.TemporaryDirectory() as directory:
        sidecar = Path(directory) / "effects.bin"
        sidecar.write_bytes(payload)
        info = vita_effects.inspect_effect_sidecar(sidecar)
        assert info["version"] == 3
        assert info["graphDefinitions"] == 1
        parsed = vita_effects._read_v2_graph(sidecar)
        assert parsed["graphs"][0]["elements"][0]["visuals"] == [
            "gfx_rain_spray"
        ]
        assert parsed["graphs"][0]["elements"][0]["spawnDef"] == (100, 3)


def test_vfxa_graph_matching_accepts_authored_path_case() -> None:
    definitions = [
        ("wood", "explosions/grenadeExp_wood"),
        ("battlefield_smokebank_S", "smoke/battlefield_smokebank_S"),
    ]
    graph = {
        "flags": 1,
        "elements": [],
    }
    source = {
        "definitions": [
            ("explosions/grenadeexp_wood", "explosions/grenadeexp_wood"),
            ("smoke/battlefield_smokebank_s", "smoke/battlefield_smokebank_s"),
        ],
        "graphs": {0: graph, 1: graph},
    }
    matches = vita_effects._match_graphs(definitions, source)
    assert [index for index, _ in matches] == [0, 1]


def test_vfxa_v2_expands_authored_runner_dependencies() -> None:
    rawfiles = {
        "maps/mp/mp_farm_fx.gsc":
            b'level._effect[ "root" ] = loadfx( "root" );\n'
            b'level._effect[ "child" ] = loadfx( "child" );',
        "maps/createfx/mp_farm_fx.gsc":
            b'ent = createOneshotEffect( "root" );\n'
            b'ent.v[ "origin" ] = ( 1, 2, 3 );',
    }
    child_visual = {
        "flags": 0x400,
        "elemType": 0,
        "visualCount": 1,
        "visuals": ["child_material"],
        "velocitySamples": [tuple(0.0 for _ in range(24))],
        "visualSamples": [
            (255, 255, 255, 255, 0.0, 0.0, 8.0, 8.0, 1.0,
             255, 255, 255, 255, 0.0, 0.0, 1.0, 1.0, 0.5)
        ],
    }
    runner = {
        "flags": 0x80,
        "elemType": 10,
        "visualCount": 1,
        "visuals": ["child"],
        "velocitySamples": [tuple(0.0 for _ in range(24))],
    }
    graph_source = {
        "path": "vita_fxdefs.bin",
        "definitions": [("root", "root"), ("child", "child")],
        "graphs": {
            0: {"flags": 1, "elemDefCountOneShot": 1,
                "elements": [runner]},
            1: {"flags": 1, "elemDefCountOneShot": 1,
                "elements": [child_visual]},
        },
    }
    payload, report = vita_effects._build_from_rawfiles(
        rawfiles, 5, "farm", graph_source)
    assert report["graphDependencies"] == 0
    assert report["graphMatched"] == 2
    with tempfile.TemporaryDirectory() as directory:
        sidecar = Path(directory) / "effects.bin"
        sidecar.write_bytes(payload)
        parsed = vita_effects._read_v2_graph(sidecar)
        assert parsed["definitions"] == [("root", "root"),
                                           ("child", "child")]


def test_vfxa_v2_appends_graph_only_runner_dependency() -> None:
    rawfiles = {
        "maps/mp/mp_farm_fx.gsc":
            b'level._effect[ "root" ] = loadfx( "root" );',
        "maps/createfx/mp_farm_fx.gsc":
            b'ent = createOneshotEffect( "root" );',
    }
    graph_source = {
        "definitions": [("root", "root"), ("child", "child")],
        "graphs": {
            0: {"flags": 1, "elemDefCountOneShot": 1,
                "elements": [{"elemType": 10, "visualCount": 1,
                               "visuals": ["child"],
                               "velocitySamples": [
                                   tuple(0.0 for _ in range(24))]}]},
            1: {"flags": 1, "elemDefCountOneShot": 0,
                "elements": []},
        },
    }
    _payload, report = vita_effects._build_from_rawfiles(
        rawfiles, 5, "farm", graph_source)
    assert report["graphDependencies"] == 1
    assert report["definitions"] == 2
    assert report["graphMatched"] == 2


def test_vfxa_v2_accepts_light_elements_without_visual_references() -> None:
    rawfiles = {
        "maps/mp/mp_farm_fx.gsc":
            b'level._effect[ "light" ] = loadfx( "weather/light" );',
        "maps/createfx/mp_farm_fx.gsc":
            b'ent = createOneshotEffect( "light" );',
    }
    light = {
        "flags": 0x42,
        "elemType": 6,
        "visualCount": 1,
        "velIntervalCount": 1,
        "visStateIntervalCount": 0,
        "velocitySamples": [tuple(0.0 for _ in range(24)),
                             tuple(0.0 for _ in range(24))],
        "visualSamples": [
            (255, 255, 255, 255, 0.0, 0.0, 1.0, 1.0, 1.0,
             0, 0, 0, 0, 0.0, 0.0, 0.0, 0.0, 0.0)
        ],
        "visuals": [],
    }
    graph_source = {
        "definitions": [("light", "weather/light")],
        "graphs": {0: {"flags": 1, "elemDefCountOneShot": 1,
                        "elements": [light]}},
    }
    payload, report = vita_effects._build_from_rawfiles(
        rawfiles, 5, "farm", graph_source)
    assert report["graphMatched"] == 1
    with tempfile.TemporaryDirectory() as directory:
        sidecar = Path(directory) / "effects.bin"
        sidecar.write_bytes(payload)
        parsed = vita_effects._read_v2_graph(sidecar)
        element = parsed["graphs"][0]["elements"][0]
        assert element["elemType"] == 6
        assert element["visualCount"] == 1
        assert element["visuals"] == []


def test_vfxa_v3_preserves_trail_metadata_and_empty_decal_slots() -> None:
    rawfiles = {
        "maps/mp/mp_farm_fx.gsc":
            b'level._effect[ "trail" ] = loadfx( "trail" );\n'
            b'level._effect[ "mark" ] = loadfx( "mark" );',
        "maps/createfx/mp_farm_fx.gsc":
            b'ent = createOneshotEffect( "trail" );',
    }
    zero_velocity = [tuple(0.0 for _ in range(24))]
    visual_sample = [
        (255, 255, 255, 255, 0.0, 0.0, 1.0, 1.0, 1.0,
         0, 0, 0, 0, 0.0, 0.0, 0.0, 0.0, 0.0)
    ]
    graph_source = {
        "version": 3,
        "definitions": [("trail", "trail"), ("mark", "mark")],
        "graphs": {
            0: {"elemDefCountOneShot": 1, "elements": [{
                "elemType": 3, "visualCount": 1,
                "visuals": ["gfx/trail"],
                "velocitySamples": zero_velocity,
                "visualSamples": visual_sample,
                "trailVertices": [(0.0, 0.0, 0.0, 0.0, 0.0)],
                "trailIndices": [0],
                "trailPresent": True,
                "trailScrollTimeMsec": -250,
                "trailRepeatDist": 16,
                "trailSplitDist": 8,
            }]},
            1: {"elemDefCountOneShot": 1, "elements": [{
                "elemType": 9, "visualCount": 1,
                "visuals": ["mc/impact", ""],
                "velocitySamples": zero_velocity,
                "visualSamples": visual_sample,
            }]},
        },
    }
    payload, report = vita_effects._build_from_rawfiles(
        rawfiles, 5, "farm", graph_source)
    assert report["version"] == 3
    with tempfile.TemporaryDirectory() as directory:
        sidecar = Path(directory) / "effects.bin"
        sidecar.write_bytes(payload)
        parsed = vita_effects._read_v2_graph(sidecar)
        trail = parsed["graphs"][0]["elements"][0]
        mark = parsed["graphs"][1]["elements"][0]
        assert (trail["trailScrollTimeMsec"], trail["trailRepeatDist"],
                trail["trailSplitDist"], trail["trailPresent"]) == (
                    -250, 16, 8, True)
        assert mark["visuals"] == ["mc/impact", ""]


if __name__ == "__main__":
    test_vfxa_preserves_source_definitions_and_placements()
    test_vfxa_rejects_non_finite_source_values()
    test_vfxa_v2_embeds_exact_element_graph()
    test_vfxa_v2_expands_authored_runner_dependencies()
    test_vfxa_v2_appends_graph_only_runner_dependency()
    test_vfxa_v2_accepts_light_elements_without_visual_references()
    test_vfxa_v3_preserves_trail_metadata_and_empty_decal_slots()
    print("vita effects tests: ok")
