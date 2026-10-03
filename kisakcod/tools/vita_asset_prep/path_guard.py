#!/usr/bin/env python3
"""Validate local COD4/workspace paths without modifying either tree."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


class PathGuardError(RuntimeError):
    pass


def is_within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def validate(workspace: Path, cod4_root: Path) -> dict[str, str]:
    workspace = workspace.resolve()
    cod4_root = cod4_root.resolve()
    if not workspace.is_dir():
        raise PathGuardError(f"workspace does not exist: {workspace}")
    if not cod4_root.is_dir():
        raise PathGuardError(f"COD4 root does not exist: {cod4_root}")
    if is_within(cod4_root, workspace) or is_within(workspace, cod4_root):
        raise PathGuardError("workspace and COD4 root must be separate trees")
    required = [cod4_root / "iw3mp.exe", cod4_root / "zone", cod4_root / "main"]
    missing = [str(path) for path in required if not path.exists()]
    if missing:
        raise PathGuardError("COD4 installation is missing: " + ", ".join(missing))
    return {"workspace": str(workspace), "cod4Root": str(cod4_root), "status": "ok"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--cod4-root", type=Path, required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(validate(args.workspace, args.cod4_root), indent=2))
        return 0
    except PathGuardError as exc:
        print(f"path-guard: {exc}")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
