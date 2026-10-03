#!/usr/bin/env python3
"""Prepare original alias/PCM dependencies from a local KisakCOD asset export.

No sound is synthesized, substituted, resampled, or downloaded. The source
registration roots and exported weapons/effects/scripts determine selection;
secondary and chain aliases are followed to a fixed point. Unknown runtime
lookups remain missing assets, and must be added to the measured closure.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import re
import struct
import zipfile

from vertical_slice_packs import parse_vits_records, build_vits

HEADER = struct.Struct("<IIIQ")
RECORD_BYTES = 1440
MAGIC = 0x4C415356
PCM_LIMIT = 40 << 20
SOURCE_DIRECTORIES = ("cgame", "cgame_mp", "client", "client_mp", "bgame", "ui_mp",
                      "game_mp", "sound", "EffectsCore")


def parse_aliases(raw: bytes) -> dict[str, list[bytes]]:
    if len(raw) < HEADER.size:
        raise ValueError("VSAL header is truncated")
    magic, version, count, size = HEADER.unpack_from(raw)
    if (magic != MAGIC or version != 1 or not 0 < count <= 32768
            or size != count * RECORD_BYTES or len(raw) != HEADER.size + size):
        raise ValueError("VSAL header/version/size is invalid")
    result: dict[str, list[bytes]] = {}
    previous = ""
    for offset in range(HEADER.size, len(raw), RECORD_BYTES):
        record = raw[offset:offset + RECORD_BYTES]
        strings = alias_strings(record)
        name = strings[0]
        if not name or name < previous:
            raise ValueError("VSAL aliases must be nonempty and sorted")
        previous = name
        sequence, flags, delay, kind, exists, knots, default = struct.unpack_from("<3i4I", record, 896)
        if kind not in (0, 1, 2) or exists > 1 or knots > 8 or default > 1:
            raise ValueError(f"invalid VSAL sound/curve record: {name}")
        if not all(map(math.isfinite, struct.unpack_from("<29f", record, 924))):
            raise ValueError(f"nonfinite VSAL playback/curve value: {name}")
        for channel in range(4):
            at = 1040 + channel * 100
            speakers, = struct.unpack_from("<i", record, at)
            if not 0 <= speakers <= 6:
                raise ValueError(f"invalid speaker count: {name}")
            for speaker in range(6):
                _, levels, left, right = struct.unpack_from("<2i2f", record, at + 4 + speaker * 16)
                if not 0 <= levels <= 2 or not math.isfinite(left) or not math.isfinite(right):
                    raise ValueError(f"invalid speaker levels: {name}")
        variants = result.setdefault(name, [])
        variants.append(record)
        if len(variants) > 256:
            raise ValueError(f"too many alias variants: {name}")
    return result


def alias_strings(record: bytes) -> list[str]:
    values = []
    for index in range(7):
        wire = record[index * 128:(index + 1) * 128]
        if b"\0" not in wire:
            raise ValueError("VSAL string is unterminated")
        values.append(wire.split(b"\0", 1)[0].decode("utf-8", "strict"))
    return values


def build_aliases(aliases: dict[str, list[bytes]]) -> bytes:
    body = b"".join(record for name in sorted(aliases) for record in aliases[name])
    raw = HEADER.pack(MAGIC, 1, len(body) // RECORD_BYTES, len(body)) + body
    parse_aliases(raw)
    return raw


def dependency_closure(aliases: dict[str, list[bytes]], seeds: set[str]) -> set[str]:
    selected = seeds & aliases.keys()
    pending = sorted(selected)
    while pending:
        name = pending.pop()
        for record in aliases[name]:
            for dependency in alias_strings(record)[2:4]:
                if not dependency:
                    continue
                if dependency not in aliases:
                    raise ValueError(f"missing alias dependency {name} -> {dependency}")
                if dependency not in selected:
                    selected.add(dependency)
                    pending.append(dependency)
    return selected


def normalized_path(name: str) -> str:
    path = PurePosixPath(name.replace("\\", "/"))
    if path.is_absolute() or ".." in path.parts or ":" in name or not path.parts:
        raise ValueError(f"invalid relative asset path: {name}")
    return str(path).lower()


def source_seeds(source_root: Path, export: Path, aliases: dict[str, list[bytes]]) -> set[str]:
    texts = []
    for directory in SOURCE_DIRECTORIES:
        texts.extend(path.read_bytes() for path in sorted((source_root / directory).glob("*.cpp")))
    texts.extend((export / name).read_bytes() for name in (
        "vita_weapons.bin", "vita_effects.bin", "vita_rawfiles.bin", "vita_animations.bin",
        "vita_map_ents.bin", "vita_menus.bin") if (export / name).is_file())
    tokens = {value.decode("ascii").casefold() for raw in texts
              for value in re.findall(rb"[A-Za-z0-9_./\\-]+", raw)}
    main = (source_root / "cgame_mp/cg_main_mp.cpp").read_text()
    prefixes = re.findall(r'CG_RegisterSurfaceTypeSounds\("([^"\n]+)"', main)
    # Original GSC composes faction announcer aliases, e.g. game["voice"] +
    # game["dialog"]["gametype"]. Match only exported aliases formed from
    # source-present prefixes and suffixes, preserving every authored variant.
    voice_prefixes = {token for token in tokens if re.fullmatch(r"[a-z]+_1mc_", token)}
    return {name for name in aliases if name.casefold() in tokens
            or any(name.casefold().startswith(prefix.casefold() + "_") for prefix in prefixes)
            or any(name.casefold().startswith(prefix) and name.casefold()[len(prefix):] in tokens
                   for prefix in voice_prefixes)}


def digest(path: Path) -> dict:
    raw = path.read_bytes()
    return {"path": str(path.resolve()), "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw)}


def prepare(export: Path, source_root: Path, owned_install: Path, output: Path, workspace: Path) -> dict:
    if not output.resolve().is_relative_to(workspace.resolve()):
        raise ValueError("output must be inside the workspace")
    aliases = parse_aliases((export / "vita_soundaliases.bin").read_bytes())
    pcm = parse_vits_records((export / "vita_soundpool.bin").read_bytes(), maximum_records=8192)
    selected = dependency_closure(aliases, source_seeds(source_root, export, aliases))
    if not selected:
        raise ValueError("source sound dependency selection is empty")
    chosen_pcm = {}
    streams = set()
    unavailable = []
    missing = []
    by_path = {normalized_path(name): record for name, record in pcm.items()}
    for name in sorted(selected):
        for record in aliases[name]:
            sound = alias_strings(record)[4]
            kind, exists = struct.unpack_from("<2I", record, 908)
            if not exists:
                unavailable.append({"alias": name, "file": sound, "reason": "source SoundFile.exists is false"})
            elif kind == 1:
                sample = by_path.get(normalized_path(sound))
                if sample is None:
                    missing.append(f"{name}: {sound}")
                else:
                    # The wire keeps the authored path; runtime comparison is
                    # normalized in the same way as the source file system.
                    chosen_pcm[sound] = sample
            elif kind == 2:
                streams.add("sound/" + normalized_path(sound))
    if missing:
        raise ValueError("missing decoded source samples: " + "; ".join(sorted(set(missing))))
    resident = sum(size for _, size in chosen_pcm.values())
    if resident > PCM_LIMIT:
        raise ValueError(f"source PCM closure exceeds its 40 MiB preparation limit: {resident}")
    # Validate all external stream dependencies before publishing any output.
    archive_index = {}
    for archive in sorted((owned_install / "main").glob("*.iwd")):
        with zipfile.ZipFile(archive) as zipped:
            for name in zipped.namelist():
                folded = normalized_path(name)
                if folded in streams:
                    archive_index[folded] = (archive, name)
    stream_data = {}
    for name in sorted(streams):
        loose = owned_install / "main" / name
        if loose.is_file():
            stream_data[name] = loose.read_bytes()
        elif name in archive_index:
            archive, member = archive_index[name]
            with zipfile.ZipFile(archive) as zipped:
                stream_data[name] = zipped.read(member)
        else:
            raise ValueError(f"missing owned-install stream: {name}")
    output.mkdir(parents=True, exist_ok=True)
    (output / "vita_soundaliases.bin").write_bytes(build_aliases({name: aliases[name] for name in selected}))
    # Normalize PCM catalog names without changing the sample data or format.
    normalized_pcm = {}
    for sound, (wire, size) in chosen_pcm.items():
        old_size, = struct.unpack_from("<I", wire)
        name = normalized_path(sound).encode()
        rewritten = struct.pack("<I", len(name)) + wire[4:28] + name + wire[28 + old_size:]
        normalized_pcm[name.decode()] = (rewritten, size)
    (output / "vita_sounds.bin").write_bytes(build_vits(normalized_pcm))
    stream_report = {}
    for name, data in stream_data.items():
        target = output / "stream" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        stream_report["stream/" + name] = {"sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}
    report = {
        "schemaVersion": 1, "qualification": "content-conversion-only",
        "sourceInputs": {name: digest(export / name) for name in (
            "vita_soundaliases.bin", "vita_soundpool.bin", "vita_weapons.bin", "vita_effects.bin",
            "vita_rawfiles.bin", "vita_animations.bin", "vita_map_ents.bin", "vita_menus.bin")
            if (export / name).is_file()},
        "sourceCode": {str(path.relative_to(source_root)): digest(path)
                       for directory in SOURCE_DIRECTORIES
                       for path in sorted((source_root / directory).glob("*.cpp"))},
        "catalog": digest(output / "vita_soundaliases.bin"), "pcm": digest(output / "vita_sounds.bin"),
        "aliases": len(selected), "variants": sum(len(aliases[name]) for name in selected),
        "pcmBytes": sum(size for _, size in normalized_pcm.values()), "streams": stream_report,
        "sourceUnavailable": unavailable,
        "omittedAliases": [{"name": name, "reason": "outside static included-source dependency closure"}
                           for name in sorted(aliases.keys() - selected)],
    }
    (output / "sound-closure.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--export", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--owned-install", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--workspace", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = prepare(args.export, args.source_root, args.owned_install, args.output, args.workspace)
        print(json.dumps({key: report[key] for key in ("aliases", "variants", "pcmBytes")}, indent=2))
        return 0
    except (OSError, ValueError) as error:
        print(f"source-sound-closure: {error}")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
