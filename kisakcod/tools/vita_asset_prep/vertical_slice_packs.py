#!/usr/bin/env python3
"""Build a source-owned COD4 Vita common/selected-original-maps pack set.

The preferred inputs are independent typed Shipment and Crossfire KVAP files
generated from the user's owned COD4 installation.  The historical combined
multi-map package remains available only as an explicit compatibility input.
Retail bytes are never fetched, modified in place, or written outside the
requested workspace output directory.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import shutil
import struct
import tempfile
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import kvap
from map_catalog import ORIGINAL_MAPS, load_inputs


SCHEMA_VERSION = 1
CPU_BUDGET = 114 * 1024 * 1024
GPU_BUDGET = 64 * 1024 * 1024
WORKING_BUDGET = 178 * 1024 * 1024
MAPS = {0: "shipment", 2: "crossfire"}
COMMON_TYPES = {
    kvap.CHUNK_TYPES[name]
    for name in ("strings", "animation", "weapon", "script", "menu", "font", "sound")
}
EXACT_SHARED_TYPES = {
    kvap.CHUNK_TYPES[name]
    for name in ("strings", "animation", "weapon", "font")
}
SIMPLE_MAP_FAMILIES = {
    0x4D43000000000000,  # collision
    0x4D45000000000000,  # entities
    0x4D47000000000000,  # light grid
    0x4D50000000000000,  # world
    0x4D4C000000000000,  # materials
    0x4D49000000000000,  # bounded image closure
    0x4D49010000000000,  # reflection images
    0x4D58000000000000,  # models
    0x4D48000000000000,  # HUD
    0x4D56000000000000,  # environment
    0x4D4B000000000000,  # primary lights
    0x4D46000000000000,  # effects
    0x4D49070000000000,  # effect images
}
SEGMENTED_MAP_FAMILIES = {
    0x4D49020000000000,  # world normals
    0x4D49030000000000,  # world specular
    0x4D49040000000000,  # model normals
    0x4D49050000000000,  # model specular
    0x4D49080000000000,  # color tail
}
WORLD_SEMANTIC_FAMILIES = {
    0x4D49020000000000,  # world normals
    0x4D49030000000000,  # world specular
}
MODEL_SEMANTIC_FAMILIES = {
    0x4D49040000000000,  # placed-XModel/viewmodel normals
    0x4D49050000000000,  # placed-XModel/viewmodel specular
}
# Crossfire's materially larger authored normal/specular catalog exhausts the
# stock VitaGL GPU pool at 256 even after immutable geometry is allocated
# first.  Keep Shipment at the higher cap and apply the measured, selective
# low-salience reduction only to Crossfire.
WORLD_SEMANTIC_TOP_MIP_BY_MAP = {**{i:256 for i in ORIGINAL_MAPS},
    0: 256,  # Shipment
    2: 128,  # Crossfire
}
# The locally generated model-semantic archive is already scoped by the
# exporter to map-placed XModels, effect models, and source weapon viewmodels.
# Static-prop relief/gloss is the last selective quality tier in the slice.
# Crossfire's complete retained-LOD set needs roughly 250 unique semantic
# records; a 128px cap prevents the protected hands/MP5 maps from fitting in
# the stock VitaGL pool. Keep ordinary prop semantics at 64 while preserving
# the fixed MP5/USP/hands records at their normal image-policy cap below.
MODEL_SEMANTIC_TOP_MIP_BY_MAP = {**{i:64 for i in ORIGINAL_MAPS},
    0: 64,
    2: 64,
}
# The supplemental VLOD capture includes Shipment's car-engine LOD surface,
# while the older map-scoped semantic export omitted that one normal alias.
# The identical source-owned image is present in Crossfire's record from the
# same certified installation. Merge only this audited alias, require the
# donor bytes to agree, and bind the repaired payload into the pack hash.
MODEL_SEMANTIC_LOD_ALIAS_REPAIRS = {
    0: {
        0x4D49040000000000: {"car_engine_nml"},
    },
}
# Older typed exports omitted these diffuse aliases used only by retained
# lower LODs. Resolve exact authored records from the other typed map or the
# owner's IWDs; never substitute a similarly named image or a white fallback.
MODEL_COLOR_LOD_ALIAS_REPAIRS = {0: {"car_engine_col", "pine_lod_#0"}}
# The map VITI is an on-disk source catalog. The native runtime uploads a
# bounded color/model/UI subset and releases the compressed source envelope;
# predicted GPU residency must model that selected set, not charge every
# mutually exclusive source record as simultaneously resident.
# The native loader enforces a 12 MiB world-image envelope and a separate
# 10 MiB selected-map XModel-colour envelope. Common MP5/USP/hands and UI
# images are charged by their own common VITI chunks, while semantic/effect
# companions are charged separately below. Model this actual contract rather
# than the old 24 MiB placeholder, which double-counted those companions.
NATIVE_SELECTED_IMAGE_GPU_BUDGET = 22 * 1024 * 1024
PAGED_WORLD_GPU_BUDGET = 16 * 1024 * 1024
TEXTURE_PAGE_CPU_RESERVE = 8 * 1024 * 1024 + 128 * 1024
COMMON_MODEL_COLOR_ASSET = 0x4D49060000000000
COMMON_PRESENTATION_IMAGE_ASSET = 0x4D49090000000000
COMMON_MODEL_COLOR_NAMES = {
    "viewhands_marine_gloves_col",
    "viewhand_black_kit_col",
    "op_force_clr",
    "weapon_parbolic_knife_col",
    "m67_grenade_col",
    "m84_flashbang_col",
    "weapon_mp5_c",
    "weapon_usp_c",
}
REQUIRED_COMMON_MODEL_COLOR_NAMES = {
    "viewhands_marine_gloves_col",
    "viewhand_black_kit_col",
    "op_force_clr",
    "weapon_parbolic_knife_col",
    "m67_grenade_col",
    "m84_flashbang_col",
    "weapon_mp5_c",
    "weapon_usp_c",
}
COMMON_PRESENTATION_IMAGE_NAMES = {
    "ammo_counter_bullet",
    "background_generic_blurred",
    "dpad_arrow",
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
    "dtimer_bg",
    "dtimer_bg_border",
    "explosion_flash",
    "gamefonts_pc",
    "grenadeicon",
    "hud_m4carbine",
    "hud_mp5",
    "hud_us_grenade",
    "hud_usp_45",
    "hud_xpticker480",
    "hud_xpticker480ws",
    "menu_button",
    "menu_button_selected",
    "minimap_tickertape_256x16",
    "minimap_tickertape_background",
    "overlay_low_health",
    "score_bar_allies",
    "score_bar_opfor",
}
REQUIRED_COMMON_PRESENTATION_IMAGE_NAMES = {
    "background_generic_blurred",
    "gamefonts_pc",
    "menu_button",
    "menu_button_selected",
}


def is_vertical_slice_viewmodel_semantic(name: str) -> bool:
    """Protect the fixed loadout's authored first-person semantic records."""
    folded = name.casefold()
    return any(token in folded for token in (
        "viewhands_",
        "viewhand_black_kit",
        "op_force_",
        "weapon_parbolic_knife",
        "m67_grenade",
        "m84_flashbang",
        "weapon_mp5_",
        "weapon_usp_",
    ))
REQUIRED_SIMPLE_FAMILIES = {
    0x4D43000000000000,
    0x4D45000000000000,
    0x4D47000000000000,
    0x4D50000000000000,
    0x4D4C000000000000,
    0x4D49000000000000,
    0x4D58000000000000,
    0x4D48000000000000,
    0x4D56000000000000,
    0x4D4B000000000000,
    0x4D46000000000000,
}
RUNTIME_SOUND_NAMES = {
    "amb_emitters/emt_chain_ratte_ship1.wav",
    "foley/wpfoly_m4_reload_chamber.wav",
    "foley/wpfoly_m4_reload_clipin.wav",
    "foley/wpfoly_m4_reload_clipout.wav",
    "foley/wpfoly_mp5_reload_chamber_v3.wav",
    "foley/wpfoly_mp5_reload_clipin_v4.wav",
    "foley/wpfoly_mp5_reload_clipout_v4.wav",
    "foley/wpfoly_usp_reload_chamber_v1.wav",
    "foley/wpfoly_usp_reload_clipin_v1.wav",
    "foley/wpfoly_usp_reload_clipout_v2.wav",
    "footsteps/foot_metal04.wav",
    "footsteps/foot_wood02.wav",
    "user_interface/ui_over_v1.wav",
    "user_interface/ui_computer_text_blip1x.wav",
    "user_interface/ui_computer_text_delete1.wav",
    "user_interface/ui_mp_countdown_v1.wav",
    "user_interface/ui_screen_in1.wav",
    "user_interface/ui_screen_out1.wav",
    "user_interface/ui_select_v1.wav",
    "user_interface/ui_sub_over_v1.wav",
    "weapons/colt45/weap_usp45_slst_y1b.wav",
    "weapons/m4/weap_m4_slmn_2c1.wav",
    "weapons/m4/weap_m4_slst_1c4.wav",
    "weapons/mp5/weap_mp5_silencer_slst_2d.wav",
    "weapons/mp5/weap_mp5_slst_4.wav",
}


@dataclass(frozen=True)
class SourceChunk:
    index: int
    kind: int
    asset_id: int
    offset: int
    stored_bytes: int
    raw_bytes: int
    codec: int
    crc32: int


@dataclass(frozen=True)
class SelectedChunk:
    package: Path
    chunk: SourceChunk
    asset_id: int
    payload: bytes | None = None


# Generic typed exports intentionally contain both runtime binary streams and
# human-readable audit JSON under the same KVAP kind.  Only the fixed-width
# streams below may enter a deployable pack.  The chunk kind disambiguates the
# two VITP formats (weapons and primary lights).
DIRECT_BINARY_MAGICS = {
    kvap.CHUNK_TYPES["strings"]: {b"VITZ", b"VITT"},
    kvap.CHUNK_TYPES["collision"]: {b"VITC"},
    kvap.CHUNK_TYPES["world"]: {b"VITW", b"VITG", b"VITE"},
    kvap.CHUNK_TYPES["material"]: {b"VITL"},
    kvap.CHUNK_TYPES["image"]: {b"VITI"},
    kvap.CHUNK_TYPES["model"]: {b"VITX", b"VLOD"},
    kvap.CHUNK_TYPES["animation"]: {b"VITQ"},
    kvap.CHUNK_TYPES["weapon"]: {b"VITP"},
    kvap.CHUNK_TYPES["script"]: {b"VITR"},
    kvap.CHUNK_TYPES["menu"]: {b"VITM"},
    kvap.CHUNK_TYPES["font"]: {b"VITF"},
    kvap.CHUNK_TYPES["sound"]: {b"VITS"},
    kvap.CHUNK_TYPES["hud"]: {b"VITH"},
    kvap.CHUNK_TYPES["environment"]: {b"VENV"},
    kvap.CHUNK_TYPES["primary_lights"]: {b"VITP"},
    kvap.CHUNK_TYPES["effects"]: {b"VFXA", b"VIFX"},
}

COMMON_BINARY_KEYS = {
    (kvap.CHUNK_TYPES["effects"], b"VIFX"),
}

DIRECT_MAP_ASSET_FAMILIES = {
    (kvap.CHUNK_TYPES["collision"], b"VITC"): 0x4D43000000000000,
    (kvap.CHUNK_TYPES["world"], b"VITE"): 0x4D45000000000000,
    (kvap.CHUNK_TYPES["world"], b"VITG"): 0x4D47000000000000,
    (kvap.CHUNK_TYPES["world"], b"VITW"): 0x4D50000000000000,
    (kvap.CHUNK_TYPES["material"], b"VITL"): 0x4D4C000000000000,
    (kvap.CHUNK_TYPES["image"], b"VITI"): 0x4D49000000000000,
    (kvap.CHUNK_TYPES["model"], b"VITX"): 0x4D58000000000000,
    (kvap.CHUNK_TYPES["model"], b"VLOD"): 0x4D58010000000000,
    (kvap.CHUNK_TYPES["hud"], b"VITH"): 0x4D48000000000000,
    (kvap.CHUNK_TYPES["environment"], b"VENV"): 0x4D56000000000000,
    (kvap.CHUNK_TYPES["primary_lights"], b"VITP"): 0x4D4B000000000000,
    (kvap.CHUNK_TYPES["effects"], b"VFXA"): 0x4D46000000000000,
}


def read_source_index(path: Path) -> tuple[bytes, list[SourceChunk]]:
    with path.open("rb") as stream:
        header = stream.read(kvap.HEADER_SIZE)
        if len(header) != kvap.HEADER_SIZE:
            raise kvap.KvapError("input package has a truncated header")
        (magic, major, _minor, endian, _flags, header_size, toc_offset,
         toc_count, _package_id, source_hash) = kvap.HEADER.unpack(header)
        if magic != kvap.MAGIC or major != kvap.MAJOR or endian != kvap.ENDIAN_LITTLE:
            raise kvap.KvapError("input package is not a supported KVAP")
        if header_size != kvap.HEADER_SIZE or toc_count > 4096:
            raise kvap.KvapError("input package header is outside bounded limits")
        stream.seek(toc_offset)
        chunks: list[SourceChunk] = []
        for index in range(toc_count):
            record = stream.read(kvap.TOC_SIZE)
            if len(record) != kvap.TOC_SIZE:
                raise kvap.KvapError(f"input TOC record {index} is truncated")
            kind, _flags, asset, offset, stored, raw, _alignment, codec, crc, _ = kvap.TOC.unpack(record)
            chunks.append(SourceChunk(index, kind, asset, offset, stored, raw, codec, crc))
    return source_hash, chunks


def read_raw_chunk(path: Path, chunk: SourceChunk) -> bytes:
    with path.open("rb") as stream:
        stream.seek(chunk.offset)
        stored = stream.read(chunk.stored_bytes)
    if len(stored) != chunk.stored_bytes:
        raise kvap.KvapError(f"chunk {chunk.index} is truncated")
    if chunk.codec == kvap.CODECS["none"]:
        raw = stored
    elif chunk.codec == kvap.CODECS["zlib"]:
        raw = zlib.decompress(stored)
    else:
        raise kvap.KvapError(f"chunk {chunk.index} uses unsupported codec {chunk.codec}")
    if len(raw) != chunk.raw_bytes or (zlib.crc32(raw) & 0xFFFFFFFF) != chunk.crc32:
        raise kvap.KvapError(f"chunk {chunk.index} failed size/CRC validation")
    return raw


def read_selected_chunk(selected: SelectedChunk) -> bytes:
    return (selected.payload if selected.payload is not None
            else read_raw_chunk(selected.package, selected.chunk))


def selected_chunk_set_hash(label: str,
                            chunks: Iterable[SelectedChunk]) -> bytes:
    """Hash the exact supplemental records selected for the certified pair.

    KVAP's header hash identifies the user's source installation, so two
    different locally generated archives legitimately carry the same value.
    Dependency identity must additionally bind asset ids and payload bytes;
    otherwise replacing a semantic archive would not change the certified
    split-pack identity.
    """
    ordered = sorted(chunks, key=lambda item: (
        item.chunk.kind, item.asset_id, item.chunk.index
    ))
    if not ordered:
        raise kvap.KvapError(f"cannot hash an empty {label} selection")
    digest = hashlib.sha256()
    digest.update(b"KISAK-COD4-VITA-SELECTED-CHUNKS\0")
    digest.update(label.encode("utf-8") + b"\0")
    for selected in ordered:
        raw = read_selected_chunk(selected)
        digest.update(struct.pack(
            "<IQQ", selected.chunk.kind, selected.asset_id, len(raw)
        ))
        digest.update(hashlib.sha256(raw).digest())
    return digest.digest()


def parse_vits_records(raw: bytes, maximum_records: int = 2048) -> dict[str, tuple[bytes, int]]:
    """Return deterministic, self-contained VITS records keyed by path."""
    if len(raw) < 20:
        raise kvap.KvapError("VITS stream is truncated")
    magic, version, count, declared = struct.unpack_from("<IIIQ", raw, 0)
    if magic != 0x53544956 or version != 1 or count == 0 or count > maximum_records:
        raise kvap.KvapError("unsupported VITS stream")
    offset = 20
    aggregate = 0
    records: dict[str, tuple[bytes, int]] = {}
    for _ in range(count):
        start = offset
        if offset + 28 > len(raw):
            raise kvap.KvapError("VITS record header is truncated")
        name_bytes, sample_bytes, rate, bits, channels, samples, fmt = \
            struct.unpack_from("<7I", raw, offset)
        offset += 28
        expected = samples * channels * (bits // 8)
        if (name_bytes == 0 or name_bytes > 4096
                or sample_bytes == 0 or sample_bytes > 8 * 1024 * 1024
                or rate < 8000 or rate > 96000 or bits != 16
                or channels not in (1, 2) or fmt != 1
                or expected != sample_bytes
                or offset + name_bytes + sample_bytes > len(raw)):
            raise kvap.KvapError("VITS record is invalid")
        name_bytes_raw = raw[offset:offset + name_bytes]
        if b"\0" in name_bytes_raw:
            raise kvap.KvapError("VITS record name contains NUL")
        name = name_bytes_raw.decode("utf-8", errors="strict")
        offset += name_bytes + sample_bytes
        if name in records:
            raise kvap.KvapError(f"duplicate VITS record: {name}")
        records[name] = (raw[start:offset], sample_bytes)
        aggregate += sample_bytes
    if offset != len(raw) or aggregate != declared:
        raise kvap.KvapError("VITS stream size accounting is invalid")
    return records


def build_vits(records: dict[str, tuple[bytes, int]]) -> bytes:
    if not records:
        raise kvap.KvapError("cannot build an empty VITS stream")
    ordered = sorted(records.items())
    declared = sum(sample_bytes for _name, (_wire, sample_bytes) in ordered)
    return (struct.pack("<IIIQ", 0x53544956, 1, len(ordered), declared)
            + b"".join(wire for _name, (wire, _sample_bytes) in ordered))


def direct_binary_chunks(path: Path, chunks: list[SourceChunk]) -> list[tuple[SourceChunk, bytes]]:
    """Validate and classify the runtime streams in one typed map export."""
    selected: list[tuple[SourceChunk, bytes]] = []
    for chunk in chunks:
        accepted = DIRECT_BINARY_MAGICS.get(chunk.kind)
        if not accepted or chunk.raw_bytes < 8:
            continue
        raw = read_raw_chunk(path, chunk)
        magic = raw[:4]
        if magic in accepted:
            selected.append((chunk, magic))
    return selected


def direct_map_asset_id(kind: int, magic: bytes, map_index: int,
                        source_asset_id: int) -> int:
    family = DIRECT_MAP_ASSET_FAMILIES.get((kind, magic))
    return (family | map_index) if family is not None else source_asset_id


def vifx_effect_names(raw: bytes) -> set[str]:
    """Validate VIFX v1 and return every referenced FxEffectDef name."""
    if len(raw) < 32:
        raise kvap.KvapError("VIFX stream is truncated")
    (magic, version, table_count, entry_count, non_flesh_count,
     flesh_count, string_bytes, reserved) = struct.unpack_from("<8I", raw)
    if (magic != 0x58464956 or version != 1 or table_count == 0
            or table_count > 4 or entry_count != 12
            or non_flesh_count != 29 or flesh_count != 4
            or string_bytes == 0 or string_bytes > 1024 * 1024
            or reserved != 0):
        raise kvap.KvapError("unsupported VIFX stream")
    slots_per_table = entry_count * (non_flesh_count + flesh_count)
    table_base = 32
    slot_base = table_base + table_count * 8
    string_base = slot_base + table_count * slots_per_table * 4
    if string_base + string_bytes != len(raw) or raw[string_base] != 0:
        raise kvap.KvapError("VIFX stream size accounting is invalid")

    def read_string(offset: int, allow_empty: bool) -> str:
        if offset >= string_bytes:
            raise kvap.KvapError("VIFX string offset is outside the table")
        end = raw.find(b"\0", string_base + offset,
                       string_base + string_bytes)
        if end < 0 or end - (string_base + offset) > 1024:
            raise kvap.KvapError("VIFX string is not bounded")
        value = raw[string_base + offset:end].decode("utf-8", errors="strict")
        if not allow_empty and not value:
            raise kvap.KvapError("VIFX contains an empty effect slot")
        return value

    table_names: set[str] = set()
    effect_names: set[str] = set()
    for table_index in range(table_count):
        name_offset, first_slot = struct.unpack_from(
            "<2I", raw, table_base + table_index * 8)
        table_name = read_string(name_offset, True)
        if table_name in table_names or first_slot != table_index * slots_per_table:
            raise kvap.KvapError("VIFX table directory is invalid")
        table_names.add(table_name)
        for slot_index in range(slots_per_table):
            (effect_offset,) = struct.unpack_from(
                "<I", raw, slot_base + (first_slot + slot_index) * 4)
            effect_name = read_string(effect_offset, True)
            if effect_name:
                effect_names.add(effect_name)
    if "" not in table_names:
        raise kvap.KvapError("VIFX lacks the canonical empty-name impact table")
    return effect_names


def vfxa_effect_names(raw: bytes) -> set[str]:
    """Return names and fastfile paths published by a VFXA definition table."""
    if len(raw) < 32 or raw[:4] != b"VFXA":
        raise kvap.KvapError("VFXA stream is truncated")
    version, effect_count, placement_count, sound_count, string_bytes = (
        struct.unpack_from("<I", raw, 4)[0],
        struct.unpack_from("<I", raw, 12)[0],
        struct.unpack_from("<I", raw, 16)[0],
        struct.unpack_from("<I", raw, 20)[0],
        struct.unpack_from("<I", raw, 24)[0],
    )
    header_bytes = 64 if version in (2, 3) else 32
    if version not in (1, 2, 3) or effect_count == 0 or effect_count > 4096:
        raise kvap.KvapError("unsupported VFXA stream")
    string_base = (header_bytes + effect_count * 8
                   + placement_count * 36 + sound_count * 28)
    if string_bytes == 0 or string_base + string_bytes > len(raw):
        raise kvap.KvapError("VFXA definition strings are truncated")

    def read_string(offset: int) -> str:
        if offset >= string_bytes:
            raise kvap.KvapError("VFXA definition offset is invalid")
        end = raw.find(b"\0", string_base + offset,
                       string_base + string_bytes)
        if end < 0 or end == string_base + offset:
            raise kvap.KvapError("VFXA definition identity is empty")
        return raw[string_base + offset:end].decode("utf-8", errors="strict")

    names: set[str] = set()
    for index in range(effect_count):
        name_offset, path_offset = struct.unpack_from(
            "<2I", raw, header_bytes + index * 8)
        names.add(read_string(name_offset))
        if path_offset:
            names.add(read_string(path_offset))
    return names


def viti_names(raw: bytes) -> set[str]:
    """Validate a VITI stream and return its exact source image aliases."""
    if len(raw) < 20:
        raise kvap.KvapError("VITI stream is truncated")
    magic, version, count, declared = struct.unpack_from("<IIIQ", raw, 0)
    if magic != 0x49544956 or version != 1 or count == 0 or count > 4096:
        raise kvap.KvapError("unsupported VITI stream")
    offset = 20
    aggregate = 0
    names: set[str] = set()
    for _ in range(count):
        if offset + 32 > len(raw):
            raise kvap.KvapError("VITI record header is truncated")
        name_bytes, payload_bytes = struct.unpack_from("<2I", raw, offset)
        offset += 32
        if (name_bytes == 0 or name_bytes > 4096 or payload_bytes == 0
                or offset + name_bytes + payload_bytes > len(raw)):
            raise kvap.KvapError("VITI record is invalid")
        name = raw[offset:offset + name_bytes].decode("utf-8", errors="strict")
        offset += name_bytes + payload_bytes
        if name in names:
            raise kvap.KvapError(f"duplicate VITI image alias: {name}")
        names.add(name)
        aggregate += payload_bytes
    if offset != len(raw) or aggregate != declared:
        raise kvap.KvapError("VITI aggregate length does not match its header")
    return names


def parse_viti_records(raw: bytes) -> dict[str, tuple[bytes, int]]:
    """Return validated, self-contained VITI records keyed by exact alias."""
    if len(raw) < 20:
        raise kvap.KvapError("VITI stream is truncated")
    magic, version, count, declared = struct.unpack_from("<IIIQ", raw, 0)
    if magic != 0x49544956 or version != 1 or count == 0 or count > 4096:
        raise kvap.KvapError("unsupported VITI stream")
    offset = 20
    aggregate = 0
    records: dict[str, tuple[bytes, int]] = {}
    for _ in range(count):
        record_start = offset
        if offset + 32 > len(raw):
            raise kvap.KvapError("VITI record header is truncated")
        name_bytes, payload_bytes = struct.unpack_from("<2I", raw, offset)
        offset += 32
        if (name_bytes == 0 or name_bytes > 4096 or payload_bytes == 0
                or offset + name_bytes + payload_bytes > len(raw)):
            raise kvap.KvapError("VITI record is invalid")
        name = raw[offset:offset + name_bytes].decode(
            "utf-8", errors="strict"
        )
        offset += name_bytes + payload_bytes
        if name in records:
            raise kvap.KvapError(f"duplicate VITI image alias: {name}")
        records[name] = (raw[record_start:offset], payload_bytes)
        aggregate += payload_bytes
    if offset != len(raw) or aggregate != declared:
        raise kvap.KvapError("VITI aggregate length does not match its header")
    return records


def build_viti_records(records: dict[str, tuple[bytes, int]]) -> bytes:
    """Build a deterministic VITI without interpreting any source texels."""
    if not records:
        raise kvap.KvapError("cannot build an empty VITI stream")
    ordered = sorted(records.items())
    declared = sum(payload_bytes
                   for _name, (_wire, payload_bytes) in ordered)
    return (struct.pack("<IIIQ", 0x49544956, 1, len(ordered), declared)
            + b"".join(wire for _name, (wire, _payload_bytes) in ordered))


def texture_residency(raw: bytes, seen: set | None = None) -> tuple[int, int, int, int]:
    """Conservative physical bytes, pageable bytes, backing extent bytes/count.

    Protected first-person/UI payloads, cubemaps and lightmaps never receive
    the renderer's pageable hint. The native pool shares exact immutable images
    within one storage class. Paging changes lifetime,
    never the authored payload or the converter's image-quality policy.
    """
    fixed = pageable = backing = count = 0
    for name, (wire, size) in parse_viti_records(raw).items():
        name_size, _, width, height, depth, levels, fmt, flags = struct.unpack_from('<8I', wire)
        protected = (flags & 4 or name.startswith('*lightmap')
                     or name in COMMON_MODEL_COLOR_NAMES
                     or name in COMMON_PRESENTATION_IMAGE_NAMES
                     or is_vertical_slice_viewmodel_semantic(name)
                     or name.startswith(('hud_', 'ui_', 'menu_', 'font', 'compass', 'minimap')))
        identity = (width, height, levels, fmt, flags & 4, bool(protected),
                    hashlib.sha256(wire[32+name_size:]).digest())
        if seen is not None:
            if identity in seen:
                continue
            seen.add(identity)
        if protected:
            fixed += size
        else:
            if size > 8 * 1024 * 1024:
                raise kvap.KvapError('Texture exceeds bounded upload scratch: '+name)
            pageable += size
            backing += ((size + 65535) // 65536) * 65536
            count += 1
    return fixed, pageable, backing, count


def merge_viti_aliases(base: bytes, donors: Iterable[bytes],
                       required_names: set[str]) -> bytes:
    """Merge exact source records, rejecting absent or ambiguous donors."""
    records = parse_viti_records(base)
    missing = required_names - records.keys()
    if not missing:
        return build_viti_records(records)
    matches: dict[str, tuple[bytes, int]] = {}
    for donor in donors:
        donor_records = parse_viti_records(donor)
        for name in missing:
            record = donor_records.get(name)
            if record is None:
                continue
            previous = matches.get(name)
            if previous is not None and previous != record:
                raise kvap.KvapError(
                    f"conflicting VITI donor records for alias: {name}"
                )
            matches[name] = record
    unresolved = missing - matches.keys()
    if unresolved:
        raise kvap.KvapError(
            "missing required VITI donor aliases: "
            + ", ".join(sorted(unresolved))
        )
    records.update(matches)
    return build_viti_records(records)


def resolve_lod_color_records(base: bytes, donors: list[bytes],
                              required: set[str], retail_root: Path | None
                              ) -> tuple[bytes, dict[str, dict]]:
    missing = required - viti_names(base)
    if not missing:
        return base, {}
    available = set().union(*(viti_names(raw) for raw in donors))
    repaired = merge_viti_aliases(base, donors, missing & available)
    records = parse_viti_records(repaired)
    report = {name: {"provider": "typed-map",
                     "recordSha256": hashlib.sha256(records[name][0]).hexdigest()}
              for name in sorted(missing & available)}
    unresolved = missing - records.keys()
    if unresolved:
        if retail_root is None:
            raise kvap.KvapError("retained LOD colors require --retail-root: "
                                 + ", ".join(sorted(unresolved)))
        from repair_map_image_closures import RetailImages, _write_image_records
        import zipfile
        retail = RetailImages(retail_root)
        for name in sorted(unresolved):
            record, status = retail.lookup(name)
            location = retail.source_cache.get(name.lower())
            if (record is None or location is None
                    or Path(location[1]).stem.lower() != name.lower()):
                raise kvap.KvapError(f"missing exact retail LOD color {name}: {status}")
            wire = parse_viti_records(_write_image_records([(name, record)]))[name]
            records[name] = wire
            with zipfile.ZipFile(location[0]) as archive:
                iwi_hash = hashlib.sha256(archive.read(location[1])).hexdigest()
            report[name] = {"provider": "owned-iwd", "archive": location[0].name,
                            "entry": location[1], "iwiSha256": iwi_hash,
                            "recordSha256": hashlib.sha256(wire[0]).hexdigest()}
    return build_viti_records(records), report


def repair_model_lod_colors(chunks_by_map: dict[int, list[SelectedChunk]],
                            retail_root: Path | None) -> dict[str, dict]:
    report = {}
    for map_index, aliases in MODEL_COLOR_LOD_ALIAS_REPAIRS.items():
        chunks = chunks_by_map[map_index]
        if not any(item.asset_id & 0xFFFFFFFFFFFFFF00 == 0x4D58010000000000
                   for item in chunks):
            continue
        referenced = set().union(*(vitl_image_names(read_selected_chunk(item))
            for item in chunks if item.chunk.kind == kvap.CHUNK_TYPES["material"]))
        required = aliases & referenced
        for index, item in enumerate(chunks):
            if item.asset_id & 0xFFFFFFFFFFFFFF00 != 0x4D49000000000000:
                continue
            donors = [read_selected_chunk(other)
                for donor_map, donor_chunks in chunks_by_map.items() if donor_map != map_index
                for other in donor_chunks
                if other.asset_id & 0xFFFFFFFFFFFFFF00 == 0x4D49000000000000]
            raw, repairs = resolve_lod_color_records(
                read_selected_chunk(item), donors, required, retail_root)
            chunks[index] = SelectedChunk(item.package, item.chunk, item.asset_id, raw)
            if repairs:
                report[MAPS[map_index]] = repairs
    return report


def repair_model_semantic_lod_aliases(
        chunks_by_map: dict[int, list[SelectedChunk]]) -> None:
    """Close audited VLOD semantic gaps with exact same-install records."""
    for map_index, repairs_by_family in MODEL_SEMANTIC_LOD_ALIAS_REPAIRS.items():
        target_chunks = chunks_by_map.get(map_index)
        if target_chunks is None:
            raise kvap.KvapError(
                f"missing semantic chunk set for map {map_index}"
            )
        for family, required_names in repairs_by_family.items():
            target_indices = [
                index for index, selected in enumerate(target_chunks)
                if selected.asset_id & 0xFFFFFFFFFFFF0000 == family
            ]
            if not target_indices:
                raise kvap.KvapError(
                    f"map {map_index} lacks model semantic family "
                    f"0x{family:016x}"
                )
            present = set().union(*(
                viti_names(read_selected_chunk(target_chunks[index]))
                for index in target_indices
            ))
            missing = required_names - present
            if not missing:
                continue
            donors = [
                read_selected_chunk(selected)
                for donor_map, donor_chunks in chunks_by_map.items()
                if donor_map != map_index
                for selected in donor_chunks
                if selected.asset_id & 0xFFFFFFFFFFFF0000 == family
            ]
            first_index = target_indices[0]
            target = target_chunks[first_index]
            repaired = merge_viti_aliases(
                read_selected_chunk(target), donors, missing
            )
            target_chunks[first_index] = SelectedChunk(
                target.package, target.chunk, target.asset_id, repaired
            )


def select_viti_records(raw: bytes, selected_names: set[str]) -> tuple[bytes, set[str]]:
    """Build a canonical VITI containing only exact selected source records."""
    if len(raw) < 20:
        raise kvap.KvapError("VITI stream is truncated")
    magic, version, count, declared = struct.unpack_from("<IIIQ", raw, 0)
    if magic != 0x49544956 or version != 1 or count == 0 or count > 4096:
        raise kvap.KvapError("unsupported VITI stream")
    offset = 20
    aggregate = 0
    records: dict[str, tuple[bytes, int]] = {}
    for _ in range(count):
        record_start = offset
        if offset + 32 > len(raw):
            raise kvap.KvapError("VITI record header is truncated")
        name_bytes, payload_bytes = struct.unpack_from("<2I", raw, offset)
        offset += 32
        if (name_bytes == 0 or name_bytes > 4096 or payload_bytes == 0
                or offset + name_bytes + payload_bytes > len(raw)):
            raise kvap.KvapError("VITI record is invalid")
        name = raw[offset:offset + name_bytes].decode("utf-8", errors="strict")
        offset += name_bytes + payload_bytes
        if name in records:
            raise kvap.KvapError(f"duplicate VITI image alias: {name}")
        if name in selected_names:
            records[name] = (raw[record_start:offset], payload_bytes)
        aggregate += payload_bytes
    if offset != len(raw) or aggregate != declared:
        raise kvap.KvapError("VITI aggregate length does not match its header")
    ordered = sorted(records)
    selected_payload_bytes = sum(records[name][1] for name in ordered)
    rebuilt = (struct.pack("<IIIQ", magic, version, len(ordered),
                           selected_payload_bytes)
               + b"".join(records[name][0] for name in ordered))
    return rebuilt, set(ordered)


def vitl_image_names(raw: bytes) -> set[str]:
    """Validate VITL v1-v5 and return every authored texture reference."""
    if len(raw) < 20:
        raise kvap.KvapError("VITL stream is truncated")
    magic, version, count, declared = struct.unpack_from("<IIIQ", raw, 0)
    if magic != 0x4C544956 or version not in (1, 2, 3, 4, 5) or count > 4096:
        raise kvap.KvapError("unsupported VITL stream")
    offset = 20
    aggregate = 0
    images: set[str] = set()
    for _ in range(count):
        header_bytes = 8 if version == 1 else (32 if version == 2 else
                                               (33 if version == 3 else (42 if version == 4 else 52)))
        if offset + header_bytes > len(raw):
            raise kvap.KvapError("VITL record header is truncated")
        length_count = 2 if version == 1 else 5
        lengths = struct.unpack_from(f"<{length_count}I", raw, offset)
        if lengths[0] == 0 or any(length > 4096 for length in lengths):
            raise kvap.KvapError("VITL record has invalid string lengths")
        offset += header_bytes
        values: list[str] = []
        for length in lengths:
            if offset + length > len(raw):
                raise kvap.KvapError("VITL record strings are truncated")
            values.append(raw[offset:offset + length].decode(
                "utf-8", errors="strict"))
            offset += length
        aggregate += sum(lengths)
        # v1: name/color; v2+: name/color/normal/specular/technique.
        for value in values[1:2] if version == 1 else values[1:4]:
            if value:
                images.add(value)
    if offset != len(raw) or aggregate != declared:
        raise kvap.KvapError("VITL aggregate length does not match its header")
    return images


def source_set_hash(shipment_hash: bytes, crossfire_hash: bytes,
                    environment_hash: bytes | None = None,
                    world_semantic_hash: bytes | None = None,
                    model_lod_hash: bytes | None = None,
                    model_semantic_hash: bytes | None = None) -> bytes:
    """Produce one deterministic identity for the exact certified pair."""
    if len(shipment_hash) != 32 or len(crossfire_hash) != 32:
        raise kvap.KvapError("typed package source hashes must be SHA-256 values")
    digest = hashlib.sha256()
    digest.update(b"KISAK-COD4-VITA-VERTICAL-SLICE\0TWO-MAP\0")
    digest.update(b"mp_shipment\0")
    digest.update(shipment_hash)
    digest.update(b"mp_crossfire\0")
    digest.update(crossfire_hash)
    if environment_hash is not None:
        if len(environment_hash) != 32:
            raise kvap.KvapError(
                "environment package source hash must be a SHA-256 value"
            )
        digest.update(b"environment_archive\0")
        digest.update(environment_hash)
    if world_semantic_hash is not None:
        if len(world_semantic_hash) != 32:
            raise kvap.KvapError(
                "world semantic package source hash must be a SHA-256 value"
            )
        digest.update(b"world_semantic_archive\0")
        digest.update(world_semantic_hash)
    if model_lod_hash is not None:
        if len(model_lod_hash) != 32:
            raise kvap.KvapError(
                "model LOD archive hash must be a SHA-256 value"
            )
        digest.update(b"model_lod_archive\0")
        digest.update(model_lod_hash)
    if model_semantic_hash is not None:
        if len(model_semantic_hash) != 32:
            raise kvap.KvapError(
                "model semantic package source hash must be a SHA-256 value"
            )
        digest.update(b"model_semantic_archive\0")
        digest.update(model_semantic_hash)
    return digest.digest()


def select_environment_chunks(path: Path, maps=None) -> tuple[bytes, dict[int, SelectedChunk]]:
    """Select and validate the two exact supported VENV map records.

    Older source-complete typed exports predate VENV.  This permits those
    immutable exports to be paired with the later locally generated
    environment archive without copying any unrelated maps or retail bytes.
    """
    source_hash, chunks = read_source_index(path)
    selected: dict[int, SelectedChunk] = {}
    maps = MAPS if maps is None else maps
    for map_index in maps:
        asset_id = 0x4D56000000000000 | map_index
        matches = [
            chunk for chunk in chunks
            if chunk.kind == kvap.CHUNK_TYPES["environment"]
            and chunk.asset_id == asset_id
        ]
        if len(matches) != 1:
            raise kvap.KvapError(
                f"{path.name} requires exactly one VENV record for "
                f"{ORIGINAL_MAPS[map_index]}"
            )
        chunk = matches[0]
        raw = read_raw_chunk(path, chunk)
        if (len(raw) < 12 or raw[:4] != b"VENV"
                or struct.unpack_from("<I", raw, 4)[0] not in (2, 3)
                or struct.unpack_from("<I", raw, 8)[0] != map_index):
            raise kvap.KvapError(
                f"{path.name} has an invalid VENV record for "
                f"{ORIGINAL_MAPS[map_index]}"
            )
        selected[map_index] = SelectedChunk(path, chunk, asset_id)
    return source_hash, selected


def select_segmented_image_chunks(
        path: Path, families: set[int], maps=None) -> tuple[bytes, dict[int, list[SelectedChunk]]]:
    """Select complete, CRC-verified VITI segment families for both slice maps."""
    maps = MAPS if maps is None else maps
    source_hash, chunks = read_source_index(path)
    selected: dict[int, list[SelectedChunk]] = {index: [] for index in maps}
    seen: set[int] = set()
    for chunk in chunks:
        family = chunk.asset_id & 0xFFFFFFFFFFFF0000
        map_index = map_chunk_index(chunk.asset_id)
        if (chunk.kind != kvap.CHUNK_TYPES["image"] or family not in families
                or map_index not in maps):
            continue
        if chunk.asset_id in seen:
            raise kvap.KvapError(
                f"{path.name} contains duplicate semantic asset "
                f"0x{chunk.asset_id:016x}"
            )
        raw = read_raw_chunk(path, chunk)
        if (len(raw) < 20 or raw[:4] != b"VITI"
                or struct.unpack_from("<I", raw, 4)[0] != 1
                or struct.unpack_from("<I", raw, 8)[0] == 0):
            raise kvap.KvapError(
                f"{path.name} contains an invalid semantic VITI segment "
                f"0x{chunk.asset_id:016x}"
            )
        seen.add(chunk.asset_id)
        selected[map_index].append(
            SelectedChunk(path, chunk, chunk.asset_id, raw)
        )
    for map_index, map_name in maps.items():
        present = {
            item.asset_id & 0xFFFFFFFFFFFF0000
            for item in selected[map_index]
        }
        missing = families - present
        if missing:
            raise kvap.KvapError(
                f"{path.name} lacks {map_name} semantic families: "
                + ", ".join(f"0x{value:016x}" for value in sorted(missing))
            )
        selected[map_index].sort(key=lambda item: item.asset_id)
    return source_hash, selected


def select_model_lod_chunk(path: Path, map_index: int) -> tuple[bytes, SelectedChunk]:
    """Validate one raw pointer-free VLOD sidecar and bind it to one map.

    VLOD was introduced after the source-complete v15 typed exports.  Keeping
    it as an explicit, content-hashed input lets those immutable map exports
    gain their authored lower-detail geometry without selecting any unrelated
    chunks from an older combined package.
    """
    if map_index not in ORIGINAL_MAPS:
        raise kvap.KvapError(f"unsupported VLOD map index: {map_index}")
    raw = path.read_bytes()
    if len(raw) < 20:
        raise kvap.KvapError(f"model LOD sidecar is truncated: {path}")
    magic, version, model_count, data_bytes = struct.unpack_from("<3IQ", raw)
    if (magic != 0x444F4C56 or version != 1 or model_count == 0
            or model_count > 320 or data_bytes != len(raw) - 20):
        raise kvap.KvapError(f"unsupported model LOD sidecar header: {path}")

    offset = 20
    names: list[str] = []
    for _ in range(model_count):
        if offset + 32 > len(raw):
            raise kvap.KvapError(f"model LOD record header is truncated: {path}")
        (name_bytes, lod_count, stored_lod_count, lod_ramp_type,
         *distances) = struct.unpack_from("<4I4f", raw, offset)
        if (name_bytes == 0 or name_bytes > 256 or lod_count < 2
                or lod_count > 4 or stored_lod_count == 0
                or stored_lod_count >= lod_count or lod_ramp_type > 1
                or any(not math.isfinite(value) or value < 0.0
                       for value in distances)):
            raise kvap.KvapError(f"model LOD record is invalid: {path}")
        previous_distance = 0.0
        terminal_distance = False
        for lod_index, distance in enumerate(distances):
            if lod_index >= lod_count:
                if distance != 0.0:
                    raise kvap.KvapError(
                        f"model LOD inactive distance is invalid: {path}"
                    )
                continue
            if (terminal_distance
                    or distance != 0.0 and distance <= previous_distance):
                raise kvap.KvapError(
                    f"model LOD distances are not ordered: {path}"
                )
            if distance == 0.0:
                terminal_distance = True
            else:
                previous_distance = distance
        offset += 32
        if offset + name_bytes > len(raw):
            raise kvap.KvapError(f"model LOD name is truncated: {path}")
        name_wire = raw[offset:offset + name_bytes]
        if b"\0" in name_wire:
            raise kvap.KvapError(f"model LOD name contains NUL: {path}")
        try:
            names.append(name_wire.decode("utf-8"))
        except UnicodeDecodeError as error:
            raise kvap.KvapError(
                f"model LOD name is not UTF-8: {path}"
            ) from error
        offset += name_bytes

        previous_lod = 0
        for _lod in range(stored_lod_count):
            if offset + 16 > len(raw):
                raise kvap.KvapError(
                    f"model LOD geometry header is truncated: {path}"
                )
            lod_index, surface_count, vertex_count, index_count = (
                struct.unpack_from("<4I", raw, offset)
            )
            offset += 16
            if (lod_index == 0 or lod_index >= lod_count
                    or lod_index <= previous_lod or surface_count == 0
                    or surface_count > 64 or vertex_count == 0
                    or vertex_count > 65535 or index_count < 3
                    or index_count > 300000 or index_count % 3 != 0):
                raise kvap.KvapError(
                    f"model LOD geometry record is invalid: {path}"
                )
            previous_lod = lod_index
            surface_ranges: list[tuple[int, int, int, int]] = []
            for _surface in range(surface_count):
                if offset + 24 > len(raw):
                    raise kvap.KvapError(
                        f"model LOD surface is truncated: {path}"
                    )
                (material_bytes, vertex_start, surface_vertices, index_start,
                 surface_indices, reserved) = struct.unpack_from(
                    "<6I", raw, offset
                )
                offset += 24
                if (material_bytes > 256 or reserved != 0
                        or surface_vertices == 0 or surface_indices == 0
                        or surface_indices % 3 != 0
                        or vertex_start + surface_vertices > vertex_count
                        or index_start + surface_indices > index_count
                        or offset + material_bytes > len(raw)):
                    raise kvap.KvapError(
                        f"model LOD surface is invalid: {path}"
                    )
                material_wire = raw[offset:offset + material_bytes]
                if b"\0" in material_wire:
                    raise kvap.KvapError(
                        f"model LOD material name contains NUL: {path}"
                    )
                try:
                    material_wire.decode("utf-8")
                except UnicodeDecodeError as error:
                    raise kvap.KvapError(
                        f"model LOD material name is not UTF-8: {path}"
                    ) from error
                offset += material_bytes
                surface_ranges.append((vertex_start, surface_vertices,
                                       index_start, surface_indices))
            vertex_bytes = vertex_count * 32
            index_bytes = index_count * 2
            if offset + vertex_bytes + index_bytes > len(raw):
                raise kvap.KvapError(f"model LOD geometry is truncated: {path}")
            offset += vertex_bytes
            indices = struct.unpack_from(f"<{index_count}H", raw, offset)
            if any(index >= vertex_count for index in indices):
                raise kvap.KvapError(
                    f"model LOD triangle index is invalid: {path}"
                )
            for vertex_start, surface_vertices, index_start, surface_indices in surface_ranges:
                if any(index < vertex_start
                       or index >= vertex_start + surface_vertices
                       for index in indices[index_start:index_start + surface_indices]):
                    raise kvap.KvapError(
                        f"model LOD surface triangle index is invalid: {path}"
                    )
            offset += index_bytes
    if (offset != len(raw) or names != sorted(names, key=str.lower)
            or len(set(name.lower() for name in names)) != len(names)):
        raise kvap.KvapError(f"model LOD sidecar size/order is invalid: {path}")

    payload_hash = hashlib.sha256(raw).digest()
    asset_id = 0x4D58010000000000 | map_index
    chunk = SourceChunk(
        index=0x1000 + map_index,
        kind=kvap.CHUNK_TYPES["model"],
        asset_id=asset_id,
        offset=0,
        stored_bytes=len(raw),
        raw_bytes=len(raw),
        codec=kvap.CODECS["none"],
        crc32=zlib.crc32(raw) & 0xFFFFFFFF,
    )
    return payload_hash, SelectedChunk(path, chunk, asset_id, raw)


def validate_direct_schema(path: Path,
                           selected: list[tuple[SourceChunk, bytes]],
                           require_material_semantics: bool = True,
                           require_model_lods: bool = True) -> None:
    """Reject a typed export that cannot own the promised source runtime."""
    versions: dict[tuple[int, bytes], tuple[int, int, int]] = {}
    for chunk, magic in selected:
        raw = read_raw_chunk(path, chunk)
        version = struct.unpack_from("<I", raw, 4)[0]
        count = struct.unpack_from("<I", raw, 8)[0] if len(raw) >= 12 else 0
        record_bytes = struct.unpack_from("<I", raw, 12)[0] if len(raw) >= 16 else 0
        versions[(chunk.kind, magic)] = (version, count, record_bytes)

    collision = versions.get((kvap.CHUNK_TYPES["collision"], b"VITC"))
    weapons = versions.get((kvap.CHUNK_TYPES["weapon"], b"VITP"))
    models = versions.get((kvap.CHUNK_TYPES["model"], b"VITX"))
    model_lods = versions.get((kvap.CHUNK_TYPES["model"], b"VLOD"))
    animations = versions.get((kvap.CHUNK_TYPES["animation"], b"VITQ"))
    effects = versions.get((kvap.CHUNK_TYPES["effects"], b"VFXA"))
    impact_fx = versions.get((kvap.CHUNK_TYPES["effects"], b"VIFX"))
    if not collision or collision[0] != 3:
        raise kvap.KvapError(f"{path.name} requires source-complete VITC v3 collision")
    # Fixed loadout, source bootstrap, short-fuse grenade and destructible
    # car definitions. Script dependencies are not extra selectable weapons.
    if not weapons or weapons != (5, 8, 15376):
        raise kvap.KvapError(
            f"{path.name} requires exactly eight source-complete VITP v5 records"
        )
    from source_runtime_profile import validate_source_weapon_set
    for chunk, magic in selected:
        if chunk.kind == kvap.CHUNK_TYPES["weapon"] and magic == b"VITP":
            validate_source_weapon_set(read_raw_chunk(path, chunk))
    if not models or models[0] != 5:
        raise kvap.KvapError(f"{path.name} requires source-complete VITX v5 models")
    if require_model_lods and (not model_lods or model_lods[0] != 1):
        raise kvap.KvapError(
            f"{path.name} requires a VLOD v1 authored model LOD archive"
        )
    if not animations or animations[0] != 3:
        raise kvap.KvapError(
            f"{path.name} requires source-complete VITQ v3 animations"
        )
    if not effects or effects[0] != 3:
        raise kvap.KvapError(f"{path.name} requires source-complete VFXA v3 effects")
    if not impact_fx or impact_fx[0] != 1:
        raise kvap.KvapError(f"{path.name} requires a source-complete VIFX v1 impact table")
    required = {
        (kvap.CHUNK_TYPES["world"], b"VITW"),
        (kvap.CHUNK_TYPES["world"], b"VITG"),
        (kvap.CHUNK_TYPES["world"], b"VITE"),
        (kvap.CHUNK_TYPES["material"], b"VITL"),
        (kvap.CHUNK_TYPES["image"], b"VITI"),
        (kvap.CHUNK_TYPES["model"], b"VITX"),
        (kvap.CHUNK_TYPES["model"], b"VLOD"),
        (kvap.CHUNK_TYPES["animation"], b"VITQ"),
        (kvap.CHUNK_TYPES["effects"], b"VFXA"),
        (kvap.CHUNK_TYPES["effects"], b"VIFX"),
        (kvap.CHUNK_TYPES["script"], b"VITR"),
        (kvap.CHUNK_TYPES["menu"], b"VITM"),
        (kvap.CHUNK_TYPES["font"], b"VITF"),
        (kvap.CHUNK_TYPES["sound"], b"VITS"),
        (kvap.CHUNK_TYPES["hud"], b"VITH"),
    }
    if not require_model_lods:
        required.remove((kvap.CHUNK_TYPES["model"], b"VLOD"))
    missing = required - versions.keys()
    if missing:
        labels = ", ".join(
            f"type={kind}/{magic.decode('ascii')}" for kind, magic in sorted(missing)
        )
        raise kvap.KvapError(f"{path.name} is missing runtime streams: {labels}")

    payload_by_key = {
        (chunk.kind, magic): read_raw_chunk(path, chunk)
        for chunk, magic in selected
    }
    impact_names = vifx_effect_names(
        payload_by_key[(kvap.CHUNK_TYPES["effects"], b"VIFX")]
    )
    graph_names = vfxa_effect_names(
        payload_by_key[(kvap.CHUNK_TYPES["effects"], b"VFXA")]
    )
    missing_impact_effects = impact_names - graph_names
    if missing_impact_effects:
        sample = ", ".join(sorted(missing_impact_effects)[:4])
        raise kvap.KvapError(
            f"{path.name} VFXA lacks {len(missing_impact_effects)} VIFX effects: {sample}"
        )
    image_names = viti_names(
        payload_by_key[(kvap.CHUNK_TYPES["image"], b"VITI")]
    )
    referenced_images = vitl_image_names(
        payload_by_key[(kvap.CHUNK_TYPES["material"], b"VITL")]
    )
    resolved_images = referenced_images & image_names
    authored_normal = any(
        name != "$identitynormalmap"
        and ("_nml" in name.lower() or "normal" in name.lower())
        for name in resolved_images
    )
    authored_specular = any(
        "_spc" in name.lower() or "spec" in name.lower()
        for name in resolved_images
    )
    if (len(resolved_images) < 32
            or require_material_semantics
            and (not authored_normal or not authored_specular)):
        raise kvap.KvapError(
            f"{path.name} VITI lacks a bounded authored material-semantic closure"
        )
    if ("$identitynormalmap" not in image_names
            or not any(name.startswith("*lightmap") for name in image_names)
            or not any(name.startswith("*reflection_probe")
                       for name in image_names)):
        raise kvap.KvapError(
            f"{path.name} VITI lacks the required normal/lightmap/reflection closure"
        )


def map_chunk_index(asset_id: int) -> int | None:
    simple_family = asset_id & 0xFFFFFFFFFFFFFF00
    if simple_family in SIMPLE_MAP_FAMILIES:
        return asset_id & 0xFF
    segmented_family = asset_id & 0xFFFFFFFFFFFF0000
    if segmented_family in SEGMENTED_MAP_FAMILIES:
        return asset_id & 0xFF
    return None


def mip_bytes(fmt: int, width: int, height: int) -> int:
    if fmt == 0x31545844:  # DXT1
        return ((width + 3) // 4) * ((height + 3) // 4) * 8
    if fmt in (0x33545844, 0x35545844):  # DXT3/DXT5
        return ((width + 3) // 4) * ((height + 3) // 4) * 16
    if fmt == 50:  # L8
        return width * height
    if fmt == 21:  # A8R8G8B8
        return width * height * 4
    return 0


def image_cap(name: str) -> int:
    lower = name.lower()
    if name in COMMON_MODEL_COLOR_NAMES or is_vertical_slice_viewmodel_semantic(name):
        return 1024
    if any(token in lower for token in (
        "menu", "hud", "font", "compass", "weapon", "viewmodel", "viewhands",
        "lightmap", "scorebar", "reticle", "loadscreen", "loading",
    )):
        return 1024
    if any(token in lower for token in (
        "decal", "paper", "trash", "rubble", "foliage", "particle",
        "smoke_puff", "bulletimpact", "shellcase", "tree_", "palmtree",
        "canopy", "xpec_", "80s_", "crate", "pallet", "barrel",
        "airconditioner", "woodcage", "me_sign", "flag_neutral",
        "desertshrub", "bus_windows", "water_tower", "wall_stain",
        "television", "satellitedish", "russian_table", "riverreeds",
        "restaurant", "refrigerator", "potted_plant", "plastic_case",
        "plastic_bottles", "me_paintings", "lightbox", "furniture_",
        "floodlight", "fire_extinguisher", "dumpster", "dining_",
        "diner_", "copperpan", "water_heater", "waching_machine",
        "couch", "mattress", "air_duct", "ac_win", "ac_big",
        "55_gallon",
    )):
        return 256
    return 512


def trim_viti(raw: bytes, maximum_top_mip: int | None = None,
              protected_names: set[str] | None = None
              ) -> tuple[bytes, dict[str, int]]:
    if len(raw) < 20:
        raise kvap.KvapError("VITI chunk is shorter than its header")
    magic, version, count, declared = struct.unpack_from("<IIIQ", raw, 0)
    if magic != 0x49544956 or version != 1 or count > 4096:
        raise kvap.KvapError("unsupported VITI stream")
    if (maximum_top_mip is not None
            and (maximum_top_mip <= 0
                 or maximum_top_mip & (maximum_top_mip - 1))):
        raise kvap.KvapError("VITI maximum top mip must be a power of two")
    offset = 20
    output_records: list[bytes] = []
    input_payload = 0
    output_payload = 0
    trimmed_records = 0
    protected_records = 0
    protected_payload = 0
    for _ in range(count):
        if offset + 32 > len(raw):
            raise kvap.KvapError("truncated VITI record header")
        name_bytes, payload_bytes, width, height, depth, levels, fmt, flags = struct.unpack_from(
            "<8I", raw, offset
        )
        offset += 32
        if name_bytes == 0 or payload_bytes == 0 or offset + name_bytes + payload_bytes > len(raw):
            raise kvap.KvapError("invalid VITI record bounds")
        name_raw = raw[offset:offset + name_bytes]
        offset += name_bytes
        payload = raw[offset:offset + payload_bytes]
        offset += payload_bytes
        name = name_raw.decode("utf-8")
        input_payload += payload_bytes

        cap = image_cap(name)
        protected = protected_names is not None and name in protected_names
        if protected:
            protected_records += 1
            protected_payload += payload_bytes
        if maximum_top_mip is not None and not protected:
            cap = min(cap, maximum_top_mip)
        skip = 0
        new_width, new_height = width, height
        while max(new_width, new_height) > cap and skip + 1 < levels:
            skip += 1
            new_width = max(1, new_width // 2)
            new_height = max(1, new_height // 2)
        level_sizes: list[int] = []
        level_width, level_height = width, height
        for _level in range(levels):
            size = mip_bytes(fmt, level_width, level_height)
            if size == 0:
                skip = 0
                level_sizes = []
                break
            level_sizes.append(size)
            level_width = max(1, level_width // 2)
            level_height = max(1, level_height // 2)
        face_count = 6 if flags & 0x4 else 1
        expected = sum(level_sizes) * face_count if level_sizes else payload_bytes
        if expected != payload_bytes:
            # Preserve unknown/exporter-extension payloads byte-for-byte.
            skip = 0
            level_sizes = []
        if skip and level_sizes:
            face_bytes = sum(level_sizes)
            kept_face_bytes = sum(level_sizes[skip:])
            prefix = sum(level_sizes[:skip])
            payload = b"".join(
                payload[face * face_bytes + prefix:
                        face * face_bytes + prefix + kept_face_bytes]
                for face in range(face_count)
            )
            levels -= skip
            width, height = new_width, new_height
            trimmed_records += 1
        output_payload += len(payload)
        output_records.append(
            struct.pack("<8I", name_bytes, len(payload), width, height,
                        depth, levels, fmt, flags) + name_raw + payload
        )
    if offset != len(raw) or input_payload != declared:
        raise kvap.KvapError("VITI aggregate length does not match its header")
    rebuilt = struct.pack("<IIIQ", magic, version, count, output_payload) + b"".join(output_records)
    return rebuilt, {
        "records": count,
        "trimmedRecords": trimmed_records,
        "inputPayloadBytes": input_payload,
        "outputPayloadBytes": output_payload,
        "maximumTopMip": maximum_top_mip or 0,
        "protectedRecords": protected_records,
        "protectedPayloadBytes": protected_payload,
    }


def retained_sound_bytes(raw: bytes, full_source_closure: bool = False) -> int:
    if len(raw) < 20:
        return 0
    magic, version, count, _declared = struct.unpack_from("<IIIQ", raw, 0)
    if magic == 0x4C415356:
        # Conservative typed alias/curve/speaker-map arena allowance. The
        # actual fixed-width catalog stays on disk behind persistent handles.
        return len(raw)
    if magic != 0x53544956 or version != 1:
        return 0
    offset = 20
    retained = 0
    for _ in range(count):
        if offset + 28 > len(raw):
            raise kvap.KvapError("truncated VITS record")
        name_bytes, sample_bytes, _rate, _bits, _channels, _samples, _format = struct.unpack_from(
            "<7I", raw, offset
        )
        offset += 28
        if offset + name_bytes + sample_bytes > len(raw):
            raise kvap.KvapError("invalid VITS record bounds")
        name = raw[offset:offset + name_bytes].decode("utf-8")
        offset += name_bytes
        if full_source_closure or name in RUNTIME_SOUND_NAMES:
            retained += sample_bytes
        offset += sample_bytes
    return retained


def world_gpu_bytes(raw: bytes) -> int:
    if len(raw) < 20 or raw[:4] != b"VITW":
        return 0
    vertex_count, index_count = struct.unpack_from("<II", raw, 8)
    return vertex_count * 44 + index_count * 2


def chunk_entry(kind: int, asset_id: int, path: Path) -> dict[str, object]:
    return {"type": kind, "assetId": f"0x{asset_id:016x}", "path": str(path), "codec": "none"}


def embedded_content_manifest(payload: dict) -> str:
    text = json.dumps(payload, indent=2) + "\n"
    if len(text.encode("utf-8")) >= 16 * 1024:
        raise kvap.KvapError("embedded content manifest exceeds the Vita 16 KiB parser capacity")
    # The legacy typed-asset probe reads a bounded prefix before mounting.
    if ('"playable": true' not in text[:4096]
            or '"payloadPolicy": "runtime-ready-v1"' not in text[:4096]):
        raise kvap.KvapError("embedded manifest startup fields exceed the Vita probe prefix")
    return text


def write_pack(manifest: dict[str, object], manifest_path: Path,
               output_path: Path, workspace: Path) -> dict[str, object]:
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    legal_root = workspace.parent / "__owned_cod4_source__"
    raw_report = kvap.create_package(manifest_path, output_path, workspace, legal_root)
    report = public_pack_report(raw_report, output_path.name)
    # kvap's general-purpose report identifies every input source path. Those
    # paths point into this function's temporary extraction directory and are
    # neither reproducible nor useful deployment metadata. Replace it with a
    # stable, path-free report beside the pack.
    output_path.with_suffix(output_path.suffix + ".report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    return report


def public_pack_report(report: dict[str, object], filename: str) -> dict[str, object]:
    chunks: list[dict[str, object]] = []
    for chunk in report.get("chunks", []):
        if not isinstance(chunk, dict):
            continue
        chunks.append({
            key: chunk[key]
            for key in (
                "type", "assetId", "offset", "storedBytes", "rawBytes",
                "codec", "crc32",
            )
            if key in chunk
        })
    return {
        "format": report.get("format", "KVAP"),
        "version": report.get("version", "unknown"),
        "packageId": report.get("packageId", "0x0000000000000000"),
        "sourceHash": report.get("sourceHash", ""),
        "file": filename,
        "bytes": report.get("bytes", 0),
        "chunks": chunks,
    }


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def _build(source: Path | None, output: Path, workspace: Path,
          menu_video: Path | None, menu_music: Path | None,
          shipment_source: Path | None = None,
          crossfire_source: Path | None = None,
          environment_source: Path | None = None,
          world_semantic_source: Path | None = None,
          model_semantic_source: Path | None = None,
          shipment_model_lods: Path | None = None,
          crossfire_model_lods: Path | None = None,
          shipment_sound_closure: Path | None = None,
          crossfire_sound_closure: Path | None = None,
          source_runtime_profile: bool = False,
          retail_root: Path | None = None,
          map_inputs: dict[int, dict[str, Path]] | None = None,
          texture_paging: bool = False) -> dict[str, object]:
    schema_version = 2 if texture_paging else SCHEMA_VERSION
    residency_contract = {'textureResidency': 'paged-world-v1'} if texture_paging else {}
    if texture_paging and map_inputs is None:
        raise kvap.KvapError('Texture paging requires a versioned original-map delivery')
    output = output.resolve()
    workspace = workspace.resolve()
    if not kvap._is_within(output, workspace):
        raise kvap.KvapError("output must remain inside the workspace")
    output.mkdir(parents=True, exist_ok=True)

    MAPS = ({i: ORIGINAL_MAPS[i] for i in sorted(map_inputs)} if map_inputs
            else {0: "shipment", 2: "crossfire"})
    direct_mode = map_inputs is not None or shipment_source is not None or crossfire_source is not None
    source_inputs: dict[str, str] = {}
    lod_color_repairs: dict[str, dict] = {}
    runtime_profile_report: dict[str, object] = {}
    source_effect_reports: dict[str, object] = {}
    source_ui_reports: dict[str, dict] = {}
    sound_split_report: dict[str, object] = {}
    source_streams: dict[str, tuple[Path, dict]] = {}
    closures = ({i:e['soundClosure'] for i,e in map_inputs.items() if 'soundClosure' in e}
                if map_inputs is not None else {i:p for i,p in ((0,shipment_sound_closure),(2,crossfire_sound_closure)) if p is not None})
    source_audio = bool(closures)
    if map_inputs is not None and (not source_audio or not source_runtime_profile):
        raise kvap.KvapError('Original-map deliveries require all source audio closures and --source-runtime-profile')
    if source_audio and (not direct_mode or set(closures)!=set(MAPS)):
        raise kvap.KvapError("source audio requires a closure report for every selected map")
    if direct_mode:
        if source is not None:
            raise kvap.KvapError("legacy source cannot be combined with independent map inputs")
        if map_inputs is not None:
            if any(x is not None for x in (shipment_source,crossfire_source,shipment_model_lods,crossfire_model_lods,shipment_sound_closure,crossfire_sound_closure)):
                raise kvap.KvapError("map inputs cannot be combined with legacy per-map arguments")
            if 0 not in map_inputs or not set(map_inputs)<=set(ORIGINAL_MAPS):
                raise kvap.KvapError("selected original maps must include Shipment common frontend")
            source_paths={i:e['source'].resolve() for i,e in map_inputs.items()}
            supplemental_model_lods=False
        else:
            if shipment_source is None or crossfire_source is None:
                raise kvap.KvapError("both --shipment-source and --crossfire-source are required")
            source_paths={0:shipment_source.resolve(),2:crossfire_source.resolve()}
            supplemental_model_lods=shipment_model_lods is not None or crossfire_model_lods is not None
            if supplemental_model_lods and (shipment_model_lods is None or crossfire_model_lods is None):
                raise kvap.KvapError("both model LOD sidecars are required")
        source_hashes: dict[int, bytes] = {}
        direct_chunks: dict[int, list[tuple[SourceChunk, bytes]]] = {}
        for map_index, source_path in source_paths.items():
            if not source_path.is_file():
                raise kvap.KvapError(
                    f"{MAPS[map_index]} typed package does not exist: {source_path}"
                )
            source_hashes[map_index], indexed = read_source_index(source_path)
            for chunk in indexed:
                if chunk.kind == kvap.CHUNK_TYPES["manifest"]:
                    metadata = json.loads(read_raw_chunk(source_path, chunk))
                    if metadata.get("sourceEffectDependencies", {}).get("available") is True:
                        source_effect_reports[MAPS[map_index]] = metadata["sourceEffectDependencies"]
                    if metadata.get("sourceUiDependencies", {}).get("available") is True:
                        source_ui_reports[MAPS[map_index]] = metadata["sourceUiDependencies"]
            if source_runtime_profile and MAPS[map_index] not in source_effect_reports:
                raise kvap.KvapError(f"{source_path.name} lacks source FX dependency provenance")
            direct_chunks[map_index] = direct_binary_chunks(source_path, indexed)
            ui_report = source_ui_reports.get(MAPS[map_index])
            if ui_report:
                ui_chunks = [chunk for chunk, magic in direct_chunks[map_index] if magic == b"VITL"]
                if (len(ui_chunks) != 1 or hashlib.sha256(read_raw_chunk(source_path, ui_chunks[0])).hexdigest()
                        != ui_report.get("materialStreamSha256")):
                    raise kvap.KvapError(f"{source_path.name} UI material/provenance mismatch")
            effect_report = source_effect_reports.get(MAPS[map_index])
            if effect_report:
                effect_chunks = [chunk for chunk, magic in direct_chunks[map_index]
                                 if magic == b"VFXA"]
                if (len(effect_chunks) != 1 or
                        hashlib.sha256(read_raw_chunk(source_path, effect_chunks[0])).hexdigest()
                        != effect_report.get("effectGraphSha256")):
                    raise kvap.KvapError(f"{source_path.name} source FX graph/provenance mismatch")
            validate_direct_schema(
                source_path, direct_chunks[map_index],
                require_material_semantics=world_semantic_source is None and map_inputs is None,
                require_model_lods=not supplemental_model_lods,
            )
            source_inputs[f"mp_{MAPS[map_index]}"] = source_hashes[map_index].hex()
            source_inputs[f"mp_{MAPS[map_index]}Package"] = sha256_file(source_path)
        environment_chunks: dict[int, SelectedChunk] = {}
        environment_hash: bytes | None = None
        if environment_source is not None:
            environment_source = environment_source.resolve()
            if not environment_source.is_file():
                raise kvap.KvapError(
                    f"environment archive does not exist: {environment_source}"
                )
            environment_source_hash, environment_chunks = (
                select_environment_chunks(environment_source, MAPS)
            )
            environment_hash = selected_chunk_set_hash(
                "environment", environment_chunks.values()
            )
            source_inputs["environmentArchive"] = environment_hash.hex()
            source_inputs["environmentSourceInstall"] = (
                environment_source_hash.hex()
            )
        indexed_by_key = {
            map_index: {
                (chunk.kind, magic): chunk
                for chunk, magic in selected
            }
            for map_index, selected in direct_chunks.items()
        }
        exact_shared_keys: set[tuple[int, bytes]] = set()
        for key, shipment_chunk in indexed_by_key[0].items():
            if shipment_chunk.kind not in EXACT_SHARED_TYPES and key not in COMMON_BINARY_KEYS:
                continue
            raw=read_raw_chunk(source_paths[0],shipment_chunk)
            if all(key in indexed_by_key[i] and read_raw_chunk(source_paths[i],indexed_by_key[i][key])==raw for i in MAPS):
                exact_shared_keys.add(key)

        common_chunks = [
            SelectedChunk(source_paths[0], chunk, chunk.asset_id)
            for chunk, _magic in direct_chunks[0]
            if (chunk.kind in COMMON_TYPES or (chunk.kind, _magic) in COMMON_BINARY_KEYS)
            and chunk.kind != kvap.CHUNK_TYPES["sound"]
        ]
        map_chunks = {
            map_index: [
                SelectedChunk(
                    source_paths[map_index], chunk,
                    direct_map_asset_id(
                        chunk.kind, magic, map_index, chunk.asset_id
                    ),
                )
                for chunk, magic in direct_chunks[map_index]
                if (chunk.kind, magic) not in exact_shared_keys
                and chunk.kind != kvap.CHUNK_TYPES["sound"]
            ]
            for map_index in MAPS
        }
        model_lod_hash: bytes | None = None
        if supplemental_model_lods:
            assert shipment_model_lods is not None
            assert crossfire_model_lods is not None
            model_lod_paths = {
                0: shipment_model_lods.resolve(),
                2: crossfire_model_lods.resolve(),
            }
            model_lod_hashes: dict[int, bytes] = {}
            for map_index, model_lod_path in model_lod_paths.items():
                if not model_lod_path.is_file():
                    raise kvap.KvapError(
                        f"{MAPS[map_index]} model LOD sidecar does not exist: "
                        f"{model_lod_path}"
                    )
                model_lod_hashes[map_index], model_lod_chunk = (
                    select_model_lod_chunk(model_lod_path, map_index)
                )
                if any(item.asset_id == model_lod_chunk.asset_id
                       for item in map_chunks[map_index]):
                    raise kvap.KvapError(
                        f"{MAPS[map_index]} typed package already contains VLOD; "
                        "remove the supplemental model LOD input"
                    )
                map_chunks[map_index].append(model_lod_chunk)
                source_inputs[f"{MAPS[map_index]}ModelLods"] = (
                    model_lod_hashes[map_index].hex()
                )
            lod_digest = hashlib.sha256()
            lod_digest.update(b"KISAK-COD4-VITA-MODEL-LOD-PAIR\0")
            for map_index in MAPS:
                lod_digest.update(MAPS[map_index].encode("ascii") + b"\0")
                lod_digest.update(model_lod_hashes[map_index])
            model_lod_hash = lod_digest.digest()
        world_semantic_hash: bytes | None = None
        if world_semantic_source is not None:
            world_semantic_source = world_semantic_source.resolve()
            if not world_semantic_source.is_file():
                raise kvap.KvapError(
                    "world semantic archive does not exist: "
                    f"{world_semantic_source}"
                )
            world_semantic_source_hash, semantic_chunks = (
                select_segmented_image_chunks(
                    world_semantic_source, WORLD_SEMANTIC_FAMILIES, MAPS
                )
            )
            world_semantic_hash = selected_chunk_set_hash(
                "world-semantics",
                (item for selected in semantic_chunks.values()
                 for item in selected),
            )
            for map_index in MAPS:
                existing_ids = {
                    item.asset_id for item in map_chunks[map_index]
                }
                for item in semantic_chunks[map_index]:
                    if item.asset_id not in existing_ids:
                        map_chunks[map_index].append(item)
                map_chunks[map_index].sort(key=lambda item: (
                    item.chunk.kind, item.asset_id, item.chunk.index
                ))
            source_inputs["worldSemanticArchive"] = world_semantic_hash.hex()
            source_inputs["worldSemanticSourceInstall"] = (
                world_semantic_source_hash.hex()
            )
        model_semantic_hash: bytes | None = None
        if model_semantic_source is not None:
            model_semantic_source = model_semantic_source.resolve()
            if not model_semantic_source.is_file():
                raise kvap.KvapError(
                    "model semantic archive does not exist: "
                    f"{model_semantic_source}"
                )
            model_semantic_source_hash, semantic_chunks = (
                select_segmented_image_chunks(
                    model_semantic_source, MODEL_SEMANTIC_FAMILIES, MAPS
                )
            )
            repair_model_semantic_lod_aliases(semantic_chunks)
            model_semantic_hash = selected_chunk_set_hash(
                "model-semantics",
                (item for selected in semantic_chunks.values()
                 for item in selected),
            )
            for map_index in MAPS:
                existing_ids = {
                    item.asset_id for item in map_chunks[map_index]
                }
                for item in semantic_chunks[map_index]:
                    if item.asset_id not in existing_ids:
                        map_chunks[map_index].append(item)
                map_chunks[map_index].sort(key=lambda item: (
                    item.chunk.kind, item.asset_id, item.chunk.index
                ))
            source_inputs["modelSemanticArchive"] = (
                model_semantic_hash.hex()
            )
            source_inputs["modelSemanticSourceInstall"] = (
                model_semantic_source_hash.hex()
            )
        if map_inputs is not None:
            for map_index, entry in map_inputs.items():
                if 'supplement' not in entry:raise kvap.KvapError('map input requires a validated supplement')
                supplement=entry['supplement']
                from prepare_map_closure import validate_supplement
                validate_supplement(source_paths[map_index],supplement,map_index,workspace)
                _, extra=read_source_index(supplement)
                source_inputs[f'{MAPS[map_index]}Supplement']=sha256_file(supplement)
                existing={item.asset_id for item in map_chunks[map_index]}
                for chunk in extra:
                    if chunk.kind==kvap.CHUNK_TYPES['manifest']:continue
                    if chunk.asset_id!=COMMON_MODEL_COLOR_ASSET and map_chunk_index(chunk.asset_id)!=map_index:
                        raise kvap.KvapError('supplement contains a different map')
                    if chunk.asset_id in existing:raise kvap.KvapError('duplicate supplemental asset')
                    map_chunks[map_index].append(SelectedChunk(supplement,chunk,chunk.asset_id))
                    existing.add(chunk.asset_id)
        lod_color_repairs = repair_model_lod_colors(map_chunks, retail_root)
        if lod_color_repairs:
            source_inputs["modelLodColorRepairs"] = hashlib.sha256(json.dumps(
                lod_color_repairs, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        if map_inputs is None:
            source_hash = source_set_hash(source_hashes[0],source_hashes[2],environment_hash,
                world_semantic_hash,model_lod_hash,model_semantic_hash)
        else:
            source_hash=hashlib.sha256(b'COD4-VITA-MAP-SET-v1\0'+json.dumps(source_inputs,sort_keys=True).encode()).digest()
        if source_runtime_profile:
            from source_runtime_profile import PROFILE, SOURCE_WEAPONS, profile_scripts, profile_tables, profile_entities
            changed_payloads: dict[str, tuple[bytes, list[dict]]] = {}
            def configure(chunks: list[SelectedChunk]) -> list[SelectedChunk]:
                configured = []
                for item in chunks:
                    if item.chunk.kind not in (kvap.CHUNK_TYPES["script"], kvap.CHUNK_TYPES["strings"], kvap.CHUNK_TYPES["world"]):
                        configured.append(item)
                        continue
                    raw = read_selected_chunk(item)
                    transform = {b"VITR": profile_scripts, b"VITT": profile_tables,
                                 b"VITE": profile_entities}.get(raw[:4])
                    if transform is None:
                        configured.append(item)
                        continue
                    identity = hashlib.sha256(raw).hexdigest()
                    if identity not in changed_payloads:
                        changed_payloads[identity] = transform(raw)
                    payload, _changes = changed_payloads[identity]
                    configured.append(SelectedChunk(item.package, item.chunk, item.asset_id, payload))
                return configured
            common_chunks = configure(common_chunks)
            map_chunks = {index: configure(chunks) for index, chunks in map_chunks.items()}
            if not changed_payloads:
                raise kvap.KvapError("source runtime profile has no source script/table payloads")
            runtime_profile_report = {
                "name": PROFILE, "qualification": "configuration-only",
                "sourceWeapons": list(SOURCE_WEAPONS),
                "chunks": {identity: {"outputSha256": hashlib.sha256(payload).hexdigest(),
                                       "records": changes}
                           for identity, (payload, changes) in sorted(changed_payloads.items())},
            }
            profile_hash = hashlib.sha256(json.dumps(runtime_profile_report,
                sort_keys=True, separators=(",", ":")).encode()).digest()
            source_inputs["sourceRuntimeProfile"] = profile_hash.hex()
            source_hash = hashlib.sha256(source_hash + b"source-runtime-profile\0" + profile_hash).digest()

        # The direct typed exports keep first-person colors inside each map's
        # VITI. Runtime map replacement destroys those GPU handles before it
        # mounts the next map, so the certified split contract also needs an
        # independently reloadable common companion. Copy only byte-identical
        # authored records shared by both source exports; never synthesize or
        # reinterpret image payloads.
        image_key = (kvap.CHUNK_TYPES["image"], b"VITI")
        if any(image_key not in indexed_by_key[i] for i in MAPS):
            raise kvap.KvapError("selected map lacks a VITI image stream")
        color_records={}
        for i in MAPS:
            selected,_=select_viti_records(read_raw_chunk(source_paths[i],indexed_by_key[i][image_key]),COMMON_MODEL_COLOR_NAMES)
            for name,record in parse_viti_records(selected).items():
                if name in color_records and color_records[name]!=record:
                    raise kvap.KvapError("conflicting common first-person image: "+name)
                color_records[name]=record
        missing=REQUIRED_COMMON_MODEL_COLOR_NAMES-color_records.keys()
        if missing:raise kvap.KvapError("missing common first-person images: "+', '.join(sorted(missing)))
        common_chunks.append(SelectedChunk(source_paths[0],indexed_by_key[0][image_key],
            COMMON_MODEL_COLOR_ASSET,build_viti_records(color_records)))

        # The font atlas and shared shell/HUD chrome belong to the frontend
        # lifetime, not to a selected map. Some map-specific closures omit
        # them entirely (Crossfire historically omitted gamefonts_pc), which
        # forced the runtime into a hundreds-of-primitives stroke fallback.
        # Preserve the exact records from the user's Shipment export in one
        # independently mounted common VITI.
        source_ui_names = {name for report in source_ui_reports.values() for name in report["images"]
                           if not name.startswith(("compass_map_mp_", "loadscreen_mp_"))}
        common_presentation_images, common_presentation_names = (
            select_viti_records(
                read_raw_chunk(source_paths[0],
                               indexed_by_key[0][image_key]),
                COMMON_PRESENTATION_IMAGE_NAMES | source_ui_names,
            )
        )
        missing_presentation_images = (
            (REQUIRED_COMMON_PRESENTATION_IMAGE_NAMES | source_ui_names)
            - common_presentation_names
        )
        if missing_presentation_images:
            raise kvap.KvapError(
                "typed Shipment package is missing required common frontend images: "
                + ", ".join(sorted(missing_presentation_images))
            )
        common_chunks.append(SelectedChunk(
            source_paths[0], indexed_by_key[0][image_key],
            COMMON_PRESENTATION_IMAGE_ASSET, common_presentation_images,
        ))

        for map_index, environment_chunk in environment_chunks.items():
            has_environment = any(
                (selected.asset_id & 0xFFFFFFFFFFFFFF00)
                == 0x4D56000000000000
                for selected in map_chunks[map_index]
            )
            if not has_environment:
                map_chunks[map_index].append(environment_chunk)

        sound_key = (kvap.CHUNK_TYPES["sound"], b"VITS")
        if any(sound_key not in indexed_by_key[i] for i in MAPS):
            raise kvap.KvapError("typed map pair is missing its VITS sound stream")
        sound_records = {
            map_index: parse_vits_records(read_raw_chunk(
                source_paths[map_index], indexed_by_key[map_index][sound_key]
            ))
            for map_index in MAPS
        }
        if source_audio:
            from source_sound_closure import parse_aliases
            for map_index, report_path in closures.items():
                assert report_path is not None
                report_path = report_path.resolve()
                report = json.loads(report_path.read_text())
                if report.get("schemaVersion") != 1:
                    raise kvap.KvapError("incompatible source sound closure")
                source_inputs[f"{MAPS[map_index]}SoundClosure"] = sha256_file(report_path)
                for key, leaf in (("catalog", "vita_soundaliases.bin"), ("pcm", "vita_sounds.bin")):
                    path = report_path.parent / leaf
                    if path.stat().st_size != report[key]["bytes"] or sha256_file(path) != report[key]["sha256"]:
                        raise kvap.KvapError(f"source sound closure hash mismatch: {path}")
                catalog_path = report_path.parent / "vita_soundaliases.bin"
                catalog = catalog_path.read_bytes()
                parse_aliases(catalog)
                alias_id = 0x4D53010000000000 | map_index
                alias_chunk = SourceChunk(0x2000 + map_index, kvap.CHUNK_TYPES["sound"], alias_id,
                    0, len(catalog), len(catalog), kvap.CODECS["none"], zlib.crc32(catalog) & 0xFFFFFFFF)
                map_chunks[map_index].append(SelectedChunk(catalog_path, alias_chunk, alias_id, catalog))
                sound_records[map_index] = parse_vits_records((report_path.parent / "vita_sounds.bin").read_bytes())
                for relative, expected in report.get("streams", {}).items():
                    path = report_path.parent / relative
                    if (not relative.startswith("stream/") or not path.resolve().is_relative_to(report_path.parent)
                            or path.stat().st_size != expected["bytes"] or sha256_file(path) != expected["sha256"]):
                        raise kvap.KvapError(f"source stream contract mismatch: {relative}")
                    previous = source_streams.get(relative)
                    if previous and previous[1] != expected:
                        raise kvap.KvapError(f"source maps disagree on shared stream: {relative}")
                    source_streams[relative] = (path, expected)
        shared_sound_names = {
            name for name, record in sound_records[0].items()
            if all(sound_records[i].get(name) == record for i in MAPS)
        }
        if not shared_sound_names:
            raise kvap.KvapError("typed map pair has no identical common sounds")
        common_sound_records = {
            name: sound_records[0][name] for name in shared_sound_names
        }
        shipment_sound_chunk = indexed_by_key[0][sound_key]
        common_sound_payload = build_vits(common_sound_records)
        common_chunks.append(SelectedChunk(
            source_paths[0], shipment_sound_chunk, shipment_sound_chunk.asset_id,
            common_sound_payload,
        ))
        unique_sound_counts: dict[str, int] = {}
        unique_sound_bytes: dict[str, int] = {}
        for map_index in MAPS:
            unique_records = {
                name: record for name, record in sound_records[map_index].items()
                if name not in shared_sound_names
            }
            if not unique_records:
                unique_sound_counts[MAPS[map_index]] = 0
                unique_sound_bytes[MAPS[map_index]] = 0
                continue
            sound_chunk = indexed_by_key[map_index][sound_key]
            unique_payload = build_vits(unique_records)
            map_chunks[map_index].append(SelectedChunk(
                source_paths[map_index], sound_chunk, sound_chunk.asset_id,
                unique_payload,
            ))
            unique_sound_counts[MAPS[map_index]] = len(unique_records)
            unique_sound_bytes[MAPS[map_index]] = len(unique_payload)
        sound_split_report = {
            "commonRecords": len(common_sound_records),
            "commonBytes": len(common_sound_payload),
            "mapUniqueRecords": unique_sound_counts,
            "mapUniqueBytes": unique_sound_bytes,
        }
    else:
        if source_runtime_profile:
            raise kvap.KvapError("source runtime profile requires independent typed map inputs")
        if (environment_source is not None or world_semantic_source is not None
                or model_semantic_source is not None
                or shipment_model_lods is not None
                or crossfire_model_lods is not None):
            raise kvap.KvapError(
                "supplemental archives are only valid with independent typed map inputs"
            )
        if source is None:
            raise kvap.KvapError(
                "provide --shipment-source/--crossfire-source or legacy --source"
            )
        source = source.resolve()
        if not source.is_file():
            raise kvap.KvapError(f"combined package does not exist: {source}")
        source_hash, chunks = read_source_index(source)
        source_inputs["legacyCombined"] = source_hash.hex()
        common_chunks = [
            SelectedChunk(source, chunk, chunk.asset_id)
            for chunk in chunks
            if chunk.kind in COMMON_TYPES
            or chunk.asset_id == COMMON_MODEL_COLOR_ASSET
        ]
        map_chunks = {
            index: [
                SelectedChunk(source, chunk, chunk.asset_id)
                for chunk in chunks
                if map_chunk_index(chunk.asset_id) == index
            ]
            for index in MAPS
        }

    common_types = {selected.chunk.kind for selected in common_chunks}
    missing_common = COMMON_TYPES - common_types
    if missing_common:
        raise kvap.KvapError(
            f"source package is missing common chunk types: {sorted(missing_common)}"
        )
    for map_index, selected in map_chunks.items():
        families = {
            item.asset_id & 0xFFFFFFFFFFFFFF00
            for item in selected
            if (item.asset_id & 0xFFFFFFFFFFFFFF00) in SIMPLE_MAP_FAMILIES
        }
        missing = REQUIRED_SIMPLE_FAMILIES - families
        if missing:
            raise kvap.KvapError(
                f"{MAPS[map_index]} is missing required families: "
                + ", ".join(f"0x{value:016x}" for value in sorted(missing))
            )

    output_reports: dict[str, object] = {}
    # Bind the exact input packages and preparation reports, not just their
    # historical source-install header values, into the pack-set identity.
    source_hash = hashlib.sha256(source_hash + json.dumps(source_inputs, sort_keys=True).encode()).digest()
    work_parent = workspace / "build" / "vita-pack-work"
    work_parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="vertical-slice-", dir=work_parent) as temporary:
        work = Path(temporary)
        common_entries: list[dict[str, object]] = []
        common_cpu = 2 * 1024 * 1024  # registry/VM/frontend arena allowance
        if texture_paging:
            common_cpu += TEXTURE_PAGE_CPU_RESERVE
        common_gpu = 0
        common_texture_identities = set()
        common_image_reports = []
        for selected in common_chunks:
            chunk = selected.chunk
            raw = read_selected_chunk(selected)
            if chunk.kind == kvap.CHUNK_TYPES["image"] and raw[:4] == b"VITI":
                raw, image_report = trim_viti(raw)
                common_image_reports.append({"assetId": f"0x{selected.asset_id:016x}", **image_report})
            extracted = work / (
                f"common-{chunk.index:03d}-{selected.asset_id:016x}.bin"
            )
            extracted.write_bytes(raw)
            common_entries.append(
                chunk_entry(chunk.kind, selected.asset_id, extracted)
            )
            if chunk.kind == kvap.CHUNK_TYPES["sound"]:
                common_cpu += retained_sound_bytes(raw, source_audio)
            else:
                common_cpu += len(raw)
            if chunk.kind == kvap.CHUNK_TYPES["image"]:
                if texture_paging:
                    fixed, pageable, _, _ = texture_residency(raw, common_texture_identities)
                    common_gpu += fixed + pageable
                else:
                    common_gpu += len(raw)
        common_manifest_payload = {
            "schemaVersion": schema_version,
            **residency_contract,
            "packageRole": "common",
            "sourceUiDependencyHashes": {name: hashlib.sha256(json.dumps(report, sort_keys=True).encode()).hexdigest()
                                        for name, report in source_ui_reports.items()},
            "certified": True,
            "playable": True,
            "payloadPolicy": "runtime-ready-v1",
            "sourceInstallHash": source_hash.hex(),
            "sourceInputs": source_inputs,
            "predictedCpuResidentBytes": common_cpu,
            "predictedGpuResidentBytes": common_gpu,
            "imageMipReports": common_image_reports,
        }
        common_manifest_file = work / "common-content-manifest.json"
        common_manifest_file.write_text(
            embedded_content_manifest(common_manifest_payload), encoding="utf-8"
        )
        common_entries.append(chunk_entry(kvap.CHUNK_TYPES["manifest"],
                                          kvap._asset_id("vita.vertical-slice.common"),
                                          common_manifest_file))
        common_spec = {
            "sourceHash": source_hash.hex(),
            "chunks": common_entries,
        }
        common_report = write_pack(common_spec, work / "common-package.json",
                                   output / "common.kvap", workspace)
        common_package_id = int(str(common_report["packageId"]), 16)
        common_sha = sha256_file(output / "common.kvap")
        output_reports["common"] = common_report

        exclusive_common = {(item.chunk.kind,item.asset_id):len(read_selected_chunk(item))
            for item in common_chunks if item.chunk.kind in (kvap.CHUNK_TYPES['animation'],kvap.CHUNK_TYPES['weapon'])}
        budget_errors=[]
        for map_index, map_name in MAPS.items():
            shadowed_common_bytes=0
            entries: list[dict[str, object]] = []
            map_cpu = 4 * 1024 * 1024  # map/transient arena metadata allowance
            # Four MiB VitaGL scratch, the native RGBA8/depth scene target,
            # and four MiB of conservative backend allocator headroom.
            map_gpu = 12 * 1024 * 1024
            pageable_bytes = backing_bytes = backing_count = 0
            texture_identities = set(common_texture_identities)
            image_report: list[dict[str, int]] = []
            gpu_components=[]
            for selected in map_chunks[map_index]:
                before_gpu=map_gpu
                chunk = selected.chunk
                raw = read_selected_chunk(selected)
                if chunk.kind == kvap.CHUNK_TYPES["image"]:
                    segmented_family = (
                        selected.asset_id & 0xFFFFFFFFFFFF0000
                    )
                    maximum_top_mip = (
                        WORLD_SEMANTIC_TOP_MIP_BY_MAP[map_index]
                        if segmented_family in WORLD_SEMANTIC_FAMILIES
                        else (MODEL_SEMANTIC_TOP_MIP_BY_MAP[map_index]
                              if segmented_family in MODEL_SEMANTIC_FAMILIES
                              else None)
                    )
                    protected_names = (
                        {
                            name for name in viti_names(raw)
                            if is_vertical_slice_viewmodel_semantic(name)
                        }
                        if segmented_family in MODEL_SEMANTIC_FAMILIES
                        else None
                    )
                    raw, trim_report = trim_viti(
                        raw, maximum_top_mip, protected_names
                    )
                    trim_report["assetId"] = selected.asset_id
                    image_report.append(trim_report)
                    simple_family = (
                        selected.asset_id & 0xFFFFFFFFFFFFFF00
                    )
                    if texture_paging:
                        fixed, pageable, extents, count = texture_residency(raw, texture_identities)
                        map_gpu += fixed
                        pageable_bytes += pageable
                        backing_bytes += extents
                        backing_count += count
                    elif simple_family == 0x4D49000000000000:
                        map_gpu += min(
                            trim_report["outputPayloadBytes"],
                            NATIVE_SELECTED_IMAGE_GPU_BUDGET,
                        )
                    else:
                        map_gpu += trim_report["outputPayloadBytes"]
                elif (selected.asset_id & 0xFFFFFFFFFFFFFF00) == 0x4D50000000000000:
                    map_gpu += world_gpu_bytes(raw)
                elif chunk.kind == kvap.CHUNK_TYPES["sound"]:
                    map_cpu += retained_sound_bytes(raw, source_audio)
                else:
                    # World/image payloads are released after immutable GPU
                    # upload. Remaining typed bundles live in the map arena.
                    # Animation/weapon readers choose the primary map chunk
                    # and return; the common fallback is not another resident
                    # bundle. Keep common charged and charge only the excess.
                    covered=exclusive_common.get((chunk.kind,selected.asset_id),0)
                    shadowed_common_bytes+=min(len(raw),covered)
                    map_cpu += max(0,len(raw)-covered)
                gpu_components.append({"assetId":f"0x{selected.asset_id:016x}","type":chunk.kind,"bytes":map_gpu-before_gpu})
                extracted = work / (
                    f"{map_name}-{chunk.index:03d}-{selected.asset_id:016x}.bin"
                )
                extracted.write_bytes(raw)
                entries.append(
                    chunk_entry(chunk.kind, selected.asset_id, extracted)
                )
            if texture_paging:
                map_gpu += min(pageable_bytes, PAGED_WORLD_GPU_BUDGET)
            (output/f'{map_name}-residency.json').write_text(json.dumps(dict(commonCpu=common_cpu,mapCpu=map_cpu,commonGpu=common_gpu,mapGpu=map_gpu,gpuComponents=gpu_components,images=image_report,pageableBytes=pageable_bytes,backingBytes=backing_bytes,backingCount=backing_count,**residency_contract),indent=2))
            map_errors=[]
            if texture_paging and (backing_bytes > 512*1024*1024 or backing_count > 2048):
                map_errors.append(f'{map_name} texture backing exceeds bounded disk/slot capacity')
            if common_cpu + map_cpu > CPU_BUDGET:
                map_errors.append(
                    f"{map_name} predicted CPU residency exceeds 114 MiB: "
                    f"{common_cpu + map_cpu} bytes"
                )
            if common_gpu + map_gpu > GPU_BUDGET:
                map_errors.append(
                    f"{map_name} predicted GPU residency exceeds 64 MiB: "
                    f"{common_gpu + map_gpu} bytes"
                )
            if common_cpu + map_cpu + common_gpu + map_gpu > WORKING_BUDGET:
                map_errors.append(f"{map_name} exceeds the 178 MiB working envelope")
            if map_errors:
                budget_errors.extend(map_errors)
                continue
            map_manifest_payload = {
                "schemaVersion": schema_version,
                **residency_contract,
                "packageRole": "map",
                "certified": True,
                "playable": True,
                "payloadPolicy": "runtime-ready-v1",
                "sourceInstallHash": source_hash.hex(),
                "sourceInputs": source_inputs,
                "mapSourceHash": source_inputs.get(f"mp_{map_name}",
                                                    source_hash.hex()),
                "mapIndex": map_index,
                "mapId": map_name,
                "commonPackageId": f"0x{common_package_id:016x}",
                "commonSha256": common_sha,
                "predictedCpuResidentBytes": map_cpu,
                "exclusiveCommonBytesAlreadyCharged": shadowed_common_bytes,
                "predictedGpuResidentBytes": map_gpu,
                "imagePolicy": {
                    "uiViewmodelLightmapMax": 1024,
                    "worldMax": 512,
                    "lowSalienceMax": 256,
                    "worldSemanticMax": WORLD_SEMANTIC_TOP_MIP_BY_MAP[map_index],
                    "modelSemanticMax": MODEL_SEMANTIC_TOP_MIP_BY_MAP[map_index],
                    "fixedViewmodelSemanticMax": 1024,
                },
                "imageReports": image_report,
            }
            manifest_file = work / f"{map_name}-content-manifest.json"
            manifest_file.write_text(
                embedded_content_manifest(map_manifest_payload), encoding="utf-8"
            )
            entries.append(chunk_entry(kvap.CHUNK_TYPES["manifest"],
                                       kvap._asset_id(f"vita.vertical-slice.{map_name}"),
                                       manifest_file))
            package_spec = {"sourceHash": source_hash.hex(), "chunks": entries}
            package_name = f"mp_{map_name}.kvap"
            output_reports[map_name] = write_pack(
                package_spec, work / f"{map_name}-package.json",
                output / package_name, workspace
            )
        if budget_errors:raise kvap.KvapError('; '.join(budget_errors))

    media: dict[str, str] = {}
    media_dir = output / "media"
    for source_media, destination_name in (
        (menu_video, "menu_bg.mp4"), (menu_music, "menu_music.at9"),
    ):
        if source_media:
            source_media = source_media.resolve()
            if not source_media.is_file():
                raise kvap.KvapError(f"menu media does not exist: {source_media}")
            media_dir.mkdir(parents=True, exist_ok=True)
            destination = media_dir / destination_name
            shutil.copyfile(source_media, destination)
            media[destination_name] = sha256_file(destination)

    checksums = {
        path.name: {"sha256": sha256_file(path), "bytes": path.stat().st_size}
        for path in (output/name for name in ['common.kvap']+[f'mp_{name}.kvap' for name in MAPS.values()])
    }
    for relative, (source_stream, expected) in source_streams.items():
        destination = output / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source_stream, destination)
    result = {
        "schemaVersion": schema_version,
        **residency_contract,
        "selectedMaps": [{"index":index,"name":name} for index,name in MAPS.items()],
        "sourceInstallHash": source_hash.hex(),
        "sourceInputs": source_inputs,
        "soundSplit": sound_split_report,
        "modelLodColorRepairs": lod_color_repairs,
        "sourceSoundClosure": source_audio,
        "sourceRuntimeProfile": runtime_profile_report,
        "sourceEffectDependencies": source_effect_reports,
        "sourceUiDependencies": source_ui_reports,
        "streams": {relative: expected for relative, (_, expected) in source_streams.items()},
        "packages": checksums,
        "media": media,
        "reports": output_reports,
    }
    (output / "vertical-slice-manifest.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8"
    )
    return result


def build(*args, **kwargs) -> dict[str, object]:
    """Publish new map sets only after every selected pack passes its gates.

    Legacy recipes retain their existing output behavior. Expansion deliveries
    require a new directory, so a failed build cannot invalidate an installed set.
    Failed expansion work remains explicitly labelled for memory/content diagnosis.
    """
    import inspect
    call=inspect.signature(_build).bind(*args,**kwargs)
    call.apply_defaults()
    if call.arguments['map_inputs'] is None:
        return _build(*args,**kwargs)
    output=call.arguments['output'].resolve()
    workspace=call.arguments['workspace'].resolve()
    if not output.is_relative_to(workspace) or output==workspace:
        raise kvap.KvapError('output must be a new directory inside the workspace')
    if output.exists():raise kvap.KvapError('Map-set output already exists; choose a new versioned directory')
    output.parent.mkdir(parents=True,exist_ok=True)
    pending=Path(tempfile.mkdtemp(prefix=output.name+'.pending-',dir=output.parent))
    call.arguments['output']=pending
    try:
        result=_build(*call.args,**call.kwargs)
        for report in result.get('reports',{}).values():
            if 'output' in report:report['output']=str(output/Path(report['output']).name)
        (pending/'vertical-slice-manifest.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
        pending.rename(output)
        return result
    except Exception as error:
        # The published name is never created on failure. Preserve diagnostics,
        # but remove any success marker left by a later publication failure.
        manifest=pending/'vertical-slice-manifest.json'
        if manifest.exists():manifest.unlink()
        (pending/'FAILED.txt').write_text(str(error)+'\n',encoding='utf-8')
        pending.rename(pending.with_name(pending.name.replace('.pending-','.failed-',1)))
        raise


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--map-inputs", type=Path, help="JSON mapping original map names to source and soundClosure paths")
    inputs.add_argument("--source", type=Path,
                        help="legacy locally generated all-map KVAP")
    inputs.add_argument("--shipment-source", type=Path,
                        help="locally generated typed Shipment KVAP")
    parser.add_argument("--crossfire-source", type=Path,
                        help="locally generated typed Crossfire KVAP")
    parser.add_argument(
        "--environment-source", type=Path,
        help=("locally generated multi-map archive providing exact VENV "
              "records when older typed map exports do not contain them"),
    )
    parser.add_argument(
        "--world-semantic-source", type=Path,
        help=("locally generated archive providing the authored segmented "
              "world normal/specular VITI families"),
    )
    parser.add_argument(
        "--model-semantic-source", type=Path,
        help=("locally generated archive providing the authored, map-scoped "
              "XModel/viewmodel normal/specular VITI families"),
    )
    parser.add_argument(
        "--shipment-model-lods", type=Path,
        help="raw pointer-free Shipment VLOD sidecar from the owned install",
    )
    parser.add_argument(
        "--crossfire-model-lods", type=Path,
        help="raw pointer-free Crossfire VLOD sidecar from the owned install",
    )
    parser.add_argument("--output", type=Path, required=True,
                        help="workspace output directory")
    parser.add_argument("--workspace", type=Path,
                        default=Path(__file__).resolve().parents[3])
    parser.add_argument("--menu-video", type=Path)
    parser.add_argument("--menu-music", type=Path)
    parser.add_argument("--shipment-sound-closure", type=Path)
    parser.add_argument("--crossfire-sound-closure", type=Path)
    parser.add_argument("--source-runtime-profile", action="store_true",
                        help="Apply and hash the offline fixed-loadout source script/data configuration")
    parser.add_argument("--texture-paging", action="store_true",
                        help="Require schema-2 bounded world texture residency")
    parser.add_argument("--retail-root", type=Path,
                        help="Read-only owned COD4 install for exact missing retained-LOD colors")
    args = parser.parse_args(list(argv) if argv is not None else None)
    try:
        print(json.dumps(build(args.source, args.output, args.workspace,
                               args.menu_video, args.menu_music,
                               args.shipment_source,
                               args.crossfire_source,
                               args.environment_source,
                               args.world_semantic_source,
                               args.model_semantic_source,
                               args.shipment_model_lods,
                               args.crossfire_model_lods,
                               args.shipment_sound_closure,
                               args.crossfire_sound_closure,
                               args.source_runtime_profile,
                               args.retail_root,
                               load_inputs(args.map_inputs) if args.map_inputs else None,
                               args.texture_paging), indent=2))
        return 0
    except (OSError, UnicodeError, ValueError, kvap.KvapError) as error:
        print(f"vertical-slice-packs: {error}")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
