#!/usr/bin/env python3
"""Convert a Windows DB export into a deterministic Vita package.

The host exporter emits native asset names plus pointer-free world/collision,
material/image/font, rawfile, menu, weapon, sound, and frontend sidecars. Names
remain typed JSON records, while geometry and bounded runtime payloads are
validated before being included as binary KVAP chunks. Model geometry and the
VITQ v3 compressed XAnimParts plus sampled tracks are portable; the Vita reader interpolates those
pointer-free per-model transforms. The runtime-ready gate additionally requires
the bounded HUD binding table and every payload class consumed by the current
Shipment bootstrap.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import math
import json
import re
import struct
import sys
import zipfile
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kvap import create_package  # noqa: E402
import vita_effects  # noqa: E402
from source_ui_closure import source_ui_dependencies  # noqa: E402


TYPE_TO_CHUNK = {
    "col_map_mp": "collision",
    "game_map_mp": "world",
    "com_map": "world",
    "gfx_map": "world",
    "map_ents": "world",
    "material": "material",
    "techset": "material",
    "lightdef": "material",
    "image": "image",
    "xmodel": "model",
    "xanim": "animation",
    "weapon": "weapon",
    "sound": "sound",
    "loaded_sound": "sound",
    "sndcurve": "sound",
    "menu": "menu",
    "menufile": "menu",
    "font": "font",
    "rawfile": "script",
    "fx": "script",
    "impactfx": "script",
    "physpreset": "collision",
    "localize": "strings",
    "stringtable": "strings",
}

# Keep the startup image stream within the known-stable VitaImageBundle
# envelope. The scalar path releases compressed payloads after staging, and
# reflection-probe cubemaps remain in a separate per-map VITI chunk. Larger
# source-semantic closures are streamed as companion chunks; putting them back
# into startup reproduces a Vita3K heap/UDF while loading the image bundle.
IMAGE_EXPORT_MAX_RECORDS = 512
IMAGE_EXPORT_MAX_DATA_BYTES = 64 * 1024 * 1024
# Companion semantic streams are read after the startup image payload has
# been staged and released. Keep each stream under the same hard VITI parser
# bound while allowing the complete authored normal/specular closure.
SEMANTIC_EXPORT_MAX_RECORDS = 2048
# Reserve enough room for up to 2048 fixed record/name headers as well as the
# VITI header, so every split segment stays below the runtime's raw 96 MiB
# bound rather than only satisfying its declared payload bound.
SEMANTIC_EXPORT_MAX_DATA_BYTES = 96 * 1024 * 1024 - 256 * 1024


def _write_json(path: Path, document: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _read_rows(source: Path) -> list[dict[str, str]]:
    with source.open("r", encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != ["type", "typeName", "name"]:
            raise ValueError("DB export must have type,typeName,name columns")
        rows: list[dict[str, str]] = []
        seen: set[tuple[str, str]] = set()
        for row in reader:
            type_name = (row.get("typeName") or "").strip()
            name = (row.get("name") or "").strip()
            if not type_name or not name:
                continue
            key = (type_name, name)
            if key in seen:
                continue
            seen.add(key)
            rows.append({
                "type": (row.get("type") or "").strip(),
                "typeName": type_name,
                "name": name,
            })
    rows.sort(key=lambda row: (row["typeName"], row["name"]))
    return rows


def _validate_geometry(path: Path, kind: str) -> dict[str, Any] | None:
    """Validate an engine-side pointer-free geometry sidecar.

    The exporter writes fixed little-endian records. Checking the exact byte
    size here prevents a truncated host run from becoming a seemingly valid
    package chunk.
    """
    if not path.is_file() or path.stat().st_size == 0:
        return None
    data = path.read_bytes()
    import struct

    if kind == "world":
        if len(data) < 44:
            raise ValueError(f"world sidecar is truncated: {path}")
        magic, version, vertex_count, index_count, surface_count = struct.unpack_from("<5I", data)
        if magic != 0x57544956 or version not in (1, 2, 3):
            raise ValueError(f"unsupported world sidecar header: {path}")
        header_bytes = 44 if version == 1 else 120
        surface_bytes = 92 if version == 3 else 88
        if len(data) < header_bytes:
            raise ValueError(f"world sidecar lighting header is truncated: {path}")
        expected = (header_bytes + vertex_count * 40 + index_count * 2
                    + surface_count * surface_bytes)
    elif kind == "lightgrid":
        if len(data) < 52:
            raise ValueError(f"light-grid sidecar is truncated: {path}")
        (magic, version, _flags, _sun_primary, row_axis, col_axis,
         min_x, min_y, min_z, max_x, max_y, max_z, row_count,
         raw_row_bytes, entry_count, color_count) = struct.unpack_from(
             "<6I6H4I", data
         )
        if (magic != 0x47544956 or version != 1 or row_axis > 2
                or col_axis > 2 or row_axis == col_axis or row_axis > 1
                or col_axis > 1 or row_count > 0x2000
                or raw_row_bytes > 0x300000 or entry_count > 1_000_000
                or color_count > 65_536):
            raise ValueError(f"unsupported light-grid sidecar header: {path}")
        # The row count is explicitly serialized because empty/partially
        # initialized grids are valid diagnostic payloads.  Validate the
        # source bounds independently when a non-empty row stream is present.
        if row_count and (max_x < min_x or max_y < min_y or max_z < min_z):
            raise ValueError(f"light-grid bounds are inverted: {path}")
        expected = (52 + row_count * 2 + raw_row_bytes + entry_count * 4
                    + color_count * 168)
    elif kind == "collision":
        if len(data) < 32:
            raise ValueError(f"collision sidecar is truncated: {path}")
        magic, version, plane_count, brush_count, side_count, vertex_count, tri_count, submodel_count = struct.unpack_from("<8I", data)
        if magic != 0x43544956 or version not in (2, 3):
            raise ValueError(f"unsupported collision sidecar header: {path}")
        legacy_bytes = (32 + plane_count * 18 + brush_count * 36
                        + side_count * 24 + vertex_count * 12
                        + tri_count * 3 * 2 + submodel_count * 28)
        expected = legacy_bytes
        if version == 3:
            tail = legacy_bytes + side_count * 4
            if tail + 8 + (submodel_count + 1) * 4 > len(data):
                raise ValueError(f"collision v3 source-closure tail is truncated: {path}")
            model_count, membership_count = struct.unpack_from("<2I", data, tail)
            if model_count != submodel_count or membership_count > 2_000_000:
                raise ValueError(f"collision v3 model membership is invalid: {path}")
            offsets = struct.unpack_from(
                f"<{submodel_count + 1}I", data, tail + 8
            )
            if (not offsets or offsets[0] != 0
                    or offsets[-1] != membership_count
                    or any(right < left for left, right in zip(offsets, offsets[1:]))):
                raise ValueError(f"collision v3 model offsets are invalid: {path}")
            membership_offset = tail + 8 + (submodel_count + 1) * 4
            brush_indices = struct.unpack_from(
                f"<{membership_count}H", data, membership_offset
            ) if membership_count else ()
            for model in range(submodel_count):
                selected = brush_indices[offsets[model]:offsets[model + 1]]
                if (any(index >= brush_count for index in selected)
                        or any(right <= left
                               for left, right in zip(selected, selected[1:]))):
                    raise ValueError(
                        f"collision v3 brush membership is invalid: {path}"
                    )
            expected = membership_offset + membership_count * 2
    else:
        raise ValueError(f"unknown geometry kind: {kind}")
    if expected != len(data):
        raise ValueError(f"{kind} sidecar size mismatch: expected {expected}, got {len(data)}")
    return {"path": path, "bytes": len(data)}


def _validate_primary_light_sidecar(path: Path) -> dict[str, Any] | None:
    """Validate the pointer-free VITP ComPrimaryLight stream."""
    if not path.is_file() or path.stat().st_size == 0:
        return None
    data = path.read_bytes()
    if len(data) < 20:
        raise ValueError(f"primary-light sidecar is truncated: {path}")
    magic, version, record_count, record_bytes, sun_index = struct.unpack_from(
        "<5I", data
    )
    if (magic != 0x50544956 or version != 1
            or record_bytes != 64
            or record_count > 128
            or (record_count == 0 and sun_index != 0)
            or (record_count != 0 and sun_index >= record_count)):
        raise ValueError(f"unsupported primary-light sidecar header: {path}")
    expected = 20 + record_count * record_bytes
    if expected != len(data):
        raise ValueError(
            f"primary-light sidecar size mismatch: expected {expected}, got {len(data)}"
        )
    offset = 20
    for _ in range(record_count):
        values = struct.unpack_from("<4B15f", data, offset)
        light_type = values[0]
        if light_type > 6 or not all(math.isfinite(value) for value in values[4:]):
            raise ValueError(f"primary-light record is invalid: {path}")
        offset += record_bytes
    return {"path": path, "bytes": len(data), "records": record_count,
            "sunIndex": sun_index, "recordBytes": record_bytes}


def _synthesize_primary_light_sidecar(world_path: Path,
                                     output_path: Path) -> dict[str, Any] | None:
    """Recover the authored sun from a VITW v2 dump made before VITP existed.

    This is only a compatibility bridge for old oracle directories. A fresh
    Windows DB dump writes every ComPrimaryLight directly; the bridge never
    invents local lights or map-specific colour constants.
    """
    if not world_path.is_file() or output_path.is_file():
        return _validate_primary_light_sidecar(output_path)
    data = world_path.read_bytes()
    if len(data) < 120:
        return None
    magic, version, _vertex_count, _index_count, _surface_count = struct.unpack_from(
        "<5I", data
    )
    if magic != 0x57544956 or version != 2:
        return None
    lighting = struct.unpack_from("<15fI3f", data, 44)
    sun_direction = lighting[0:3]
    sun_color = lighting[3:6]
    if not all(math.isfinite(value) for value in (*sun_direction, *sun_color)):
        return None
    record = struct.pack(
        "<4B15f",
        1,  # GFX_LIGHT_TYPE_DIR
        1,  # authored sun participates in the shadow path
        0,
        0,
        *sun_color,
        *sun_direction,
        0.0, 0.0, 0.0,  # origin (unused by a directional light)
        0.0,             # radius (unused by a directional light)
        0.0, 0.0, 0.0,   # spot cone values (unused)
        0.0, 0.0,        # rotation/translation limits
    )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(struct.pack("<5I", 0x50544956, 1, 1, 64, 0) + record)
    result = _validate_primary_light_sidecar(output_path)
    if result is not None:
        result["compatibilitySource"] = "VITW v2 authored sun"
    return result


def _validate_entity_sidecar(path: Path) -> dict[str, Any] | None:
    if not path.is_file() or path.stat().st_size == 0:
        return None
    import struct

    data = path.read_bytes()
    if len(data) < 12:
        raise ValueError(f"map-entity sidecar is truncated: {path}")
    magic, version, entity_bytes = struct.unpack_from("<3I", data)
    if magic != 0x45544956 or version != 1:
        raise ValueError(f"unsupported map-entity sidecar header: {path}")
    if 12 + entity_bytes != len(data):
        raise ValueError(f"map-entity sidecar size mismatch: {path}")
    return {"path": path, "bytes": len(data)}


def _validate_weapon_sidecar(path: Path) -> dict[str, Any] | None:
    if not path.is_file() or path.stat().st_size == 0:
        return None
    import struct

    data = path.read_bytes()
    if len(data) < 16:
        raise ValueError(f"weapon sidecar is truncated: {path}")
    magic, version, record_count, record_bytes = struct.unpack_from("<4I", data)
    supported_layouts = {
        (1, 160), (2, 244), (3, 2356), (4, 2360), (5, 15376)
    }
    if magic != 0x50544956 or (version, record_bytes) not in supported_layouts:
        raise ValueError(f"unsupported weapon sidecar header: {path}")
    if 16 + record_count * record_bytes != len(data):
        raise ValueError(f"weapon sidecar size mismatch: {path}")
    return {"path": path, "bytes": len(data), "records": record_count,
            "version": version, "recordBytes": record_bytes}


def _validate_rawfile_sidecar(path: Path) -> dict[str, Any] | None:
    """Validate the pointer-free VITR rawfile bundle from the Windows oracle."""
    if not path.is_file() or path.stat().st_size == 0:
        return None
    import struct

    data = path.read_bytes()
    if len(data) < 20:
        raise ValueError(f"rawfile sidecar is truncated: {path}")
    magic, version, record_count, data_bytes = struct.unpack_from("<3IQ", data)
    if magic != 0x52544956 or version != 1:
        raise ValueError(f"unsupported rawfile sidecar header: {path}")
    offset = 20
    total_data = 0
    for _ in range(record_count):
        if offset + 8 > len(data):
            raise ValueError(f"rawfile sidecar record header is truncated: {path}")
        name_bytes, payload_bytes = struct.unpack_from("<2I", data, offset)
        offset += 8
        if name_bytes == 0 or name_bytes > 4096 or offset + name_bytes + payload_bytes > len(data):
            raise ValueError(f"rawfile sidecar record is invalid: {path}")
        offset += name_bytes + payload_bytes
        total_data += payload_bytes
    if offset != len(data) or total_data != data_bytes:
        raise ValueError(f"rawfile sidecar size mismatch: {path}")
    return {"path": path, "bytes": len(data), "records": record_count,
            "dataBytes": data_bytes}


def _validate_impact_fx_sidecar(path: Path) -> dict[str, Any] | None:
    """Validate the pointer-free VIFX fastfile impact table.

    COD4 fixes this table at 12 impact material classes, each with 29
    non-flesh and four flesh effect slots.  Every slot must name a real
    FxEffectDef and the canonical empty-name table must be present.
    """
    if not path.is_file():
        return None
    data = path.read_bytes()
    if len(data) < 32:
        return None
    (magic, version, table_count, entry_count, non_flesh_count,
     flesh_count, string_bytes, reserved) = struct.unpack_from("<8I", data)
    if (magic != 0x58464956 or version != 1 or table_count == 0
            or table_count > 4 or entry_count != 12
            or non_flesh_count != 29 or flesh_count != 4
            or string_bytes == 0 or string_bytes > 1024 * 1024
            or reserved != 0):
        return None
    slots_per_table = entry_count * (non_flesh_count + flesh_count)
    table_base = 32
    slot_base = table_base + table_count * 8
    string_base = slot_base + table_count * slots_per_table * 4
    if string_base + string_bytes != len(data) or data[string_base] != 0:
        return None

    def read_string(offset: int, allow_empty: bool) -> str | None:
        if offset >= string_bytes:
            return None
        end = data.find(b"\0", string_base + offset,
                        string_base + string_bytes)
        if end < 0 or end - (string_base + offset) > 1024:
            return None
        raw = data[string_base + offset:end]
        if not allow_empty and not raw:
            return None
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError:
            return None

    table_names: list[str] = []
    effect_names: set[str] = set()
    for table_index in range(table_count):
        name_offset, first_slot = struct.unpack_from(
            "<2I", data, table_base + table_index * 8)
        name = read_string(name_offset, True)
        if (name is None or name in table_names
                or first_slot != table_index * slots_per_table):
            return None
        table_names.append(name)
        for slot_index in range(slots_per_table):
            (effect_offset,) = struct.unpack_from(
                "<I", data, slot_base + (first_slot + slot_index) * 4)
            effect_name = read_string(effect_offset, True)
            if effect_name is None:
                return None
            if effect_name:
                effect_names.add(effect_name)
    if "" not in table_names:
        return None
    return {
        "path": path,
        "bytes": len(data),
        "version": version,
        "tables": table_count,
        "slots": table_count * slots_per_table,
        "uniqueEffects": len(effect_names),
        "effectNames": sorted(effect_names),
    }


def _validate_localize_sidecar(path: Path) -> dict[str, Any] | None:
    """Validate the pointer-free VITZ localized key/value stream."""
    if not path.is_file() or path.stat().st_size == 0:
        return None
    data = path.read_bytes()
    if len(data) < 20:
        raise ValueError(f"localized sidecar is truncated: {path}")
    magic, version, record_count, data_bytes = struct.unpack_from("<3IQ", data)
    if magic != 0x5A544956 or version != 1:
        raise ValueError(f"unsupported localized sidecar header: {path}")
    if record_count > 32768 or data_bytes > 8 * 1024 * 1024:
        raise ValueError(f"localized sidecar bounds are invalid: {path}")
    offset = 20
    aggregate = 0
    for _ in range(record_count):
        if offset + 8 > len(data):
            raise ValueError(f"localized sidecar record header is truncated: {path}")
        name_bytes, value_bytes = struct.unpack_from("<2I", data, offset)
        if name_bytes == 0 or name_bytes > 4096 or value_bytes > 4096:
            raise ValueError(f"localized sidecar record is invalid: {path}")
        offset += 8
        if offset + name_bytes + value_bytes > len(data):
            raise ValueError(f"localized sidecar strings are truncated: {path}")
        offset += name_bytes + value_bytes
        aggregate += name_bytes + value_bytes + 8
    if offset != len(data) or aggregate != data_bytes:
        raise ValueError(f"localized sidecar size mismatch: {path}")
    return {"path": path, "bytes": len(data), "records": record_count,
            "dataBytes": data_bytes}


def _validate_stringtable_sidecar(path: Path) -> dict[str, Any] | None:
    """Validate the pointer-free VITT StringTable cell stream."""
    if not path.is_file() or path.stat().st_size == 0:
        return None
    data = path.read_bytes()
    if len(data) < 20:
        raise ValueError(f"string-table sidecar is truncated: {path}")
    magic, version, record_count, data_bytes = struct.unpack_from("<3IQ", data)
    if magic != 0x54544956 or version != 1:
        raise ValueError(f"unsupported string-table sidecar header: {path}")
    if record_count > 32768 or data_bytes > 16 * 1024 * 1024:
        raise ValueError(f"string-table sidecar bounds are invalid: {path}")
    offset = 20
    aggregate = 0
    for _ in range(record_count):
        if offset + 12 > len(data):
            raise ValueError(f"string-table record header is truncated: {path}")
        name_bytes, row_count, column_count = struct.unpack_from(
            "<3I", data, offset
        )
        offset += 12
        if (name_bytes == 0 or name_bytes > 4096 or row_count > 4096
                or column_count > 4096
                or row_count * column_count > 16 * 1024 * 1024):
            raise ValueError(f"string-table record is invalid: {path}")
        if offset + name_bytes > len(data):
            raise ValueError(f"string-table name is truncated: {path}")
        offset += name_bytes
        for _ in range(row_count * column_count):
            if offset + 4 > len(data):
                raise ValueError(f"string-table cell header is truncated: {path}")
            cell_bytes = struct.unpack_from("<I", data, offset)[0]
            offset += 4
            if cell_bytes > 4096 or offset + cell_bytes > len(data):
                raise ValueError(f"string-table cell is invalid: {path}")
            offset += cell_bytes
        aggregate = offset - 20
        if aggregate > 16 * 1024 * 1024:
            raise ValueError(f"string-table sidecar is too large: {path}")
    if offset != len(data) or aggregate != data_bytes:
        raise ValueError(f"string-table sidecar size mismatch: {path}")
    return {"path": path, "bytes": len(data), "records": record_count,
            "dataBytes": data_bytes}


def _validate_menu_sidecar(path: Path) -> dict[str, Any] | None:
    """Validate the pointer-free VITM menu/layout stream from the oracle."""
    if not path.is_file() or path.stat().st_size == 0:
        return None
    import struct

    data = path.read_bytes()
    if len(data) < 20:
        raise ValueError(f"menu sidecar is truncated: {path}")
    magic, version, menu_count, data_bytes = struct.unpack_from("<3IQ", data)
    if magic != 0x4D544956 or version != 1:
        raise ValueError(f"unsupported menu sidecar header: {path}")
    offset = 20
    aggregate = 0
    for _ in range(menu_count):
        if offset + 100 > len(data):
            raise ValueError(f"menu sidecar record header is truncated: {path}")
        name_bytes, font_bytes, item_count = struct.unpack_from("<3I", data, offset)
        if name_bytes > 4096 or font_bytes > 4096 or item_count > 256:
            raise ValueError(f"menu sidecar record is invalid: {path}")
        offset += 100
        if offset + name_bytes + font_bytes > len(data):
            raise ValueError(f"menu sidecar strings are truncated: {path}")
        offset += name_bytes + font_bytes
        for _ in range(item_count):
            if offset + 68 > len(data):
                raise ValueError(f"menu sidecar item header is truncated: {path}")
            lengths = struct.unpack_from("<4I", data, offset)
            if any(length > 4096 for length in lengths):
                raise ValueError(f"menu sidecar item string is invalid: {path}")
            offset += 68
            item_bytes = sum(lengths)
            if offset + item_bytes > len(data):
                raise ValueError(f"menu sidecar item strings are truncated: {path}")
            offset += item_bytes
    aggregate = offset - 20
    if offset != len(data) or aggregate != data_bytes:
        raise ValueError(f"menu sidecar size mismatch: {path}")
    return {"path": path, "bytes": len(data), "records": menu_count,
            "dataBytes": data_bytes}


def _validate_sound_sidecar(path: Path) -> dict[str, Any] | None:
    """Validate the optional pointer-free VITS decoded-PCM stream."""
    if not path.is_file() or path.stat().st_size == 0:
        return None
    import struct

    data = path.read_bytes()
    if len(data) < 20:
        raise ValueError(f"sound sidecar is truncated: {path}")
    magic, version, record_count, data_bytes = struct.unpack_from("<3IQ", data)
    if magic != 0x53544956 or version != 1:
        raise ValueError(f"unsupported sound sidecar header: {path}")
    offset = 20
    aggregate = 0
    for _ in range(record_count):
        if offset + 28 > len(data):
            raise ValueError(f"sound sidecar record header is truncated: {path}")
        name_bytes, sample_bytes, rate, bits, channels, samples, fmt = struct.unpack_from(
            "<7I", data, offset
        )
        if name_bytes == 0 or name_bytes > 4096 or sample_bytes > 8 * 1024 * 1024:
            raise ValueError(f"sound sidecar record is invalid: {path}")
        if rate < 8000 or rate > 96000 or bits != 16 or channels not in (1, 2) or fmt != 1:
            raise ValueError(f"sound sidecar format is invalid: {path}")
        if samples == 0 or sample_bytes != samples * channels * 2:
            raise ValueError(f"sound sidecar sample count is invalid: {path}")
        offset += 28
        if offset + name_bytes + sample_bytes > len(data):
            raise ValueError(f"sound sidecar payload is truncated: {path}")
        offset += name_bytes + sample_bytes
        aggregate += sample_bytes
    if offset != len(data) or aggregate != data_bytes:
        raise ValueError(f"sound sidecar size mismatch: {path}")
    return {"path": path, "bytes": len(data), "records": record_count,
            "dataBytes": data_bytes}


def _validate_image_sidecar(path: Path) -> dict[str, Any] | None:
    """Validate the optional pointer-free VITI texture stream."""
    if not path.is_file() or path.stat().st_size == 0:
        return None
    import struct

    data = path.read_bytes()
    if len(data) < 20:
        raise ValueError(f"image sidecar is truncated: {path}")
    magic, version, record_count, data_bytes = struct.unpack_from("<3IQ", data)
    if magic != 0x49544956 or version != 1:
        raise ValueError(f"unsupported image sidecar header: {path}")
    offset = 20
    aggregate = 0
    for _ in range(record_count):
        if offset + 32 > len(data):
            raise ValueError(f"image sidecar record header is truncated: {path}")
        name_bytes, payload_bytes, width, height, depth, levels, fmt, flags = struct.unpack_from(
            "<8I", data, offset
        )
        if (name_bytes == 0 or name_bytes > 4096 or payload_bytes == 0
                or payload_bytes > 8 * 1024 * 1024 or width == 0 or width > 4096
                or height == 0 or height > 4096 or depth != 1 or levels == 0 or levels > 16):
            raise ValueError(f"image sidecar record is invalid: {path}")
        if flags & 0x4 and fmt == 21:
            # Generated reflection probes are uncompressed A8R8G8B8
            # cubemaps. VITI stores every mip on each face, matching
            # Image_GenerateCube's face-major upload order.
            face_bytes = sum(
                max(1, width >> level) * max(1, height >> level) * 4
                for level in range(levels)
            )
            if width != height or payload_bytes != face_bytes * 6:
                raise ValueError(f"image cubemap payload is invalid: {path}")
        offset += 32
        if offset + name_bytes + payload_bytes > len(data):
            raise ValueError(f"image sidecar payload is truncated: {path}")
        offset += name_bytes + payload_bytes
        aggregate += payload_bytes
    if offset != len(data) or aggregate != data_bytes:
        raise ValueError(f"image sidecar size mismatch: {path}")
    return {"path": path, "bytes": len(data), "records": record_count,
            "dataBytes": data_bytes}


def _image_names(path: Path) -> set[str]:
    """Read only the names from a validated VITI stream."""
    data = path.read_bytes()
    _, _, record_count, _ = struct.unpack_from("<3IQ", data)
    offset = 20
    names: set[str] = set()
    for _ in range(record_count):
        name_bytes, payload_bytes = struct.unpack_from("<2I", data, offset)
        offset += 32
        names.add(data[offset:offset + name_bytes].decode("utf-8", "strict"))
        offset += name_bytes + payload_bytes
    return names


def _build_hud_sidecar(image_path: Path | None,
                       map_name: str = "shipment") -> Path | None:
    """Emit the fixed HUD-to-image bindings used by the Vita overlay.

    The table contains no copyrighted pixels or native pointers; the image
    payload remains in the validated VITI chunk. Keeping this binding table
    explicit lets the package launch gate distinguish a complete HUD closure
    from a package that merely happens to contain arbitrary images.
    """
    if image_path is None or not image_path.is_file():
        return None
    _validate_image_sidecar(image_path)
    names = _image_names(image_path)
    bindings = [
        ("weapon_m4", "hud_m4carbine"),
        ("weapon_mp5", "hud_mp5"),
        ("weapon_usp", "hud_usp_45"),
        # cg_newDraw's low-health ownerdraw uses the full-screen authored
        # overlay. `damage_feedback` is the small hit-direction material and
        # must not be stretched over the whole framebuffer.
        ("low_health_overlay", "overlay_low_health"),
        ("compass", f"compass_map_mp_{map_name}"),
        ("compass_ticker", "minimap_tickertape_256x16"),
        ("compass_ticker_background", "minimap_tickertape_background"),
        ("muzzle_flash", "explosion_flash"),
        # The retail ammo counter glyphs are tiny, but important: they let the
        # runtime present the weapon's clip state with the same texture family
        # as the original HUD instead of relying on a geometry-only bar.
        # COD4's weaponinfo uses the compact 4x8 magazine glyph for the
        # stock M4/MP5 counters; the 32x8 short-magazine strip is reserved
        # for weapons whose authored ownerdraw explicitly selects it.
        ("ammo_rifle", "ammo_counter_bullet"),
        ("ammo_pistol", "ammo_counter_bullet"),
        ("scorebar_allies", "score_bar_allies"),
        ("scorebar_opfor", "score_bar_opfor"),
        # MP HUD ownerdraws registered by cg_main/cg_newdraw.  These names
        # are the exact IWI stems used by the retail materials (the material
        # names are hud_xpticker480/hud_dpad_arrow/hud_grenadeicon, while
        # their image records are hud_xpticker480/dpad_arrow/grenadeicon).
        ("xp_ticker", "hud_xpticker480"),
        ("xp_ticker_ws", "hud_xpticker480ws"),
        ("dpad_arrow", "dpad_arrow"),
        ("grenade_icon", "grenadeicon"),
        ("grenade_hud", "hud_us_grenade"),
        ("timer_background", "dtimer_bg"),
        ("timer_border", "dtimer_bg_border"),
        ("timer_0", "dtimer_0"),
        ("timer_1", "dtimer_1"),
        ("timer_2", "dtimer_2"),
        ("timer_3", "dtimer_3"),
        ("timer_4", "dtimer_4"),
        ("timer_5", "dtimer_5"),
        ("timer_6", "dtimer_6"),
        ("timer_7", "dtimer_7"),
        ("timer_8", "dtimer_8"),
        ("timer_9", "dtimer_9"),
    ]
    if any(image not in names for _, image in bindings):
        return None
    sidecar = image_path.parent / "vita_hud.bin"
    data_bytes = sum(8 + len(label.encode("utf-8")) + len(image.encode("utf-8"))
                     for label, image in bindings)
    with sidecar.open("wb") as stream:
        stream.write(struct.pack("<3IQ", 0x48544956, 1, len(bindings), data_bytes))
        for label, image in bindings:
            label_bytes = label.encode("utf-8")
            image_bytes = image.encode("utf-8")
            stream.write(struct.pack("<2I", len(label_bytes), len(image_bytes)))
            stream.write(label_bytes)
            stream.write(image_bytes)
    return sidecar


def _validate_hud_sidecar(path: Path) -> dict[str, Any] | None:
    """Validate the pointer-free VITH HUD binding table."""
    if not path.is_file() or path.stat().st_size == 0:
        return None
    data = path.read_bytes()
    if len(data) < 20:
        raise ValueError(f"HUD sidecar is truncated: {path}")
    magic, version, record_count, data_bytes = struct.unpack_from("<3IQ", data)
    if magic != 0x48544956 or version != 1 or record_count == 0 or record_count > 64:
        raise ValueError(f"unsupported HUD sidecar header: {path}")
    offset = 20
    aggregate = 0
    for _ in range(record_count):
        if offset + 8 > len(data):
            raise ValueError(f"HUD sidecar record header is truncated: {path}")
        label_bytes, image_bytes = struct.unpack_from("<2I", data, offset)
        if (label_bytes == 0 or label_bytes > 256 or image_bytes == 0
                or image_bytes > 4096):
            raise ValueError(f"HUD sidecar record is invalid: {path}")
        offset += 8
        if offset + label_bytes + image_bytes > len(data):
            raise ValueError(f"HUD sidecar strings are truncated: {path}")
        offset += label_bytes + image_bytes
        aggregate += 8 + label_bytes + image_bytes
    if offset != len(data) or aggregate != data_bytes:
        raise ValueError(f"HUD sidecar size mismatch: {path}")
    return {"path": path, "bytes": len(data), "records": record_count,
            "dataBytes": data_bytes}


def _validate_animation_sidecar(path: Path) -> dict[str, Any] | None:
    if not path.is_file() or path.stat().st_size == 0:
        return None
    data = path.read_bytes()
    if len(data) < 16:
        raise ValueError(f"animation sidecar is truncated: {path}")
    magic, version, count, record_bytes = struct.unpack_from("<4I", data)
    expected_record_bytes = {1: 182, 2: 162, 3: 194}
    if magic != 0x51544956 or version not in expected_record_bytes \
            or record_bytes != expected_record_bytes.get(version):
        raise ValueError(f"unsupported animation sidecar header: {path}")
    if count == 0 or count > 2048:
        raise ValueError(f"animation sidecar size mismatch: {path}")
    names: list[str] = []
    offset = 16
    for _ in range(count):
        if offset + record_bytes > len(data):
            raise ValueError(f"animation sidecar record is truncated: {path}")
        record = data[offset:offset + record_bytes]
        name_bytes = record[:128]
        nul = name_bytes.find(b"\0")
        if nul <= 0:
            raise ValueError(f"animation sidecar has invalid name: {path}")
        names.append(name_bytes[:nul].decode("utf-8", errors="strict"))
        framerate, frequency = struct.unpack_from(
            "<2f", record, 174 if version == 1 else (154 if version == 2 else 186)
        )
        if not (framerate >= 0.0 and frequency >= 0.0):
            raise ValueError(f"animation sidecar has invalid timing: {path}")
        if version == 1:
            offset += record_bytes
            continue
        if version == 3:
            numframes, looped, delta_flag = struct.unpack_from("<HBB", record, 128)
            bone_counts = record[132:142]
            notify_count, _asset_type, is_default, reserved8 = struct.unpack_from(
                "<4B", record, 142
            )
            (data_byte_count, data_short_count, data_int_count,
             random_byte_count, random_int_count,
             reserved16) = struct.unpack_from("<6H", record, 146)
            (random_short_count, index_count, bone_name_count,
             native_payload_bytes, track_count, sampled_payload_bytes,
             delta_payload_bytes) = struct.unpack_from("<7I", record, 158)
            if (looped > 1 or delta_flag > 1 or is_default > 1
                    or reserved8 != 0 or reserved16 != 0
                    or bone_name_count != bone_counts[9]
                    or bone_name_count > 128 or notify_count > 128
                    or sum(bone_counts[:5]) != bone_name_count
                    or sum(bone_counts[5:9]) != bone_name_count
                    or track_count > 64
                    or native_payload_bytes > 16 * 1024 * 1024
                    or sampled_payload_bytes > 16 * 1024 * 1024
                    or (delta_flag == 0 and delta_payload_bytes != 0)
                    or (delta_flag != 0 and delta_payload_bytes < 56)):
                raise ValueError(f"animation sidecar source header is invalid: {path}")
            index_bytes = index_count * (2 if numframes >= 256 else 1)
            expected_native = (
                bone_name_count * 64
                + data_byte_count + data_short_count * 2 + data_int_count * 4
                + random_byte_count + random_short_count * 2
                + random_int_count * 4 + index_bytes
                + notify_count * 132 + delta_payload_bytes
            )
            if expected_native != native_payload_bytes:
                raise ValueError(f"animation sidecar native size mismatch: {path}")
            native_offset = offset + record_bytes
            native_end = native_offset + native_payload_bytes
            sampled_end = native_end + sampled_payload_bytes
            if sampled_end > len(data):
                raise ValueError(f"animation sidecar source payload is truncated: {path}")
            for _bone in range(bone_name_count):
                name_field = data[native_offset:native_offset + 64]
                nul = name_field.find(b"\0")
                if nul <= 0 or any(name_field[nul + 1:]):
                    raise ValueError(f"animation sidecar bone name is invalid: {path}")
                name_field[:nul].decode("utf-8", errors="strict")
                native_offset += 64
            native_offset += (
                data_byte_count + data_short_count * 2 + data_int_count * 4
                + random_byte_count + random_short_count * 2
                + random_int_count * 4 + index_bytes
            )
            previous_time = -1.0
            for _notify in range(notify_count):
                if native_offset + 132 > native_end:
                    raise ValueError(f"animation sidecar notify is truncated: {path}")
                name_field = data[native_offset:native_offset + 128]
                nul = name_field.find(b"\0")
                notify_time = struct.unpack_from("<f", data, native_offset + 128)[0]
                if (nul <= 0 or any(name_field[nul + 1:])
                        or not (0.0 <= notify_time <= 1.0)
                        or notify_time < previous_time):
                    raise ValueError(f"animation sidecar notify is invalid: {path}")
                name_field[:nul].decode("utf-8", errors="strict")
                previous_time = notify_time
                native_offset += 132
            if delta_flag:
                if native_offset + 56 > native_end:
                    raise ValueError(f"animation sidecar delta is truncated: {path}")
                delta = struct.unpack_from("<4B2HI6f2h4I", data, native_offset)
                (has_trans, has_quat, trans_small, delta_reserved8,
                 trans_size, quat_size, delta_reserved32,
                 *delta_tail) = delta
                trans_values = delta_tail[:6]
                (trans_frame_bytes, trans_index_bytes,
                 quat_frame_bytes, quat_index_bytes) = delta_tail[8:12]
                index_stride = 2 if numframes >= 256 else 1
                expected_trans_frames = ((trans_size + 1) * (3 if trans_small else 6)
                                         if has_trans and trans_size else 0)
                expected_trans_indices = ((trans_size + 1) * index_stride
                                          if has_trans and trans_size else 0)
                expected_quat_frames = ((quat_size + 1) * 4
                                        if has_quat and quat_size else 0)
                expected_quat_indices = ((quat_size + 1) * index_stride
                                         if has_quat and quat_size else 0)
                expected_delta = (56 + expected_trans_frames
                                  + expected_trans_indices + expected_quat_frames
                                  + expected_quat_indices)
                if (has_trans > 1 or has_quat > 1 or trans_small > 1
                        or delta_reserved8 != 0 or delta_reserved32 != 0
                        or (not has_trans and (trans_size or trans_small))
                        or (not has_quat and quat_size)
                        or trans_size > numframes or quat_size > numframes
                        or trans_frame_bytes != expected_trans_frames
                        or trans_index_bytes != expected_trans_indices
                        or quat_frame_bytes != expected_quat_frames
                        or quat_index_bytes != expected_quat_indices
                        or expected_delta != delta_payload_bytes
                        or any(value != value or value in (float("inf"), float("-inf"))
                               for value in trans_values)):
                    raise ValueError(f"animation sidecar delta is invalid: {path}")
                native_offset += delta_payload_bytes
            if native_offset != native_end:
                raise ValueError(f"animation sidecar native payload mismatch: {path}")
            track_offset = native_end
            for _track in range(track_count):
                if track_offset + 16 > sampled_end:
                    raise ValueError(f"animation sidecar track is truncated: {path}")
                model_name_bytes, bone_count, frame_count, reserved = struct.unpack_from(
                    "<4I", data, track_offset
                )
                if (model_name_bytes == 0 or model_name_bytes > 256
                        or bone_count == 0 or bone_count > 128
                        or frame_count == 0 or frame_count > 256
                        or frame_count != numframes or reserved != 0):
                    raise ValueError(f"animation sidecar track header is invalid: {path}")
                track_offset += 16
                if track_offset + model_name_bytes > sampled_end:
                    raise ValueError(f"animation sidecar model name is truncated: {path}")
                model_name = data[track_offset:track_offset + model_name_bytes]
                if b"\0" in model_name:
                    raise ValueError(f"animation sidecar model name is invalid: {path}")
                track_offset += model_name_bytes
                sample_bytes = bone_count * frame_count * 32
                if track_offset + sample_bytes > sampled_end:
                    raise ValueError(f"animation sidecar samples are truncated: {path}")
                for sample_offset in range(track_offset, track_offset + sample_bytes, 4):
                    value = struct.unpack_from("<f", data, sample_offset)[0]
                    if value != value or value in (float("inf"), float("-inf")):
                        raise ValueError(f"animation sidecar sample is not finite: {path}")
                track_offset += sample_bytes
            if track_offset != sampled_end:
                raise ValueError(f"animation sidecar sampled payload mismatch: {path}")
            offset = sampled_end
            continue
        track_count, payload_bytes = struct.unpack_from("<2I", record, 146)
        if track_count > 64 or payload_bytes > 16 * 1024 * 1024:
            raise ValueError(f"animation sidecar track header is invalid: {path}")
        payload_end = offset + record_bytes + payload_bytes
        if payload_end > len(data):
            raise ValueError(f"animation sidecar payload is truncated: {path}")
        track_offset = offset + record_bytes
        for _track in range(track_count):
            if track_offset + 16 > payload_end:
                raise ValueError(f"animation sidecar track is truncated: {path}")
            model_name_bytes, bone_count, frame_count, reserved = struct.unpack_from(
                "<4I", data, track_offset
            )
            if (model_name_bytes == 0 or model_name_bytes > 256
                    or bone_count == 0 or bone_count > 128
                    or frame_count == 0 or frame_count > 256
                    or frame_count != struct.unpack_from("<H", record, 128)[0]
                    or reserved != 0):
                raise ValueError(f"animation sidecar track header is invalid: {path}")
            track_offset += 16
            if track_offset + model_name_bytes > payload_end:
                raise ValueError(f"animation sidecar model name is truncated: {path}")
            model_name = data[track_offset:track_offset + model_name_bytes]
            if b"\0" in model_name:
                raise ValueError(f"animation sidecar model name is invalid: {path}")
            track_offset += model_name_bytes
            sample_bytes = bone_count * frame_count * 32
            if track_offset + sample_bytes > payload_end:
                raise ValueError(f"animation sidecar samples are truncated: {path}")
            for sample_offset in range(track_offset, track_offset + sample_bytes, 4):
                value = struct.unpack_from("<f", data, sample_offset)[0]
                if value != value or value in (float("inf"), float("-inf")):
                    raise ValueError(f"animation sidecar sample is not finite: {path}")
            track_offset += sample_bytes
        # VITQ v2 packages may append the authored XAnim notetrack table
        # after the sampled tracks. Older v2 packages end at payload_end and
        # remain valid, so this is an additive compatibility extension.
        notify_count = record[142]
        if track_offset < payload_end:
            if notify_count == 0 or notify_count > 128:
                raise ValueError(f"animation sidecar notify header is invalid: {path}")
            previous_time = -1.0
            for _notify in range(notify_count):
                if track_offset + 8 > payload_end:
                    raise ValueError(f"animation sidecar notify is truncated: {path}")
                name_bytes, notify_time = struct.unpack_from(
                    "<If", data, track_offset
                )
                track_offset += 8
                if (name_bytes == 0 or name_bytes > 256
                        or track_offset + name_bytes > payload_end
                        or not (0.0 <= notify_time <= 1.0)):
                    raise ValueError(f"animation sidecar notify is invalid: {path}")
                name = data[track_offset:track_offset + name_bytes]
                if b"\0" in name or notify_time < previous_time:
                    raise ValueError(f"animation sidecar notify is invalid: {path}")
                previous_time = notify_time
                track_offset += name_bytes
        if track_offset != payload_end:
            raise ValueError(f"animation sidecar payload size mismatch: {path}")
        offset = payload_end
    if offset != len(data):
        raise ValueError(f"animation sidecar size mismatch: {path}")
    if names != sorted(names) or len(set(names)) != len(names):
        raise ValueError(f"animation sidecar is not deterministic: {path}")
    return {"path": path, "bytes": len(data), "records": count, "version": version}


def _validate_model_sidecar(path: Path) -> dict[str, Any] | None:
    """Validate the bounded pointer-free VITX LOD0 model stream.

    VITX v1 contains static geometry.  VITX v2 adds a bounded XModel skeleton
    and one four-influence weight record per vertex. VITX v3 keeps that layout
    while storing both raw local bone pose and composed baseMat for exact
    KisakCOD-style inverse-bind skinning. VITX v4 appends the source
    tag_flash bone index to the authored tag metadata. VITX v5 adds one
    fixed, NUL-terminated textual name record per bone so the Vita runtime can
    reconstruct XModel::boneNames through its own string table.
    """
    if not path.is_file() or path.stat().st_size == 0:
        return None
    data = path.read_bytes()
    if len(data) < 20:
        raise ValueError(f"model sidecar is truncated: {path}")
    magic, version, model_count, data_bytes = struct.unpack_from("<3IQ", data)
    if magic != 0x58544956 or version not in (1, 2, 3, 4, 5):
        raise ValueError(f"unsupported model sidecar header: {path}")
    if model_count > 512:
        raise ValueError(f"model sidecar record count is invalid: {path}")
    offset = 20
    for _ in range(model_count):
        if offset + 40 > len(data):
            raise ValueError(f"model sidecar record header is truncated: {path}")
        name_bytes, surface_count, vertex_count, index_count = struct.unpack_from(
            "<4I", data, offset
        )
        if (name_bytes == 0 or name_bytes > 256 or surface_count == 0
                or surface_count > 64 or vertex_count == 0 or vertex_count > 65535
                or index_count < 3 or index_count > 300000 or index_count % 3 != 0):
            raise ValueError(f"model sidecar record is invalid: {path}")
        offset += 40
        if offset + name_bytes > len(data):
            raise ValueError(f"model sidecar name is truncated: {path}")
        offset += name_bytes
        bone_count = 0
        if version in (2, 3, 4, 5):
            if offset + 16 > len(data):
                raise ValueError(f"model sidecar skin header is truncated: {path}")
            bone_count, root_bones, parent_bytes, reserved = struct.unpack_from(
                "<4I", data, offset
            )
            if (bone_count > 128 or root_bones > bone_count
                    or parent_bytes != bone_count
                    # VITX v3-v5 encode one-based tag indices in the reserved
                    # field: tag_weapon in the low half and tag_camera in the
                    # high half. VITX v4/v5 append one u32 for tag_flash.
                    or (version not in (3, 4, 5) and reserved != 0)
                    or (version in (3, 4, 5) and ((reserved & 0xFFFF) > bone_count
                                                or (reserved >> 16) > bone_count))):
                raise ValueError(f"model sidecar skin header is invalid: {path}")
            skin_header_bytes = 24 if version == 5 else (20 if version == 4 else 16)
            if offset + skin_header_bytes > len(data):
                raise ValueError(f"model sidecar skin header is truncated: {path}")
            if version in (4, 5):
                flash_tag = struct.unpack_from("<I", data, offset + 16)[0]
                if flash_tag > bone_count:
                    raise ValueError(f"model sidecar flash tag index is invalid: {path}")
            bone_name_bytes = (struct.unpack_from("<I", data, offset + 20)[0]
                               if version == 5 else 0)
            if version == 5 and bone_name_bytes != bone_count * 64:
                raise ValueError(f"model sidecar bone-name table is invalid: {path}")
            offset += skin_header_bytes
            bone_bytes = 64 if version in (3, 4, 5) else 32
            if (offset + parent_bytes + bone_name_bytes
                    + bone_count * bone_bytes > len(data)):
                raise ValueError(f"model sidecar skeleton is truncated: {path}")
            parents = data[offset:offset + parent_bytes]
            if any(parent != 0xFF and parent >= bone_count for parent in parents):
                raise ValueError(f"model sidecar parent index is invalid: {path}")
            offset += parent_bytes
            if version == 5:
                for bone_index in range(bone_count):
                    record = data[offset + bone_index * 64:offset + (bone_index + 1) * 64]
                    terminator = record.find(b"\0")
                    # The retail static/projectile model convention uses the
                    # source string-table's empty entry for an anonymous root.
                    if terminator < 0:
                        raise ValueError(f"model sidecar bone name is invalid: {path}")
                    try:
                        record[:terminator].decode("utf-8")
                    except UnicodeDecodeError as error:
                        raise ValueError(
                            f"model sidecar bone name is not UTF-8: {path}") from error
                offset += bone_name_bytes
            offset += bone_count * bone_bytes
        surfaces: list[tuple[int, int, int, int, int]] = []
        for _ in range(surface_count):
            if offset + 24 > len(data):
                raise ValueError(f"model sidecar surface header is truncated: {path}")
            material_bytes, vertex_start, surface_vertices, index_start, surface_indices, _ = struct.unpack_from(
                "<6I", data, offset
            )
            if (material_bytes > 256
                    or vertex_start + surface_vertices > vertex_count
                    or index_start + surface_indices > index_count
                    or surface_indices == 0 or surface_indices % 3 != 0):
                raise ValueError(f"model sidecar surface is invalid: {path}")
            offset += 24
            if offset + material_bytes > len(data):
                raise ValueError(f"model sidecar material name is truncated: {path}")
            offset += material_bytes
            surfaces.append((vertex_start, surface_vertices, index_start, surface_indices, material_bytes))
        vertex_bytes = vertex_count * 32
        index_bytes = index_count * 2
        if offset + vertex_bytes + index_bytes > len(data):
            raise ValueError(f"model sidecar geometry is truncated: {path}")
        offset += vertex_bytes
        if version in (2, 3, 4, 5):
            weight_bytes = vertex_count * 16
            if offset + weight_bytes > len(data):
                raise ValueError(f"model sidecar weights are truncated: {path}")
            for vertex_index in range(vertex_count):
                bones = struct.unpack_from("<4H", data, offset + vertex_index * 16)
                weights = struct.unpack_from("<4H", data, offset + vertex_index * 16 + 8)
                if sum(weights) > 65535 or any(
                        weight and bone >= bone_count
                        for bone, weight in zip(bones, weights)):
                    raise ValueError(f"model sidecar vertex weights are invalid: {path}")
            offset += weight_bytes
        offset += index_bytes
    if offset != len(data) or offset - 20 != data_bytes:
        raise ValueError(f"model sidecar size mismatch: {path}")
    return {"path": path, "bytes": len(data), "records": model_count,
            "dataBytes": data_bytes, "version": version}


def _validate_model_lod_sidecar(path: Path) -> dict[str, Any] | None:
    """Validate the pointer-free VLOD lower-detail render companion."""
    if not path.is_file() or path.stat().st_size == 0:
        return None
    data = path.read_bytes()
    if len(data) < 20:
        raise ValueError(f"model LOD sidecar is truncated: {path}")
    magic, version, model_count, data_bytes = struct.unpack_from("<3IQ", data)
    if magic != 0x444F4C56 or version != 1:
        raise ValueError(f"unsupported model LOD sidecar header: {path}")
    if model_count == 0 or model_count > 512:
        raise ValueError(f"model LOD sidecar record count is invalid: {path}")
    offset = 20
    names: list[str] = []
    lod_records = 0
    vertices = 0
    indices = 0
    for _ in range(model_count):
        if offset + 32 > len(data):
            raise ValueError(f"model LOD record header is truncated: {path}")
        (name_bytes, lod_count, stored_lod_count, lod_ramp_type,
         *distances) = struct.unpack_from("<4I4f", data, offset)
        if (name_bytes == 0 or name_bytes > 256 or lod_count < 2
                or lod_count > 4 or stored_lod_count == 0
                or stored_lod_count >= lod_count or lod_ramp_type > 1
                or any(not math.isfinite(distance) or distance < 0.0
                       for distance in distances)):
            raise ValueError(f"model LOD record is invalid: {path}")
        previous_distance = 0.0
        terminal_distance = False
        for lod_index, distance in enumerate(distances):
            if lod_index >= lod_count:
                if distance != 0.0:
                    raise ValueError(f"model LOD inactive distance is invalid: {path}")
                continue
            if (terminal_distance
                    or (distance != 0.0 and distance <= previous_distance)):
                raise ValueError(f"model LOD distances are not ordered: {path}")
            if distance == 0.0:
                terminal_distance = True
            else:
                previous_distance = distance
        offset += 32
        if offset + name_bytes > len(data):
            raise ValueError(f"model LOD name is truncated: {path}")
        raw_name = data[offset:offset + name_bytes]
        if b"\0" in raw_name:
            raise ValueError(f"model LOD name is invalid: {path}")
        try:
            names.append(raw_name.decode("utf-8"))
        except UnicodeDecodeError as error:
            raise ValueError(f"model LOD name is not UTF-8: {path}") from error
        offset += name_bytes
        previous_lod = 0
        for _lod in range(stored_lod_count):
            if offset + 16 > len(data):
                raise ValueError(f"model LOD geometry header is truncated: {path}")
            lod_index, surface_count, vertex_count, index_count = struct.unpack_from(
                "<4I", data, offset
            )
            offset += 16
            if (lod_index == 0 or lod_index >= lod_count
                    or lod_index <= previous_lod or surface_count == 0
                    or surface_count > 64 or vertex_count == 0
                    or vertex_count > 65535 or index_count < 3
                    or index_count > 300000 or index_count % 3 != 0):
                raise ValueError(f"model LOD geometry record is invalid: {path}")
            previous_lod = lod_index
            surface_ranges: list[tuple[int, int, int, int]] = []
            for _surface in range(surface_count):
                if offset + 24 > len(data):
                    raise ValueError(f"model LOD surface is truncated: {path}")
                (material_bytes, vertex_start, surface_vertices, index_start,
                 surface_indices, reserved) = struct.unpack_from("<6I", data, offset)
                offset += 24
                if (material_bytes > 256 or reserved != 0
                        or surface_vertices == 0 or surface_indices == 0
                        or surface_indices % 3 != 0
                        or vertex_start + surface_vertices > vertex_count
                        or index_start + surface_indices > index_count
                        or offset + material_bytes > len(data)):
                    raise ValueError(f"model LOD surface is invalid: {path}")
                if b"\0" in data[offset:offset + material_bytes]:
                    raise ValueError(f"model LOD material name is invalid: {path}")
                offset += material_bytes
                surface_ranges.append((vertex_start, surface_vertices,
                                       index_start, surface_indices))
            vertex_bytes = vertex_count * 32
            index_bytes = index_count * 2
            if offset + vertex_bytes + index_bytes > len(data):
                raise ValueError(f"model LOD geometry is truncated: {path}")
            offset += vertex_bytes
            unpacked_indices = struct.unpack_from(
                f"<{index_count}H", data, offset
            )
            if any(index >= vertex_count for index in unpacked_indices):
                raise ValueError(f"model LOD triangle index is invalid: {path}")
            for vertex_start, surface_vertices, index_start, surface_indices in surface_ranges:
                if any(index < vertex_start
                       or index >= vertex_start + surface_vertices
                       for index in unpacked_indices[
                           index_start:index_start + surface_indices]):
                    raise ValueError(
                        f"model LOD surface triangle index is invalid: {path}"
                    )
            offset += index_bytes
            lod_records += 1
            vertices += vertex_count
            indices += index_count
    if (offset != len(data) or offset - 20 != data_bytes
            or names != sorted(names, key=str.lower)
            or len(set(name.lower() for name in names)) != len(names)):
        raise ValueError(f"model LOD sidecar size/order is invalid: {path}")
    return {"path": path, "bytes": len(data), "records": model_count,
            "lodRecords": lod_records, "vertices": vertices,
            "indices": indices, "dataBytes": data_bytes, "version": version}


def _material_records(path: Path) -> list[dict[str, str]]:
    """Read VITL v1-v5 records without exposing native material pointers."""
    if not path.is_file() or path.stat().st_size < 20:
        return []
    data = path.read_bytes()
    try:
        magic, version, record_count, _ = struct.unpack_from("<3IQ", data)
        if magic != 0x4C544956 or version not in (1, 2, 3, 4, 5) or record_count > 4096:
            return []
        offset = 20
        records: list[dict[str, str]] = []
        for _ in range(record_count):
            if version == 1:
                header_bytes = 8
                lengths = struct.unpack_from("<2I", data, offset)
                names = ("name", "color")
            else:
                header_bytes = (32 if version == 2 else
                                (33 if version == 3 else (42 if version == 4 else 52)))
                lengths = struct.unpack_from("<5I", data, offset)
                names = ("name", "color", "normal", "specular", "technique")
            if offset + header_bytes > len(data):
                return []
            if any(length > 4096 for length in lengths):
                return []
            offset += header_bytes
            record: dict[str, str] = {}
            for name, length in zip(names, lengths):
                if offset + length > len(data):
                    return []
                record[name] = data[offset:offset + length].decode(
                    "utf-8", "ignore")
                offset += length
            records.append(record)
        if offset != len(data):
            return []
        return records
    except (struct.error, UnicodeError, OverflowError):
        return []


def _material_lookup(path: Path) -> dict[str, str]:
    """Return material-to-color-map aliases from the VITL sidecar."""
    return {
        record["name"]: record["color"]
        for record in _material_records(path)
        if record.get("name") and record.get("color")
    }


def _material_image_names(path: Path) -> list[str]:
    """Return unique authored image aliases from all VITL semantic slots."""
    names: set[str] = set()
    for record in _material_records(path):
        for key in ("color", "normal", "specular"):
            image = record.get(key, "")
            if image:
                names.add(image)
    return sorted(names)


def _entity_model_names(path: Path) -> set[str]:
    """Return map-placed XModel names from the pointer-free VITE sidecar."""
    if not path.is_file() or path.stat().st_size < 12:
        return set()
    data = path.read_bytes()
    try:
        magic, version, text_bytes = struct.unpack_from("<3I", data)
        if magic != 0x45544956 or version != 1 or 12 + text_bytes != len(data):
            return set()
        text = data[12:].decode("utf-8", "ignore")
        names: set[str] = set()
        for block in re.findall(r"\{([^{}]*)\}", text, re.S):
            values = dict(re.findall(r'"([^\"]+)"\s+"([^\"]*)"', block))
            if values.get("classname") not in ("script_model", "vita_static_model"):
                continue
            model = values.get("model", "")
            if model and not model.startswith("*"):
                names.add(model)
        return names
    except (struct.error, UnicodeError, OverflowError):
        return set()


def _weapon_view_model_names(path: Path) -> set[str]:
    """Return authored first-person model names from the pointer-free VITP sidecar."""
    if not path.is_file() or path.stat().st_size < 16:
        return set()
    data = path.read_bytes()
    try:
        magic, version, record_count, record_bytes = struct.unpack_from(
            "<4I", data
        )
        if (magic != 0x50544956 or version not in (2, 3, 4, 5)
                or record_count > 4096 or record_bytes < 216
                or 16 + record_count * record_bytes != len(data)):
            return set()
        # ShipmentWeaponDefinition keeps the source gunXModel[0] name after
        # the 64-byte asset name, seventeen integer fields, and seven floats.
        # The v2-v5 records append the same fixed 64-byte field before their
        # hide-tag and animation tails.
        view_model_offset = 64 + 17 * 4 + 7 * 4
        names: set[str] = set()
        for index in range(record_count):
            offset = 16 + index * record_bytes + view_model_offset
            raw_name = data[offset:offset + 64]
            name = raw_name.split(b"\0", 1)[0].decode("utf-8", "ignore")
            if name and not name.startswith("*"):
                names.add(name)
        return names
    except (struct.error, UnicodeError, OverflowError):
        return set()


def _model_surface_material_names(
        path: Path, entity_model_names: set[str] | None = None) -> list[str]:
    """Read VITX surface names, optionally limited to map-placed XModels."""
    if not path.is_file() or path.stat().st_size < 20:
        return []
    data = path.read_bytes()
    try:
        magic, version, model_count, _ = struct.unpack_from("<3IQ", data)
        if magic != 0x58544956 or version not in (1, 2, 3, 4, 5) or model_count > 512:
            return []
        offset = 20
        names: set[str] = set()
        for _ in range(model_count):
            if offset + 40 > len(data):
                return []
            name_bytes, surface_count, vertex_count, index_count = struct.unpack_from(
                "<4I", data, offset
            )
            if (name_bytes == 0 or name_bytes > 256 or surface_count > 64
                    or vertex_count > 65535 or index_count > 300000):
                return []
            if offset + 40 + name_bytes > len(data):
                return []
            model_name = data[offset + 40:offset + 40 + name_bytes].decode(
                "utf-8", "ignore")
            include_model = entity_model_names is None or any(
                model_name == requested or model_name.find(requested) != -1
                for requested in entity_model_names
            )
            offset += 40 + name_bytes
            if version in (2, 3, 4, 5):
                if offset + 16 > len(data):
                    return []
                bone_count, root_bones, parent_bytes, reserved = struct.unpack_from(
                    "<4I", data, offset
                )
                if (bone_count > 128 or root_bones > bone_count
                        or parent_bytes != bone_count
                        or (version not in (3, 4, 5) and reserved != 0)
                        or (version in (3, 4, 5) and ((reserved & 0xFFFF) > bone_count
                                                    or (reserved >> 16) > bone_count))):
                    return []
                skin_header_bytes = 24 if version == 5 else (20 if version == 4 else 16)
                if offset + skin_header_bytes > len(data):
                    return []
                if version in (4, 5):
                    flash_tag = struct.unpack_from("<I", data, offset + 16)[0]
                    if flash_tag > bone_count:
                        return []
                bone_name_bytes = (struct.unpack_from("<I", data, offset + 20)[0]
                                   if version == 5 else 0)
                if version == 5 and bone_name_bytes != bone_count * 64:
                    return []
                bone_bytes = 64 if version in (3, 4, 5) else 32
                offset += (skin_header_bytes + parent_bytes + bone_name_bytes
                           + bone_count * bone_bytes)
                if offset > len(data):
                    return []
            for _ in range(surface_count):
                if offset + 24 > len(data):
                    return []
                material_bytes = struct.unpack_from("<I", data, offset)[0]
                offset += 24
                if material_bytes > 256 or offset + material_bytes > len(data):
                    return []
                if material_bytes and include_model:
                    names.add(data[offset:offset + material_bytes].decode("utf-8", "ignore"))
                offset += material_bytes
            offset += vertex_count * 32
            if version in (2, 3, 4, 5):
                offset += vertex_count * 16
            offset += index_count * 2
            if offset > len(data):
                return []
        return sorted(names)
    except (struct.error, UnicodeError):
        return []


def _world_surface_material_names(path: Path) -> list[str]:
    """Read bounded world-surface material names from the VITW sidecar."""
    if not path.is_file() or path.stat().st_size < 44:
        return []
    data = path.read_bytes()
    try:
        magic, version, vertex_count, index_count, surface_count = struct.unpack_from(
            "<5I", data
        )
        if magic != 0x57544956 or version not in (1, 2, 3) or surface_count > 100000:
            return []
        header_bytes = 44 if version == 1 else 120
        surface_bytes = 92 if version == 3 else 88
        if len(data) < header_bytes:
            return []
        expected = (header_bytes + vertex_count * 40 + index_count * 2
                    + surface_count * surface_bytes)
        if expected != len(data):
            return []
        surface_offset = header_bytes + vertex_count * 40 + index_count * 2
        names: set[str] = set()
        for surface_index in range(surface_count):
            record_offset = surface_offset + surface_index * surface_bytes
            material = data[record_offset + 24:record_offset + 88]
            name = material.split(b"\0", 1)[0].decode("utf-8", "ignore")
            if name:
                names.add(name)
        return sorted(names)
    except (struct.error, UnicodeError, OverflowError):
        return []


def _read_image_records(path: Path) -> list[tuple[str, bytes, int, int, int, int, int, int]]:
    """Read a bounded VITI stream while retaining payloads for special images.

    The retail IWI pass below deliberately rebuilds the ordinary material
    image set.  Generated BSP lightmaps are not IWI files, however: the
    Windows oracle can emit them as L8/A8R8G8B8 VITI records.  Keep only the
    small subset needed by the renderer and leave all other records to the
    deterministic IWI selection policy.
    """
    if not path.is_file() or path.stat().st_size < 20:
        return []
    data = path.read_bytes()
    try:
        magic, version, record_count, declared_bytes = struct.unpack_from("<3IQ", data)
        if magic != 0x49544956 or version != 1 or record_count > 4096:
            return []
        offset = 20
        aggregate = 0
        records: list[tuple[str, bytes, int, int, int, int, int, int]] = []
        for _ in range(record_count):
            if offset + 32 > len(data):
                return []
            name_bytes, payload_bytes, width, height, depth, levels, fmt, flags = struct.unpack_from(
                "<8I", data, offset
            )
            offset += 32
            if (name_bytes == 0 or name_bytes > 4096 or payload_bytes == 0
                    or payload_bytes > 8 * 1024 * 1024 or width == 0 or width > 4096
                    or height == 0 or height > 4096 or depth != 1
                    or levels == 0 or levels > 16
                    or offset + name_bytes + payload_bytes > len(data)):
                return []
            name = data[offset:offset + name_bytes].decode("utf-8", "strict")
            offset += name_bytes
            payload = bytes(data[offset:offset + payload_bytes])
            offset += payload_bytes
            aggregate += payload_bytes
            records.append((name, payload, width, height, depth, levels, fmt, flags))
        if offset != len(data) or aggregate != declared_bytes:
            return []
        return records
    except (struct.error, UnicodeError, OverflowError):
        return []


def _write_image_records(
        path: Path,
        records: list[tuple[str, bytes, int, int, int, int, int, int]]) -> None:
    """Write a deterministic VITI stream from already validated records."""
    path.parent.mkdir(parents=True, exist_ok=True)
    total_bytes = sum(len(record[1]) for record in records)
    with path.open("wb") as stream:
        stream.write(struct.pack("<3IQ", 0x49544956, 1, len(records),
                                 total_bytes))
        for name, payload, width, height, depth, levels, fourcc, flags in records:
            encoded = name.encode("utf-8")
            stream.write(struct.pack(
                "<8I", len(encoded), len(payload), width, height, depth,
                levels, fourcc, flags))
            stream.write(encoded)
            stream.write(payload)


def _write_image_segments(
        output: Path,
        records: list[tuple[str, bytes, int, int, int, int, int, int]],
        max_records: int = SEMANTIC_EXPORT_MAX_RECORDS,
        max_data_bytes: int = SEMANTIC_EXPORT_MAX_DATA_BYTES,
        ) -> list[dict[str, Any]]:
    """Write a bounded, deterministic family of VITI image chunks.

    The startup image stream is intentionally bounded because Vita3K's scalar
    path expands source payloads while the projection buffers are resident.
    Records that are still part of the authored map color closure therefore
    live in this streamed family instead of being silently replaced by the
    runtime's white fallback texture.
    """
    if max_records <= 0 or max_data_bytes <= 0:
        raise ValueError("image segment envelope must be positive")
    output.parent.mkdir(parents=True, exist_ok=True)
    for stale in output.parent.glob(f"{output.stem}-*.bin"):
        stale.unlink()
    if not records:
        if output.is_file():
            output.unlink()
        return []

    segments: list[list[tuple[str, bytes, int, int, int, int, int, int]]] = []
    current: list[tuple[str, bytes, int, int, int, int, int, int]] = []
    current_bytes = 0
    for record in records:
        payload_bytes = len(record[1])
        if payload_bytes > max_data_bytes:
            raise ValueError(
                f"image record exceeds the segment envelope "
                f"({payload_bytes}/{max_data_bytes})")
        if (current and (len(current) >= max_records
                          or current_bytes + payload_bytes > max_data_bytes)):
            segments.append(current)
            current = []
            current_bytes = 0
        current.append(record)
        current_bytes += payload_bytes
    if current:
        segments.append(current)

    result: list[dict[str, Any]] = []
    for segment_index, segment in enumerate(segments):
        segment_path = (output if segment_index == 0 else output.with_name(
            f"{output.stem}-{segment_index}{output.suffix}"))
        _write_image_records(segment_path, segment)
        result.append({
            "path": segment_path,
            "bytes": segment_path.stat().st_size,
            "records": len(segment),
            "dataBytes": sum(len(item[1]) for item in segment),
        })
    return result


def _split_reflection_probe_sidecar(image_path: Path,
                                    reflection_path: Path) -> dict[str, int]:
    """Move generated probe records into a second per-map VITI stream.

    Reflection probes are source-derived environment inputs, but retaining
    them beside every ordinary image during the first scalar package load can
    displace authored BSP color maps at the fixed Vita3K heap boundary.  The
    runtime appends this small stream after the main image bundle is loaded.
    """
    records = _read_image_records(image_path)
    if not records and image_path.is_file() and image_path.stat().st_size > 0:
        raise ValueError(f"image sidecar cannot be split: {image_path}")
    probes = [record for record in records
              if record[0].lower().startswith("*reflection_probe")]
    ordinary = [record for record in records
                if not record[0].lower().startswith("*reflection_probe")]
    # Keep the generated companion deterministic even for maps whose desktop
    # dump has no reflection probes yet.
    _write_image_records(image_path, ordinary)
    _write_image_records(reflection_path, probes)
    return {
        "records": len(probes),
        "dataBytes": sum(len(record[1]) for record in probes),
    }


def _reorder_iwi_mips(payload: bytes, width: int, height: int,
                      levels: int, image_format: int, flags: int = 0) -> bytes:
    """Convert COD4's smallest-first IWI mip stream to base-first order.

    The retail image decoder walks the IWI payload from the smallest mip to
    mip 0.  VitaGL/OpenGL and the scalar decoder consume the conventional
    base-level-first stream.  Passing the IWI bytes through unchanged makes a
    512x1024 font atlas decode its 1x2 tail as if it were the base image,
    producing the garbled menu/HUD text seen in the emulator capture.
    """
    if not payload or levels <= 1 or width <= 0 or height <= 0:
        return payload
    if image_format in (11, 12, 13):
        block_bytes = 8 if image_format == 11 else 16

        def level_bytes(level_width: int, level_height: int) -> int:
            return ((level_width + 3) // 4) * ((level_height + 3) // 4) * block_bytes
    elif image_format in (1, 2, 3, 4, 5):
        bytes_per_pixel = {1: 4, 2: 3, 3: 2, 4: 1, 5: 1}[image_format]

        def level_bytes(level_width: int, level_height: int) -> int:
            return level_width * level_height * bytes_per_pixel
    else:
        return payload

    sizes = [level_bytes(max(1, width >> level), max(1, height >> level))
             for level in range(levels)]
    face_count = 6 if flags & 0x4 else 1  # IMG_FLAG_CUBEMAP
    if sum(sizes) * face_count != len(payload):
        # Preserve the source bytes if an uncommon IWI variant does not match
        # the dimensions advertised in its header; validation will still
        # reject a truly malformed record rather than inventing offsets.
        return payload
    if face_count == 1:
        mip_streams: list[bytes] = []
        offset = 0
        # IWI stores level N down to level 0. Read that order, then emit 0..N.
        for level in range(levels - 1, -1, -1):
            size = sizes[level]
            mip_streams.append(payload[offset:offset + size])
            offset += size
        return b"".join(reversed(mip_streams))
    # Cube IWI payloads are mip-major (all six faces at the smallest level,
    # then all six at the next level), while the VitaGL upload bridge consumes
    # face-major chains. Repack only the authored bytes; no cubemap faces are
    # synthesized or dropped.
    face_streams: list[list[bytes]] = [[] for _ in range(face_count)]
    offset = 0
    for level in range(levels - 1, -1, -1):
        size = sizes[level]
        for face in range(face_count):
            face_streams[face].append(payload[offset:offset + size])
            offset += size
    return b"".join(b"".join(reversed(streams)) for streams in face_streams)


def _expand_iwi_bitmap(payload: bytes, image_format: int) -> bytes:
    """Losslessly map source XRGB/A8L8/A8 samples to A8R8G8B8 (BGRA bytes)."""
    if image_format not in (2, 3, 5):
        return payload
    stride = {2: 3, 3: 2, 5: 1}[image_format]
    if len(payload) % stride:
        raise ValueError("truncated uncompressed IWI pixel")
    output = bytearray(len(payload) // stride * 4)
    for pixel, start in enumerate(range(0, len(payload), stride)):
        if image_format == 2:
            color = payload[start:start + 3] + b"\xff"
        elif image_format == 3:
            color = bytes((payload[start],) * 3 + (payload[start + 1],))
        else:
            color = bytes((255, 255, 255, payload[start]))
        output[pixel * 4:pixel * 4 + 4] = color
    return bytes(output)


def _synthesize_images_from_iwd(source_dir: Path, retail_root: Path | None,
                                map_name: str = "shipment",
                                reflection_output: Path | None = None,
                                extra_model_names: set[str] | None = None,
                                color_tail_output: Path | None = None) -> Path:
    """Build a bounded VITI sidecar from read-only retail IWD archives.

    Fastfile image load definitions are often released by the Windows renderer
    before export.  The IWI payloads in the retail archives are the canonical
    source and are already in the compressed DXT format Vita consumes.
    """
    sidecar = source_dir / "vita_images.bin"
    if reflection_output is not None:
        _split_reflection_probe_sidecar(sidecar, reflection_output)
    if retail_root is None:
        return sidecar
    material_path = source_dir / "vita_materials.bin"
    ui_dependencies = source_ui_dependencies(source_dir, Path(__file__).resolve().parents[2] / "src")
    material_lookup = _material_lookup(material_path)
    material_records = {
        record["name"]: record
        for record in _material_records(material_path)
        if record.get("name")
    }
    # VITL v2 exposes the normal/specular semantic aliases as well as the
    # primary color map.  Include every authored image in the bounded IWI
    # closure; v1 sidecars naturally return only their color aliases.
    material_names = _material_image_names(material_path)
    if not material_names:
        return sidecar
    world_materials = _world_surface_material_names(source_dir / "vita_world.bin")
    world_priority_names = [material_lookup[name] for name in world_materials
                            if name in material_lookup]
    # Sky materials are part of the authored map environment, not optional
    # world-image tail data.  Reserve their source color maps before the
    # bounded image envelope is filled by ordinary BSP/model semantics.  The
    # technique comes from the KisakCOD VITL record, so this remains a
    # renderer-wide source rule rather than a map-name or tint exception.
    sky_priority_names: list[str] = []
    for material_name in world_materials:
        record = material_records.get(material_name)
        color = record.get("color", "") if record else ""
        technique = record.get("technique", "").lower() if record else ""
        if color and "sky" in technique and color not in sky_priority_names:
            sky_priority_names.append(color)
    # The desktop renderer binds colorMap/normalMap/specularMap from the same
    # MaterialTextureDef.  Keep a small, deterministic set of the normal and
    # specular aliases used by Shipment's visible BSP materials in the VITI
    # envelope.  These are direct retail IWI names; no normal/specular artwork
    # is synthesized here.  The full semantic closure remains in VITL for the
    # accelerated shader path, while this reservation gives the Vita fallback
    # the source maps needed for its bounded material-lighting bridge.
    world_normal_names: list[str] = []
    world_specular_names: list[str] = []
    for material_name in world_materials:
        record = material_records.get(material_name)
        if not record:
            continue
        normal = record.get("normal", "")
        if normal and normal != "$identitynormalmap" and normal not in world_normal_names:
            world_normal_names.append(normal)
        specular = record.get("specular", "")
        if specular and specular not in world_specular_names:
            world_specular_names.append(specular)
    # World materials include a large long-tail of campaign/editor assets. The
    # following aliases are the source surfaces that define Shipment's
    # containers, ground, grass, fence and industrial walls. Keep their order
    # explicit so archive order and Python hash randomization cannot change
    # which defining maps are visited first. The complete world semantic lists
    # below still retain every normal/specular alias referenced by the parsed
    # KisakCOD material records; this list only establishes the visual-anchor
    # order within that authored closure.
    shipment_semantic_aliases = [
        "me_asphalt_01_n",
        "me_cargocontainerexteriorwall_nml",
        "me_cargocontainerexteriordoor_nml",
        "me_cargocontainertop_nml",
        "me_cargocontainerinteriorwall_nml",
        "me_cargocontainerinteriorwall_flat_nml",
        "me_metal_rusty03_nml",
        "ch_ground_gravel02_nml",
        "ch_grass_02_nml",
        "me_grass_dry_nml",
        "body_wall1_nml",
        "inside_wall_nml",
        "outside_wall_nml",
        "ch_concretewall02_nml",
        "me_chainlink_fence_normal",
        "me_door_07_nml",
    ]
    shipment_specular_aliases = [
        "~me_asphalt_01_s-r-25g-25b-25l-11",
        "~me_cargocontainerexteriorwal~27c3d421",
        "~me_cargocontainerexteriordoo~436ac6b3",
        "~me_cargocontainertop_spc-rgb~4465eb4f",
        "~me_metal_rusty03_spc-rgb&me_~16796c42",
        "~ch_ground_gravel02_spc-rgb&c~dd59f1ea",
        "~ch_grass_02_spc-rgb&ch_grass~3fc77e0f",
    ]
    shipment_normal_priority = [name for name in shipment_semantic_aliases
                                if name in world_normal_names]
    world_normal_names = shipment_normal_priority + [
        name for name in world_normal_names if name not in shipment_normal_priority]
    shipment_specular_priority = [name for name in shipment_specular_aliases
                                  if name in world_specular_names]
    world_specular_names = shipment_specular_priority + [
        name for name in world_specular_names if name not in shipment_specular_priority]
    entity_model_names = _entity_model_names(source_dir / "vita_map_ents.bin")
    # FX model elements are source XModels too, but they are spawned from
    # VFXA graphs rather than map entity text.  The caller supplies those
    # exact names after reading the graph; keep them in the same model
    # material priority path so an authored splinter/debris model does not
    # arrive with geometry but without its color IWI.
    if extra_model_names:
        entity_model_names.update(extra_model_names)
    model_materials = _model_surface_material_names(
        source_dir / "vita_models.bin",
        entity_model_names if entity_model_names else None,
    )
    model_priority_names = [material_lookup[name] for name in model_materials
                            if name in material_lookup]
    model_normal_priority_names = [material_records[name].get("normal", "")
                                   for name in model_materials
                                   if name in material_records
                                   and material_records[name].get("normal", "")
                                   and material_records[name].get("normal", "")
                                       != "$identitynormalmap"]
    model_specular_priority_names = [material_records[name].get("specular", "")
                                     for name in model_materials
                                     if name in material_records
                                     and material_records[name].get("specular", "")]
    # World color maps are the highest visual priority after geometry: without
    # them the complete Shipment sidecar renders as a flat vertex-color proxy.
    # Reserve the authored first-person closure before the broad world list so
    # the bounded record/data caps can never evict the M4 or hands textures.
    # All entries are still direct retail IWI aliases, never generated artwork.
    critical_viewmodel_names = [
        "viewhands_marine_gloves_col",
        "viewhand_black_kit_col",
        "op_force_clr",
        "weapon_parbolic_knife_col",
        "m67_grenade_col",
        "m84_flashbang_col",
        "weapon_m4_col",
        "weapon_acog_col",
        "weapon_acog_alpha_col",
        "weapon_acog_lens_col",
        "weapon_acog_reticle_col",
        "weapon_foregrip_a_col",
        "weapon_holographic_col",
        "weapon_reflex_lens_col",
        "weapon_red_dot_reflexsight",
        "weapon_reflex_sight_col",
        "weapon_mp5_c",
        "weapon_usp_c",
        "weapon_peq6_col",
        "weapon_trinium_site_c",
    ]
    # These final Shipment source aliases are easy to lose at the bounded
    # cutoff because they sort after the broad prop closure. Keep the exact
    # retail maps in the reserved source phase so the BSP and the visible
    # delivery-truck dressing never fall back to generated tints just because
    # optional prop maps were encountered first.
    critical_world_names = [
        "wall_stain5_col",
        "xpec_delivery_truck_body_col",
        "xpec_delivery_truck_glass_col",
        "weapon_stinger_col",
        "case_styroform_col",
        "case_styroform_d_col",
        "plastic_case_bomb_col",
        "plastic_case_bomb_d_col",
        "tntblock_and_timer_col",
    ]
    # Keep the small set of authored static-prop maps that makes the outdoor
    # perimeter read as Shipment (grass blades, fence poles, corrugated trim,
    # pallet/drums and the container hardware) ahead of the long model tail.
    # These are direct KisakCOD material aliases; no substitute geometry or
    # generated texture is introduced when an alias is absent from the retail
    # IWD closure.
    critical_static_model_names = [
        "dry_grass_col#0",
        "light_grass_col#0",
        # The source tree/facade XModels use these exact material maps. Keep
        # them in the reserved static-model phase: when the broad world
        # normal/specular closure fills the bounded IWI envelope first, the
        # pine canopy otherwise reaches Vita as an opaque vertex-colour
        # silhouette even though its authored geometry is present.
        "tree_pine_col",
        "tree_shadowcaster",
        "me_chainlink_fence_pole_color",
        "me_corrugated_metal1_col",
        "me_i_beam_col",
        "pallet_col",
        "cargocontainerlockingbar_col",
        "cargocontainerdoorhinge_col",
        "cargocontainerdoorhingeblue_col",
        "cargocontainerdoorhingegreen_col",
        "cargocontainerdoorhingewhite_col",
        "me_cargocontainerinteriorwall_col",
        "cardboard_box_color",
        "cardboard_box_decal",
        "55_gallon_drum_blue_rust",
        "55_gallon_drum_green_rust",
        "55_gallon_drum_white_rust",
    ]
    # The static XModels above also carry authored tangent-space normal maps.
    # Keep the small set used by Shipment's visible barrels, objective cases,
    # pallets, container hardware, corrugated walls, and phone in the bounded
    # IWI extraction phase.  These are direct KisakCOD aliases; the runtime
    # only consumes them when the source MaterialTextureDef names the slot.
    critical_static_model_normal_names = [
        "rusty_barrel_bump",
        "plastic_case_bomb_d_nml",
        "case_styroform_nml",
        "case_styroform_d_nml",
        "pallet_nml",
        "cargocontainerlockingbar_nml",
        "cargocontainerdoorhinge_nml",
        "me_corrugated_metal1_nml",
        "me_i_beam_norm",
        "me_trim_metal_conduit_norm",
        "cellphone_nml",
        "ch_crates_nml",
        "cardboard_box_normal",
        "tntblock_and_timer_nml",
        "tree_pine_nml",
        "pine02_norm",
        "xpec_delivery_truck_body_nml",
        "xpec_delivery_truck_glass_nml",
    ]
    frontend_priority_names = [
        f"loadscreen_mp_{map_name}",
        f"compass_map_mp_{map_name}",
        "minimap_tickertape_256x16",
        "minimap_tickertape_background",
        "hud_m4carbine",
        "hud_mp5",
        "hud_usp_45",
        "hud_bullets_rifle",
        "hud_bullets_pistol",
        "ammo_counter_bullet",
        "ammo_counter_riflebullet",
        "damage_feedback",
        "explosion_flash",
        "overlay_low_health",
        "background_generic_blurred",
        "menu_screenbg",
        "menu_button",
        "menu_button_selected",
        "gamefonts_pc",
        "devfonts",
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
    frontend_priority_names.extend(name for name in ui_dependencies.get("images", [])
                                   if name not in frontend_priority_names)
    # Farm/Overgrown and Cargo Ship/Wet Work both load the retail weather FX
    # materials from their rawfile closure. These are not BSP color maps, so
    # they otherwise fall through the long-tail image list and are commonly
    # evicted by the bounded source-image envelope. Keep the actual IWI
    # payloads ahead of optional semantic layers; no rain texture is
    # synthesized here.
    weather_priority_names = (
        ["rain_spray", "wind_rain_atlas"]
        if map_name in ("farm", "cargoship") else []
    )
    # Keep the authored first-person and frontend closures ahead of the broad
    # model/world material lists. The bounded 1024-record/96 MiB cap must not
    # evict an exact HUD texture merely because the map has many unique
    # static-model materials; those assets are consumed by the runtime launch
    # gate and by the retail ownerdraw routes.
    priority_names = (critical_viewmodel_names + critical_world_names
                      + sky_priority_names + critical_static_model_names
                      + critical_static_model_normal_names
                      + model_priority_names + model_normal_priority_names
                      + model_specular_priority_names + weather_priority_names)
    priority_names.extend(name for name in frontend_priority_names
                          if name not in priority_names)
    priority_names.extend(name for name in model_priority_names
                          if name not in priority_names)
    priority_names.extend(name for name in world_priority_names
                          if name not in priority_names)
    priority_names.extend(name for name in world_normal_names
                          if name not in priority_names)
    priority_names.extend(name for name in world_specular_names
                          if name not in priority_names)
    ordered_aliases = priority_names + [name for name in material_names if name not in priority_names]
    wanted: dict[str, str] = {}
    for alias in ordered_aliases:
        # Material aliases such as impact_asphalt01_col#0 map to the same IWI
        # basename while retaining the alias as the runtime lookup key.
        basename = re.sub(r"#\d+$", "", alias).lower()
        wanted.setdefault(basename, alias)
    # These are the color slots consumed by the selected BSP, its placed
    # XModels/viewmodels, and the frontend/weather routes.  They are the only
    # records allowed into the streamed color tail: unrelated campaign/UI
    # material aliases must not turn a per-map repair into an all-game image
    # dump.  The aliases remain the exact VITL names and are resolved from the
    # retail IWI archives below.
    color_tail_basenames = {
        re.sub(r"#\d+$", "", alias).lower()
        for alias in (critical_viewmodel_names + critical_world_names
                      + sky_priority_names + critical_static_model_names
                      + model_priority_names + world_priority_names
                      + frontend_priority_names + weather_priority_names)
    }
    # Font atlases live in the language-specific localized IWDs rather than
    # the numbered iw_*.iwd set.  Scan every IWD in the read-only main tree;
    # the deterministic filename order and bounded priority/cap still keep
    # this a small, reproducible Shipment extraction.
    archives = sorted((retail_root / "main").glob("*.iwd"))
    if not archives:
        return sidecar
    # Keep headroom for the authored scoreboard bars and localized font atlas
    # while retaining the complete bounded Shipment world/model closure.
    # VitaImageBundle accepts up to 4096 records, so this is an exporter-side
    # closure bound rather than a runtime parser limit.
    max_records = IMAGE_EXPORT_MAX_RECORDS
    # The scalar first-frame path releases compressed payloads after each
    # source semantic pass, so use the complete parser envelope here. This is
    # still a bounded per-map closure rather than an all-game image dump. The
    # reserved world phase keeps authored wall-stain, specular, and semantic
    # layers in the envelope instead of falling back to a synthetic tint;
    # optional long-tail normal layers yield first.
    max_data_bytes = IMAGE_EXPORT_MAX_DATA_BYTES
    native_special_records = [record for record in _read_image_records(sidecar)
                              if record[0].lower().startswith("*lightmap")
                              or (reflection_output is None
                                  and record[0].lower().startswith(
                                      "*reflection_probe"))]
    # Primary L8 carries the static intensity used below; retain secondary
    # records and generated reflection probes when the bounded budget permits
    # so the scalar/native paths consume the complete source environment input.
    native_special_records.sort(key=lambda record: (
        0 if record[0].lower().startswith("*lightmap")
        and "_primary" in record[0].lower() else
        (1 if record[0].lower().startswith("*lightmap") else 2),
        record[0]))
    records: list[tuple[str, int, int, int, int, int, int, int, bytes]] = []
    deferred_color_records: dict[
        str, tuple[str, int, int, int, int, int, int, int, bytes]
    ] = {}
    total_bytes = 0
    for name, payload, width, height, depth, levels, fourcc, flags in native_special_records:
        if len(records) >= max_records or total_bytes + len(payload) > max_data_bytes:
            continue
        records.append((name, len(payload), width, height, depth, levels, fourcc,
                        flags, payload))
        total_bytes += len(payload)

    def extract_phase(phase: list[str]) -> None:
        nonlocal total_bytes
        # Preserve the caller's priority order.  A set here would make the
        # bounded 96 MiB cutoff depend on Python's per-process hash seed.
        remaining = list(dict.fromkeys(phase))
        for archive in archives:
            if not remaining:
                break
            # Once the startup envelope is full, continue scanning only for
            # authored color-closure aliases. This preserves the exact late
            # BSP/model maps without walking every unrelated material alias.
            if len(records) >= max_records:
                remaining = [basename for basename in remaining
                             if basename in color_tail_basenames]
                if not remaining:
                    break
            try:
                with zipfile.ZipFile(archive) as bundle:
                    candidates: dict[str, zipfile.ZipInfo] = {}
                    # Material aliases may carry a `#N` image variant suffix
                    # while the IWI archive keeps that suffix in the actual
                    # filename (for example
                    # `me_chainlink_fence_color#0.iwi`).  The old exact-stem
                    # lookup stripped the alias suffix but not the archive
                    # stem, silently dropping the chainlink/alpha textures.
                    # Keep both spellings, with deterministic archive-order
                    # selection when an archive contains both forms.
                    for info in sorted(bundle.infolist(),
                                       key=lambda item: item.filename.lower()):
                        if (not info.filename.lower().startswith("images/")
                                or not info.filename.lower().endswith(".iwi")):
                            continue
                        stem = Path(info.filename).stem.lower()
                        candidates.setdefault(stem, info)
                        candidates.setdefault(re.sub(r"#\d+$", "", stem), info)
                    for basename in list(remaining):
                        info = candidates.get(basename)
                        if info is None:
                            continue
                        try:
                            raw = bundle.read(info)
                            if len(raw) < 28:
                                remaining.remove(basename)
                                continue
                            tag, version, image_format, flags, width, height, depth, *sizes = struct.unpack_from(
                                "<3sBBB3h4i", raw
                            )
                            declared = sizes[0]
                            if (tag != b"IWi" or version != 6 or image_format not in (1, 2, 3, 4, 5, 11, 12, 13)
                                    or width <= 0 or height <= 0 or depth != 1
                                    or declared < 28 or declared > len(raw)):
                                remaining.remove(basename)
                                continue
                            payload = raw[28:declared]
                            if not payload or len(payload) > 8 * 1024 * 1024:
                                remaining.remove(basename)
                                continue
                            # IWI format 1 is the uncompressed A8R8G8B8
                            # family used by small authored HUD surfaces;
                            # 11/12/13 are the DXT1/3/5 families.
                            fourcc = {1: 21, 2: 21, 3: 21, 4: 50, 5: 21, 11: 0x31545844, 12: 0x33545844,
                                      13: 0x35545844}[image_format]
                            levels = 1 if flags & 2 else max(width.bit_length(), height.bit_length())
                            payload = _reorder_iwi_mips(payload, width, height,
                                                       levels, image_format, flags)
                            payload = _expand_iwi_bitmap(payload, image_format)
                            alias = wanted.get(basename)
                            if not alias:
                                remaining.remove(basename)
                                continue
                            candidate = (alias, len(payload), width, height,
                                         depth, levels, fourcc, flags, payload)
                            if (len(records) >= max_records
                                    or total_bytes + len(payload) > max_data_bytes):
                                # Keep only a source-referenced color alias in
                                # the companion. Remove it from ``wanted`` so
                                # later broad phases cannot re-read it and
                                # produce a duplicate record.
                                if basename in color_tail_basenames:
                                    deferred_color_records.setdefault(alias,
                                                                       candidate)
                                wanted.pop(basename, None)
                                remaining.remove(basename)
                                continue
                            wanted.pop(basename, None)
                            # Preserve the retail IWI image flags.  In
                            # particular IMG_FLAG_CUBEMAP (0x4) marks the
                            # six-face sp_ambush sky payload; collapsing it to
                            # a 2D record makes the Vita renderer upload only
                            # the first face and turns the sky into a flat
                            # colour field.
                            records.append(candidate)
                            total_bytes += len(payload)
                            remaining.remove(basename)
                        except (struct.error, ValueError, KeyError, zipfile.BadZipFile):
                            if basename in remaining:
                                remaining.remove(basename)
            except (OSError, zipfile.BadZipFile):
                continue

    priority_basenames = [re.sub(r"#\d+$", "", alias).lower() for alias in priority_names]
    critical_basenames = [re.sub(r"#\d+$", "", alias).lower()
                          for alias in critical_viewmodel_names]
    critical_world_basenames = [re.sub(r"#\d+$", "", alias).lower()
                                for alias in critical_world_names]
    critical_static_model_basenames = [re.sub(r"#\d+$", "", alias).lower()
                                       for alias in critical_static_model_names]
    critical_static_model_normal_basenames = [
        re.sub(r"#\d+$", "", alias).lower()
        for alias in critical_static_model_normal_names
    ]
    world_normal_basenames = [re.sub(r"#\d+$", "", alias).lower()
                              for alias in world_normal_names]
    world_specular_basenames = [re.sub(r"#\d+$", "", alias).lower()
                                for alias in world_specular_names]
    frontend_basenames = [re.sub(r"#\d+$", "", alias).lower()
                          for alias in frontend_priority_names]
    sky_basenames = [re.sub(r"#\d+$", "", alias).lower()
                     for alias in sky_priority_names]
    world_basenames = [re.sub(r"#\d+$", "", alias).lower()
                       for alias in world_priority_names]
    weather_basenames = [re.sub(r"#\d+$", "", alias).lower()
                         for alias in weather_priority_names]
    # The localized English IWD containing gamefonts_pc sorts after the
    # numbered world archives.  Extract both authored font atlases first so
    # the bounded record cap cannot spend all slots on map materials and leave
    # the VITF menu/HUD renderer with only its bitmap fallback.
    extract_phase(["gamefonts_pc", "devfonts"])
    # Source renderer utility images are mandatory even when a larger UI
    # closure fills the bounded record envelope before the optional tail.
    extract_phase(["$identitynormalmap", "$white", "$black"])
    # Run the complete frontend closure as its own phase.  Keeping this
    # explicit makes the reservation invariant obvious and prevents a future
    # expansion of the viewmodel/model priority list from consuming the image
    # cap before the exact retail HUD/scoreboard textures are visited.
    extract_phase(critical_basenames)
    extract_phase(critical_world_basenames)
    extract_phase(critical_static_model_basenames)
    extract_phase(frontend_basenames)
    # The map's source sky is a core environment input.  Extract it before
    # weather/model/world closure so a saturated bounded envelope cannot turn
    # the sky surface into the white fallback texture.
    extract_phase(sky_basenames)
    # Keep the source-authored rain spray/atlas images in the same package as
    # the VENV weather flag. They are consumed by the effect batch, not by a
    # map surface, so this phase must run before broad semantic closure.
    extract_phase(weather_basenames)
    # The selected map's placed XModels are part of the authored scene, not an
    # optional tail.  Reserve their color aliases before the broad BSP
    # semantic closure so a large map cannot spend the bounded IWI envelope on
    # normal/specular records and then render its barrels, signs, foliage, and
    # vehicles as vertex-colour silhouettes.  World color maps follow next;
    # both closures fit comfortably below the 64 MiB payload envelope on the
    # stock MP maps, while the later normal/specular phases remain bounded.
    extract_phase([re.sub(r"#\d+$", "", alias).lower()
                   for alias in model_priority_names])
    extract_phase(world_basenames)
    # Color closure is required for a populated world.  Do not let the
    # optional static-model normal priority list consume the bounded envelope
    # before a late window/wood/trash or small model color alias is admitted.
    # Those normal records remain in the semantic tail below when space is
    # available, and the VITL material stream still preserves every authored
    # normal binding for the accelerated path.
    # Follow the desktop material binding order: the BSP color map is followed
    # by the authored specular semantic layer referenced by the selected map's
    # parsed world materials. Specular is source-visible on every lit surface,
    # so it must precede the optional normal-map tail when the bounded source
    # envelope is saturated. Normal aliases then fill the remaining envelope.
    extract_phase(world_specular_basenames)
    extract_phase([re.sub(r"#\d+$", "", alias).lower()
                   for alias in model_specular_priority_names])
    extract_phase(world_normal_basenames)
    extract_phase([re.sub(r"#\d+$", "", alias).lower()
                   for alias in model_normal_priority_names])
    extract_phase(critical_static_model_normal_basenames)
    reserved_basenames = (critical_basenames + critical_world_basenames
                          + sky_basenames
                          + critical_static_model_basenames
                          + critical_static_model_normal_basenames
                          + frontend_basenames + weather_basenames + world_basenames
                          + world_normal_basenames + world_specular_basenames)
    extract_phase([basename for basename in priority_basenames
                   if basename not in reserved_basenames])
    extract_phase(list(wanted))
    if color_tail_output is not None:
        deferred = sorted(deferred_color_records.values(),
                          key=lambda item: item[0])
        _write_image_segments(
            color_tail_output,
            [(name, payload, width, height, depth, levels, fourcc, flags)
             for name, _payload_bytes, width, height, depth, levels, fourcc,
             flags, payload in deferred],
        )
    if not records:
        return sidecar
    records.sort(key=lambda item: item[0])
    _write_image_records(
        sidecar,
        [(name, payload, width, height, depth, levels, fourcc, flags)
         for name, _payload_bytes, width, height, depth, levels, fourcc, flags,
         payload in records],
    )
    return sidecar


def _synthesize_effect_images_from_iwd(
        retail_root: Path | None, aliases: list[str], output: Path,
        map_name: str = "map") -> dict[str, Any]:
    """Emit the exact color images referenced by a matched VFXA graph.

    Effect materials are not part of the BSP/model color priority list.  Keep
    them in a tiny companion VITI stream instead of promoting guessed weather
    aliases into the ordinary map envelope.  The caller supplies names from
    the source VFXA graph after resolving each visual through its VITL
    MaterialTextureDef, so this function only performs exact retail IWI
    extraction.
    """
    ordered_aliases: list[str] = []
    seen: set[str] = set()
    for alias in aliases:
        if not alias or alias in seen:
            continue
        seen.add(alias)
        ordered_aliases.append(alias)

    archives = (sorted((retail_root / "main").glob("*.iwd"))
                if retail_root is not None else [])
    records: list[tuple[str, bytes, int, int, int, int, int, int]] = []
    remaining = list(ordered_aliases)
    if archives and remaining:
        for archive in archives:
            if not remaining:
                break
            try:
                with zipfile.ZipFile(archive) as bundle:
                    candidates: dict[str, zipfile.ZipInfo] = {}
                    for info in sorted(bundle.infolist(),
                                       key=lambda item: item.filename.lower()):
                        filename = info.filename.lower()
                        if (not filename.startswith("images/")
                                or not filename.endswith(".iwi")):
                            continue
                        stem = Path(info.filename).stem.lower()
                        candidates.setdefault(stem, info)
                        # A source alias ending in #0 is commonly represented
                        # by the same retail basename. Do not map #1/#2/etc.
                        # to another numbered source image: that would hide a
                        # real closure omission behind a visually plausible
                        # but incorrect payload.
                        if not re.search(r"#\d+$", stem):
                            candidates.setdefault(stem, info)
                    for alias in list(remaining):
                        lower_alias = alias.lower()
                        basename = re.sub(r"#\d+$", "", lower_alias)
                        info = candidates.get(lower_alias)
                        if info is None and (
                                not re.search(r"#\d+$", lower_alias)
                                or lower_alias.endswith("#0")):
                            info = candidates.get(basename)
                        if info is None:
                            continue
                        try:
                            raw = bundle.read(info)
                            if len(raw) < 28:
                                continue
                            (tag, version, image_format, flags, width, height,
                             depth, *sizes) = struct.unpack_from(
                                 "<3sBBB3h4i", raw)
                            declared = sizes[0]
                            if (tag != b"IWi" or version != 6
                                    or image_format not in (1, 11, 12, 13)
                                    or width <= 0 or height <= 0 or depth != 1
                                    or declared < 28 or declared > len(raw)):
                                continue
                            payload = raw[28:declared]
                            if not payload or len(payload) > 8 * 1024 * 1024:
                                continue
                            fourcc = {
                                1: 21,
                                11: 0x31545844,
                                12: 0x33545844,
                                13: 0x35545844,
                            }[image_format]
                            levels = (1 if flags & 2 else
                                      max(width.bit_length(), height.bit_length()))
                            payload = _reorder_iwi_mips(
                                payload, width, height, levels,
                                image_format, flags)
                            records.append((alias, payload, width, height,
                                            depth, levels, fourcc, flags))
                            remaining.remove(alias)
                        except (struct.error, ValueError, KeyError):
                            continue
            except (OSError, zipfile.BadZipFile):
                continue

    records.sort(key=lambda record: record[0])
    if len(records) > IMAGE_EXPORT_MAX_RECORDS:
        raise ValueError(
            f"{map_name}: effect image closure exceeds the record envelope "
            f"({len(records)}/{IMAGE_EXPORT_MAX_RECORDS})")
    data_bytes = sum(len(record[1]) for record in records)
    if data_bytes > IMAGE_EXPORT_MAX_DATA_BYTES:
        raise ValueError(
            f"{map_name}: effect image closure exceeds the data envelope "
            f"({data_bytes}/{IMAGE_EXPORT_MAX_DATA_BYTES})")
    _write_image_records(output, records)
    return {
        "path": output,
        "bytes": output.stat().st_size,
        "records": len(records),
        "dataBytes": data_bytes,
        "requested": len(ordered_aliases),
        "missing": len(remaining),
        "missingNames": remaining[:32],
        "source": "retail-iwd-vfxa",
    }


def _synthesize_semantic_images_from_iwd(
        source_dir: Path, retail_root: Path | None, map_name: str,
        semantic: str, output: Path, scope: str = "world",
        extra_model_names: set[str] | None = None) -> dict[str, Any]:
    """Emit a complete authored normal/specular VITI companion stream.

    The ordinary map VITI is intentionally a startup stream.  Its 64 MiB
    envelope is enough for the world/model color closure but not for every
    tangent-space and specular alias on a large BSP.  Keep the selected
    closure's exact retail IWI payloads in separate streams so the runtime can
    load one semantic family after the startup bytes have been released.  The
    record names are the VITL MaterialTextureDef aliases, not generated
    basenames; this keeps lookup behavior identical to the desktop material
    binder.  ``scope`` is deliberately explicit: the first streamed pass is
    the selected BSP world, while placed-model/viewmodel closure can be added
    as another streamed phase without bloating the all-map package.
    """
    if semantic not in ("normal", "specular"):
        raise ValueError(f"unsupported semantic image family: {semantic}")
    if scope not in ("world", "model", "all"):
        raise ValueError(f"unsupported semantic image scope: {scope}")
    material_path = source_dir / "vita_materials.bin"
    scoped_materials: set[str] | None = None
    if scope == "world":
        scoped_materials = set(_world_surface_material_names(
            source_dir / "vita_world.bin"))
    elif scope == "model":
        entity_models = _entity_model_names(source_dir / "vita_map_ents.bin")
        # The runtime retains the map-placed models plus every weapon
        # viewmodel needed by the Create-a-Class/spawn path. Include the same
        # source model set here so the optional semantic package cannot leave
        # the hands, rifle, or pistol on a color-only material pass.
        entity_models.update(_weapon_view_model_names(
            source_dir / "vita_weapons.bin"))
        # The melee model is held in WeaponDef.knifeModel, not gunXModel[0].
        # Its material must survive the same first-person closure as the gun.
        entity_models.update({"viewmodel_base_viewhands", "viewhands_black_kit",
                              "viewhands_op_force", "viewmodel_knife"})
        if extra_model_names:
            entity_models.update(extra_model_names)
        scoped_materials = set(_model_surface_material_names(
            source_dir / "vita_models.bin",
            entity_models if entity_models else None))
    aliases: list[str] = []
    seen: set[str] = set()
    for material in _material_records(material_path):
        if (scoped_materials is not None
                and material.get("name", "") not in scoped_materials):
            continue
        alias = material.get(semantic, "")
        if (not alias or (semantic == "normal" and alias == "$identitynormalmap")
                or alias in seen):
            continue
        seen.add(alias)
        aliases.append(alias)

    archives = (sorted((retail_root / "main").glob("*.iwd"))
                if retail_root is not None else [])
    records: list[tuple[str, bytes, int, int, int, int, int, int]] = []
    missing: list[str] = []
    if aliases and archives:
        remaining = list(aliases)
        for archive in archives:
            if not remaining:
                break
            try:
                with zipfile.ZipFile(archive) as bundle:
                    candidates: dict[str, zipfile.ZipInfo] = {}
                    for info in sorted(bundle.infolist(),
                                       key=lambda item: item.filename.lower()):
                        filename = info.filename.lower()
                        if (not filename.startswith("images/")
                                or not filename.endswith(".iwi")):
                            continue
                        stem = Path(info.filename).stem.lower()
                        candidates.setdefault(stem, info)
                        candidates.setdefault(re.sub(r"#\d+$", "", stem), info)
                    for alias in list(remaining):
                        basename = re.sub(r"#\d+$", "", alias).lower()
                        info = candidates.get(basename)
                        if info is None:
                            continue
                        try:
                            raw = bundle.read(info)
                            if len(raw) < 28:
                                continue
                            tag, version, image_format, flags, width, height, depth, *sizes = struct.unpack_from(
                                "<3sBBB3h4i", raw
                            )
                            declared = sizes[0]
                            if (tag != b"IWi" or version != 6
                                    or image_format not in (1, 11, 12, 13)
                                    or width <= 0 or height <= 0 or depth != 1
                                    or declared < 28 or declared > len(raw)):
                                continue
                            payload = raw[28:declared]
                            if not payload or len(payload) > 8 * 1024 * 1024:
                                continue
                            fourcc = {
                                1: 21,
                                11: 0x31545844,
                                12: 0x33545844,
                                13: 0x35545844,
                            }[image_format]
                            levels = (1 if flags & 2 else
                                      max(width.bit_length(), height.bit_length()))
                            payload = _reorder_iwi_mips(
                                payload, width, height, levels,
                                image_format, flags)
                            records.append((alias, payload, width, height, depth,
                                            levels, fourcc, flags))
                            remaining.remove(alias)
                        except (struct.error, ValueError, KeyError):
                            continue
            except (OSError, zipfile.BadZipFile):
                continue
        missing = remaining
    else:
        missing = aliases

    records.sort(key=lambda record: record[0])
    if len(records) > SEMANTIC_EXPORT_MAX_RECORDS:
        raise ValueError(
            f"{map_name}: {semantic} semantic closure exceeds the record "
            f"envelope ({len(records)}/{SEMANTIC_EXPORT_MAX_RECORDS})")
    # A single semantic family can exceed the parser's bounded 96 MiB raw
    # chunk envelope on Crossfire-class maps. Partition only between complete
    # VITI records; the runtime loads each segment in turn after releasing the
    # previous source bytes, so no startup or companion allocation needs to
    # hold the entire family at once.
    segments: list[list[tuple[str, bytes, int, int, int, int, int, int]]] = []
    current: list[tuple[str, bytes, int, int, int, int, int, int]] = []
    current_bytes = 0
    for record in records:
        payload_bytes = len(record[1])
        if payload_bytes > SEMANTIC_EXPORT_MAX_DATA_BYTES:
            raise ValueError(
                f"{map_name}: {semantic} image record exceeds the data "
                f"envelope ({payload_bytes}/{SEMANTIC_EXPORT_MAX_DATA_BYTES})")
        if (current and current_bytes + payload_bytes
                > SEMANTIC_EXPORT_MAX_DATA_BYTES):
            segments.append(current)
            current = []
            current_bytes = 0
        current.append(record)
        current_bytes += payload_bytes
    if current or not segments:
        segments.append(current)
    # Remove only generated segment siblings for this exact output stem. This
    # prevents a previous wider closure from consuming disk space or being
    # mistaken for a current package input after the scope is narrowed.
    output.parent.mkdir(parents=True, exist_ok=True)
    for stale in output.parent.glob(f"{output.stem}-*.bin"):
        stale.unlink()
    segment_info: list[dict[str, Any]] = []
    for segment_index, segment in enumerate(segments):
        segment_path = output if segment_index == 0 else output.with_name(
            f"{output.stem}-{segment_index}{output.suffix}")
        _write_image_records(segment_path, segment)
        segment_info.append({
            "path": segment_path,
            "bytes": segment_path.stat().st_size,
            "records": len(segment),
            "dataBytes": sum(len(item[1]) for item in segment),
        })
    data_bytes = sum(item["dataBytes"] for item in segment_info)
    return {
        "path": output,
        "bytes": sum(item["bytes"] for item in segment_info),
        "records": len(records),
        "dataBytes": data_bytes,
        "requested": len(aliases),
        "missing": len(missing),
        "scope": scope,
        "segments": segment_info,
        # Keep the report compact but actionable when a retail IWD really
        # lacks an alias referenced by VITL.
        "missingNames": missing[:32],
    }


def _validate_material_sidecar(path: Path) -> dict[str, Any] | None:
    """Validate the optional pointer-free VITL v1-v5 material stream."""
    if not path.is_file() or path.stat().st_size == 0:
        return None
    import struct

    data = path.read_bytes()
    if len(data) < 20:
        raise ValueError(f"material sidecar is truncated: {path}")
    magic, version, record_count, data_bytes = struct.unpack_from("<3IQ", data)
    if magic != 0x4C544956 or version not in (1, 2, 3, 4, 5):
        raise ValueError(f"unsupported material sidecar header: {path}")
    offset = 20
    aggregate = 0
    for _ in range(record_count):
        header_bytes = (8 if version == 1 else
                        (32 if version == 2 else
                         (33 if version == 3 else (42 if version == 4 else 52))))
        if offset + header_bytes > len(data):
            raise ValueError(f"material sidecar record header is truncated: {path}")
        if version == 1:
            lengths = struct.unpack_from("<2I", data, offset)
        else:
            lengths = struct.unpack_from("<5I", data, offset)
        name_bytes = lengths[0]
        if (name_bytes == 0 or name_bytes > 4096
                or any(length > 4096 for length in lengths)):
            raise ValueError(f"material sidecar record is invalid: {path}")
        offset += header_bytes
        string_bytes = sum(lengths)
        if offset + string_bytes > len(data):
            raise ValueError(f"material sidecar strings are truncated: {path}")
        offset += string_bytes
        aggregate += string_bytes
    if offset != len(data) or aggregate != data_bytes:
        raise ValueError(f"material sidecar size mismatch: {path}")
    return {"path": path, "bytes": len(data), "records": record_count,
            "dataBytes": data_bytes}


def _validate_font_sidecar(path: Path) -> dict[str, Any] | None:
    """Validate the optional pointer-free VITF font/glyph stream."""
    if not path.is_file() or path.stat().st_size == 0:
        return None
    import struct

    data = path.read_bytes()
    if len(data) < 20:
        raise ValueError(f"font sidecar is truncated: {path}")
    magic, version, record_count, data_bytes = struct.unpack_from("<3IQ", data)
    if magic != 0x46544956 or version not in (1, 2):
        raise ValueError(f"unsupported font sidecar header: {path}")
    if not 0 < record_count <= 64:
        raise ValueError(f"font sidecar record count is invalid: {path}")
    offset = 20
    aggregate = 0
    names: set[bytes] = set()
    record_header_bytes = 24 if version == 2 else 16
    for _ in range(record_count):
        if offset + record_header_bytes > len(data):
            raise ValueError(f"font sidecar record header is truncated: {path}")
        name_bytes, image_bytes, glyph_count, pixel_height = struct.unpack_from(
            "<4I", data, offset
        )
        material_bytes, glow_bytes = struct.unpack_from("<2I", data, offset + 16) if version == 2 else (0, 0)
        if (name_bytes == 0 or name_bytes > 4096 or image_bytes > 4096
                or glyph_count == 0 or glyph_count > 2048 or pixel_height == 0
                or pixel_height > 256 or material_bytes > 4096 or glow_bytes > 4096
                or (version == 2 and material_bytes == 0)):
            raise ValueError(f"font sidecar record is invalid: {path}")
        offset += record_header_bytes
        string_bytes = name_bytes + image_bytes + material_bytes + glow_bytes
        if offset + string_bytes > len(data):
            raise ValueError(f"font sidecar strings are truncated: {path}")
        name = data[offset:offset + name_bytes]
        if name in names or b"\0" in data[offset:offset + string_bytes]:
            raise ValueError(f"font sidecar strings are invalid or duplicated: {path}")
        names.add(name)
        offset += string_bytes
        glyph_bytes = glyph_count * 24
        if offset + glyph_bytes > len(data):
            raise ValueError(f"font sidecar glyphs are truncated: {path}")
        for glyph in range(glyph_count):
            glyph_offset = offset + glyph * 24
            letter, _, _, dx, pixel_width, pixel_glyph_height, _ = struct.unpack_from(
                "<H6B", data, glyph_offset
            )
            s0, t0, s1, t1 = struct.unpack_from("<4f", data, glyph_offset + 8)
            if (dx == 0 or (letter != 32
                            and (pixel_width == 0 or pixel_glyph_height == 0))
                    or not all(0.0 <= value <= 1.0 for value in (s0, t0, s1, t1))):
                raise ValueError(f"font sidecar glyph is invalid: {path}")
        offset += glyph_bytes
        aggregate += string_bytes + glyph_bytes
    if offset != len(data) or aggregate != data_bytes:
        raise ValueError(f"font sidecar size mismatch: {path}")
    return {"path": path, "bytes": len(data), "records": record_count,
            "dataBytes": data_bytes, "version": version}


def _source_effect_dependencies(directory: Path) -> dict[str, Any]:
    source = directory / "vita_source_fx_dependencies.csv"
    if not source.is_file():
        return {"available": False}
    graph_path = directory / "vita_fxdefs.bin"
    graph = vita_effects._read_v2_graph(graph_path)
    definitions = {name: len(graph["graphs"][index]["elements"])
                   for index, pair in enumerate(graph["definitions"])
                   for name in pair if name}
    with source.open(encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != ["name", "sourceDefault", "elements"]:
            raise ValueError("invalid source FX dependency report header")
        rows = list(reader)
    if not rows or len(rows) > 99:
        raise ValueError("invalid source FX dependency count")
    records, seen = [], set()
    for row in rows:
        name = row.get("name", "")
        if (not re.fullmatch(r"[A-Za-z0-9_./-]{1,1023}", name)
                or name.casefold() in seen or row.get("sourceDefault") not in ("0", "1")
                or not row.get("elements", "").isdigit()):
            raise ValueError("invalid source FX dependency record")
        count = int(row["elements"])
        if name not in definitions or definitions[name] != count:
            raise ValueError(f"source FX dependency differs from exported graph: {name}")
        seen.add(name.casefold())
        records.append({"name": name, "sourceDefault": row["sourceDefault"] == "1",
                        "elementCount": count})
    return {"available": True, "format": "source-fx-dependencies-v1",
            "sourceSha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "effectGraphSha256": hashlib.sha256(graph_path.read_bytes()).hexdigest(),
            "requiredCount": len(records),
            "sourceDefaultCount": sum(row["sourceDefault"] for row in records),
            "records": sorted(records, key=lambda row: row["name"])}


def build_package(csv_source: Path, workspace: Path, output: Path,
                  retail_root: Path | None = None) -> dict[str, Any]:
    workspace = workspace.resolve()
    csv_source = csv_source.resolve()
    output = output.resolve()
    csv_source.relative_to(workspace)
    output.relative_to(workspace)
    if not csv_source.is_file():
        raise FileNotFoundError(csv_source)

    rows = _read_rows(csv_source)
    source_hash = hashlib.sha256(csv_source.read_bytes()).hexdigest()
    converted = workspace / "assets" / "converted" / "shipment-db-export"
    world_sidecar = _validate_geometry(csv_source.parent / "vita_world.bin", "world")
    lightgrid_sidecar = _validate_geometry(csv_source.parent / "vita_lightgrid.bin", "lightgrid")
    collision_sidecar = _validate_geometry(csv_source.parent / "vita_collision.bin", "collision")
    entity_sidecar = _validate_entity_sidecar(csv_source.parent / "vita_map_ents.bin")
    weapon_sidecar = _validate_weapon_sidecar(csv_source.parent / "vita_weapons.bin")
    model_sidecar = _validate_model_sidecar(csv_source.parent / "vita_models.bin")
    model_lod_sidecar = _validate_model_lod_sidecar(
        csv_source.parent / "vita_model_lods.bin"
    )
    rawfile_sidecar = _validate_rawfile_sidecar(csv_source.parent / "vita_rawfiles.bin")
    localize_sidecar = _validate_localize_sidecar(csv_source.parent / "vita_localize.bin")
    stringtable_sidecar = _validate_stringtable_sidecar(
        csv_source.parent / "vita_stringtables.bin"
    )
    animation_sidecar = _validate_animation_sidecar(csv_source.parent / "vita_animations.bin")
    effect_path = csv_source.parent / "vita_fxdefs.bin"
    effect_sidecar = (vita_effects.inspect_effect_sidecar(effect_path)
                      if effect_path.is_file() else None)
    source_effect_dependencies = _source_effect_dependencies(csv_source.parent)
    impact_fx_path = csv_source.parent / "vita_impactfx.bin"
    impact_fx_sidecar = _validate_impact_fx_sidecar(impact_fx_path)
    menu_sidecar = _validate_menu_sidecar(csv_source.parent / "vita_menus.bin")
    sound_sidecar = _validate_sound_sidecar(csv_source.parent / "vita_sounds.bin")
    material_sidecar = _validate_material_sidecar(csv_source.parent / "vita_materials.bin")
    font_sidecar = _validate_font_sidecar(csv_source.parent / "vita_fonts.bin")
    map_name = "shipment"
    for row in rows:
        if row["typeName"] == "col_map_mp" and row["name"].startswith("maps/mp/"):
            map_name = Path(row["name"]).stem.removeprefix("mp_")
            break
    primary_light_path = csv_source.parent / "vita_primary_lights.bin"
    primary_light_sidecar = _synthesize_primary_light_sidecar(
        csv_source.parent / "vita_world.bin", primary_light_path)
    if primary_light_sidecar is None:
        primary_light_sidecar = _validate_primary_light_sidecar(primary_light_path)
    image_path = _synthesize_images_from_iwd(
        csv_source.parent, retail_root.resolve() if retail_root else None, map_name)
    image_sidecar = _validate_image_sidecar(image_path)
    hud_path = _build_hud_sidecar(image_path if image_sidecar else None, map_name)
    hud_sidecar = _validate_hud_sidecar(hud_path) if hud_path else None
    has_geometry = world_sidecar is not None and collision_sidecar is not None
    runtime_ready = all((
        has_geometry,
        entity_sidecar is not None,
        weapon_sidecar is not None,
        model_sidecar is not None,
        animation_sidecar is not None,
        effect_sidecar is not None,
        impact_fx_sidecar is not None,
        rawfile_sidecar is not None,
        localize_sidecar is not None,
        stringtable_sidecar is not None,
        menu_sidecar is not None,
        sound_sidecar is not None,
        image_sidecar is not None,
        material_sidecar is not None,
        font_sidecar is not None,
        hud_sidecar is not None,
        primary_light_sidecar is not None,
    ))
    status = ("runtime-ready-v1" if runtime_ready else
              ("reference-only-with-typed-geometry-pending-runtime"
               if has_geometry else "reference-only-pending-typed-db-export"))
    manifest = {
        "format": "KisakCOD Vita Shipment Windows DB export",
        "version": 1,
        "status": status,
        "playable": runtime_ready,
        "recordSchema": "typed-db-name-export-v1",
        "source": {
            "source": csv_source.name,
            "relativeSource": "build/windows/reference/runtime-home/*/main/vita_assets.csv",
            "sourceSha256": source_hash,
        },
        "counts": {},
        "sourceEffectDependencies": source_effect_dependencies,
        "sourceUiDependencies": source_ui_dependencies(
            csv_source.parent, Path(__file__).resolve().parents[2] / "src"),
        "payloadPolicy": ("runtime-ready-v1" if runtime_ready else (
            "typed-name-records-plus-pointer-free-world-collision-material-image-font-rawfile-stringtable-sidecars; "
            "renderable-asset-payload-conversion-pending"
            if has_geometry else
            "names-and-native-type-tags-only; binary-payload-conversion-pending"
        )),
    }
    manifest_path = converted / "manifest.json"

    grouped: dict[str, list[dict[str, str]]] = {}
    unknown: dict[str, int] = {}
    for row in rows:
        chunk = TYPE_TO_CHUNK.get(row["typeName"])
        if chunk is None:
            unknown[row["typeName"]] = unknown.get(row["typeName"], 0) + 1
            continue
        grouped.setdefault(chunk, []).append({
            "name": row["name"],
            "nativeType": row["typeName"],
            "nativeTypeId": row["type"],
            "payloadStatus": "pending-binary-payload-conversion",
        })
    manifest["counts"] = {kind: len(records) for kind, records in sorted(grouped.items())}
    manifest["unknownNativeTypes"] = unknown
    _write_json(manifest_path, manifest)

    strings_path = converted / "strings.json"
    _write_json(strings_path, {
        "format": "KisakCOD Vita typed DB string records",
        "version": 1,
        "status": status,
        "records": [{"typeName": row["typeName"], "name": row["name"]} for row in rows],
    })
    chunks: list[dict[str, str]] = [
        {"type": "manifest", "assetId": "shipment.db.manifest", "path": str(manifest_path)},
        {"type": "strings", "assetId": "shipment.db.strings", "path": str(strings_path)},
    ]
    for kind, records in sorted(grouped.items()):
        # The canonical strings chunk above already contains every DB row;
        # avoid emitting a duplicate KVAP type-2 chunk for the localized
        # subset.
        if kind == "strings":
            continue
        records_path = converted / f"{kind}.json"
        _write_json(records_path, {
            "format": "KisakCOD Vita typed DB asset records",
            "version": 1,
            "assetType": kind,
            "recordSchema": "typed-db-name-export-v1",
            "status": status,
            "records": records,
        })
        chunks.append({
            "type": kind,
            "assetId": f"shipment.db.{kind}",
            "path": str(records_path),
        })

    if has_geometry:
        chunks.extend([
            {"type": "world", "assetId": "shipment.db.world-geometry",
             "path": str(world_sidecar["path"])},
            {"type": "collision", "assetId": "shipment.db.collision-geometry",
             "path": str(collision_sidecar["path"])},
        ])
        if lightgrid_sidecar:
            chunks.append({"type": "world", "assetId": "shipment.db.light-grid",
                           "path": str(lightgrid_sidecar["path"])})
        if entity_sidecar:
            chunks.append({"type": "world", "assetId": "shipment.db.map-entities",
                           "path": str(entity_sidecar["path"])})
        if primary_light_sidecar:
            chunks.append({"type": "primary_lights",
                           "assetId": "shipment.db.primary-lights",
                           "path": str(primary_light_sidecar["path"])})
    if weapon_sidecar:
        chunks.append({"type": "weapon", "assetId": "shipment.db.weapon-definitions",
                       "path": str(weapon_sidecar["path"])})
    if model_sidecar:
        chunks.append({"type": "model", "assetId": "shipment.db.weapon-models",
                       "path": str(model_sidecar["path"])})
    if model_lod_sidecar:
        chunks.append({"type": "model", "assetId": "shipment.db.model-render-lods",
                       "path": str(model_lod_sidecar["path"])})
    if animation_sidecar:
        chunks.append({"type": "animation", "assetId": "shipment.db.weapon-animations",
                       "path": str(animation_sidecar["path"])})
    if effect_sidecar:
        chunks.append({"type": "effects", "assetId": "shipment.db.source-effects",
                       "path": str(effect_path)})
    if impact_fx_sidecar:
        chunks.append({"type": "effects", "assetId": "shipment.db.impact-effects",
                       "path": str(impact_fx_path)})
    if rawfile_sidecar:
        chunks.append({"type": "script", "assetId": "shipment.db.rawfiles",
                       "path": str(rawfile_sidecar["path"])})
    if localize_sidecar:
        chunks.append({"type": "strings", "assetId": "shipment.db.localized-strings",
                       "path": str(localize_sidecar["path"])})
    if stringtable_sidecar:
        chunks.append({"type": "strings", "assetId": "shipment.db.stringtables",
                       "path": str(stringtable_sidecar["path"])})
    if menu_sidecar:
        chunks.append({"type": "menu", "assetId": "shipment.db.menus",
                       "path": str(menu_sidecar["path"])})
    if sound_sidecar:
        chunks.append({"type": "sound", "assetId": "shipment.db.decoded-sounds",
                       "path": str(sound_sidecar["path"])})
    if image_sidecar:
        chunks.append({"type": "image", "assetId": "shipment.db.decoded-images",
                       "path": str(image_sidecar["path"])})
    if material_sidecar:
        chunks.append({"type": "material", "assetId": "shipment.db.material-lookups",
                       "path": str(material_sidecar["path"])})
    if font_sidecar:
        chunks.append({"type": "font", "assetId": "shipment.db.font-glyphs",
                       "path": str(font_sidecar["path"])})
    if hud_sidecar:
        chunks.append({"type": "hud", "assetId": "shipment.db.hud-bindings",
                       "path": str(hud_sidecar["path"])})

    package_manifest = converted / "package-manifest.json"
    _write_json(package_manifest, {
        "sourceHash": source_hash,
        "status": status,
        "playable": runtime_ready,
        "chunks": chunks,
    })
    report = create_package(package_manifest, output, workspace, workspace / "__cod4_root_not_used__")
    report.update({
        "status": status,
        "playable": runtime_ready,
        "recordCounts": manifest["counts"],
        "unknownNativeTypes": unknown,
        "sourceRows": len(rows),
        "geometry": {
            "worldBytes": world_sidecar["bytes"] if world_sidecar else 0,
            "lightGridBytes": lightgrid_sidecar["bytes"] if lightgrid_sidecar else 0,
            "primaryLightBytes": primary_light_sidecar["bytes"] if primary_light_sidecar else 0,
            "primaryLightRecords": primary_light_sidecar["records"] if primary_light_sidecar else 0,
            "collisionBytes": collision_sidecar["bytes"] if collision_sidecar else 0,
            "mapEntityBytes": entity_sidecar["bytes"] if entity_sidecar else 0,
            "weaponBytes": weapon_sidecar["bytes"] if weapon_sidecar else 0,
            "weaponRecords": weapon_sidecar["records"] if weapon_sidecar else 0,
            "modelBytes": model_sidecar["bytes"] if model_sidecar else 0,
            "modelRecords": model_sidecar["records"] if model_sidecar else 0,
            "modelDataBytes": model_sidecar["dataBytes"] if model_sidecar else 0,
            "modelLodBytes": model_lod_sidecar["bytes"] if model_lod_sidecar else 0,
            "modelLodModels": model_lod_sidecar["records"] if model_lod_sidecar else 0,
            "modelLodRecords": model_lod_sidecar["lodRecords"] if model_lod_sidecar else 0,
            "modelLodVertices": model_lod_sidecar["vertices"] if model_lod_sidecar else 0,
            "modelLodIndices": model_lod_sidecar["indices"] if model_lod_sidecar else 0,
            "animationBytes": animation_sidecar["bytes"] if animation_sidecar else 0,
            "animationRecords": animation_sidecar["records"] if animation_sidecar else 0,
            "effectBytes": effect_sidecar["bytes"] if effect_sidecar else 0,
            "effectRecords": effect_sidecar["definitions"] if effect_sidecar else 0,
            "effectGraphRecords": effect_sidecar["graphDefinitions"] if effect_sidecar else 0,
            "impactFxBytes": impact_fx_sidecar["bytes"] if impact_fx_sidecar else 0,
            "impactFxTables": impact_fx_sidecar["tables"] if impact_fx_sidecar else 0,
            "impactFxUniqueEffects": impact_fx_sidecar["uniqueEffects"] if impact_fx_sidecar else 0,
            "rawfileBytes": rawfile_sidecar["bytes"] if rawfile_sidecar else 0,
            "rawfileRecords": rawfile_sidecar["records"] if rawfile_sidecar else 0,
            "rawfileDataBytes": rawfile_sidecar["dataBytes"] if rawfile_sidecar else 0,
            "localizedBytes": localize_sidecar["bytes"] if localize_sidecar else 0,
            "localizedRecords": localize_sidecar["records"] if localize_sidecar else 0,
            "localizedDataBytes": localize_sidecar["dataBytes"] if localize_sidecar else 0,
            "stringTableBytes": stringtable_sidecar["bytes"] if stringtable_sidecar else 0,
            "stringTableRecords": stringtable_sidecar["records"] if stringtable_sidecar else 0,
            "stringTableDataBytes": stringtable_sidecar["dataBytes"] if stringtable_sidecar else 0,
            "menuBytes": menu_sidecar["bytes"] if menu_sidecar else 0,
            "menuRecords": menu_sidecar["records"] if menu_sidecar else 0,
            "menuDataBytes": menu_sidecar["dataBytes"] if menu_sidecar else 0,
            "soundBytes": sound_sidecar["bytes"] if sound_sidecar else 0,
            "soundRecords": sound_sidecar["records"] if sound_sidecar else 0,
            "soundDataBytes": sound_sidecar["dataBytes"] if sound_sidecar else 0,
            "imageBytes": image_sidecar["bytes"] if image_sidecar else 0,
            "imageRecords": image_sidecar["records"] if image_sidecar else 0,
            "imageDataBytes": image_sidecar["dataBytes"] if image_sidecar else 0,
            "materialBytes": material_sidecar["bytes"] if material_sidecar else 0,
            "materialRecords": material_sidecar["records"] if material_sidecar else 0,
            "materialDataBytes": material_sidecar["dataBytes"] if material_sidecar else 0,
            "fontBytes": font_sidecar["bytes"] if font_sidecar else 0,
            "fontRecords": font_sidecar["records"] if font_sidecar else 0,
            "fontDataBytes": font_sidecar["dataBytes"] if font_sidecar else 0,
            "hudBytes": hud_sidecar["bytes"] if hud_sidecar else 0,
            "hudRecords": hud_sidecar["records"] if hud_sidecar else 0,
            "hudDataBytes": hud_sidecar["dataBytes"] if hud_sidecar else 0,
        },
    })
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--retail-root", type=Path,
                        help="Optional read-only Call of Duty 4 install for IWI extraction")
    args = parser.parse_args()
    try:
        print(json.dumps(build_package(args.csv, args.workspace, args.output, args.retail_root), indent=2))
        return 0
    except (OSError, ValueError, KeyError) as exc:
        parser.error(str(exc))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
