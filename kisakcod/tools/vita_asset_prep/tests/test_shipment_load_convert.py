from __future__ import annotations

import importlib.util
import json
import tempfile
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("shipment_load_convert", ROOT / "shipment_load_convert.py")
assert SPEC and SPEC.loader
convert = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(convert)
fastfile_spec = importlib.util.spec_from_file_location("fastfile_fixture", ROOT / "fastfile.py")
assert fastfile_spec and fastfile_spec.loader
fastfile = importlib.util.module_from_spec(fastfile_spec)
fastfile_spec.loader.exec_module(fastfile)


def test_pointer_free_load_records() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        source = root / "load.ff"
        payload = b"$victorybackdrop\0DXT1\0loadscreen_mp_shipment\0"
        source.write_bytes(fastfile.HEADER.pack(fastfile.MAGIC, 5)
                           + zlib.compress(payload))
        result = convert.convert(source)
        assert result["status"] == "typed-load-records"
        assert result["source"]["source"] == "load.ff"
        assert "D:" not in json.dumps(result)
        assert result["assetTypes"]["image"] == [
            {"name": "$victorybackdrop", "format": "DXT1"},
            {"name": "loadscreen_mp_shipment", "format": "DXT1"},
        ]
        assert all("pointer" not in json.dumps(item) for item in result["assetTypes"]["strings"])


if __name__ == "__main__":
    test_pointer_free_load_records()
    print("shipment load conversion tests: ok")
