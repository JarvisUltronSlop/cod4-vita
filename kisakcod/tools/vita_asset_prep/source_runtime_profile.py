"""Apply the bounded offline slice configuration to locally owned GSC/data.

Original DM, spawning, damage, movement and weapon implementations are retained.
Every changed source record is hashed and reported; unknown input shapes fail.
No retail bytes are stored in this module or modified in their source install.
"""
from __future__ import annotations

import hashlib
import re
import struct

PROFILE = "cod4-vita-offline-fixed-loadout-v1"
LOADOUT = ("mp5", "usp", "frag_grenade", "flash_grenade")
SOURCE_WEAPONS = ("none", "defaultweapon_mp", "mp5_mp", "usp_mp",
                  "frag_grenade_mp", "flash_grenade_mp",
                  "frag_grenade_short_mp", "destructible_car")


def validate_source_weapon_set(data: bytes) -> None:
    if len(data) < 16:
        raise ValueError("truncated source weapon set")
    magic, version, count, stride = struct.unpack_from("<4sIII", data)
    if (magic, version, count, stride) != (b"VITP", 5, len(SOURCE_WEAPONS), 15376):
        raise ValueError("source weapon set requires eight VITP v5 definitions")
    if len(data) != 16 + count * stride:
        raise ValueError("source weapon set length mismatch")
    names = []
    for index in range(count):
        field = data[16 + index * stride:16 + index * stride + 64]
        end = field.find(b"\0")
        if end <= 0:
            raise ValueError("source weapon set has an invalid name")
        names.append(field[:end].decode("ascii"))
    if len(set(names)) != count or set(names) != set(SOURCE_WEAPONS):
        raise ValueError("source weapon set differs from the fixed-loadout/script dependency contract")


class Reader:
    def __init__(self, data: bytes):
        self.data, self.at = data, 0

    def take(self, count: int) -> bytes:
        if count < 0 or count > len(self.data) - self.at:
            raise ValueError("truncated source runtime profile input")
        value = self.data[self.at:self.at + count]
        self.at += count
        return value

    def values(self, fmt: str) -> tuple:
        return struct.unpack(fmt, self.take(struct.calcsize(fmt)))

    def text(self, count: int) -> str:
        value = self.take(count).decode("utf-8")
        if "\0" in value:
            raise ValueError("NUL in source runtime profile identity")
        return value

    def finish(self):
        if self.at != len(self.data):
            raise ValueError("trailing source runtime profile input")


def rawfiles(data: bytes) -> dict[str, bytes]:
    reader = Reader(data)
    magic, version, count, size = reader.values("<4sIIQ")
    if magic != b"VITR" or version != 1 or count > 32768:
        raise ValueError("invalid source rawfile profile header")
    result = {}
    for _ in range(count):
        name_size, payload_size = reader.values("<II")
        if not 0 < name_size <= 4096 or payload_size > 32 << 20:
            raise ValueError("invalid source rawfile profile record")
        name = reader.text(name_size)
        if name in result:
            raise ValueError("duplicate source rawfile profile record")
        result[name] = reader.take(payload_size)
    reader.finish()
    if sum(map(len, result.values())) != size:
        raise ValueError("source rawfile profile payload size mismatch")
    return result


def build_rawfiles(records: dict[str, bytes]) -> bytes:
    return struct.pack("<4sIIQ", b"VITR", 1, len(records),
                       sum(map(len, records.values()))) + b"".join(
        struct.pack("<II", len(name.encode()), len(payload)) + name.encode() + payload
        for name, payload in records.items())


def replace_once(text: str, pattern: str, replacement: str, reason: str) -> str:
    result, count = re.subn(pattern, lambda _: replacement, text, flags=re.M | re.I)
    if count != 1:
        raise ValueError(f"source runtime profile expected one {reason}, found {count}")
    return result


def change_record(name: str, before: bytes, after: bytes, reasons: list[str]) -> dict:
    return {"name": name, "sourceSha256": hashlib.sha256(before).hexdigest(),
            "outputSha256": hashlib.sha256(after).hexdigest(), "reasons": reasons}


def profile_scripts(data: bytes) -> tuple[bytes, list[dict]]:
    records = rawfiles(data)
    original = dict(records)
    reports = []
    changes = {
        "maps/mp/gametypes/_globallogic.gsc": [
            (r"^\s*level\.onlineGame\s*=\s*true\s*;", "\n\tlevel.onlineGame = false;",
             "offline session configuration"),
        ],
        "maps/mp/gametypes/_hardpoints.gsc": [
            (rf'^\s*precacheItem\(\s*"{name}"\s*\)\s*;',
             "\n\t// Vita capability: killstreak item unavailable.", f"{name} precache")
            for name in ("radar_mp", "airstrike_mp", "helicopter_mp", "artillery_mp")
        ] + [
            (r"^init\(\s*\)\s*\{",
             "init()\n{\n\tlevel.vitaHardpointsEnabled = false;",
             "fixed-loadout killstreak capability"),
            (r"^\s*maps\\mp\\_helicopter::init\(\s*\)\s*;",
             "\n\t// Vita capability: helicopter system unavailable.", "helicopter startup"),
            (r"^giveHardpointItemForStreak\(\s*\)\s*\{",
             "giveHardpointItemForStreak()\n{\n\tif ( !level.vitaHardpointsEnabled )\n\t\treturn;",
             "killstreak award entry"),
        ],
        "maps/mp/gametypes/_gameobjects.gsc": [
            (rf'^\s*precacheItem\(\s*"{name}"\s*\)\s*;',
             "\n\t// Vita capability: objective mode item unavailable.", f"{name} precache")
            for name in ("briefcase_bomb_mp", "briefcase_bomb_defuse_mp")
        ],
    }
    for name, replacements in changes.items():
        if name not in records:
            raise ValueError(f"source runtime profile missing {name}")
        text = records[name].decode("utf-8")
        reasons = []
        for pattern, replacement, reason in replacements:
            text = replace_once(text, pattern, replacement, reason)
            reasons.append(reason)
        records[name] = text.encode()
        reports.append(change_record(name, original[name], records[name], reasons))
    # An accidental rule change must never rewrite the original DM callbacks.
    if records.get("maps/mp/gametypes/dm.gsc") != original.get("maps/mp/gametypes/dm.gsc"):
        raise ValueError("source DM rules changed during capability configuration")
    return build_rawfiles(records), reports


def tables(data: bytes) -> dict[str, list[list[str]]]:
    reader = Reader(data)
    magic, version, count, size = reader.values("<4sIIQ")
    if magic != b"VITT" or version != 1 or count > 32768:
        raise ValueError("invalid source table profile header")
    result = {}
    for _ in range(count):
        name_size, rows, columns = reader.values("<III")
        if not 0 < name_size <= 4096 or rows * columns > 16 << 20:
            raise ValueError("invalid source table profile record")
        name = reader.text(name_size)
        if name in result:
            raise ValueError("duplicate source table profile record")
        values = []
        for _ in range(rows * columns):
            cell_size, = reader.values("<I")
            if cell_size > 4096:
                raise ValueError("invalid source table profile cell")
            values.append(reader.text(cell_size))
        result[name] = [values[i * columns:(i + 1) * columns] for i in range(rows)]
    reader.finish()
    if size != len(data) - 20:
        raise ValueError("source table profile size mismatch")
    return result


def build_tables(records: dict[str, list[list[str]]]) -> bytes:
    parts = []
    for name, rows in records.items():
        columns = len(rows[0]) if rows else 0
        if any(len(row) != columns for row in rows):
            raise ValueError("ragged source table profile")
        parts.append(struct.pack("<III", len(name.encode()), len(rows), columns) + name.encode())
        parts.extend(struct.pack("<I", len(cell.encode())) + cell.encode()
                     for row in rows for cell in row)
    body = b"".join(parts)
    return struct.pack("<4sIIQ", b"VITT", 1, len(records), len(body)) + body


def profile_tables(data: bytes) -> tuple[bytes, list[dict]]:
    records = tables(data)
    original = {name: [list(row) for row in rows] for name, rows in records.items()}
    lookup = {name.casefold(): name for name in records}
    if len(lookup) != len(records) or not {"mp/statstable.csv", "mp/offline_classtable.csv"} <= lookup.keys():
        raise ValueError("source runtime profile table identities are missing or duplicated")
    stats = records[lookup["mp/statstable.csv"]]
    if not stats or any(len(row) < 9 for row in stats):
        raise ValueError("source stats table shape changed")
    disabled, enabled = [], set()
    for row in stats:
        if not row[0].isdigit() or not 0 <= int(row[0]) <= 149 or not row[4]:
            continue
        if row[4] in LOADOUT:
            enabled.add(row[4])
        else:
            disabled.append(row[4])
            row[4] = ""
        row[8] = ""  # attachments are outside the fixed loadout
    if enabled != set(LOADOUT):
        raise ValueError("source stats table is missing fixed-loadout identities")
    offline_name = lookup["mp/offline_classtable.csv"]
    offline = records[offline_name]
    rows = {row[1]: row for row in offline if len(row) == 7 and row[1].isdigit()}
    # Preserve the source class machinery and row identities. Values are
    # references to original source definitions, not replacement gameplay.
    values = {
        200: ("frag_grenade", "100", "1"), 201: ("mp5", "10", ""),
        202: ("none", "0", ""), 203: ("usp", "2", ""),
        204: ("none", "0", ""), 205: ("specialty_null", "191", ""),
        206: ("specialty_null", "191", ""), 207: ("specialty_null", "191", ""),
        208: ("flash_grenade", "101", "1"), 209: ("camo_none", "0", ""),
    }
    for index, value in values.items():
        if str(index) not in rows:
            raise ValueError("source offline class 1 is incomplete")
        rows[str(index)][4:7] = value
    reports = []
    for name, reasons in ((lookup["mp/statstable.csv"], ["fixed weapons", "no attachments"]),
                          (offline_name, ["OFFLINE_CLASS1 fixed loadout; no perks"])):
        report = change_record(name, build_tables({name: original[name]}),
                               build_tables({name: records[name]}), reasons)
        if name.casefold() == "mp/statstable.csv":
            report["unavailableWeapons"] = disabled
        reports.append(report)
    return build_tables(records), reports


def profile_entities(data: bytes) -> tuple[bytes, list[dict]]:
    """Apply the original non-oldschool pickup removal before asset mounting.

    _oldschool::removePickups deletes these entities and their linked triggers
    in ordinary DM. Doing that during conversion avoids loading excluded weapon
    definitions solely to create and immediately delete those pickups.
    """
    reader = Reader(data)
    magic, version, size = reader.values("<4sII")
    if magic != b"VITE" or version != 1 or size > 2 << 20:
        raise ValueError("invalid source entity profile header")
    text = reader.take(size).decode("utf-8")
    reader.finish()
    # Tokenize without interpreting quoted braces as entity boundaries. Keep
    # exact byte ranges of every retained record, including unknown fields.
    token = re.compile(r'\s+|\x00|//[^\r\n]*|/\*.*?\*/|"[^"\x00]*"|[{}]', re.S)
    at, start, fields, key = 0, None, {}, None
    records = []
    while at < len(text):
        match = token.match(text, at)
        if not match:
            raise ValueError("invalid source entity profile token")
        value = match.group()
        begin, at = at, match.end()
        if value == "\0":
            # The exporter retains the original C-string terminator before
            # appending renderer-only static instances. Preserve that seam.
            if start is not None:
                raise ValueError("NUL inside source entity profile record")
            continue
        if value.isspace() or value.startswith(("//", "/*")):
            continue
        if value == "{":
            if start is not None:
                raise ValueError("nested source entity profile record")
            start, fields, key = begin, {}, None
        elif value == "}":
            if start is None or key is not None:
                raise ValueError("incomplete source entity profile record")
            records.append((start, at, fields))
            start = None
        elif start is None:
            raise ValueError("source entity profile field outside record")
        elif key is None:
            key = value[1:-1]
            if key in fields:
                raise ValueError("duplicate source entity profile field")
        else:
            fields[key], key = value[1:-1], None
    if start is not None or not records or records[0][2].get("classname") != "worldspawn":
        raise ValueError("incomplete source entity profile world")
    removed = {i for i, (_, _, values) in enumerate(records)
               if values.get("targetname") == "oldschool_pickup"}
    for index in tuple(removed):
        target = records[index][2].get("target")
        if target:
            linked = [i for i, (_, _, values) in enumerate(records)
                      if values.get("targetname") == target]
            if len(linked) != 1:
                raise ValueError("oldschool pickup target is missing or ambiguous")
            removed.update(linked)
    parts, reports, previous = [], [], 0
    for index in sorted(removed):
        begin, end, fields = records[index]
        if fields.get("classname", "").startswith(("worldspawn", "mp_dm_spawn")):
            raise ValueError("source entity profile would remove a required DM root")
        parts.append(text[previous:begin])
        previous = end
        report = change_record(f"entity[{index}]", text[begin:end].encode(), b"",
                               ["source non-oldschool removePickups capability"])
        report["fields"] = {key: fields[key] for key in (
            "classname", "model", "origin", "targetname", "target") if key in fields}
        reports.append(report)
    parts.append(text[previous:])
    payload = "".join(parts).encode()
    return struct.pack("<4sII", b"VITE", 1, len(payload)) + payload, reports
