#!/usr/bin/env python3
"""Extract source-authored multiplayer FX placements into a VFXA sidecar.

The retail map FX scripts contain two different pieces of data that cannot be
reconstructed from a single ``loadfx`` name: the effect definition/path and
the authored createfx origin/angles/delay (plus the looped ambient sound
anchors).  VENV intentionally remains the compact atmosphere summary; VFXA
keeps this larger, pointer-free closure separate so old VENV readers remain
compatible.
"""

from __future__ import annotations

import math
import re
import struct
from pathlib import Path
from typing import Any, Iterable, Mapping

MAGIC = b"VFXA"
VERSION = 1
GRAPH_VERSION = 1

# Header: magic, version, map index, definition count, effect placement count,
# loop sound count, string table bytes, reserved.
HEADER = struct.Struct("<4s7I")
HEADER_V2 = struct.Struct("<4s15I")
DEFINITION = struct.Struct("<2I")
PLACEMENT = struct.Struct("<II3f3ff")
LOOP_SOUND = struct.Struct("<I3f3f")
GRAPH_EFFECT = struct.Struct("<9I")
GRAPH_VISUAL = struct.Struct("<I")
GRAPH_VELOCITY_SAMPLE = struct.Struct("<24f")
GRAPH_VISUAL_SAMPLE = struct.Struct("<4B5f4B5f")
GRAPH_TRAIL_VERTEX = struct.Struct("<5f")
GRAPH_TRAIL_INDEX = struct.Struct("<H")
GRAPH_ELEMENT_V2 = struct.Struct(
    "<20I f 2f 2f 2f f 2i 2i 6f 2f 2f 6f 6f 2f 2f 2f "
    "7I 3f 3f 2f 2f 2i")
GRAPH_ELEMENT_V3 = struct.Struct(
    "<20I f 2f 2f 2f f 2i 2i 6f 2f 2f 6f 6f 2f 2f 2f "
    "7I 3f 3f 2f 2f 2i 3i I")

EFFECT_ONESHOT = 1 << 0
EFFECT_LOOP = 1 << 1
EFFECT_EXPLODER = 1 << 2

MAX_DEFINITIONS = 4096
MAX_PLACEMENTS = 4096
MAX_LOOP_SOUNDS = 4096
MAX_STRING_BYTES = 1 * 1024 * 1024
MAX_NAME_BYTES = 1024
MAX_PATH_BYTES = 2048
MAX_SOUND_BYTES = 1024

MAX_GRAPH_ELEMENTS = 131_072
MAX_GRAPH_VISUALS = 131_072
MAX_GRAPH_VELOCITY_SAMPLES = 262_144
MAX_GRAPH_VISUAL_SAMPLES = 262_144
MAX_GRAPH_TRAIL_VERTICES = 131_072
MAX_GRAPH_TRAIL_INDICES = 262_144

_NUMBER = r"[-+0-9.eE]+"
_CREATE_CALL = re.compile(
    r"(?P<call>createOneshotEffect|createLoopEffect|createNewExploder|"
    r"createLoopSound)\s*\((?P<argument>[^)]*)\)\s*;"
    r"(?P<body>.*?)(?=\n\s*ent\s*=|\Z)", re.IGNORECASE | re.DOTALL)
_VECTOR = re.compile(
    rf"\[?\(\s*({_NUMBER})\s*,\s*({_NUMBER})\s*,\s*({_NUMBER})\s*\)\]?")
_ASSIGNMENT = re.compile(
    rf"\[\s*\"(?P<key>origin|angles|fxid|soundalias|delay)\"\s*\]"
    rf"\s*=\s*(?:\(\s*(?P<x>{_NUMBER})\s*,\s*(?P<y>{_NUMBER})\s*,\s*(?P<z>{_NUMBER})\s*\)"
    rf"|\"(?P<string>[^\"]*)\""
    rf"|(?P<number>{_NUMBER}))", re.IGNORECASE)
_LOADFX = re.compile(
    r"(?:\[\s*\"(?P<alias>[^\"]+)\"\s*\])?\s*=\s*"
    r"loadfx\s*\(\s*\"(?P<path>[^\"]+)\"\s*\)", re.IGNORECASE)


def _rawfiles(path: Path) -> dict[str, bytes]:
    data = path.read_bytes()
    if len(data) < 20:
        raise ValueError(f"rawfile sidecar is truncated: {path}")
    magic, version, count, declared = struct.unpack_from("<3IQ", data)
    if magic != 0x52544956 or version != 1 or count > 4096:
        raise ValueError(f"unsupported rawfile sidecar: {path}")
    offset = 20
    result: dict[str, bytes] = {}
    aggregate = 0
    for _ in range(count):
        if offset + 8 > len(data):
            raise ValueError(f"rawfile record header is truncated: {path}")
        name_bytes, payload_bytes = struct.unpack_from("<2I", data, offset)
        offset += 8
        if name_bytes == 0 or name_bytes > 4096 or payload_bytes > 32 * 1024 * 1024:
            raise ValueError(f"rawfile record is invalid: {path}")
        if offset + name_bytes + payload_bytes > len(data):
            raise ValueError(f"rawfile record exceeds sidecar: {path}")
        name = data[offset:offset + name_bytes].decode("utf-8", "ignore")
        offset += name_bytes
        result[name] = bytes(data[offset:offset + payload_bytes])
        offset += payload_bytes
        aggregate += payload_bytes
    if offset != len(data) or aggregate != declared:
        raise ValueError(f"rawfile sidecar size mismatch: {path}")
    return result


def _text(rawfiles: Mapping[str, bytes], name: str) -> str:
    return rawfiles.get(name, b"").decode("utf-8", "ignore").replace("\r", "")


def _finite_vector(values: list[float], label: str) -> tuple[float, float, float]:
    if len(values) != 3 or not all(math.isfinite(value) for value in values):
        raise ValueError(f"{label} is not a finite 3-vector")
    if any(abs(value) > 1_000_000.0 for value in values):
        raise ValueError(f"{label} is outside the authored map bounds")
    return values[0], values[1], values[2]


def _value_assignments(body: str) -> dict[str, Any]:
    values: dict[str, Any] = {}
    for match in _ASSIGNMENT.finditer(body):
        key = match.group("key").lower()
        if match.group("string") is not None:
            values[key] = match.group("string")
        elif match.group("number") is not None:
            values[key] = float(match.group("number"))
        else:
            values[key] = [float(match.group(axis)) for axis in ("x", "y", "z")]
    return values


def _loadfx_definitions(texts: list[str]) -> list[tuple[str, str]]:
    definitions: dict[str, str] = {}
    for text in texts:
        for match in _LOADFX.finditer(text):
            path = match.group("path").strip()
            alias = (match.group("alias") or path.rsplit("/", 1)[-1]).strip()
            if not alias or not path:
                continue
            # The first source declaration wins, matching the script's
            # deterministic level._effect table rather than host map order.
            definitions.setdefault(alias, path)
    return list(definitions.items())


def _effect_records(rawfiles: Mapping[str, bytes], map_name: str) -> tuple[
    list[tuple[str, str]], list[tuple[str, tuple[float, float, float], tuple[float, float, float], float, int]],
    list[tuple[str, tuple[float, float, float], tuple[float, float, float]]],
    int,
]:
    map_fx_name = f"maps/mp/mp_{map_name}_fx.gsc"
    createfx_name = f"maps/createfx/mp_{map_name}_fx.gsc"
    # The map closure declares its own effects first; _global_fx provides
    # authored fire/light haze definitions used by prefab anchors.
    definition_texts = [_text(rawfiles, map_fx_name),
                        _text(rawfiles, "maps/mp/_global_fx.gsc")]
    definitions = _loadfx_definitions(definition_texts)
    definition_by_name = {name: index for index, (name, _) in enumerate(definitions)}

    placements: list[tuple[str, tuple[float, float, float], tuple[float, float, float], float, int]] = []
    sounds: list[tuple[str, tuple[float, float, float], tuple[float, float, float]]] = []
    malformed = 0
    createfx = _text(rawfiles, createfx_name)
    for match in _CREATE_CALL.finditer(createfx):
        call = match.group("call").lower()
        values = _value_assignments(match.group("body"))
        origin = values.get("origin", [0.0, 0.0, 0.0])
        angles = values.get("angles", [0.0, 0.0, 0.0])
        try:
            origin_tuple = _finite_vector(origin, "effect origin")
            angles_tuple = _finite_vector(angles, "effect angles")
        except (TypeError, ValueError):
            malformed += 1
            continue
        if call == "createloopsound":
            alias = values.get("soundalias", "")
            if not isinstance(alias, str) or not alias:
                malformed += 1
                continue
            sounds.append((alias, origin_tuple, angles_tuple))
            if len(sounds) > MAX_LOOP_SOUNDS:
                raise ValueError(f"{map_name}: loop sound placement limit exceeded")
            continue

        alias = values.get("fxid", "")
        if not isinstance(alias, str) or not alias:
            argument = match.group("argument").strip()
            alias = argument.strip('" ')
        if not alias:
            malformed += 1
            continue
        if alias not in definition_by_name:
            # Preserve the source placement even when an older rawfile dump
            # omitted the declaration. An empty path is surfaced by the
            # runtime closure audit instead of silently substituting a prop.
            definition_by_name[alias] = len(definitions)
            definitions.append((alias, ""))
        delay = values.get("delay", 0.0)
        if not isinstance(delay, (int, float)) or not math.isfinite(float(delay)):
            malformed += 1
            continue
        flags = EFFECT_ONESHOT
        if call == "createloopeffect":
            flags = EFFECT_LOOP
        elif call == "createnewexploder":
            flags = EFFECT_EXPLODER | EFFECT_ONESHOT
        placements.append((alias, origin_tuple, angles_tuple, float(delay), flags))
        if len(placements) > MAX_PLACEMENTS:
            raise ValueError(f"{map_name}: FX placement limit exceeded")

    if len(definitions) > MAX_DEFINITIONS:
        raise ValueError(f"{map_name}: effect definition limit exceeded")
    return definitions, placements, sounds, malformed


def _string_table(definitions: list[tuple[str, str]],
                  placements: list[tuple[str, tuple[float, float, float], tuple[float, float, float], float, int]],
                  sounds: list[tuple[str, tuple[float, float, float], tuple[float, float, float]]],
                  graph_records: list[tuple[int, dict[str, Any]]] | None = None) -> tuple[bytes, dict[str, int]]:
    table = bytearray(b"\0")
    offsets: dict[str, int] = {"": 0}

    def intern(value: str, maximum: int) -> int:
        if "\0" in value:
            raise ValueError("effect string contains NUL")
        encoded = value.encode("utf-8")
        if not value or len(encoded) > maximum:
            raise ValueError("effect string exceeds its bounded field")
        if value in offsets:
            return offsets[value]
        offset = len(table)
        table.extend(encoded)
        table.append(0)
        offsets[value] = offset
        return offset

    for name, path in definitions:
        intern(name, MAX_NAME_BYTES)
        if path:
            intern(path, MAX_PATH_BYTES)
    for alias, *_ in placements:
        intern(alias, MAX_NAME_BYTES)
    for alias, *_ in sounds:
        intern(alias, MAX_SOUND_BYTES)
    for _effect_index, graph in graph_records or []:
        for element in graph.get("elements", []):
            for value in element.get("visuals", []):
                if value:
                    intern(value, MAX_PATH_BYTES)
            for key in ("effectOnImpact", "effectOnDeath", "effectEmitted"):
                value = element.get(key, "")
                if value:
                    intern(value, MAX_PATH_BYTES)
    if len(table) > MAX_STRING_BYTES:
        raise ValueError("effect string table exceeds its bounded size")
    return bytes(table), offsets


def _within(offset: int, count: int, total: int) -> bool:
    return 0 <= offset <= total and 0 <= count <= total - offset


def _graph_element_from_values(values: tuple[Any, ...],
                               version: int,
                               visual_names: list[str],
                               velocity_samples: list[tuple[float, ...]],
                               visual_samples: list[tuple[Any, ...]],
                               trail_vertices: list[tuple[float, ...]],
                               trail_indices: list[int]) -> dict[str, Any]:
    cursor = 0

    def take(count: int) -> tuple[Any, ...]:
        nonlocal cursor
        result = values[cursor:cursor + count]
        cursor += count
        return result

    integers = take(20)
    element: dict[str, Any] = {
        "flags": integers[0],
        "elemType": integers[1],
        "visualCount": integers[2],
        "velIntervalCount": integers[3],
        "visStateIntervalCount": integers[4],
        "visualOffset": integers[5],
        "visualRefCount": integers[6],
        "velocityOffset": integers[7],
        "velocityCount": integers[8],
        "visualSampleOffset": integers[9],
        "visualSampleCount": integers[10],
        "trailVertexOffset": integers[11],
        "trailVertexCount": integers[12],
        "trailIndexOffset": integers[13],
        "trailIndexCount": integers[14],
        "effectOnImpactOffset": integers[15],
        "effectOnDeathOffset": integers[16],
        "effectEmittedOffset": integers[17],
        "sortOrder": integers[18],
        "useItemClip": integers[19],
        "lightingFrac": take(1)[0],
        "spawnRange": take(2),
        "fadeInRange": take(2),
        "fadeOutRange": take(2),
        "spawnFrustumCullRadius": take(1)[0],
        "spawnDelayMsec": take(2),
        "lifeSpanMsec": take(2),
        "spawnOrigin": take(6),
        "spawnOffsetRadius": take(2),
        "spawnOffsetHeight": take(2),
        "spawnAngles": take(6),
        "angularVelocity": take(6),
        "initialRotation": take(2),
        "gravity": take(2),
        "reflectionFactor": take(2),
        "atlas": take(7),
        "collMins": take(3),
        "collMaxs": take(3),
        "emitDist": take(2),
        "emitDistVariance": take(2),
        "spawnDef": take(2),
    }
    if version >= 3:
        element["trailScrollTimeMsec"] = take(1)[0]
        element["trailRepeatDist"] = take(1)[0]
        element["trailSplitDist"] = take(1)[0]
        element["trailPresent"] = bool(take(1)[0])
    else:
        element["trailScrollTimeMsec"] = 0
        element["trailRepeatDist"] = 0
        element["trailSplitDist"] = 0
        element["trailPresent"] = False
    if cursor != len(values):
        raise ValueError("VFXA graph element record has an unexpected shape")
    element["visuals"] = visual_names
    element["velocitySamples"] = velocity_samples
    element["visualSamples"] = visual_samples
    element["trailVertices"] = trail_vertices
    element["trailIndices"] = trail_indices
    return element


def _graph_element_float_groups(element: Mapping[str, Any]) -> list[float]:
    groups: list[float] = []
    for key in (
        "lightingFrac", "spawnRange", "fadeInRange", "fadeOutRange",
        "spawnFrustumCullRadius", "spawnOrigin", "spawnOffsetRadius",
        "spawnOffsetHeight", "spawnAngles", "angularVelocity",
        "initialRotation", "gravity", "reflectionFactor", "collMins",
        "collMaxs", "emitDist", "emitDistVariance",
        "spawnDef",
    ):
        value = element.get(key, ())
        if isinstance(value, (int, float)):
            groups.append(float(value))
        else:
            groups.extend(float(item) for item in value)
    for sample in element.get("velocitySamples", []):
        groups.extend(float(item) for item in sample)
    for sample in element.get("visualSamples", []):
        for item in sample[4:9]:
            groups.append(float(item))
        for item in sample[13:18]:
            groups.append(float(item))
    for vertex in element.get("trailVertices", []):
        groups.extend(float(item) for item in vertex)
    return groups


def _read_v2_graph(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    if len(data) < HEADER_V2.size:
        raise ValueError(f"VFXA v2 sidecar is truncated: {path}")
    header = HEADER_V2.unpack_from(data)
    (magic, version, map_index, definitions, placements, sounds, string_bytes,
     graph_definitions, graph_elements, graph_visuals, graph_velocity_samples,
     graph_visual_samples, graph_trail_vertices, graph_trail_indices,
     graph_bytes, reserved) = header
    if magic != MAGIC or version not in (2, 3) or reserved != 0:
        raise ValueError(f"unsupported VFXA graph sidecar: {path}")
    graph_element = GRAPH_ELEMENT_V3 if version == 3 else GRAPH_ELEMENT_V2
    if (definitions == 0 or definitions > MAX_DEFINITIONS
        or placements > MAX_PLACEMENTS or sounds > MAX_LOOP_SOUNDS
        or graph_definitions > definitions
        or graph_elements > MAX_GRAPH_ELEMENTS
        or graph_visuals > MAX_GRAPH_VISUALS
        or graph_velocity_samples > MAX_GRAPH_VELOCITY_SAMPLES
        or graph_visual_samples > MAX_GRAPH_VISUAL_SAMPLES
        or graph_trail_vertices > MAX_GRAPH_TRAIL_VERTICES
        or graph_trail_indices > MAX_GRAPH_TRAIL_INDICES):
        raise ValueError(f"VFXA v2 record count is too large: {path}")
    expected_graph_bytes = (
        graph_definitions * GRAPH_EFFECT.size
        + graph_elements * graph_element.size
        + graph_visuals * GRAPH_VISUAL.size
        + graph_velocity_samples * GRAPH_VELOCITY_SAMPLE.size
        + graph_visual_samples * GRAPH_VISUAL_SAMPLE.size
        + graph_trail_vertices * GRAPH_TRAIL_VERTEX.size
        + graph_trail_indices * GRAPH_TRAIL_INDEX.size)
    if graph_bytes != expected_graph_bytes or graph_bytes > 4 * 1024 * 1024:
        raise ValueError(f"VFXA v2 graph size mismatch: {path}")
    records_end = (HEADER_V2.size + definitions * DEFINITION.size
                   + placements * PLACEMENT.size + sounds * LOOP_SOUND.size)
    if (string_bytes == 0 or string_bytes > MAX_STRING_BYTES
        or records_end + string_bytes + graph_bytes != len(data)):
        raise ValueError(f"VFXA v2 sidecar size mismatch: {path}")
    string_base = records_end
    definitions_out: list[tuple[str, str]] = []
    for index in range(definitions):
        name_offset, path_offset = DEFINITION.unpack_from(
            data, HEADER_V2.size + index * DEFINITION.size)
        name = _read_string(data, string_base, string_bytes, name_offset,
                             MAX_NAME_BYTES, "effect name")
        effect_path = (_read_string(data, string_base, string_bytes,
                                    path_offset, MAX_PATH_BYTES, "effect path")
                       if path_offset else "")
        definitions_out.append((name, effect_path))

    placements_offset = HEADER_V2.size + definitions * DEFINITION.size
    for index in range(placements):
        effect_index, flags, *values = PLACEMENT.unpack_from(
            data, placements_offset + index * PLACEMENT.size)
        if (effect_index >= definitions or flags == 0
            or not all(math.isfinite(value) for value in values)):
            raise ValueError(f"VFXA v2 placement {index} is invalid")
    sounds_offset = placements_offset + placements * PLACEMENT.size
    for index in range(sounds):
        alias_offset, *values = LOOP_SOUND.unpack_from(
            data, sounds_offset + index * LOOP_SOUND.size)
        _read_string(data, string_base, string_bytes, alias_offset,
                     MAX_SOUND_BYTES, "loop sound alias")
        if not all(math.isfinite(value) for value in values):
            raise ValueError(f"VFXA v2 loop sound {index} is invalid")

    graph_base = string_base + string_bytes
    graph_effect_base = graph_base
    graph_element_base = graph_effect_base + graph_definitions * GRAPH_EFFECT.size
    graph_visual_base = graph_element_base + graph_elements * graph_element.size
    graph_velocity_base = graph_visual_base + graph_visuals * GRAPH_VISUAL.size
    graph_visual_sample_base = (graph_velocity_base
                                + graph_velocity_samples
                                * GRAPH_VELOCITY_SAMPLE.size)
    graph_trail_vertex_base = (graph_visual_sample_base
                               + graph_visual_samples
                               * GRAPH_VISUAL_SAMPLE.size)
    graph_trail_index_base = (graph_trail_vertex_base
                              + graph_trail_vertices
                              * GRAPH_TRAIL_VERTEX.size)
    if graph_trail_index_base + graph_trail_indices * GRAPH_TRAIL_INDEX.size != len(data):
        raise ValueError(f"VFXA v2 graph sections are not contiguous: {path}")

    all_visuals: list[str] = []
    for index in range(graph_visuals):
        offset = GRAPH_VISUAL.unpack_from(
            data, graph_visual_base + index * GRAPH_VISUAL.size)[0]
        all_visuals.append(_read_string(data, string_base, string_bytes,
                                         offset, MAX_PATH_BYTES,
                                         "effect visual"))
    all_velocity_samples = [
        GRAPH_VELOCITY_SAMPLE.unpack_from(
            data, graph_velocity_base + index * GRAPH_VELOCITY_SAMPLE.size)
        for index in range(graph_velocity_samples)]
    all_visual_samples = [
        GRAPH_VISUAL_SAMPLE.unpack_from(
            data, graph_visual_sample_base + index * GRAPH_VISUAL_SAMPLE.size)
        for index in range(graph_visual_samples)]
    all_trail_vertices = [
        GRAPH_TRAIL_VERTEX.unpack_from(
            data, graph_trail_vertex_base + index * GRAPH_TRAIL_VERTEX.size)
        for index in range(graph_trail_vertices)]
    all_trail_indices = [
        GRAPH_TRAIL_INDEX.unpack_from(
            data, graph_trail_index_base + index * GRAPH_TRAIL_INDEX.size)[0]
        for index in range(graph_trail_indices)]

    elements_out: list[dict[str, Any]] = []
    for index in range(graph_elements):
        values = graph_element.unpack_from(
            data, graph_element_base + index * graph_element.size)
        visual_offset, visual_ref_count = int(values[5]), int(values[6])
        velocity_offset, velocity_count = int(values[7]), int(values[8])
        visual_sample_offset, visual_sample_count = int(values[9]), int(values[10])
        trail_vertex_offset, trail_vertex_count = int(values[11]), int(values[12])
        trail_index_offset, trail_index_count = int(values[13]), int(values[14])
        if (values[1] > 10 or values[2] > 32 or visual_ref_count > 64
            or not _within(visual_offset, visual_ref_count, graph_visuals)
            or not _within(velocity_offset, velocity_count,
                           graph_velocity_samples)
            or not _within(visual_sample_offset, visual_sample_count,
                           graph_visual_samples)
            or not _within(trail_vertex_offset, trail_vertex_count,
                           graph_trail_vertices)
            or not _within(trail_index_offset, trail_index_count,
                           graph_trail_indices)
            or (version == 3
                and velocity_count != int(values[3]) + 1)
            or (version == 2 and velocity_count != 0
                and velocity_count != int(values[3]) + 1)
            or (version == 3 and int(values[1]) == 10
                and visual_sample_count != 0)
            or (version == 3 and int(values[1]) != 10
                and visual_sample_count != int(values[4]) + 1)
            or (version == 2 and visual_sample_count != 0
                and visual_sample_count != int(values[4]) + 1)):
            raise ValueError(f"VFXA v2 element {index} is invalid")
        element = _graph_element_from_values(
            values, version,
            all_visuals[visual_offset:visual_offset + visual_ref_count],
            all_velocity_samples[velocity_offset:velocity_offset + velocity_count],
            all_visual_samples[visual_sample_offset:
                               visual_sample_offset + visual_sample_count],
            all_trail_vertices[trail_vertex_offset:
                               trail_vertex_offset + trail_vertex_count],
            all_trail_indices[trail_index_offset:
                              trail_index_offset + trail_index_count])
        # The three offsets are the 16th through 18th uint32 in the fixed
        # record; preserve their decoded strings separately from the integer
        # offsets used while validating the file.
        element["effectOnImpact"] = _read_string(
            data, string_base, string_bytes, int(values[15]), MAX_PATH_BYTES,
            "effectOnImpact") if values[15] else ""
        element["effectOnDeath"] = _read_string(
            data, string_base, string_bytes, int(values[16]), MAX_PATH_BYTES,
            "effectOnDeath") if values[16] else ""
        element["effectEmitted"] = _read_string(
            data, string_base, string_bytes, int(values[17]), MAX_PATH_BYTES,
            "effectEmitted") if values[17] else ""
        elem_type = int(element.get("elemType", 0))
        visual_count = int(element.get("visualCount", 0))
        visual_ref_count = len(element.get("visuals", []))
        # FxElemType 6/7 are light and spot-light elements.  Their authored
        # visualCount selects state samples but the retail layout has no
        # visual pointer to serialize.  Keep the same closure rule as the
        # Vita reader: drawable elements need one reference per visual, and
        # marks may carry two material slots per mark.
        visual_references_valid = (
            elem_type in (6, 7) and visual_ref_count == 0
        ) or (
            elem_type == 9 and (visual_ref_count == visual_count * 2
                                if version == 3
                                else visual_ref_count <= visual_count * 2)
        ) or (
            elem_type not in (6, 7, 9) and visual_ref_count == visual_count
        )
        if not visual_references_valid:
            raise ValueError(f"VFXA v2 element {index} has invalid visual closure")
        if version == 3:
            trail_present = bool(element["trailPresent"])
            if trail_present != (elem_type == 3):
                raise ValueError(f"VFXA v3 element {index} has invalid trail ownership")
            if (not trail_present and
                    (element["trailVertices"] or element["trailIndices"]
                     or element["trailScrollTimeMsec"] != 0
                     or element["trailRepeatDist"] != 0
                     or element["trailSplitDist"] != 0)):
                raise ValueError(f"VFXA v3 element {index} has stray trail data")
        if not all(math.isfinite(value) for value in _graph_element_float_groups(element)):
            raise ValueError(f"VFXA v2 element {index} contains a non-finite value")
        if any(index >= trail_vertex_count for index in element["trailIndices"]):
            raise ValueError(f"VFXA v2 element {index} has an invalid trail index")
        elements_out.append(element)

    graphs: dict[int, dict[str, Any]] = {}
    for index in range(graph_definitions):
        values = GRAPH_EFFECT.unpack_from(
            data, graph_effect_base + index * GRAPH_EFFECT.size)
        effect_index, flags, looping_life, looping, oneshot, emission, element_offset, element_count, reserved_effect = values
        if (effect_index >= definitions or reserved_effect != 0
            or effect_index in graphs
            or not _within(element_offset, element_count, graph_elements)
            or element_count > 32
            or looping + oneshot + emission != element_count):
            raise ValueError(f"VFXA v2 graph definition {index} is invalid")
        graphs[int(effect_index)] = {
            "flags": flags,
            "msecLoopingLife": looping_life,
            "elemDefCountLooping": looping,
            "elemDefCountOneShot": oneshot,
            "elemDefCountEmission": emission,
            "elements": elements_out[element_offset:element_offset + element_count],
        }
    return {
        "path": str(path),
        "version": version,
        "mapIndex": map_index,
        "definitions": definitions_out,
        "graphs": graphs,
        "placements": placements,
        "loopSounds": sounds,
        "graphDefinitions": graph_definitions,
        "graphElements": graph_elements,
        "graphVisuals": graph_visuals,
        "graphVelocitySamples": graph_velocity_samples,
        "graphVisualSamples": graph_visual_samples,
        "graphTrailVertices": graph_trail_vertices,
        "graphTrailIndices": graph_trail_indices,
    }


def _pack_graph_element(element: Mapping[str, Any], offsets: Mapping[str, int],
                        visual_offset: int, velocity_offset: int,
                        visual_sample_offset: int, trail_vertex_offset: int,
                        trail_index_offset: int) -> bytes:
    visuals = list(element.get("visuals", []))
    velocities = list(element.get("velocitySamples", []))
    visual_samples = list(element.get("visualSamples", []))
    trail_vertices = list(element.get("trailVertices", []))
    trail_indices = list(element.get("trailIndices", []))
    elem_type = int(element.get("elemType", 0))
    return GRAPH_ELEMENT_V3.pack(
        int(element.get("flags", 0)), elem_type,
        int(element.get("visualCount", len(visuals))),
        int(element.get("velIntervalCount", 0)),
        int(element.get("visStateIntervalCount", 0)), visual_offset,
        len(visuals), velocity_offset, len(velocities), visual_sample_offset,
        len(visual_samples), trail_vertex_offset, len(trail_vertices),
        trail_index_offset, len(trail_indices),
        offsets.get(str(element.get("effectOnImpact", "")), 0),
        offsets.get(str(element.get("effectOnDeath", "")), 0),
        offsets.get(str(element.get("effectEmitted", "")), 0),
        int(element.get("sortOrder", 0)), int(element.get("useItemClip", 0)),
        float(element.get("lightingFrac", 0.0)),
        *[float(value) for value in element.get("spawnRange", (0.0, 0.0))],
        *[float(value) for value in element.get("fadeInRange", (0.0, 0.0))],
        *[float(value) for value in element.get("fadeOutRange", (0.0, 0.0))],
        float(element.get("spawnFrustumCullRadius", 0.0)),
        *[int(value) for value in element.get("spawnDelayMsec", (0, 0))],
        *[int(value) for value in element.get("lifeSpanMsec", (0, 0))],
        *[float(value) for value in element.get("spawnOrigin", (0.0,) * 6)],
        *[float(value) for value in element.get("spawnOffsetRadius", (0.0, 0.0))],
        *[float(value) for value in element.get("spawnOffsetHeight", (0.0, 0.0))],
        *[float(value) for value in element.get("spawnAngles", (0.0,) * 6)],
        *[float(value) for value in element.get("angularVelocity", (0.0,) * 6)],
        *[float(value) for value in element.get("initialRotation", (0.0, 0.0))],
        *[float(value) for value in element.get("gravity", (0.0, 0.0))],
        *[float(value) for value in element.get("reflectionFactor", (0.0, 0.0))],
        *[int(value) for value in element.get("atlas", (0,) * 7)],
        *[float(value) for value in element.get("collMins", (0.0,) * 3)],
        *[float(value) for value in element.get("collMaxs", (0.0,) * 3)],
        *[float(value) for value in element.get("emitDist", (0.0, 0.0))],
        *[float(value) for value in element.get("emitDistVariance", (0.0, 0.0))],
        *[int(value) for value in element.get("spawnDef", (0, 0))],
        int(element.get("trailScrollTimeMsec", 0)),
        int(element.get("trailRepeatDist", 0)),
        int(element.get("trailSplitDist", 0)),
        int(bool(element.get("trailPresent", elem_type == 3))),
    )


def _build_graph_section(graph_records: list[tuple[int, dict[str, Any]]],
                         offsets: Mapping[str, int]) -> tuple[bytes, dict[str, int]]:
    graph_effect_bytes = bytearray()
    graph_element_bytes = bytearray()
    graph_visual_bytes = bytearray()
    graph_velocity_bytes = bytearray()
    graph_visual_sample_bytes = bytearray()
    graph_trail_vertex_bytes = bytearray()
    graph_trail_index_bytes = bytearray()
    element_count = visual_count = velocity_count = visual_sample_count = 0
    trail_vertex_count = trail_index_count = 0
    element_records: list[tuple[int, dict[str, Any], int]] = []
    for effect_index, graph in graph_records:
        first_element = element_count
        for element in graph.get("elements", []):
            visuals = list(element.get("visuals", []))
            velocities = list(element.get("velocitySamples", []))
            visual_samples = list(element.get("visualSamples", []))
            trail_vertices = list(element.get("trailVertices", []))
            trail_indices = list(element.get("trailIndices", []))
            element_records.append((element_count, element, visual_count))
            for value in visuals:
                graph_visual_bytes.extend(GRAPH_VISUAL.pack(offsets[value]))
            for sample in velocities:
                graph_velocity_bytes.extend(GRAPH_VELOCITY_SAMPLE.pack(*sample))
            for sample in visual_samples:
                graph_visual_sample_bytes.extend(GRAPH_VISUAL_SAMPLE.pack(*sample))
            for vertex in trail_vertices:
                graph_trail_vertex_bytes.extend(GRAPH_TRAIL_VERTEX.pack(*vertex))
            for value in trail_indices:
                graph_trail_index_bytes.extend(GRAPH_TRAIL_INDEX.pack(value))
            element_count += 1
            visual_count += len(visuals)
            velocity_count += len(velocities)
            visual_sample_count += len(visual_samples)
            trail_vertex_count += len(trail_vertices)
            trail_index_count += len(trail_indices)
        graph_effect_bytes.extend(GRAPH_EFFECT.pack(
            effect_index, int(graph.get("flags", 0)),
            int(graph.get("msecLoopingLife", 0)),
            int(graph.get("elemDefCountLooping", 0)),
            int(graph.get("elemDefCountOneShot", 0)),
            int(graph.get("elemDefCountEmission", 0)),
            first_element, element_count - first_element, 0))

    element_bytes_by_index: dict[int, bytes] = {}
    for index, element, visual_offset in element_records:
        # Recompute the section offsets from the element's preceding records;
        # this is deterministic and avoids storing host pointers in the graph.
        velocity_offset = sum(len(element_records[item][1].get("velocitySamples", []))
                             for item in range(index))
        visual_sample_offset = sum(len(element_records[item][1].get("visualSamples", []))
                                   for item in range(index))
        trail_vertex_offset = sum(len(element_records[item][1].get("trailVertices", []))
                                  for item in range(index))
        trail_index_offset = sum(len(element_records[item][1].get("trailIndices", []))
                                 for item in range(index))
        element_bytes_by_index[index] = _pack_graph_element(
            element, offsets, visual_offset, velocity_offset,
            visual_sample_offset, trail_vertex_offset, trail_index_offset)
    for index in range(element_count):
        graph_element_bytes.extend(element_bytes_by_index[index])

    payload = (bytes(graph_effect_bytes) + bytes(graph_element_bytes)
               + bytes(graph_visual_bytes) + bytes(graph_velocity_bytes)
               + bytes(graph_visual_sample_bytes)
               + bytes(graph_trail_vertex_bytes)
               + bytes(graph_trail_index_bytes))
    return payload, {
        "graphDefinitions": len(graph_records),
        "graphElements": element_count,
        "graphVisuals": visual_count,
        "graphVelocitySamples": velocity_count,
        "graphVisualSamples": visual_sample_count,
        "graphTrailVertices": trail_vertex_count,
        "graphTrailIndices": trail_index_count,
    }


def _match_graphs(definitions: list[tuple[str, str]],
                  graph_source: Mapping[str, Any] | None
                  ) -> list[tuple[int, dict[str, Any]]]:
    if not graph_source:
        return []
    source_definitions = graph_source.get("definitions", [])
    source_graphs = graph_source.get("graphs", {})
    by_name: dict[str, dict[str, Any]] = {}
    by_path: dict[str, dict[str, Any]] = {}
    by_name_folded: dict[str, dict[str, Any]] = {}
    by_path_folded: dict[str, dict[str, Any]] = {}
    for source_index, (name, path) in enumerate(source_definitions):
        graph = source_graphs.get(source_index)
        if graph is None:
            continue
        if name:
            by_name.setdefault(name, graph)
            by_name_folded.setdefault(name.casefold(), graph)
        if path:
            by_path.setdefault(path, graph)
            by_path_folded.setdefault(path.casefold(), graph)
    matches: list[tuple[int, dict[str, Any]]] = []
    for effect_index, (name, path) in enumerate(definitions):
        graph = by_path.get(path) if path else None
        if graph is None:
            graph = by_name.get(name)
        if graph is None and path:
            graph = by_path_folded.get(path.casefold())
        if graph is None and name:
            graph = by_name_folded.get(name.casefold())
        if graph is not None:
            matches.append((effect_index, graph))
    return matches


def _expand_graph_dependencies(
        definitions: list[tuple[str, str]],
        graph_source: Mapping[str, Any] | None) -> int:
    """Append source graph definitions referenced by runner elements.

    A map's rawfile closure contains the effects that are placed by createfx,
    but a runner element can spawn another FxEffectDef that is not itself
    named by a map script.  Keep the VFXA package source-complete by walking
    only those authored runner/effect references.  The source graph bundle is
    the authority for the appended name/path pair; an unresolved reference is
    deliberately left unresolved for the runtime closure audit.
    """
    if not graph_source:
        return 0
    source_definitions = list(graph_source.get("definitions", []))
    source_graphs = graph_source.get("graphs", {})
    if not source_definitions or not source_graphs:
        return 0

    by_name: dict[str, int] = {}
    by_path: dict[str, int] = {}
    by_name_folded: dict[str, int] = {}
    by_path_folded: dict[str, int] = {}
    for source_index, pair in enumerate(source_definitions):
        if len(pair) != 2:
            continue
        name, path = str(pair[0]), str(pair[1])
        if name:
            by_name.setdefault(name, source_index)
            by_name_folded.setdefault(name.casefold(), source_index)
        if path:
            by_path.setdefault(path, source_index)
            by_path_folded.setdefault(path.casefold(), source_index)

    def resolve(value: Any) -> int | None:
        reference = str(value or "").strip()
        if not reference:
            return None
        source_index = by_path.get(reference)
        if source_index is None:
            source_index = by_name.get(reference)
        if source_index is None:
            source_index = by_path_folded.get(reference.casefold())
        if source_index is None:
            source_index = by_name_folded.get(reference.casefold())
        return source_index

    # Map each source graph to the VFXA definition index that owns it.  Use
    # the same path-first/name-second precedence as _match_graphs, including
    # its case-insensitive compatibility lookup.
    source_to_effect: dict[int, int] = {}
    queue: list[int] = []
    queued: set[int] = set()
    for effect_index, pair in enumerate(definitions):
        if len(pair) != 2:
            continue
        source_index = resolve(pair[1])
        if source_index is None:
            source_index = resolve(pair[0])
        if source_index is None or source_index not in source_graphs:
            continue
        source_to_effect.setdefault(source_index, effect_index)
        if source_index not in queued:
            queued.add(source_index)
            queue.append(source_index)

    added = 0
    cursor = 0
    while cursor < len(queue):
        source_index = queue[cursor]
        cursor += 1
        graph = source_graphs.get(source_index)
        if graph is None:
            continue
        references: list[str] = []
        for element in graph.get("elements", []):
            # FxElemType 10 is the source runner element.  Its visual list
            # contains child effect paths, not material names.  The impact,
            # death, and emitted links are also source effect references and
            # must participate in the same bounded dependency walk.
            if int(element.get("elemType", 0)) == 10:
                references.extend(str(value) for value in
                                  element.get("visuals", []) if value)
            references.extend(str(element.get(key, "")) for key in (
                "effectOnImpact", "effectOnDeath", "effectEmitted")
                if element.get(key, ""))

        for reference in references:
            child_source_index = resolve(reference)
            if child_source_index is None:
                continue
            child_graph = source_graphs.get(child_source_index)
            if child_graph is None:
                continue
            if child_source_index not in source_to_effect:
                name, path = source_definitions[child_source_index]
                definitions.append((str(name), str(path)))
                source_to_effect[child_source_index] = len(definitions) - 1
                added += 1
            if child_source_index not in queued:
                queued.add(child_source_index)
                queue.append(child_source_index)
    return added


def _seed_graph_definitions(
        definitions: list[tuple[str, str]], effect_names: Iterable[str],
        graph_source: Mapping[str, Any] | None) -> int:
    """Add the exact VIFX-referenced definitions to a map VFXA closure."""
    names = sorted({str(name).strip() for name in effect_names if str(name).strip()})
    if not names:
        return 0
    if not graph_source:
        raise ValueError("impact FX names require a desktop VFXA graph source")
    source_definitions = list(graph_source.get("definitions", []))
    source_graphs = graph_source.get("graphs", {})
    by_identity: dict[str, int] = {}
    by_identity_folded: dict[str, int] = {}
    for source_index, pair in enumerate(source_definitions):
        if len(pair) != 2 or source_index not in source_graphs:
            continue
        for identity in (str(pair[0]), str(pair[1])):
            if identity:
                by_identity.setdefault(identity, source_index)
                by_identity_folded.setdefault(identity.casefold(), source_index)
    existing = {identity.casefold() for pair in definitions
                for identity in pair if identity}
    added = 0
    for name in names:
        if name.casefold() in existing:
            continue
        source_index = by_identity.get(name)
        if source_index is None:
            source_index = by_identity_folded.get(name.casefold())
        if source_index is None:
            raise ValueError(f"VIFX effect is absent from desktop VFXA: {name}")
        pair = source_definitions[source_index]
        normalized = (str(pair[0]), str(pair[1]))
        definitions.append(normalized)
        existing.update(identity.casefold() for identity in normalized if identity)
        added += 1
    return added


def _build_from_rawfiles(rawfiles: Mapping[str, bytes], map_index: int,
                         map_name: str,
                         graph_source: Mapping[str, Any] | None = None,
                         impact_effect_names: Iterable[str] = ()
                         ) -> tuple[bytes, dict[str, object]]:
    definitions, placements, sounds, malformed = _effect_records(rawfiles, map_name)
    impact_dependencies = _seed_graph_definitions(
        definitions, impact_effect_names, graph_source)
    graph_dependencies = _expand_graph_dependencies(definitions, graph_source)
    graph_records = _match_graphs(definitions, graph_source)
    if (graph_records and graph_source is not None
            and int(graph_source.get("version", 3)) < 3):
        raise ValueError(
            "VFXA v2 cannot be promoted to source-complete v3; regenerate "
            "the desktop FX graph so trail metadata and decal slots are present"
        )
    strings, offsets = _string_table(definitions, placements, sounds,
                                     graph_records)
    definition_bytes = b"".join(
        DEFINITION.pack(offsets[name], offsets[path] if path else 0)
        for name, path in definitions)
    placement_bytes = b"".join(
        PLACEMENT.pack(definition_index, flags, *origin, *angles, delay)
        for alias, origin, angles, delay, flags in placements
        for definition_index in (next(index for index, (name, _) in enumerate(definitions)
                                      if name == alias),))
    sound_bytes = b"".join(
        LOOP_SOUND.pack(offsets[alias], *origin, *angles)
        for alias, origin, angles in sounds)
    graph_payload, graph_report = _build_graph_section(graph_records, offsets)
    if graph_records:
        header = HEADER_V2.pack(
            MAGIC, 3, map_index, len(definitions), len(placements),
            len(sounds), len(strings), graph_report["graphDefinitions"],
            graph_report["graphElements"], graph_report["graphVisuals"],
            graph_report["graphVelocitySamples"],
            graph_report["graphVisualSamples"],
            graph_report["graphTrailVertices"],
            graph_report["graphTrailIndices"], len(graph_payload), 0)
    else:
        header = HEADER.pack(MAGIC, VERSION, map_index, len(definitions),
                             len(placements), len(sounds), len(strings), 0)
    payload = (header + definition_bytes + placement_bytes + sound_bytes
               + strings + graph_payload)
    report: dict[str, object] = {
        "path": "",
        "bytes": len(payload),
        "mapIndex": map_index,
        "version": 3 if graph_records else VERSION,
        "definitions": len(definitions),
        "placements": len(placements),
        "loopSounds": len(sounds),
        "malformedRecords": malformed,
        "unresolvedDefinitions": sum(1 for _, path in definitions if not path),
    }
    report.update(graph_report)
    report["graphMatched"] = len(graph_records)
    report["graphDependencies"] = graph_dependencies
    report["impactDependencies"] = impact_dependencies
    report["graphSource"] = str(graph_source.get("path", "")) if graph_source else ""
    return payload, report


def _read_string(data: bytes, base: int, size: int, offset: int,
                 maximum: int, label: str) -> str:
    if offset == 0:
        return ""
    if offset >= size:
        raise ValueError(f"{label} offset is outside the VFXA string table")
    end = data.find(b"\0", base + offset, base + size)
    if end < 0 or end - (base + offset) == 0 or end - (base + offset) > maximum:
        raise ValueError(f"{label} is not a bounded NUL-terminated string")
    return data[base + offset:end].decode("utf-8")


def inspect_effect_sidecar(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    if len(data) < HEADER.size:
        raise ValueError(f"effect sidecar is truncated: {path}")
    if (data[:4] == MAGIC and len(data) >= 8
            and struct.unpack_from("<I", data, 4)[0] in (2, 3)):
        parsed = _read_v2_graph(path)
        return {
            "path": str(path),
            "bytes": len(data),
            "mapIndex": parsed["mapIndex"],
            "version": parsed["version"],
            "definitions": len(parsed["definitions"]),
            "placements": parsed["placements"],
            "loopSounds": parsed["loopSounds"],
            "stringBytes": struct.unpack_from("<I", data, 24)[0],
            "graphDefinitions": parsed["graphDefinitions"],
            "graphElements": parsed["graphElements"],
            "graphVisuals": parsed["graphVisuals"],
            "graphVelocitySamples": parsed["graphVelocitySamples"],
            "graphVisualSamples": parsed["graphVisualSamples"],
            "graphTrailVertices": parsed["graphTrailVertices"],
            "graphTrailIndices": parsed["graphTrailIndices"],
        }
    magic, version, map_index, definitions, placements, sounds, string_bytes, reserved = HEADER.unpack_from(data)
    if magic != MAGIC or version != VERSION or reserved != 0:
        raise ValueError(f"unsupported effect sidecar: {path}")
    if definitions > MAX_DEFINITIONS or placements > MAX_PLACEMENTS or sounds > MAX_LOOP_SOUNDS:
        raise ValueError(f"effect sidecar record count is too large: {path}")
    records_end = (HEADER.size + definitions * DEFINITION.size
                   + placements * PLACEMENT.size + sounds * LOOP_SOUND.size)
    if string_bytes == 0 or string_bytes > MAX_STRING_BYTES or records_end + string_bytes != len(data):
        raise ValueError(f"effect sidecar size mismatch: {path}")
    string_base = records_end
    for index in range(definitions):
        name_offset, path_offset = DEFINITION.unpack_from(data, HEADER.size + index * DEFINITION.size)
        _read_string(data, string_base, string_bytes, name_offset, MAX_NAME_BYTES, "effect name")
        if path_offset:
            _read_string(data, string_base, string_bytes, path_offset, MAX_PATH_BYTES, "effect path")
    placements_offset = HEADER.size + definitions * DEFINITION.size
    for index in range(placements):
        effect_index, flags, *values = PLACEMENT.unpack_from(data, placements_offset + index * PLACEMENT.size)
        if effect_index >= definitions or flags == 0 or not all(math.isfinite(value) for value in values):
            raise ValueError(f"effect placement {index} is invalid")
    sounds_offset = placements_offset + placements * PLACEMENT.size
    for index in range(sounds):
        alias_offset, *values = LOOP_SOUND.unpack_from(data, sounds_offset + index * LOOP_SOUND.size)
        _read_string(data, string_base, string_bytes, alias_offset, MAX_SOUND_BYTES, "loop sound alias")
        if not all(math.isfinite(value) for value in values):
            raise ValueError(f"loop sound placement {index} is invalid")
    return {
        "path": str(path),
        "bytes": len(data),
        "mapIndex": map_index,
        "version": version,
        "definitions": definitions,
        "placements": placements,
        "loopSounds": sounds,
        "stringBytes": string_bytes,
    }


def impact_effect_names(path: Path) -> set[str]:
    """Read an already validated-shaped VIFX stream without pointer data."""
    data = path.read_bytes()
    if len(data) < 32:
        raise ValueError(f"VIFX sidecar is truncated: {path}")
    (magic, version, table_count, entry_count, non_flesh_count,
     flesh_count, string_bytes, reserved) = struct.unpack_from("<8I", data)
    if (magic != 0x58464956 or version != 1 or table_count == 0
            or table_count > 4 or entry_count != 12
            or non_flesh_count != 29 or flesh_count != 4
            or string_bytes == 0 or string_bytes > MAX_STRING_BYTES
            or reserved != 0):
        raise ValueError(f"unsupported VIFX sidecar: {path}")
    slots_per_table = entry_count * (non_flesh_count + flesh_count)
    slot_base = 32 + table_count * 8
    string_base = slot_base + table_count * slots_per_table * 4
    if string_base + string_bytes != len(data) or data[string_base] != 0:
        raise ValueError(f"VIFX sidecar size mismatch: {path}")

    def read_string(offset: int, allow_empty: bool) -> str:
        if offset >= string_bytes:
            raise ValueError(f"VIFX string offset is invalid: {path}")
        end = data.find(b"\0", string_base + offset,
                        string_base + string_bytes)
        if end < 0 or end - (string_base + offset) > MAX_NAME_BYTES:
            raise ValueError(f"VIFX string is not bounded: {path}")
        value = data[string_base + offset:end].decode("utf-8")
        if not allow_empty and not value:
            raise ValueError(f"VIFX effect slot is empty: {path}")
        return value

    table_names: set[str] = set()
    names: set[str] = set()
    for table_index in range(table_count):
        name_offset, first_slot = struct.unpack_from(
            "<2I", data, 32 + table_index * 8)
        table_name = read_string(name_offset, True)
        if table_name in table_names or first_slot != table_index * slots_per_table:
            raise ValueError(f"VIFX table directory is invalid: {path}")
        table_names.add(table_name)
        for slot_index in range(slots_per_table):
            (offset,) = struct.unpack_from(
                "<I", data, slot_base + (first_slot + slot_index) * 4)
            effect_name = read_string(offset, True)
            if effect_name:
                names.add(effect_name)
    if "" not in table_names:
        raise ValueError(f"VIFX canonical table is missing: {path}")
    return names


def build_effect_sidecar(rawfile_path: Path, output_path: Path,
                         map_index: int, map_name: str,
                         graph_path: Path | None = None,
                         impact_path: Path | None = None) -> dict[str, object]:
    graph_source = None
    if graph_path is not None and graph_path.is_file():
        graph_source = _read_v2_graph(graph_path)
    impact_names = (impact_effect_names(impact_path)
                    if impact_path is not None and impact_path.is_file()
                    else set())
    payload, report = _build_from_rawfiles(
        _rawfiles(rawfile_path), map_index, map_name, graph_source,
        impact_names)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(payload)
    report["path"] = str(output_path)
    inspect_effect_sidecar(output_path)
    return report


if __name__ == "__main__":
    raise SystemExit("vita_effects.py is an importable converter module")
