from __future__ import annotations

import importlib.util
import hashlib
import struct
import sys
from pathlib import Path


MODULE = Path(__file__).resolve().parents[1] / "vertical_slice_packs.py"
sys.path.insert(0, str(MODULE.parent))
SPEC = importlib.util.spec_from_file_location("vertical_slice_packs", MODULE)
assert SPEC and SPEC.loader
packs = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = packs
SPEC.loader.exec_module(packs)


def test_vitl_v5_collects_original_2d_texture_slot() -> None:
    strings = [b"fonts/source", b"source_atlas", b"", b"", b"2d"]
    fixed = struct.pack("<6I9B2IB2I2B", *(len(s) for s in strings),
        0, 0, 43, 0, 1, 0x62, 0, 0, 3, 0, 0, 0, 0, 0x18004965, 2, 1, 0)
    data = struct.pack("<3IQ", 0x4C544956, 5, 1, sum(map(len, strings)))
    assert packs.vitl_image_names(data + fixed + b"".join(strings)) == {"source_atlas"}


def test_runtime_manifest_stays_within_vita_parser_and_prefix_limits() -> None:
    payload = {"playable": True, "payloadPolicy": "runtime-ready-v1", "proofHash": "a" * 64}
    assert '"playable": true' in packs.embedded_content_manifest(payload)
    for invalid in (dict(payload, large="a" * (16 * 1024)),
                    {"leadingReport": "a" * 4096, **payload}):
        try:
            packs.embedded_content_manifest(invalid)
        except packs.kvap.KvapError:
            pass
        else:
            raise AssertionError("runtime-unreadable manifest accepted")


def _dxt1_chain(width: int, height: int, levels: int) -> bytes:
    payload = bytearray()
    for level in range(levels):
        level_width = max(1, width >> level)
        level_height = max(1, height >> level)
        payload.extend(bytes([level + 1]) * packs.mip_bytes(
            0x31545844, level_width, level_height
        ))
    return bytes(payload)


def test_map_family_classifier_handles_segments_and_common_images() -> None:
    assert packs.map_chunk_index(0x4D50000000000000) == 0
    assert packs.map_chunk_index(0x4D50000000000002) == 2
    assert packs.map_chunk_index(0x4D49020000000702) == 2
    assert packs.map_chunk_index(packs.COMMON_MODEL_COLOR_ASSET) is None
    assert packs.map_chunk_index(0x1234567890ABCDEF) is None


def test_viti_trimming_keeps_authored_lower_mips() -> None:
    name = b"world/concrete_col"
    payload = _dxt1_chain(2048, 2048, 12)
    record = struct.pack("<8I", len(name), len(payload), 2048, 2048,
                         1, 12, 0x31545844, 0) + name + payload
    source = struct.pack("<IIIQ", 0x49544956, 1, 1, len(payload)) + record
    rebuilt, report = packs.trim_viti(source)
    _, _, count, aggregate = struct.unpack_from("<IIIQ", rebuilt, 0)
    _, payload_bytes, width, height, _, levels, _, _ = struct.unpack_from(
        "<8I", rebuilt, 20
    )
    assert count == 1
    assert (width, height, levels) == (512, 512, 10)
    assert payload_bytes == aggregate
    assert report["trimmedRecords"] == 1
    payload_offset = 20 + 32 + len(name)
    assert rebuilt[payload_offset] == 3  # original 512x512 mip


def test_semantic_viti_trimming_enforces_vita_top_mip() -> None:
    name = b"world/concrete_nml"
    payload = _dxt1_chain(1024, 1024, 11)
    record = struct.pack("<8I", len(name), len(payload), 1024, 1024,
                         1, 11, 0x31545844, 0) + name + payload
    source = struct.pack("<IIIQ", 0x49544956, 1, 1, len(payload)) + record
    rebuilt, report = packs.trim_viti(source, 256)
    _, payload_bytes, width, height, _, levels, _, _ = struct.unpack_from(
        "<8I", rebuilt, 20
    )
    assert (width, height, levels) == (256, 256, 9)
    assert payload_bytes < len(payload)
    assert report["maximumTopMip"] == 256


def test_model_semantic_trim_protects_fixed_viewmodel_top_mip() -> None:
    protected_name = b"viewhands_marine_gloves_nml"
    prop_name = b"cardboard_box_normal"
    payload = _dxt1_chain(1024, 1024, 11)
    records = [
        struct.pack("<8I", len(protected_name), len(payload), 1024, 1024,
                    1, 11, 0x31545844, 0) + protected_name + payload,
        struct.pack("<8I", len(prop_name), len(payload), 1024, 1024,
                    1, 11, 0x31545844, 0) + prop_name + payload,
    ]
    source = (struct.pack("<IIIQ", 0x49544956, 1, len(records),
                          len(payload) * 2)
              + b"".join(records))
    rebuilt, report = packs.trim_viti(
        source, 128, {protected_name.decode("ascii")}
    )
    first = struct.unpack_from("<8I", rebuilt, 20)
    second_offset = 20 + 32 + len(protected_name) + first[1]
    second = struct.unpack_from("<8I", rebuilt, second_offset)
    assert first[2:4] == (1024, 1024)
    assert second[2:4] == (128, 128)
    assert report["protectedRecords"] == 1
    assert packs.is_vertical_slice_viewmodel_semantic(
        "~weapon_mp5_s-rgb&weapon_mp5_cos"
    )
    assert not packs.is_vertical_slice_viewmodel_semantic(
        "cardboard_box_normal"
    )


def test_script_selected_hands_and_offhands_keep_first_person_priority() -> None:
    # The source DM team scripts select these hands; the base viewhands
    # model alone does not cover either Shipment or Crossfire's team pair.
    for color in ("viewhand_black_kit_col", "op_force_clr",
                  "weapon_parbolic_knife_col", "m67_grenade_col", "m84_flashbang_col"):
        assert color in packs.REQUIRED_COMMON_MODEL_COLOR_NAMES
        assert packs.image_cap(color) == 1024
    for semantic in ("viewhand_black_kit_nml", "op_force_nml",
                     "~m67_grenade_spc-rgb&m67_gren~8648b09e"):
        assert packs.is_vertical_slice_viewmodel_semantic(semantic)
        assert packs.image_cap(semantic) == 1024


def _viti_record(name: str, payload: bytes) -> bytes:
    encoded = name.encode("utf-8")
    return (struct.pack("<8I", len(encoded), len(payload), 4, 4, 1, 1,
                        0x31545844, 0)
            + encoded + payload)


def test_viti_selection_is_exact_and_canonical() -> None:
    records = [
        _viti_record("weapon_usp_c", b"usp-data"),
        _viti_record("world/concrete_col", b"world-data"),
        _viti_record("weapon_mp5_c", b"mp5-data"),
    ]
    source = (struct.pack("<IIIQ", 0x49544956, 1, len(records),
                          len(b"usp-dataworld-datamp5-data"))
              + b"".join(records))
    selected, names = packs.select_viti_records(
        source, {"weapon_mp5_c", "weapon_usp_c"}
    )
    assert names == {"weapon_mp5_c", "weapon_usp_c"}
    assert packs.viti_names(selected) == names
    # The common companion is independent of source record order.
    mp5_offset = selected.find(b"weapon_mp5_c")
    usp_offset = selected.find(b"weapon_usp_c")
    assert 0 <= mp5_offset < usp_offset
    assert b"world/concrete_col" not in selected


def test_viti_lod_repair_uses_exact_canonical_donor_record() -> None:
    base_record = _viti_record("shipment_normal", b"base")
    donor_record = _viti_record("car_engine_nml", b"source-owned-normal")
    base = (struct.pack("<IIIQ", 0x49544956, 1, 1, len(b"base"))
            + base_record)
    donor = (struct.pack("<IIIQ", 0x49544956, 1, 1,
                         len(b"source-owned-normal"))
             + donor_record)
    repaired = packs.merge_viti_aliases(
        base, [donor], {"car_engine_nml"}
    )
    assert packs.viti_names(repaired) == {
        "car_engine_nml", "shipment_normal"
    }
    assert repaired.find(b"car_engine_nml") < repaired.find(b"shipment_normal")
    assert packs.MODEL_SEMANTIC_LOD_ALIAS_REPAIRS[0][
        0x4D49040000000000
    ] == {"car_engine_nml"}


def test_selected_chunk_hash_binds_asset_identity_and_payload(tmp_path: Path) -> None:
    source_chunk = packs.SourceChunk(0, 6, 0x10, 0, 3, 3, 0, 0)
    first = packs.SelectedChunk(tmp_path / "unused.kvap", source_chunk,
                                0x4D49040000000000, b"abc")
    renamed = packs.SelectedChunk(tmp_path / "unused.kvap", source_chunk,
                                  0x4D49040000000002, b"abc")
    changed = packs.SelectedChunk(tmp_path / "unused.kvap", source_chunk,
                                  0x4D49040000000000, b"abd")
    first_hash = packs.selected_chunk_set_hash("model-semantics", [first])
    assert first_hash != packs.selected_chunk_set_hash(
        "model-semantics", [renamed]
    )
    assert first_hash != packs.selected_chunk_set_hash(
        "model-semantics", [changed]
    )


def test_texture_policy_protects_identity_and_caps_small_props() -> None:
    assert packs.image_cap("*lightmap0_primary") == 1024
    assert packs.image_cap("viewhands_marine_gloves_col") == 1024
    assert packs.image_cap("weapon_mp5_c") == 1024
    assert packs.image_cap("bus1_col") == 512
    assert packs.image_cap("bus_windows_col") == 256
    assert packs.image_cap("restaurant_stove_col") == 256


def test_source_pack_requires_retail_bootstrap_weapon_record() -> None:
    source = MODULE.read_text(encoding="utf-8")
    assert "validate_source_weapon_set(read_raw_chunk(path, chunk))" in source


def test_public_report_drops_ephemeral_source_paths() -> None:
    report = packs.public_pack_report({
        "format": "KVAP",
        "version": "1.0",
        "packageId": "0x1234",
        "sourceHash": "ab" * 32,
        "output": "C:/temporary/output/common.kvap",
        "bytes": 123,
        "chunks": [{
            "type": 8,
            "assetId": "0x1",
            "source": "C:/temporary/extraction/chunk.bin",
            "offset": 256,
            "storedBytes": 8,
            "rawBytes": 8,
            "codec": 0,
            "crc32": "12345678",
        }],
    }, "common.kvap")
    assert report["file"] == "common.kvap"
    assert "output" not in report
    assert "source" not in report["chunks"][0]


def test_direct_typed_chunks_receive_runtime_map_ids() -> None:
    collision = packs.direct_map_asset_id(
        packs.kvap.CHUNK_TYPES["collision"], b"VITC", 2, 0x1234
    )
    world = packs.direct_map_asset_id(
        packs.kvap.CHUNK_TYPES["world"], b"VITW", 0, 0x5678
    )
    script = packs.direct_map_asset_id(
        packs.kvap.CHUNK_TYPES["script"], b"VITR", 2, 0x9ABC
    )
    assert collision == 0x4D43000000000002
    assert world == 0x4D50000000000000
    assert script == 0x9ABC


def test_source_set_hash_is_ordered_and_stable() -> None:
    shipment = bytes(range(32))
    crossfire = bytes(reversed(range(32)))
    first = packs.source_set_hash(shipment, crossfire)
    second = packs.source_set_hash(shipment, crossfire)
    swapped = packs.source_set_hash(crossfire, shipment)
    assert len(first) == 32
    assert first == second
    assert first != swapped
    with_environment = packs.source_set_hash(shipment, crossfire, b"e" * 32)
    assert with_environment != first
    with_semantics = packs.source_set_hash(
        shipment, crossfire, None, b"s" * 32
    )
    assert with_semantics != first
    with_model_lods = packs.source_set_hash(
        shipment, crossfire, None, None, b"l" * 32
    )
    with_model_semantics = packs.source_set_hash(
        shipment, crossfire, None, None, None, b"m" * 32
    )
    assert with_model_lods != first
    assert with_model_lods != with_semantics
    assert with_model_semantics != first
    assert with_model_semantics != with_model_lods


def _model_lod_sidecar() -> bytes:
    name = b"test_model"
    model_header = struct.pack(
        "<4I4f", len(name), 2, 1, 0, 64.0, 128.0, 0.0, 0.0
    )
    geometry_header = struct.pack("<4I", 1, 1, 3, 3)
    surface = struct.pack("<6I", 0, 0, 3, 0, 3, 0)
    vertices = bytes(3 * 32)
    indices = struct.pack("<3H", 0, 1, 2)
    body = model_header + name + geometry_header + surface + vertices + indices
    return struct.pack("<3IQ", 0x444F4C56, 1, 1, len(body)) + body


def test_supplemental_model_lod_is_validated_and_map_bound(tmp_path: Path) -> None:
    sidecar = tmp_path / "vita_model_lods.bin"
    payload = _model_lod_sidecar()
    sidecar.write_bytes(payload)
    payload_hash, selected = packs.select_model_lod_chunk(sidecar, 2)
    assert payload_hash == hashlib.sha256(payload).digest()
    assert selected.asset_id == 0x4D58010000000002
    assert selected.chunk.kind == packs.kvap.CHUNK_TYPES["model"]
    assert selected.payload == payload


def test_required_map_closure_includes_environment_and_primary_lights() -> None:
    assert 0x4D56000000000000 in packs.REQUIRED_SIMPLE_FAMILIES
    assert 0x4D4B000000000000 in packs.REQUIRED_SIMPLE_FAMILIES
    assert packs.DIRECT_BINARY_MAGICS[packs.kvap.CHUNK_TYPES["environment"]] == {
        b"VENV"
    }


def _vits_record(name: str, sample: bytes, rate: int = 8000) -> tuple[bytes, int]:
    encoded = name.encode("utf-8")
    assert len(sample) % 2 == 0
    header = struct.pack(
        "<7I", len(encoded), len(sample), rate, 16, 1, len(sample) // 2, 1
    )
    return header + encoded + sample, len(sample)


def test_vits_record_split_round_trips_canonically() -> None:
    records = {
        "user_interface/ui_select_v1.wav": _vits_record(
            "user_interface/ui_select_v1.wav", b"\x01\x00\x02\x00"
        ),
        "footsteps/foot_metal04.wav": _vits_record(
            "footsteps/foot_metal04.wav", b"\x03\x00"
        ),
    }
    rebuilt = packs.build_vits(records)
    parsed = packs.parse_vits_records(rebuilt)
    assert parsed == records
    assert list(parsed) == sorted(records)
    assert struct.unpack_from("<Q", rebuilt, 12)[0] == 6


def test_vits_builder_canonicalizes_source_record_order() -> None:
    first = _vits_record("z.wav", b"\x01\x00")
    second = _vits_record("a.wav", b"\x02\x00")
    raw = (struct.pack("<IIIQ", 0x53544956, 1, 2, 4)
           + first[0] + second[0])
    parsed = packs.parse_vits_records(raw)
    rebuilt = packs.build_vits(parsed)
    rebuilt_records = packs.parse_vits_records(rebuilt)
    assert list(rebuilt_records) == ["a.wav", "z.wav"]


def test_lod_colors_keep_exact_donor_and_reject_missing_or_ambiguous() -> None:
    import pytest
    def stream(name, payload):
        return struct.pack("<IIIQ", 0x49544956, 1, 1, len(payload)) + _viti_record(name, payload)
    base = stream("present", b"base")
    donor = stream("car_engine_col", b"authored")
    fixed, report = packs.resolve_lod_color_records(base, [donor], {"car_engine_col"}, None)
    assert packs.parse_viti_records(fixed)["car_engine_col"] == packs.parse_viti_records(donor)["car_engine_col"]
    assert report["car_engine_col"]["provider"] == "typed-map"
    with pytest.raises(packs.kvap.KvapError, match="retail-root"):
        packs.resolve_lod_color_records(base, [], {"pine_lod_#0"}, None)
    with pytest.raises(packs.kvap.KvapError, match="conflicting"):
        packs.resolve_lod_color_records(base, [donor, stream("car_engine_col", b"different")], {"car_engine_col"}, None)


def test_lod_color_reads_owned_iwi_and_preserves_mips(tmp_path: Path) -> None:
    import zipfile
    root = tmp_path / "owned-cod4"
    (root / "main").mkdir(parents=True)
    # IWI stores the 1x1 mip before 2x2. VITI must put the base mip first.
    pixels = bytes(range(20))
    iwi = struct.pack("<3sBBB3h4i", b"IWi", 6, 1, 0, 2, 2, 1, 48, 0, 0, 0) + pixels
    with zipfile.ZipFile(root / "main" / "iw_02.iwd", "w") as archive:
        archive.writestr("images/pine_lod_#0.iwi", iwi)
    base = struct.pack("<IIIQ", 0x49544956, 1, 1, 4) + _viti_record("present", b"base")
    fixed, report = packs.resolve_lod_color_records(base, [], {"pine_lod_#0"}, root)
    wire, size = packs.parse_viti_records(fixed)["pine_lod_#0"]
    assert size == 20 and wire.endswith(pixels[4:] + pixels[:4])
    assert report["pine_lod_#0"]["iwiSha256"] == hashlib.sha256(iwi).hexdigest()
    assert packs.resolve_lod_color_records(fixed, [], {"pine_lod_#0"}, None) == (fixed, {})


if __name__ == "__main__":
    test_map_family_classifier_handles_segments_and_common_images()
    test_viti_trimming_keeps_authored_lower_mips()
    test_viti_selection_is_exact_and_canonical()
    test_texture_policy_protects_identity_and_caps_small_props()
    test_source_pack_requires_retail_bootstrap_weapon_record()
    test_public_report_drops_ephemeral_source_paths()
    test_direct_typed_chunks_receive_runtime_map_ids()
    test_source_set_hash_is_ordered_and_stable()
    test_required_map_closure_includes_environment_and_primary_lights()
    test_vits_record_split_round_trips_canonically()
    test_vits_builder_canonicalizes_source_record_order()
    print("vertical-slice pack tests: ok")
