"""Conservative original-source 2D material closure; not a reachability proof."""
from __future__ import annotations
import hashlib
import re
import struct
from pathlib import Path

SOURCE_DIRECTORIES = ("cgame", "cgame_mp", "client", "client_mp", "bgame", "game_mp", "ui", "ui_mp")
BINARY_INPUTS = ("vita_rawfiles.bin", "vita_weapons.bin", "vita_fonts.bin")

def _font_material_references(data: bytes) -> set[str]:
    # VITF strings are length-prefixed and adjacent, without NUL separators.
    # Token scanning alone merges the font, image and material names.
    if len(data) < 20: raise ValueError("truncated VITF header")
    magic, version, count, declared = struct.unpack_from("<3IQ", data)
    if magic != 0x46544956 or version not in (1, 2) or not 1 <= count <= 64:
        raise ValueError("invalid VITF header")
    offset, aggregate, names = 20, 0, set()
    for _ in range(count):
        header = 24 if version == 2 else 16
        if offset + header > len(data): raise ValueError("truncated VITF record")
        name, image, glyphs, height = struct.unpack_from("<3Ii", data, offset)
        material, glow = struct.unpack_from("<2I", data, offset + 16) if version == 2 else (0, 0)
        if not name or not 1 <= glyphs <= 2048 or not 1 <= height <= 256:
            raise ValueError("invalid VITF record")
        if version == 2 and not material: raise ValueError("VITF material missing")
        offset += header
        for index, length in enumerate((name, image, material, glow)):
            if length > 4096 or offset + length > len(data): raise ValueError("truncated VITF string")
            value = data[offset:offset + length]
            if b"\0" in value: raise ValueError("invalid VITF string")
            if index >= 2 and value: names.add(value.decode("utf-8").casefold())
            offset += length
            aggregate += length
        glyph_bytes = glyphs * 24
        if offset + glyph_bytes > len(data): raise ValueError("truncated VITF glyphs")
        offset += glyph_bytes
        aggregate += glyph_bytes
    if offset != len(data) or aggregate != declared: raise ValueError("invalid VITF aggregate")
    return names

def source_ui_dependencies(directory: Path, source_root: Path) -> dict:
    material_path = directory / "vita_materials.bin"
    if not material_path.is_file(): return {"available": False}
    raw = material_path.read_bytes()
    if len(raw) < 20: return {"available": False}
    magic, version, count, declared = struct.unpack_from("<3IQ", raw)
    if magic != 0x4C544956 or version != 5: return {"available": False}
    if count > 4096: raise ValueError("invalid VITL material count")
    tokens: set[str] = set()
    inputs = {}
    paths = [(f"source/{p.relative_to(source_root).as_posix()}", p)
             for folder in SOURCE_DIRECTORIES for p in sorted((source_root / folder).glob("*.cpp"))]
    paths += [(f"export/{name}", directory / name) for name in BINARY_INPUTS if (directory / name).is_file()]
    for name, path in paths:
        data = path.read_bytes()
        inputs[name] = hashlib.sha256(data).hexdigest()
        tokens.update(t.decode("ascii").casefold() for t in re.findall(rb"[A-Za-z0-9_./$\\-]+", data))
        if name == "export/vita_fonts.bin":
            tokens.update(_font_material_references(data))
    offset, aggregate = 20, 0
    materials = {}
    for _ in range(count):
        if offset + 52 > len(raw): raise ValueError("truncated VITL v5 header")
        lengths = struct.unpack_from("<5I", raw, offset)
        enabled, semantic = raw[offset + 50:offset + 52]
        if enabled > 1 or semantic not in (0, 2) or not lengths[0] or max(lengths) > 4096:
            raise ValueError("invalid VITL v5 2D metadata")
        offset += 52
        strings = []
        for length in lengths:
            if offset + length > len(raw): raise ValueError("truncated VITL string")
            strings.append(raw[offset:offset + length].decode("utf-8"))
            offset += length
            aggregate += length
        name, image = strings[:2]
        if enabled and semantic == 0 and image and name.casefold() in tokens:
            materials[name] = image
    if offset != len(raw) or aggregate != declared: raise ValueError("invalid VITL aggregate")
    return {"available": True, "schemaVersion": 1,
        "method": "conservative source and exported-asset name references",
        "materialStreamSha256": hashlib.sha256(raw).hexdigest(),
        "inputs": inputs, "materials": dict(sorted(materials.items())),
        "images": sorted(set(materials.values())), "runtimeClosureQualified": False}
