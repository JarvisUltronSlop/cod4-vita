#!/usr/bin/env python3
"""Regression checks for the read-only accelerated-environment report."""

from __future__ import annotations

import json
import os
import tempfile
from argparse import Namespace
from pathlib import Path

import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from vita_environment_report import build_report  # noqa: E402


def args_for(root: Path) -> Namespace:
    sdk = root / "vitasdk"
    (sdk / "share").mkdir(parents=True)
    (sdk / "share" / "vita.toolchain.cmake").write_text("toolchain", encoding="utf-8")
    (sdk / "share" / "vita.cmake").write_text("vita", encoding="utf-8")
    data = root / "vita3k"
    shader = data / "ur0" / "data" / "external" / "libshacccg.suprx"
    shader.parent.mkdir(parents=True)
    shader.write_bytes(b"shader")
    exe = root / "Vita3K.exe"
    exe.write_bytes(b"exe")
    strict = root / "strict.self"
    strict.write_bytes(b"strict")
    scalar = root / "scalar.self"
    scalar.write_bytes(b"scalar")
    return Namespace(
        vita_sdk=sdk,
        vita3k_exe=exe,
        vita3k_data_root=data,
        strict_self=strict,
        scalar_self=scalar,
    )


def main() -> int:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        report = build_report(args_for(root))
        assert report["format"] == "CODVIT-ENV-REPORT-1"
        assert report["gates"]["acceleratedReady"] is True
        assert report["prerequisites"]["vita3k"]["shaderCompiler"]["file"] is True
        assert report["artifacts"]["strictSelf"]["sha256"]

        missing = args_for(root / "missing")
        missing.vita3k_data_root = root / "missing" / "no-vita3k"
        report = build_report(missing)
        assert report["gates"]["shaderCompiler"] is False
        assert report["gates"]["acceleratedReady"] is False

    print("vita environment report tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
