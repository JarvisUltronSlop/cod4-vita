#!/usr/bin/env python3
"""Normalize VPK ZIP timestamps without changing payload bytes.

vita-pack-vpk uses the source file mtimes for ZIP metadata.  That makes an
otherwise identical VPK hash vary from build to build.  VPKs are ordinary ZIP
archives, so normalize the local-file and central-directory DOS timestamps in
place after the normal VitaSDK pack step.  Keeping the compressed payload and
directory order untouched preserves the VitaSDK-produced archive exactly apart
from reproducibility metadata.
"""

from __future__ import annotations

import argparse
import struct
import zipfile
from pathlib import Path


LOCAL_SIG = b"PK\x03\x04"
CENTRAL_SIG = b"PK\x01\x02"
EOCD_SIG = b"PK\x05\x06"

# 00:00:00, 1980-01-01 — the earliest representable DOS date/time.
FIXED_DOS_TIME = 0
FIXED_DOS_DATE = 33


def _patch_local_headers(data: bytearray, archive: zipfile.ZipFile) -> None:
    for info in archive.infolist():
        offset = info.header_offset
        if data[offset : offset + 4] != LOCAL_SIG:
            raise ValueError(f"invalid local header at offset {offset}")
        struct.pack_into("<HH", data, offset + 10, FIXED_DOS_TIME, FIXED_DOS_DATE)


def _patch_central_directory(data: bytearray) -> int:
    eocd = data.rfind(EOCD_SIG)
    if eocd < 0 or eocd + 22 > len(data):
        raise ValueError("VPK is missing the ZIP end-of-central-directory record")

    (
        _signature,
        _disk,
        _central_disk,
        _entries_on_disk,
        entries,
        central_size,
        central_offset,
        comment_size,
    ) = struct.unpack_from("<4s4H2LH", data, eocd)
    if central_offset + central_size > len(data):
        raise ValueError("VPK central-directory range is outside the archive")
    if eocd + 22 + comment_size > len(data):
        raise ValueError("VPK end-of-central-directory comment is truncated")

    cursor = central_offset
    for _ in range(entries):
        if data[cursor : cursor + 4] != CENTRAL_SIG:
            raise ValueError(f"invalid central-directory header at offset {cursor}")
        # Central-directory fixed header: signature (4) + version fields,
        # flags, method, then DOS time/date at offsets 12 and 14.
        struct.pack_into("<HH", data, cursor + 12, FIXED_DOS_TIME, FIXED_DOS_DATE)
        name_size, extra_size, comment_len = struct.unpack_from("<HHH", data, cursor + 28)
        cursor += 46 + name_size + extra_size + comment_len

    if cursor != central_offset + central_size:
        raise ValueError("central-directory entry count/size mismatch")
    return entries


def normalize_vpk(path: Path) -> int:
    data = bytearray(path.read_bytes())
    with zipfile.ZipFile(path, "r") as archive:
        _patch_local_headers(data, archive)
    entries = _patch_central_directory(data)
    path.write_bytes(data)
    return entries


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path, help="VPK to normalize in place")
    args = parser.parse_args()
    if not args.path.is_file():
        parser.error(f"VPK does not exist: {args.path}")
    entries = normalize_vpk(args.path)
    print(f"normalized {entries} VPK ZIP entries: {args.path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
