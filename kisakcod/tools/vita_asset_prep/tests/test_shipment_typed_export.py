from __future__ import annotations

import csv
import hashlib
import importlib.util
import json
import struct
import tempfile
import zipfile
from unittest import mock
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("shipment_typed_export", ROOT / "shipment_typed_export.py")
assert SPEC and SPEC.loader
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)
MAP_SPEC = importlib.util.spec_from_file_location(
    "map_bundle_export", ROOT / "map_bundle_export.py")
assert MAP_SPEC and MAP_SPEC.loader
map_bundle = importlib.util.module_from_spec(MAP_SPEC)
MAP_SPEC.loader.exec_module(map_bundle)


def test_source_effect_provenance_matches_exported_graph(tmp_path: Path) -> None:
    import pytest

    effects = module.vita_effects
    graph = {"definitions": [("fx/source", "fx/source"), ("fx/fallback", "fx/fallback")],
             "graphs": {0: {"flags": 1, "elements": []},
                        1: {"flags": 1, "elements": []}}}
    scripts = {"maps/mp/mp_shipment_fx.gsc":
               b'level._effect["a"] = loadfx("fx/source");\n'
               b'level._effect["b"] = loadfx("fx/fallback");'}
    payload, _ = effects._build_from_rawfiles(scripts, 0, "shipment", graph)
    (tmp_path / "vita_fxdefs.bin").write_bytes(payload)
    report_path = tmp_path / "vita_source_fx_dependencies.csv"
    valid = "name,sourceDefault,elements\nfx/source,0,0\nfx/fallback,1,0\n"
    report_path.write_text(valid, encoding="utf-8")
    report = module._source_effect_dependencies(tmp_path)
    assert report["requiredCount"] == 2 and report["sourceDefaultCount"] == 1
    assert report["effectGraphSha256"] == hashlib.sha256(payload).hexdigest()
    assert report["records"][0] == {"name": "fx/fallback", "sourceDefault": True, "elementCount": 0}
    for invalid in (
            valid.replace("fx/source,0,0", "fx/missing,0,0"),
            valid.replace("fx/source,0,0", "fx/source,0,1"),
            valid.replace("fx/source,0,0", "fx/source,2,0"),
            valid + "FX/SOURCE,0,0\n",
            "name,sourceDefault,elements\n",
            valid.replace("sourceDefault", "default")):
        report_path.write_text(invalid, encoding="utf-8")
        with pytest.raises(ValueError):
            module._source_effect_dependencies(tmp_path)


def test_vitp_v2_source_hide_mask_sidecar_is_validated() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        path = Path(root_name) / "vita_weapons.bin"
        path.write_bytes(struct.pack("<4I", 0x50544956, 2, 1, 244) + bytes(244))
        report = module._validate_weapon_sidecar(path)
        assert report and report["version"] == 2
        assert report["recordBytes"] == 244
        assert report["records"] == 1


def test_vitp_v3_source_animation_table_sidecar_is_validated() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        path = Path(root_name) / "vita_weapons.bin"
        path.write_bytes(struct.pack("<4I", 0x50544956, 3, 1, 2356) + bytes(2356))
        report = module._validate_weapon_sidecar(path)
        assert report and report["version"] == 3
        assert report["recordBytes"] == 2356
        assert report["records"] == 1


def test_vitp_v4_source_freeze_flag_sidecar_is_validated() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        path = Path(root_name) / "vita_weapons.bin"
        path.write_bytes(struct.pack("<4I", 0x50544956, 4, 1, 2360) + bytes(2360))
        report = module._validate_weapon_sidecar(path)
        assert report and report["version"] == 4
        assert report["recordBytes"] == 2360
        assert report["records"] == 1


def test_vitp_v5_pointer_free_source_weapon_is_validated() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        path = Path(root_name) / "vita_weapons.bin"
        path.write_bytes(
            struct.pack("<4I", 0x50544956, 5, 1, 15376) + bytes(15376)
        )
        report = module._validate_weapon_sidecar(path)
        assert report and report["version"] == 5
        assert report["recordBytes"] == 15376
        assert report["records"] == 1


def test_vitt_source_string_tables_are_validated() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        path = Path(root_name) / "vita_stringtables.bin"
        name = b"mp/statstable.csv"
        cells = [b"m16", b"0"]
        record = (
            struct.pack("<3I", len(name), 1, 2)
            + name
            + b"".join(struct.pack("<I", len(cell)) + cell for cell in cells)
        )
        path.write_bytes(
            struct.pack("<3IQ", 0x54544956, 1, 1, len(record)) + record
        )
        report = module._validate_stringtable_sidecar(path)
        assert report and report["records"] == 1
        assert report["dataBytes"] == len(record)


def test_shipment_image_envelope_keeps_source_material_closure() -> None:
    # The startup stream uses the known-stable envelope. Probes and the larger
    # source-semantic closures are companion VITI chunks loaded after staging.
    assert module.IMAGE_EXPORT_MAX_RECORDS == 512
    assert module.IMAGE_EXPORT_MAX_DATA_BYTES == 64 * 1024 * 1024


def test_vfxa_effect_images_extract_exact_retail_aliases() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        retail = root / "retail"
        (retail / "main").mkdir(parents=True)
        payload = bytes(range(8))
        iwi = (struct.pack("<3sBBB3h4i", b"IWi", 6, 11, 2,
                           4, 4, 1, 28 + len(payload), 0, 0, 0)
               + payload)
        with zipfile.ZipFile(retail / "main" / "iw_00.iwd", "w") as bundle:
            bundle.writestr("images/effect_sprite#0.iwi", iwi)
            bundle.writestr("images/unrelated.iwi", iwi)

        output = root / "effect-images.bin"
        report = module._synthesize_effect_images_from_iwd(
            retail, ["effect_sprite#0", "effect_missing#1"], output, "farm")
        assert report["requested"] == 2
        assert report["records"] == 1
        assert report["missing"] == 1
        assert report["missingNames"] == ["effect_missing#1"]
        records = module._read_image_records(output)
        assert [record[0] for record in records] == ["effect_sprite#0"]
        assert records[0][1] == payload


def test_vfxa_effect_image_aliases_follow_child_runners() -> None:
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
                               "velocitySamples": [(0.0,) * 24]}]},
            1: {"flags": 1, "elemDefCountOneShot": 1,
                "elements": [{
                    "elemType": 0, "visualCount": 1,
                    "visuals": ["child_material"],
                    "velocitySamples": [(0.0,) * 24],
                    "visualSamples": [
                        (255, 255, 255, 255, 0.0, 0.0, 8.0, 8.0, 1.0,
                         255, 255, 255, 255, 0.0, 0.0, 1.0, 1.0, 0.5)
                    ],
                }]},
        },
    }
    payload, _report = map_bundle.vita_effects._build_from_rawfiles(
        rawfiles, 5, "farm", graph_source)
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        effect_path = root / "effects.bin"
        material_path = root / "vita_materials.bin"
        effect_path.write_bytes(payload)
        material_path.write_bytes(b"material fixture")
        with mock.patch.object(
                map_bundle.typed, "_material_records",
                return_value=[{"name": "child_material",
                               "color": "child_color"}]):
            assert map_bundle._effect_image_aliases(
                effect_path, material_path) == ["child_color"]


def test_specular_closure_precedes_optional_normal_tail() -> None:
    source = Path(module.__file__).read_text(encoding="utf-8")
    assert source.index("extract_phase(world_specular_basenames)") < source.index(
        "extract_phase(world_normal_basenames)"
    )


def test_world_semantic_companions_are_scoped_and_segmented() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        source = root / "source"
        source.mkdir()

        def material_record(name: str, normal: str, specular: str) -> bytes:
            encoded = [name.encode(), b"colour", normal.encode(),
                       specular.encode(), b"lm_r0c0d0n1s1"]
            header = struct.pack(
                "<5I I 9B 2I B", *(len(value) for value in encoded),
                0x10, 0, 7, 0, 2, 0x62, 0x62, 0x62, 0, 0x06,
                0, 0, 1
            )
            return header + b"".join(encoded)

        records = [
            material_record("world/a", "normal_a", "specular_a"),
            material_record("world/b", "normal_b", "specular_b"),
            material_record("model/only", "normal_model", "specular_model"),
        ]
        source.joinpath("vita_materials.bin").write_bytes(
            struct.pack("<3IQ", 0x4C544956, 4, len(records),
                        sum(len(value) for value in records))
            + b"".join(records)
        )

        # VITW v1 stores the material name in the final 64 bytes of each
        # fixed surface record. Only the first two records are in the world
        # closure; the model-only material must not enter the companion.
        surfaces = []
        for name in ("world/a", "world/b"):
            surface = bytearray(88)
            surface[24:24 + len(name)] = name.encode()
            surfaces.append(bytes(surface))
        source.joinpath("vita_world.bin").write_bytes(
            struct.pack("<5I6f", 0x57544956, 1, 0, 0, len(surfaces),
                        0.0, 0.0, 0.0, 1.0, 1.0, 1.0)
            + b"".join(surfaces)
        )

        retail = root / "retail"
        (retail / "main").mkdir(parents=True)
        payloads = {
            "normal_a": bytes([1]) * 8,
            "normal_b": bytes([2]) * 8,
            "specular_a": bytes([3]) * 8,
            "specular_b": bytes([4]) * 8,
            "normal_model": bytes([5]) * 8,
            "specular_model": bytes([6]) * 8,
        }
        with zipfile.ZipFile(retail / "main" / "iw_00.iwd", "w") as bundle:
            for name, payload in payloads.items():
                iwi = (struct.pack("<3sBBB3h4i", b"IWi", 6, 11, 2,
                                   4, 4, 1, 28 + len(payload), 0, 0, 0)
                       + payload)
                bundle.writestr(f"images/{name}.iwi", iwi)

        old_limit = module.SEMANTIC_EXPORT_MAX_DATA_BYTES
        module.SEMANTIC_EXPORT_MAX_DATA_BYTES = 8
        try:
            normal = module._synthesize_semantic_images_from_iwd(
                source, retail, "test", "normal", root / "normal.bin")
            specular = module._synthesize_semantic_images_from_iwd(
                source, retail, "test", "specular", root / "specular.bin")
        finally:
            module.SEMANTIC_EXPORT_MAX_DATA_BYTES = old_limit

        for report, prefix, expected in (
                (normal, "normal", ["normal_a", "normal_b"]),
                (specular, "specular", ["specular_a", "specular_b"])):
            assert report["scope"] == "world"
            assert report["requested"] == 2
            assert report["missing"] == 0
            assert report["records"] == 2
            assert len(report["segments"]) == 2
            assert [item["records"] for item in report["segments"]] == [1, 1]
            assert [record[0] for record in module._read_image_records(
                root / f"{prefix}.bin")] == [expected[0]]
            assert [record[0] for record in module._read_image_records(
                root / f"{prefix}-1.bin")] == [expected[1]]

        # The knife is an auxiliary WeaponDef model, so a gunXModel-only
        # closure misses it. Preserve its authored material while excluding
        # a model that is neither placed nor used by the fixed loadout.
        model_records = []
        for model_name, material_name in ((b"viewmodel_knife", b"model/only"),
                                          (b"unrelated_model", b"world/a")):
            model_records.append(
                struct.pack("<4I6f", len(model_name), 1, 0, 0,
                            0, 0, 0, 1, 1, 1) + model_name
                + struct.pack("<6I", len(material_name), 0, 0, 0, 0, 0)
                + material_name)
        source.joinpath("vita_models.bin").write_bytes(
            struct.pack("<3IQ", 0x58544956, 1, 2,
                        sum(map(len, model_records))) + b"".join(model_records))
        model_specular = module._synthesize_semantic_images_from_iwd(
            source, retail, "test", "specular", root / "model-specular.bin", "model")
        assert model_specular["requested"] == 1
        assert model_specular["missing"] == 0
        assert [record[0] for record in module._read_image_records(
            root / "model-specular.bin")] == ["specular_model"]


def test_vitl_v3_source_lightmap_sampler_flags_are_validated() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        path = Path(root_name) / "vita_materials.bin"
        name = b"wc/ship_wall"
        color = b"ship_wall_col"
        normal = b"ship_wall_nml"
        specular = b"ship_wall_spc"
        technique = b"l_sm_b0c0d0n0s0"
        header = struct.pack(
            "<5I I 9B", len(name), len(color), len(normal), len(specular),
            len(technique), 0x10, 0, 7, 0, 3, 0x62, 0x62, 0x62, 0, 0x06
        )
        payload = header + name + color + normal + specular + technique
        path.write_bytes(struct.pack("<3IQ", 0x4C544956, 3, 1,
                                     len(name) + len(color) + len(normal)
                                     + len(specular) + len(technique)) + payload)
        report = module._validate_material_sidecar(path)
        assert report and report["records"] == 1
        records = module._material_records(path)
        assert records and records[0]["normal"] == normal.decode()
        assert module._material_image_names(path) == sorted(
            [color.decode(), normal.decode(), specular.decode()]
        )


def test_vitl_v4_source_lit_state_bits_are_validated() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        path = Path(root_name) / "vita_materials.bin"
        name = b"wc/ship_fence"
        color = b"ship_fence_col"
        technique = b"wc_l_sm_t0c0n0s0"
        # v4 extends the v3 fixed header with source GfxStateBits[0/1] and a
        # validity byte. The parser only needs to skip those fixed bytes while
        # preserving the same five variable string slots.
        header = struct.pack(
            "<5I I 9B 2I B", len(name), len(color), 0, 0,
            len(technique), 0x10, 0, 7, 0, 2, 0x62, 0, 0, 0, 0x02,
            0x00001800, 0x00000005, 1
        )
        assert len(header) == 42
        payload = header + name + color + technique
        path.write_bytes(struct.pack(
            "<3IQ", 0x4C544956, 4, 1,
            len(name) + len(color) + len(technique)
        ) + payload)
        report = module._validate_material_sidecar(path)
        assert report and report["records"] == 1
        records = module._material_records(path)
        assert records and records[0]["technique"] == technique.decode()


def test_vitl_v5_preserves_2d_image_and_unlit_state_record_boundaries() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        path = Path(root_name) / "vita_materials.bin"
        name, image, technique = b"fonts/test", b"source_atlas", b"2d"
        # Two records with different string lengths expose a wrong v4 stride.
        records = []
        total = 0
        for material_name in (name, b"fonts/test_glow"):
            header = struct.pack("<6I9B2IB2I2B", len(material_name), len(image), 0, 0,
                len(technique), 0, 0, 43, 0, 1, 0x62, 0, 0, 3, 0,
                0, 0, 0, 0x18004965, 2, 1, 0)
            assert len(header) == 52
            strings = material_name + image + technique
            total += len(strings)
            records.append(header + strings)
        data = struct.pack("<3IQ", 0x4C544956, 5, 2, total) + b"".join(records)
        path.write_bytes(data)
        assert module._validate_material_sidecar(path)["records"] == 2
        assert module._material_image_names(path) == [image.decode()]
        assert module._material_records(path)[1]["name"] == "fonts/test_glow"
        for length in (20, 71, len(data) - 1):
            path.write_bytes(data[:length])
            try:
                module._validate_material_sidecar(path)
            except ValueError:
                pass
            else:
                raise AssertionError("truncated VITL v5 was accepted")


def test_source_ui_closure_uses_original_names_and_tracks_inputs() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        source = root / "src/cgame"
        source.mkdir(parents=True)
        (source / "draw.cpp").write_text('R_RegisterMaterial("reticle_side");')
        (root / "vita_weapons.bin").write_bytes(b"reticle_alt\0")
        records, aggregate = [], 0
        for name, image, semantic in ((b"reticle_side", b"side", 0),
                                      (b"reticle_alt", b"center", 0),
                                      (b"unreferenced_menu", b"unused", 0),
                                      (b"world_material", b"world", 2)):
            strings = name + image + b"2d"
            fixed = struct.pack("<6I9B2IB2I2B", len(name), len(image), 0, 0, 2,
                0, 0, 43, 0, 1, 0x62, 0, 0, 3, 0, 0, 0, 0, 0x18004965, 2, 1, semantic)
            records.append(fixed + strings)
            aggregate += len(strings)
        (root / "vita_materials.bin").write_bytes(struct.pack("<3IQ", 0x4C544956, 5, 4, aggregate) + b"".join(records))
        report = module.source_ui_dependencies(root, root / "src")
        assert report["images"] == ["center", "side"]
        assert report["materials"] == {"reticle_alt": "center", "reticle_side": "side"}
        assert "source/cgame/draw.cpp" in report["inputs"] and "export/vita_weapons.bin" in report["inputs"]
        assert report["runtimeClosureQualified"] is False


def test_source_bitmap_expansion_preserves_luminance_alpha_and_bgr() -> None:
    assert module._expand_iwi_bitmap(bytes((3, 17, 240, 90)), 3) == bytes((3, 3, 3, 17, 240, 240, 240, 90))
    assert module._expand_iwi_bitmap(bytes((12, 34, 56)), 2) == bytes((12, 34, 56, 255))
    assert module._expand_iwi_bitmap(bytes((0, 127)), 5) == bytes((255, 255, 255, 0, 255, 255, 255, 127))
    for fmt in (1, 4, 11, 12, 13):
        assert module._expand_iwi_bitmap(b"unchanged", fmt) == b"unchanged"
    try:
        module._expand_iwi_bitmap(b"x", 3)
    except ValueError:
        pass
    else:
        raise AssertionError("truncated A8L8 pixel accepted")


def test_source_ui_closure_reads_adjacent_font_material_names() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        font_strings = (b"fonts/normalFont", b"source_atlas", b"source_text", b"source_glow")
        font_body = b"".join(font_strings) + bytes(24)
        font = struct.pack("<3IQ", 0x46544956, 2, 1, len(font_body)) + struct.pack(
            "<3Ii2I", len(font_strings[0]), len(font_strings[1]), 1, 16,
            len(font_strings[2]), len(font_strings[3])) + font_body
        (root / "vita_fonts.bin").write_bytes(font)
        records, aggregate = [], 0
        for name in font_strings[2:]:
            image = b"source_atlas"
            strings = name + image + b"2d"
            fixed = struct.pack("<6I9B2IB2I2B", len(name), len(image), 0, 0, 2,
                0, 0, 43, 0, 1, 0x62, 0, 0, 3, 0, 0, 0, 0, 0x18004965, 2, 1, 0)
            records.append(fixed + strings)
            aggregate += len(strings)
        (root / "vita_materials.bin").write_bytes(struct.pack("<3IQ", 0x4C544956, 5, 2, aggregate) + b"".join(records))
        report = module.source_ui_dependencies(root, root / "src")
        assert report["materials"] == {"source_text": "source_atlas", "source_glow": "source_atlas"}
        assert "export/vita_fonts.bin" in report["inputs"]
        for truncated in (font[:19], font[:-1], font + b"x"):
            (root / "vita_fonts.bin").write_bytes(truncated)
            try:
                module.source_ui_dependencies(root, root / "src")
            except ValueError:
                pass
            else:
                raise AssertionError("malformed font dependency record accepted")


def test_typed_db_export_is_deterministic_and_not_playable() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        workspace = root / "workspace"
        source = workspace / "build" / "vita_assets.csv"
        source.parent.mkdir(parents=True)
        with source.open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=["type", "typeName", "name"])
            writer.writeheader()
            writer.writerows([
                {"type": "1", "typeName": "col_map_mp", "name": "maps/mp/mp_shipment.d3dbsp"},
                {"type": "2", "typeName": "material", "name": "mc/mtl_cargocontainer"},
                {"type": "3", "typeName": "weapon", "name": "m4"},
                {"type": "4", "typeName": "sound", "name": "weapons/m4_fire"},
                {"type": "5", "typeName": "xmodel", "name": "xmodel/cargo"},
                {"type": "6", "typeName": "unknown", "name": "ignored"},
            ])
        output = workspace / "assets" / "packages" / "shipment-db-reference.kvap"
        report = module.build_package(source, workspace, output)
        assert report["status"] == "reference-only-pending-typed-db-export"
        assert report["playable"] is False
        assert report["sourceRows"] == 6
        assert report["recordCounts"]["collision"] == 1
        assert report["recordCounts"]["weapon"] == 1
        manifest = workspace / "assets" / "converted" / "shipment-db-export" / "manifest.json"
        document = json.loads(manifest.read_text(encoding="utf-8"))
        assert document["playable"] is False
        assert document["recordSchema"] == "typed-db-name-export-v1"
        assert document["unknownNativeTypes"] == {"unknown": 1}


def test_pointer_free_geometry_sidecars_are_validated_and_packaged() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        workspace = root / "workspace"
        source_dir = workspace / "build" / "vita"
        source_dir.mkdir(parents=True)
        source = source_dir / "vita_assets.csv"
        source.write_text("type,typeName,name\n1,col_map_mp,maps/mp/mp_shipment.d3dbsp\n", encoding="utf-8")
        (source_dir / "vita_world.bin").write_bytes(
            struct.pack("<5I6f", 0x57544956, 1, 1, 3, 1, 0, 0, 0, 1, 1, 1)
            + bytes(40) + struct.pack("<3H", 0, 0, 0) + bytes(88)
        )
        lightgrid_raw = struct.pack("<4H I", 0, 1, 0, 1, 0) + bytes((1, 1, 0))
        lightgrid = (
            struct.pack("<6I6H4I", 0x47544956, 1, 1, 1, 0, 1,
                        0, 0, 0, 0, 0, 0, 1, len(lightgrid_raw), 1, 1)
            + struct.pack("<H", 0) + lightgrid_raw + bytes(4) + bytes(168)
        )
        (source_dir / "vita_lightgrid.bin").write_bytes(lightgrid)
        (source_dir / "vita_collision.bin").write_bytes(
            struct.pack("<8I", 0x43544956, 2, 1, 1, 1, 1, 1, 1)
            + bytes(18) + bytes(36) + bytes(24) + bytes(12) + bytes(6) + bytes(28)
        )
        (source_dir / "vita_weapons.bin").write_bytes(
            struct.pack("<4I", 0x50544956, 1, 1, 160) + bytes(160)
        )
        animation_record = bytearray(182)
        animation_record[:len(b"viewmodel_mp5_fire")] = b"viewmodel_mp5_fire"
        struct.pack_into("<H", animation_record, 128, 9)
        animation_record[130] = 0
        struct.pack_into("<2f", animation_record, 174, 30.0, 30.0)
        (source_dir / "vita_animations.bin").write_bytes(
            struct.pack("<4I", 0x51544956, 1, 1, 182) + animation_record
        )
        raw_payload = b"menuDef { name \"mp_main\" }\0"
        (source_dir / "vita_rawfiles.bin").write_bytes(
            struct.pack("<3IQ", 0x52544956, 1, 1, len(raw_payload))
            + struct.pack("<2I", len("ui_mp/mp_main.menu"), len(raw_payload))
            + b"ui_mp/mp_main.menu" + raw_payload
        )
        localize_name = b"MENU_MULTIPLAYER"
        localize_value = b"MULTIPLAYER"
        localize_record = (struct.pack("<2I", len(localize_name), len(localize_value))
                           + localize_name + localize_value)
        (source_dir / "vita_localize.bin").write_bytes(
            struct.pack("<3IQ", 0x5A544956, 1, 1, len(localize_record))
            + localize_record
        )
        menu_name = b"main"
        menu_font = b"fonts/normalFont"
        menu_item = b"MULTIPLAYER"
        menu_record = (
            struct.pack("<4Ii", len(menu_name), len(menu_font), 1, 1, 0)
            + bytes(20 * 4)
            + menu_name + menu_font
            + struct.pack("<4IiiiI", 0, len(menu_item), 0, 0, 1, 1, 0, 0)
            + bytes(8 * 4 + 4)
            + menu_item
        )
        (source_dir / "vita_menus.bin").write_bytes(
            struct.pack("<3IQ", 0x4D544956, 1, 1, len(menu_record)) + menu_record
        )
        sound_name = b"weapons/m4_fire"
        pcm = struct.pack("<4h", 0, 256, -256, 0)
        sound_record = (
            struct.pack("<7I", len(sound_name), len(pcm), 48000, 16, 1, 4, 1)
            + sound_name + pcm
        )
        (source_dir / "vita_sounds.bin").write_bytes(
            struct.pack("<3IQ", 0x53544956, 1, 1, len(pcm)) + sound_record
        )
        image_name = b"mc/mtl_cargocontainer"
        image_payload = bytes(16)
        image_record = (
            struct.pack("<8I", len(image_name), len(image_payload), 2, 2, 1, 1, 0x31545844, 0)
            + image_name + image_payload
        )
        (source_dir / "vita_images.bin").write_bytes(
            struct.pack("<3IQ", 0x49544956, 1, 1, len(image_payload)) + image_record
        )
        material_name = b"mc/mtl_cargocontainer"
        material_image = b"mc/mtl_cargocontainer"
        material_record = (
            struct.pack("<2I", len(material_name), len(material_image))
            + material_name + material_image
        )
        (source_dir / "vita_materials.bin").write_bytes(
            struct.pack("<3IQ", 0x4C544956, 1, 1,
                        len(material_name) + len(material_image)) + material_record
        )
        font_name = b"fonts/normalFont"
        font_image = b"fonts/normalFont_image"
        glyph = struct.pack("<H6B4f", ord("A"), 0, 0, 8, 8, 12, 0,
                            0.0, 0.0, 0.25, 0.5)
        font_record = (struct.pack("<4I", len(font_name), len(font_image), 1, 16)
                       + font_name + font_image + glyph)
        (source_dir / "vita_fonts.bin").write_bytes(
            struct.pack("<3IQ", 0x46544956, 1, 1,
                        len(font_name) + len(font_image) + len(glyph)) + font_record
        )
        output = workspace / "assets" / "packages" / "shipment-geometry.kvap"
        report = module.build_package(source, workspace, output)
        assert report["status"] == "reference-only-with-typed-geometry-pending-runtime"
        assert report["playable"] is False
        assert report["geometry"]["weaponRecords"] == 1
        assert report["geometry"]["weaponBytes"] == 176
        assert report["geometry"]["animationRecords"] == 1
        assert report["geometry"]["animationBytes"] == 198
        assert report["geometry"]["rawfileRecords"] == 1
        assert report["geometry"]["rawfileDataBytes"] == len(raw_payload)
        assert report["geometry"]["localizedRecords"] == 1
        assert report["geometry"]["localizedDataBytes"] == len(localize_record)
        assert report["geometry"]["menuRecords"] == 1
        assert report["geometry"]["menuDataBytes"] == len(menu_record)
        assert report["geometry"]["soundRecords"] == 1
        assert report["geometry"]["soundDataBytes"] == len(pcm)
        assert report["geometry"]["imageRecords"] == 1
        assert report["geometry"]["imageDataBytes"] == len(image_payload)
        assert report["geometry"]["materialRecords"] == 1
        assert report["geometry"]["materialDataBytes"] == len(material_name) + len(material_image)
        assert report["geometry"]["fontRecords"] == 1
        assert report["geometry"]["fontDataBytes"] == len(font_name) + len(font_image) + len(glyph)
        assert report["geometry"]["lightGridBytes"] == len(lightgrid)
        document = json.loads((workspace / "assets" / "converted" / "shipment-db-export" / "manifest.json").read_text(encoding="utf-8"))
    assert "pointer-free-world-collision-material-image-font-rawfile-stringtable-sidecars" in document["payloadPolicy"]


def test_hud_sidecar_requires_the_runtime_overlay_images() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        sidecar = root / "vita_images.bin"
        names = [
            "hud_m4carbine",
            "hud_mp5",
            "hud_usp_45",
            "overlay_low_health",
            "compass_map_mp_shipment",
            "minimap_tickertape_256x16",
            "minimap_tickertape_background",
            "explosion_flash",
            "ammo_counter_riflebullet",
            "ammo_counter_bullet",
            "score_bar_allies",
            "score_bar_opfor",
            "hud_xpticker480",
            "hud_xpticker480ws",
            "dpad_arrow",
            "grenadeicon",
            "hud_us_grenade",
            "dtimer_bg",
            "dtimer_bg_border",
            "dtimer_0",
            "dtimer_1",
            "dtimer_2",
            "dtimer_3",
            "dtimer_4",
            "dtimer_5",
            "dtimer_6",
            "dtimer_7",
            "dtimer_8",
            "dtimer_9",
        ]
        payload = bytes(8)
        records = []
        for name in names:
            encoded = name.encode("utf-8")
            records.append(
                struct.pack("<8I", len(encoded), len(payload), 2, 2, 1, 1,
                            0x31545844, 0) + encoded + payload
            )
        sidecar.write_bytes(
            struct.pack("<3IQ", 0x49544956, 1, len(records), len(payload) * len(records))
            + b"".join(records)
        )
        hud = module._build_hud_sidecar(sidecar)
        assert hud is not None
        report = module._validate_hud_sidecar(hud)
        assert report and report["records"] == 29
        assert report["dataBytes"] == 920


def test_vitx_v2_skeleton_and_weights_are_validated() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        sidecar = root / "vita_models.bin"
        name = b"xmodel/test"
        material = b"mc/test"
        model_header = struct.pack(
            "<4I6f", len(name), 1, 1, 3, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0
        )
        skin_header = struct.pack("<4I", 1, 1, 1, 0)
        parent = b"\xff"
        bone = struct.pack("<7f", 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0) + struct.pack("<f", 1.0)
        surface = struct.pack("<6I", len(material), 0, 1, 0, 3, 0)
        vertex = struct.pack("<3f2f2I", 0.0, 0.0, 0.0, 0.0, 0.0, 0, 0) + struct.pack("<I", 0xFFFFFFFF)
        weights = struct.pack("<4H4H", 0, 0, 0, 0, 65535, 0, 0, 0)
        payload = (
            model_header + name + skin_header + parent + bone + surface + material
            + vertex + weights + struct.pack("<3H", 0, 0, 0)
        )
        sidecar.write_bytes(struct.pack("<3IQ", 0x58544956, 2, 1, len(payload)) + payload)
        report = module._validate_model_sidecar(sidecar)
        assert report and report["version"] == 2 and report["records"] == 1


def test_vitx_v4_authored_flash_tag_is_validated() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        sidecar = root / "vita_models.bin"
        name = b"viewmodel_m4_mp"
        material = b"weapon_m4_col"
        model_header = struct.pack(
            "<4I6f", len(name), 1, 1, 3, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0
        )
        # VITX v4 keeps the v3 local/bind pose record and appends tag_flash
        # as a one-based bone index in the skin header.
        skin_header = struct.pack("<5I", 1, 1, 1, 0, 1)
        parent = b"\xff"
        bone = struct.pack(
            "<16f", 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 2.0,
            0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 2.0
        )
        surface = struct.pack("<6I", len(material), 0, 1, 0, 3, 0)
        vertex = (struct.pack("<3f2f2I", 0.0, 0.0, 0.0, 0.0, 0.0, 0, 0)
                  + struct.pack("<I", 0xFFFFFFFF))
        weights = struct.pack("<4H4H", 0, 0, 0, 0, 65535, 0, 0, 0)
        payload = (
            model_header + name + skin_header + parent + bone + surface + material
            + vertex + weights + struct.pack("<3H", 0, 0, 0)
        )
        sidecar.write_bytes(struct.pack("<3IQ", 0x58544956, 4, 1, len(payload)) + payload)
        report = module._validate_model_sidecar(sidecar)
        assert report and report["version"] == 4 and report["records"] == 1


def test_vitx_v5_bone_names_and_anonymous_root_are_validated() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        sidecar = root / "vita_models.bin"
        name = b"viewmodel_mp5"
        material = b"weapon_mp5_col"
        model_header = struct.pack(
            "<4I6f", len(name), 1, 1, 3, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0
        )
        bone_name = b"tag_weapon\0".ljust(64, b"\0")
        skin_header = struct.pack("<6I", 1, 1, 1, 1, 1, len(bone_name))
        parent = b"\xff"
        bone = struct.pack(
            "<16f", 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 2.0,
            0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 2.0
        )
        surface = struct.pack("<6I", len(material), 0, 1, 0, 3, 0)
        vertex = (struct.pack("<3f2f2I", 0.0, 0.0, 0.0, 0.0, 0.0, 0, 0)
                  + struct.pack("<I", 0xFFFFFFFF))
        weights = struct.pack("<4H4H", 0, 0, 0, 0, 65535, 0, 0, 0)
        payload = (
            model_header + name + skin_header + parent + bone_name + bone
            + surface + material + vertex + weights + struct.pack("<3H", 0, 0, 0)
        )
        sidecar.write_bytes(struct.pack("<3IQ", 0x58544956, 5, 1, len(payload)) + payload)
        report = module._validate_model_sidecar(sidecar)
        assert report and report["version"] == 5 and report["records"] == 1

        anonymous_root = bytes(64)
        anonymous_payload = (
            model_header + name + skin_header + parent + anonymous_root + bone
            + surface + material + vertex + weights + struct.pack("<3H", 0, 0, 0)
        )
        sidecar.write_bytes(
            struct.pack("<3IQ", 0x58544956, 5, 1, len(anonymous_payload))
            + anonymous_payload
        )
        anonymous_report = module._validate_model_sidecar(sidecar)
        assert anonymous_report and anonymous_report["records"] == 1


def _write_vlod_fixture(path: Path, *, surface_vertex_count: int = 3,
                        distances: tuple[float, float, float, float] =
                        (100.0, 1000000.0, 0.0, 0.0)) -> None:
    name = b"me_test_prop"
    material = b"mc/test_prop"
    vertices = b"".join(
        struct.pack("<3f2f3I", float(index), 0.0, 0.0,
                    0.0, 0.0, 0, 0, 0xFFFFFFFF)
        for index in range(3)
    )
    surface = struct.pack(
        "<6I", len(material), 0, surface_vertex_count, 0, 3, 0
    ) + material
    lod = struct.pack("<4I", 1, 1, 3, 3) + surface + vertices
    lod += struct.pack("<3H", 0, 1, 2)
    model = struct.pack("<4I4f", len(name), 2, 1, 0, *distances)
    payload = model + name + lod
    path.write_bytes(
        struct.pack("<3IQ", 0x444F4C56, 1, 1, len(payload)) + payload
    )


def test_vlod_v1_authored_lower_model_lods_are_validated() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        sidecar = Path(root_name) / "vita_model_lods.bin"
        _write_vlod_fixture(sidecar)
        report = module._validate_model_lod_sidecar(sidecar)
        assert report and report["version"] == 1
        assert report["records"] == 1 and report["lodRecords"] == 1
        assert report["vertices"] == 3 and report["indices"] == 3


def test_vlod_rejects_cross_surface_indices_and_unordered_distances() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        sidecar = Path(root_name) / "vita_model_lods.bin"
        _write_vlod_fixture(sidecar, surface_vertex_count=2)
        try:
            module._validate_model_lod_sidecar(sidecar)
        except ValueError as error:
            assert "surface triangle index" in str(error)
        else:
            raise AssertionError("cross-surface VLOD index was accepted")

        _write_vlod_fixture(
            sidecar, distances=(500.0, 100.0, 0.0, 0.0)
        )
        try:
            module._validate_model_lod_sidecar(sidecar)
        except ValueError as error:
            assert "not ordered" in str(error)
        else:
            raise AssertionError("unordered VLOD thresholds were accepted")


def test_vitq_v2_decoded_tracks_are_validated() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        sidecar = root / "vita_animations.bin"
        record = bytearray(162)
        record[:len(b"viewmodel_mp5_fire")] = b"viewmodel_mp5_fire"
        struct.pack_into("<HBB", record, 128, 2, 0, 0)
        record[132] = 1
        record[142] = 2
        notify_a = struct.pack("<If", len(b"sound_fire"), 0.25) + b"sound_fire"
        notify_b = struct.pack("<If", len(b"end"), 1.0) + b"end"
        struct.pack_into("<2I2f", record, 146, 1,
                         16 + len(b"xmodel/test") + 64 + len(notify_a) + len(notify_b),
                         30.0, 30.0)
        model_name = b"xmodel/test"
        track = struct.pack("<4I", len(model_name), 1, 2, 0) + model_name
        sample = struct.pack("<7f", 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0) + struct.pack("<f", 1.0)
        payload = bytes(record) + track + sample + sample + notify_a + notify_b
        sidecar.write_bytes(struct.pack("<4I", 0x51544956, 2, 1, 162) + payload)
        report = module._validate_animation_sidecar(sidecar)
        assert report and report["version"] == 2 and report["records"] == 1


def test_vitq_v3_compressed_source_closure_is_validated() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        sidecar = root / "vita_animations.bin"
        record = bytearray(194)
        record[:len(b"viewmodel_mp5_idle")] = b"viewmodel_mp5_idle"
        struct.pack_into("<HBB", record, 128, 1, 0, 0)
        record[132:142] = bytes((1, 0, 0, 0, 0, 0, 0, 0, 1, 1))
        struct.pack_into("<4B", record, 142, 1, 0, 0, 0)
        struct.pack_into("<6H", record, 146, 1, 0, 0, 0, 0, 0)
        native_bytes = 64 + 1 + 132
        struct.pack_into("<7I", record, 158, 0, 0, 1,
                         native_bytes, 0, 0, 0)
        struct.pack_into("<2f", record, 186, 30.0, 30.0)
        bone_name = b"tag_weapon\0".ljust(64, b"\0")
        notify = b"end\0".ljust(128, b"\0") + struct.pack("<f", 1.0)
        payload = bytes(record) + bone_name + b"\0" + notify
        sidecar.write_bytes(struct.pack("<4I", 0x51544956, 3, 1, 194) + payload)
        report = module._validate_animation_sidecar(sidecar)
        assert report and report["version"] == 3 and report["records"] == 1


def test_world_surface_materials_are_bounded_and_deterministic() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        sidecar = root / "vita_world.bin"
        material = b"wc/ship_wall"
        surface = struct.pack("<6I", 0, 0, 1, 0, 3, 0) + material.ljust(64, b"\0")
        sidecar.write_bytes(
            struct.pack("<5I6f", 0x57544956, 1, 1, 3, 1,
                        0.0, 0.0, 0.0, 1.0, 1.0, 1.0)
            + bytes(40) + struct.pack("<3H", 0, 0, 0) + surface
        )
        assert module._world_surface_material_names(sidecar) == ["wc/ship_wall"]


def test_vitw_v2_lighting_extension_is_validated() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        sidecar = root / "vita_world.bin"
        material = b"wc/ship_wall"
        surface = struct.pack("<6I", 0, 0, 1, 0, 3, 0) + material.ljust(64, b"\0")
        lighting = struct.pack(
            "<15fI3f",
            0.0, 0.0, -1.0,
            1.0, 0.95, 0.9,
            0.1, 0.1, 0.12,
            0.9, 1.0, 0.75,
            0.1, 0.2, 1.3,
            1,
            -43.0, 51.5, 0.0,
        )
        sidecar.write_bytes(
            struct.pack("<5I6f", 0x57544956, 2, 1, 3, 1,
                        0.0, 0.0, 0.0, 1.0, 1.0, 1.0)
            + lighting + bytes(40) + struct.pack("<3H", 0, 0, 0) + surface
        )
        report = module._validate_geometry(sidecar, "world")
        assert report and report["bytes"] == len(sidecar.read_bytes())
        assert module._world_surface_material_names(sidecar) == ["wc/ship_wall"]


def test_vitw_v3_reflection_probe_index_is_validated() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        sidecar = root / "vita_world.bin"
        material = b"wc/ship_wall"
        surface = (struct.pack("<6I", 0, 0, 1, 0, 3, 0)
                   + material.ljust(64, b"\0") + struct.pack("<I", 4))
        lighting = struct.pack(
            "<15fI3f",
            0.0, 0.0, -1.0,
            1.0, 0.95, 0.9,
            0.1, 0.1, 0.12,
            0.9, 1.0, 0.75,
            0.1, 0.2, 1.3,
            1,
            -43.0, 51.5, 0.0,
        )
        sidecar.write_bytes(
            struct.pack("<5I6f", 0x57544956, 3, 1, 3, 1,
                        0.0, 0.0, 0.0, 1.0, 1.0, 1.0)
            + lighting + bytes(40) + struct.pack("<3H", 0, 0, 0) + surface
        )
        report = module._validate_geometry(sidecar, "world")
        assert report and report["bytes"] == len(sidecar.read_bytes())
        assert module._world_surface_material_names(sidecar) == ["wc/ship_wall"]


def test_generated_lightmap_records_survive_iwi_synthesis() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        source = root / "source"
        source.mkdir()
        material_name = b"wc/ship_wall"
        image_name = b"ship_wall"
        source.joinpath("vita_materials.bin").write_bytes(
            struct.pack("<3IQ", 0x4C544956, 1, 1, len(material_name) + len(image_name))
            + struct.pack("<2I", len(material_name), len(image_name))
            + material_name + image_name
        )
        surface = struct.pack("<6I", 0, 0, 1, 0, 3, 0) + material_name.ljust(64, b"\0")
        source.joinpath("vita_world.bin").write_bytes(
            struct.pack("<5I6f", 0x57544956, 1, 1, 3, 1,
                        0.0, 0.0, 0.0, 1.0, 1.0, 1.0)
            + bytes(40) + struct.pack("<3H", 0, 0, 0) + surface
        )
        lightmap_name = b"*lightmap0_primary"
        lightmap_payload = bytes([32, 64, 192, 255])
        reflection_name = b"*reflection_probe0"
        reflection_payload = bytes(range(96))  # six 2x2 A8R8G8B8 faces
        image_bytes = len(lightmap_payload) + len(reflection_payload)
        source.joinpath("vita_images.bin").write_bytes(
            struct.pack("<3IQ", 0x49544956, 1, 2, image_bytes)
            + struct.pack("<8I", len(lightmap_name), len(lightmap_payload),
                          2, 2, 1, 1, 50, 0)
            + lightmap_name + lightmap_payload
            + struct.pack("<8I", len(reflection_name), len(reflection_payload),
                          2, 2, 1, 1, 21, 7)
            + reflection_name + reflection_payload
        )
        retail = root / "retail"
        (retail / "main").mkdir(parents=True)
        with zipfile.ZipFile(retail / "main" / "iw_00.iwd", "w"):
            pass
        output = module._synthesize_images_from_iwd(source, retail)
        records = module._read_image_records(output)
        assert [record[0] for record in records] == [
            "*lightmap0_primary", "*reflection_probe0"]
        assert records[0][1] == lightmap_payload and records[0][6] == 50
        assert records[1][1] == reflection_payload and records[1][6] == 21
        assert records[1][7] == 7


def test_reflection_probe_records_split_from_main_image_stream() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        image_path = root / "vita_images.bin"
        reflection_path = root / "reflection-probes.bin"
        ordinary = ("wall", bytes(8), 4, 4, 1, 1, 0x31545844, 0)
        probe = ("*reflection_probe0", bytes(96), 2, 2, 1, 1, 21, 7)
        module._write_image_records(image_path, [ordinary, probe])
        report = module._split_reflection_probe_sidecar(
            image_path, reflection_path)
        assert report == {"records": 1, "dataBytes": 96}
        assert [record[0] for record in module._read_image_records(image_path)] == [
            "wall"
        ]
        probe_records = module._read_image_records(reflection_path)
        assert [record[0] for record in probe_records] == ["*reflection_probe0"]
        assert probe_records[0][1] == bytes(96)


def test_iwi_alias_suffix_matches_archive_variant_filename() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        source = root / "source"
        source.mkdir()
        material_name = b"wc/me_fence_chainlink"
        image_name = b"me_chainlink_fence_color#0"
        material_record = struct.pack("<2I", len(material_name), len(image_name))
        source.joinpath("vita_materials.bin").write_bytes(
            struct.pack("<3IQ", 0x4C544956, 1, 1,
                        len(material_record) + len(material_name) + len(image_name))
            + material_record + material_name + image_name
        )
        surface = struct.pack("<6I", 0, 0, 1, 0, 3, 0) + material_name.ljust(64, b"\0")
        source.joinpath("vita_world.bin").write_bytes(
            struct.pack("<5I6f", 0x57544956, 1, 1, 3, 1,
                        0.0, 0.0, 0.0, 1.0, 1.0, 1.0)
            + bytes(40) + struct.pack("<3H", 0, 0, 0) + surface
        )
        retail = root / "retail"
        (retail / "main").mkdir(parents=True)
        payload = bytes(8)
        declared = 28 + len(payload)
        iwi = (struct.pack("<3sBBB3h4i", b"IWi", 6, 11, 2, 4, 4, 1,
                           declared, 0, 0, 0) + payload)
        with zipfile.ZipFile(retail / "main" / "iw_00.iwd", "w") as bundle:
            bundle.writestr("images/me_chainlink_fence_color#0.iwi", iwi)
        output = module._synthesize_images_from_iwd(source, retail)
        records = module._read_image_records(output)
        assert [record[0] for record in records] == [image_name.decode()]
        assert records[0][1] == payload and records[0][2:4] == (4, 4)
        assert records[0][7] == 2


def test_static_model_tree_maps_are_reserved_from_retail_iwds() -> None:
    """Keep authored pine/facade maps ahead of the bounded image cutoff."""
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        source = root / "source"
        source.mkdir()
        materials = [
            (b"mc/mtl_pine", b"tree_pine_col"),
            (b"mc/mtl_tree_shadow_caster", b"tree_shadowcaster"),
        ]
        material_payload = b"".join(
            struct.pack("<2I", len(name), len(color)) + name + color
            for name, color in materials
        )
        source.joinpath("vita_materials.bin").write_bytes(
            struct.pack("<3IQ", 0x4C544956, 1, len(materials), len(material_payload))
            + material_payload
        )
        retail = root / "retail"
        (retail / "main").mkdir(parents=True)
        payload = bytes(range(16))
        iwi = (
            struct.pack("<3sBBB3h4i", b"IWi", 6, 12, 2, 4, 4, 1,
                        28 + len(payload), 0, 0, 0)
            + payload
        )
        with zipfile.ZipFile(retail / "main" / "iw_00.iwd", "w") as bundle:
            bundle.writestr("images/tree_pine_col.iwi", iwi)
            bundle.writestr("images/tree_shadowcaster.iwi", iwi)
        output = module._synthesize_images_from_iwd(source, retail)
        records = module._read_image_records(output)
        assert [record[0] for record in records] == [
            "tree_pine_col", "tree_shadowcaster"
        ]
        assert all(record[1] == payload for record in records)


def test_weather_iwi_payloads_are_reserved_for_rain_maps() -> None:
    """The VENV rain flag must have its authored image closure beside it."""
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        source = root / "source"
        source.mkdir()
        materials = [
            (b"gfx_rain_spray", b"rain_spray"),
            (b"gfx_wind_rain_atlas", b"wind_rain_atlas"),
        ]
        material_payload = b"".join(
            struct.pack("<2I", len(name), len(color)) + name + color
            for name, color in materials
        )
        source.joinpath("vita_materials.bin").write_bytes(
            struct.pack("<3IQ", 0x4C544956, 1, len(materials), len(material_payload))
            + material_payload
        )
        retail = root / "retail"
        (retail / "main").mkdir(parents=True)
        payload = bytes(8)
        iwi = (
            struct.pack("<3sBBB3h4i", b"IWi", 6, 11, 2, 4, 4, 1,
                        28 + len(payload), 0, 0, 0)
            + payload
        )
        with zipfile.ZipFile(retail / "main" / "iw_00.iwd", "w") as bundle:
            bundle.writestr("images/rain_spray.iwi", iwi)
            bundle.writestr("images/wind_rain_atlas.iwi", iwi)
        output = module._synthesize_images_from_iwd(source, retail, "farm")
        records = module._read_image_records(output)
        assert [record[0] for record in records] == [
            "rain_spray", "wind_rain_atlas"
        ]
        assert all(record[1] == payload for record in records)


def test_source_sky_color_is_reserved_before_bounded_world_tail() -> None:
    """A saturated image envelope must retain the authored map sky first."""
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        source = root / "source"
        source.mkdir()
        materials = [
            (b"wc/sky_launchfacility", b"sp_launchfacility_a_ft", b"wc_sky"),
            (b"wc/ordinary_wall", b"ordinary_wall_col", b"l_sm_b0c0d0n0s0"),
        ]
        material_payload = b"".join(
            struct.pack("<5I I 9B", len(name), len(color), 0, 0,
                        len(technique), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
            + name + color + technique
            for name, color, technique in materials
        )
        source.joinpath("vita_materials.bin").write_bytes(
            struct.pack("<3IQ", 0x4C544956, 3, len(materials),
                        len(material_payload)) + material_payload
        )
        surfaces = b"".join(
            struct.pack("<6I", 0, 0, 0, 0, 0, 0) + name.ljust(64, b"\0")
            for name, _, _ in materials
        )
        source.joinpath("vita_world.bin").write_bytes(
            struct.pack("<5I6f", 0x57544956, 1, 0, 0, len(materials),
                        0.0, 0.0, 0.0, 1.0, 1.0, 1.0) + surfaces
        )
        retail = root / "retail"
        (retail / "main").mkdir(parents=True)
        payload = bytes(8)
        iwi = (struct.pack("<3sBBB3h4i", b"IWi", 6, 11, 2, 4, 4, 1,
                           28 + len(payload), 0, 0, 0) + payload)
        with zipfile.ZipFile(retail / "main" / "iw_00.iwd", "w") as bundle:
            bundle.writestr("images/sp_launchfacility_a_ft.iwi", iwi)
            bundle.writestr("images/ordinary_wall_col.iwi", iwi)

        previous_cap = module.IMAGE_EXPORT_MAX_DATA_BYTES
        module.IMAGE_EXPORT_MAX_DATA_BYTES = len(payload)
        try:
            output = module._synthesize_images_from_iwd(source, retail, "countdown")
        finally:
            module.IMAGE_EXPORT_MAX_DATA_BYTES = previous_cap
        records = module._read_image_records(output)
        assert [record[0] for record in records] == ["sp_launchfacility_a_ft"]


def test_localized_iwd_font_atlas_is_prioritized() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        source = root / "source"
        source.mkdir()
        material_name = b"fonts/gamefonts_pc_glow"
        image_name = b"gamefonts_pc"
        material_record = struct.pack("<2I", len(material_name), len(image_name))
        source.joinpath("vita_materials.bin").write_bytes(
            struct.pack("<3IQ", 0x4C544956, 1, 1, len(material_record)
                        + len(material_name) + len(image_name))
            + material_record + material_name + image_name
        )
        retail = root / "retail"
        (retail / "main").mkdir(parents=True)
        # The real game stores gamefonts_pc in the localized IWD set.  Keep a
        # minimal valid DXT1 IWI here so the bounded extractor must scan it.
        payload = bytes(8)
        declared = 28 + len(payload)
        iwi = (struct.pack("<3sBBB3h4i", b"IWi", 6, 11, 2, 2, 2, 1,
                           declared, 0, 0, 0) + payload)
        with zipfile.ZipFile(retail / "main" / "localized_english_iw00.iwd", "w") as bundle:
            bundle.writestr("images/gamefonts_pc.iwi", iwi)
        output = module._synthesize_images_from_iwd(source, retail)
        records = module._read_image_records(output)
        assert [record[0] for record in records] == ["gamefonts_pc"]
        assert records[0][1] == payload and records[0][2:4] == (2, 2)
        assert records[0][7] == 2


def test_iwi_mips_are_reordered_for_vita_texture_consumers() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        source = root / "source"
        source.mkdir()
        material_name = b"fonts/gamefonts_pc_glow"
        image_name = b"gamefonts_pc"
        material_record = struct.pack("<2I", len(material_name), len(image_name))
        source.joinpath("vita_materials.bin").write_bytes(
            struct.pack("<3IQ", 0x4C544956, 1, 1,
                        len(material_record) + len(material_name) + len(image_name))
            + material_record + material_name + image_name
        )
        retail = root / "retail"
        (retail / "main").mkdir(parents=True)
        # IWI stores the smallest mip first.  Three DXT1 4x4/2x2/1x1 levels
        # make the expected conversion order unambiguous without a large
        # image payload.
        smallest_first = bytes([1]) * 8 + bytes([2]) * 8 + bytes([3]) * 8
        declared = 28 + len(smallest_first)
        iwi = (struct.pack("<3sBBB3h4i", b"IWi", 6, 11, 0, 4, 4, 1,
                           declared, 0, 0, 0) + smallest_first)
        with zipfile.ZipFile(retail / "main" / "localized_english_iw00.iwd", "w") as bundle:
            bundle.writestr("images/gamefonts_pc.iwi", iwi)
        output = module._synthesize_images_from_iwd(source, retail)
        records = module._read_image_records(output)
        assert records[0][1] == bytes([3]) * 8 + bytes([2]) * 8 + bytes([1]) * 8


def test_uncompressed_hud_iwi_is_supported() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        source = root / "source"
        source.mkdir()
        material_name = b"hud_score_bar"
        image_name = b"score_bar_allies"
        material_record = struct.pack("<2I", len(material_name), len(image_name))
        source.joinpath("vita_materials.bin").write_bytes(
            struct.pack("<3IQ", 0x4C544956, 1, 1,
                        len(material_record) + len(material_name) + len(image_name))
            + material_record + material_name + image_name
        )
        retail = root / "retail"
        (retail / "main").mkdir(parents=True)
        payload = bytes(range(16))  # 2x2 A8R8G8B8
        declared = 28 + len(payload)
        iwi = (struct.pack("<3sBBB3h4i", b"IWi", 6, 1, 2, 2, 2, 1,
                           declared, 0, 0, 0) + payload)
        with zipfile.ZipFile(retail / "main" / "iw_00.iwd", "w") as bundle:
            bundle.writestr("images/score_bar_allies.iwi", iwi)
        output = module._synthesize_images_from_iwd(source, retail)
        records = module._read_image_records(output)
        assert [record[0] for record in records] == ["score_bar_allies"]
        assert records[0][1] == payload and records[0][6] == 21


def test_vitf_v2_source_material_identity_and_corruption() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        sidecar = Path(temporary) / "vita_fonts.bin"
        name, image = b"fonts/fixture", b"fixture_atlas"
        material, glow = b"fonts/fixture_material", b"fonts/fixture_glow"
        glyph = struct.pack("<HbbBBBB4f", 65, -1, -14, 12, 10, 14, 0,
                            0.1, 0.2, 0.3, 0.4)

        def fixture(version: int, material_name: bytes = material,
                    glow_name: bytes = glow) -> bytes:
            strings = name + image
            header = struct.pack("<4I", len(name), len(image), 1, 16)
            if version == 2:
                strings += material_name + glow_name
                header += struct.pack("<2I", len(material_name), len(glow_name))
            return (struct.pack("<3IQ", 0x46544956, version, 1, len(strings) + len(glyph))
                    + header + strings + glyph)

        for version in (1, 2):
            sidecar.write_bytes(fixture(version))
            report = module._validate_font_sidecar(sidecar)
            assert report and report["version"] == version and report["records"] == 1
        sidecar.write_bytes(fixture(2, glow_name=b""))
        assert module._validate_font_sidecar(sidecar)["version"] == 2
        good = fixture(2)
        damaged = [fixture(2, material_name=b""), fixture(2, material_name=b"bad\0name")]
        damaged += [good[:cut] for cut in range(1, len(good))]
        nonfinite = bytearray(good)
        struct.pack_into("<f", nonfinite, len(nonfinite) - 4, float("nan"))
        damaged.append(bytes(nonfinite))
        for data in damaged:
            sidecar.write_bytes(data)
            try:
                module._validate_font_sidecar(sidecar)
            except ValueError:
                pass
            else:
                raise AssertionError("corrupt source font was accepted")


if __name__ == "__main__":
    test_shipment_image_envelope_keeps_source_material_closure()
    test_specular_closure_precedes_optional_normal_tail()
    test_world_semantic_companions_are_scoped_and_segmented()
    test_typed_db_export_is_deterministic_and_not_playable()
    test_pointer_free_geometry_sidecars_are_validated_and_packaged()
    test_vitq_v2_decoded_tracks_are_validated()
    test_vitq_v3_compressed_source_closure_is_validated()
    test_world_surface_materials_are_bounded_and_deterministic()
    test_vitw_v3_reflection_probe_index_is_validated()
    test_generated_lightmap_records_survive_iwi_synthesis()
    test_reflection_probe_records_split_from_main_image_stream()
    test_iwi_alias_suffix_matches_archive_variant_filename()
    test_source_sky_color_is_reserved_before_bounded_world_tail()
    test_localized_iwd_font_atlas_is_prioritized()
    test_uncompressed_hud_iwi_is_supported()
    test_vitf_v2_source_material_identity_and_corruption()
    print("shipment typed export tests: ok")
