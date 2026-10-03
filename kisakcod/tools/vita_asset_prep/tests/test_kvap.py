from __future__ import annotations

import importlib.util
import json
import tempfile
from pathlib import Path


MODULE = Path(__file__).resolve().parents[1] / "kvap.py"
SPEC = importlib.util.spec_from_file_location("kvap", MODULE)
assert SPEC and SPEC.loader
kvap = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(kvap)


def test_deterministic_round_trip() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        workspace = root / "workspace"
        cod4 = root / "cod4"
        workspace.mkdir()
        cod4.mkdir()
        (cod4 / "iw3mp.exe").write_bytes(b"fixture")
        (cod4 / "zone").mkdir()
        (cod4 / "main").mkdir()
        source = workspace / "converted"
        source.mkdir()
        payload = source / "surface.bin"
        payload.write_bytes(b"fixture surface\0\x01\x02")
        manifest = workspace / "manifest.json"
        manifest.write_text(json.dumps({
            "sourceHash": "00" * 32,
            "chunks": [{"type": "world", "assetId": "shipment.surface", "path": str(payload)}],
        }), encoding="utf-8")
        out_a = workspace / "a.kvap"
        out_b = workspace / "b.kvap"
        kvap.create_package(manifest, out_a, workspace, cod4)
        kvap.create_package(manifest, out_b, workspace, cod4)
        assert out_a.read_bytes() == out_b.read_bytes()
        info = kvap.inspect_package(out_a)
        assert info["format"] == "KVAP"
        assert info["chunks"][0]["rawBytes"] == len(payload.read_bytes())


def test_rejects_output_in_cod4_tree() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        workspace = root / "workspace"
        cod4 = root / "cod4"
        workspace.mkdir()
        cod4.mkdir()
        (cod4 / "iw3mp.exe").write_bytes(b"fixture")
        (cod4 / "zone").mkdir()
        (cod4 / "main").mkdir()
        payload = workspace / "payload.bin"
        payload.write_bytes(b"payload")
        manifest = workspace / "manifest.json"
        manifest.write_text(json.dumps({"chunks": [{"type": "world", "path": str(payload)}]}), encoding="utf-8")
        try:
            kvap.create_package(manifest, cod4 / "bad.kvap", workspace, cod4)
        except kvap.KvapError as exc:
            assert "output must be inside workspace" in str(exc)
        else:
            raise AssertionError("package creation accepted a COD4 output path")


if __name__ == "__main__":
    test_deterministic_round_trip()
    test_rejects_output_in_cod4_tree()
    print("kvap tests: ok")
