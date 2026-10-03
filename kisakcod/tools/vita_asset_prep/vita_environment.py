#!/usr/bin/env python3
"""Extract the source-authored multiplayer environment into a VENV sidecar.

The Windows oracle already exports the map rawfile closure. This converter
keeps the relevant createart/vision/FX values in a bounded, fixed-width
payload so the Vita runtime does not need to parse GSC or vision text.
"""

from __future__ import annotations

import re
import struct
from pathlib import Path

MAGIC = b"VENV"
VERSION = 3
# Keep the wire offsets explicit: magic/version/map/flags are followed by
# fog at byte 16, matching the Vita reader. HEADER_V1 remains available to
# document and validate the original 116-byte payload; HEADER appends the
# source glow block after that stable prefix so old readers can still reject
# or inspect a v2 record without any shifted v1 fields.
HEADER_V1 = struct.Struct("<4sIII3f4f3f3f4f4f4I")
# V3 appends the source map-script r_specularcolorscale after the V2 glow
# block. Keep the V2 layout named so older sidecars remain easy to validate.
HEADER_V2 = struct.Struct("<4sIII3f4f3f3f4f4f4I9f")
HEADER = struct.Struct("<4sIII3f4f3f3f4f4f4I10f")

HAS_FOG = 1 << 0
HAS_VISION = 1 << 1
HAS_RAIN = 1 << 2
HAS_LIGHTNING = 1 << 3
HAS_WATER = 1 << 4
HAS_CLIENT_FX = 1 << 5
HAS_GLOW = 1 << 6


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


def _text(rawfiles: dict[str, bytes], name: str) -> str:
    return rawfiles.get(name, b"").decode("utf-8", "ignore").replace("\r", "")


def _strip_gsc_comments(text: str) -> str:
    """Remove GSC comments without touching quoted strings.

    The retail createart scripts keep disabled experiments beside the active
    call (Cargo Ship, for example, has a commented-out setExpFog line).  A
    regex over the raw text would mistake those comments for renderer state.
    Preserve newlines so any later diagnostics retain useful line positions.
    """
    output: list[str] = []
    index = 0
    quoted = False
    escaped = False
    while index < len(text):
        character = text[index]
        if quoted:
            output.append(character)
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                quoted = False
            index += 1
            continue
        if character == '"':
            quoted = True
            output.append(character)
            index += 1
            continue
        if character == "/" and index + 1 < len(text):
            next_character = text[index + 1]
            if next_character == "/":
                index += 2
                while index < len(text) and text[index] not in "\r\n":
                    index += 1
                continue
            if next_character == "*":
                index += 2
                while index < len(text):
                    if text[index] == "*" and index + 1 < len(text) \
                            and text[index + 1] == "/":
                        index += 2
                        break
                    if text[index] in "\r\n":
                        output.append(text[index])
                    index += 1
                continue
        output.append(character)
        index += 1
    return "".join(output)


def _number_list(text: str, key: str, count: int) -> list[float] | None:
    match = re.search(rf"\b{re.escape(key)}\s+\"([^\"]+)\"", text)
    if not match:
        return None
    try:
        values = [float(item) for item in match.group(1).split()]
    except ValueError:
        return None
    return values if len(values) == count else None


def _setdvar_value(text: str, key: str, default: float) -> float:
    """Read a numeric setdvar from a comment-stripped map script."""
    match = re.search(
        rf"\bsetdvar\s*\(\s*[\"']{re.escape(key)}[\"']\s*,\s*"
        rf"[\"']([-+0-9.eE]+)[\"']\s*\)", text, re.IGNORECASE)
    if not match:
        return default
    try:
        value = float(match.group(1))
    except ValueError:
        return default
    return value if value == value and abs(value) != float("inf") else default


def _fog(art: str) -> tuple[int, list[float], float, float, float, float]:
    # setExpFog(start, halfway, red, green, blue, transition)
    # Scr_SetExponentialFog in the source computes density as ln(2) /
    # halfway, then stores the start and density dvars used by R_SwitchFog.
    match = re.search(
        r"\bsetExpFog\s*\(\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*,\s*"
        r"([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*,\s*"
        r"([-+0-9.eE]+)\s*\)", _strip_gsc_comments(art))
    if not match:
        return 0, [0.0, 0.0, 0.0], 0.0, 0.0, 0.0, 0.0
    start, halfway, red, green, blue, transition = [
        float(v) for v in match.groups()
    ]
    density = 0.69314718 / max(halfway, 1.0)
    return 1, [red, green, blue], start, density, halfway, transition


def build_environment_sidecar(rawfile_path: Path, output_path: Path,
                              map_index: int, map_name: str) -> dict[str, object]:
    rawfiles = _rawfiles(rawfile_path)
    art_name = f"maps/createart/mp_{map_name}_art.gsc"
    map_script_name = f"maps/mp/mp_{map_name}.gsc"
    fx_name = f"maps/mp/mp_{map_name}_fx.gsc"
    vision_name = f"vision/mp_{map_name}.vision"
    art = _strip_gsc_comments(_text(rawfiles, art_name))
    map_script = _strip_gsc_comments(_text(rawfiles, map_script_name))
    fx = _strip_gsc_comments(_text(rawfiles, fx_name))
    vision = _strip_gsc_comments(_text(rawfiles, vision_name))
    createfx_name = f"maps/createfx/mp_{map_name}_fx.gsc"
    createfx = _strip_gsc_comments(_text(rawfiles, createfx_name))

    fog_flag, fog_color, fog_start, fog_density, fog_halfway, fog_transition = _fog(art)
    flags = fog_flag * HAS_FOG
    if vision:
        flags |= HAS_VISION
    effects = re.findall(r"\bloadfx\s*\(\s*\"([^\"]+)\"\s*\)", fx)
    lower_effects = [effect.lower() for effect in effects]
    weather_effect = next((effect for effect in effects if "rain" in effect.lower()), "")
    if any("rain" in effect for effect in lower_effects):
        flags |= HAS_RAIN
    if any("lightning" in effect for effect in lower_effects):
        flags |= HAS_LIGHTNING
    if any(token in effect for effect in lower_effects for token in ("water", "splash", "drip")):
        flags |= HAS_WATER
    if effects or createfx:
        flags |= HAS_CLIENT_FX

    light_tint = _number_list(vision, "r_filmLightTint", 3) or [1.0, 1.0, 1.0]
    dark_tint = _number_list(vision, "r_filmDarkTint", 3) or [1.0, 1.0, 1.0]

    def vision_value(key: str, default: float) -> float:
        match = re.search(rf"\b{re.escape(key)}\s+\"([-+0-9.eE]+)\"", vision)
        return float(match.group(1)) if match else default

    film_contrast = vision_value("r_filmContrast", 1.0)
    film_brightness = vision_value("r_filmBrightness", 0.0)
    film_desaturation = vision_value("r_filmDesaturation", 0.0)
    film_invert = vision_value("r_filmInvert", 0.0)
    # R_SetGlowInfo copies this authored block into the view and only enables
    # the post pass when r_glow, radius, and bloom intensity are all active.
    # Preserve both glow layers even though the current scalar implementation
    # consumes layer 0; layer 1 is part of the source contract and is needed
    # when the VitaGL post chain grows to the full two-layer filter.
    glow_enabled = vision_value("r_glow", 0.0)
    glow_radius0 = vision_value("r_glowRadius0", 0.0)
    glow_radius1 = vision_value("r_glowRadius1", 0.0)
    glow_cutoff = vision_value("r_glowBloomCutoff", 0.0)
    glow_desaturation = vision_value("r_glowBloomDesaturation", 0.0)
    glow_intensity0 = vision_value("r_glowBloomIntensity0", 0.0)
    glow_intensity1 = vision_value("r_glowBloomIntensity1", 0.0)
    glow_sky_bleed0 = vision_value("r_glowSkyBleedIntensity0", 0.0)
    glow_sky_bleed1 = vision_value("r_glowSkyBleedIntensity1", 0.0)
    # R_SetSunConstants sends sun.color through r_specularColorScale for the
    # source sun-specular input. Crossfire authors a value above the engine
    # default in its map script.
    specular_color_scale = max(
        0.0, min(100.0, _setdvar_value(
            map_script, "r_specularcolorscale", 1.0)))
    if glow_enabled > 0.5 and glow_radius0 > 0.0 and glow_intensity0 > 0.0:
        flags |= HAS_GLOW
    rain = 1.0 if flags & HAS_RAIN else 0.0
    rain_slant = 0.18 if rain else 0.0
    rain_coverage = 0.72 if rain else 0.0
    wetness = 0.35 if flags & HAS_WATER else 0.0
    vision_bytes = vision_name.encode("utf-8") if vision else b""
    weather_bytes = weather_effect.encode("utf-8")
    if len(vision_bytes) > 256 or len(weather_bytes) > 256:
        raise ValueError(f"{map_name}: environment name exceeds 256 bytes")
    header = HEADER.pack(
        MAGIC, VERSION, map_index, flags,
        *fog_color, fog_start, fog_density, fog_halfway, fog_transition,
        *light_tint, *dark_tint,
        film_contrast, film_brightness, film_desaturation, film_invert,
        rain, rain_slant, rain_coverage, wetness,
        len(vision_bytes), len(weather_bytes),
        len(re.findall(r"createOneshotEffect", createfx)), 0,
        glow_enabled, glow_radius0, glow_radius1, glow_cutoff,
        glow_desaturation, glow_intensity0, glow_intensity1,
        glow_sky_bleed0, glow_sky_bleed1,
        specular_color_scale,
    )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(header + vision_bytes + weather_bytes)
    return {
        "path": str(output_path),
        "bytes": output_path.stat().st_size,
        "mapIndex": map_index,
        "flags": flags,
        "vision": vision_name if vision else "",
        "weatherEffect": weather_effect,
        "effects": len(effects),
        "createFxEntities": len(re.findall(r"createOneshotEffect", createfx)),
        "glow": {
            "enabled": bool(flags & HAS_GLOW),
            "radius0": glow_radius0,
            "radius1": glow_radius1,
            "bloomCutoff": glow_cutoff,
            "bloomDesaturation": glow_desaturation,
            "bloomIntensity0": glow_intensity0,
            "bloomIntensity1": glow_intensity1,
            "skyBleedIntensity0": glow_sky_bleed0,
            "skyBleedIntensity1": glow_sky_bleed1,
        },
        "specularColorScale": specular_color_scale,
    }
