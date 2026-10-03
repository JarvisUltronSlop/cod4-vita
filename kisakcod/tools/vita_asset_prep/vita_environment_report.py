#!/usr/bin/env python3
"""Report the external prerequisites for the accelerated Vita slice.

This is intentionally read-only.  It checks the configured VitaSDK, the
Vita3K executable/data locations, the legal guest shader compiler module, the
optional OpenAL SDK surface, and the two local SELF artifacts.  It never
starts Vita3K and never scans or modifies unrelated game/emulator trees.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
from typing import Any


REPORT_FORMAT = "CODVIT-ENV-REPORT-1"


def _path_info(path: Path | None) -> dict[str, Any]:
    if path is None:
        return {"path": None, "present": False, "file": False, "directory": False}
    try:
        file_present = path.is_file()
        directory_present = path.is_dir()
    except OSError:
        file_present = False
        directory_present = False
    return {
        "path": str(path),
        "present": file_present or directory_present,
        "file": file_present,
        "directory": directory_present,
    }


def _sha256(path: Path | None) -> str | None:
    if path is None or not path.is_file():
        return None
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError:
        return None
    return digest.hexdigest().upper()


def _first_existing(candidates: list[Path]) -> Path | None:
    return next((candidate for candidate in candidates if candidate.is_file()), None)


def build_report(args: argparse.Namespace) -> dict[str, Any]:
    vita_sdk = Path(args.vita_sdk).expanduser() if args.vita_sdk else None
    if vita_sdk is None:
        env_sdk = os.environ.get("VITASDK", "")
        vita_sdk = Path(env_sdk).expanduser() if env_sdk else None

    vita3k_exe = Path(args.vita3k_exe).expanduser() if args.vita3k_exe else None
    data_root = Path(args.vita3k_data_root).expanduser() if args.vita3k_data_root else None
    if data_root is None:
        appdata = os.environ.get("APPDATA", "")
        if appdata:
            data_root = Path(appdata) / "Vita3K" / "Vita3K"

    strict_self = Path(args.strict_self).expanduser() if args.strict_self else None
    scalar_self = Path(args.scalar_self).expanduser() if args.scalar_self else None

    shader_candidates: list[Path] = []
    if data_root is not None:
        shader_candidates = [
            data_root / "ur0" / "data" / "libshacccg.suprx",
            data_root / "ur0" / "data" / "external" / "libshacccg.suprx",
        ]
    shader = _first_existing(shader_candidates)

    sdk_toolchain = vita_sdk / "share" / "vita.toolchain.cmake" if vita_sdk else None
    sdk_cmake = vita_sdk / "share" / "vita.cmake" if vita_sdk else None
    openal_candidates: list[Path] = []
    if vita_sdk:
        openal_candidates = [
            vita_sdk / "include" / "AL" / "al.h",
            vita_sdk / "include" / "AL" / "alc.h",
            vita_sdk / "lib" / "libopenal.a",
            vita_sdk / "lib" / "libopenal-soft.a",
        ]
    openal_present = [str(path) for path in openal_candidates if path.is_file()]

    artifacts = {
        "strictSelf": {
            **_path_info(strict_self),
            "sha256": _sha256(strict_self),
        },
        "scalarSelf": {
            **_path_info(scalar_self),
            "sha256": _sha256(scalar_self),
        },
    }
    prerequisites = {
        "vitaSdk": {
            **_path_info(vita_sdk),
            "toolchain": _path_info(sdk_toolchain),
            "vitaCmake": _path_info(sdk_cmake),
        },
        "vita3k": {
            "executable": _path_info(vita3k_exe),
            "dataRoot": _path_info(data_root),
            "shaderCompiler": _path_info(shader),
            "shaderCandidates": [str(path) for path in shader_candidates],
        },
        "openal": {
            "present": bool(openal_present),
            "matches": openal_present,
        },
    }
    gates = {
        "vitaSdk": bool(sdk_toolchain and sdk_toolchain.is_file() and sdk_cmake and sdk_cmake.is_file()),
        "vita3k": bool(vita3k_exe and vita3k_exe.is_file()),
        "shaderCompiler": bool(shader),
        "strictSelf": bool(strict_self and strict_self.is_file()),
        "scalarSelf": bool(scalar_self and scalar_self.is_file()),
    }
    gates["acceleratedReady"] = all(
        gates[key] for key in ("vitaSdk", "vita3k", "shaderCompiler", "strictSelf")
    )
    return {
        "format": REPORT_FORMAT,
        "cwd": str(Path.cwd()),
        "prerequisites": prerequisites,
        "artifacts": artifacts,
        "gates": gates,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vita-sdk", type=Path, help="VitaSDK root; defaults to $env:VITASDK")
    parser.add_argument("--vita3k-exe", type=Path, help="Vita3K executable")
    parser.add_argument("--vita3k-data-root", type=Path, help="Vita3K data root")
    parser.add_argument("--strict-self", type=Path, help="accelerated/production SELF")
    parser.add_argument("--scalar-self", type=Path, help="diagnostic scalar SELF")
    parser.add_argument("--output", type=Path, help="write JSON to this path")
    args = parser.parse_args()
    report = build_report(args)
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8")
        print(f"vita environment report: {args.output}")
    else:
        print(encoded, end="")
    return 0 if report["gates"]["acceleratedReady"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
