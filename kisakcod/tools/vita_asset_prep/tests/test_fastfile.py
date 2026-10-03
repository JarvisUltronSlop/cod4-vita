from __future__ import annotations

import importlib.util
import tempfile
import zlib
from pathlib import Path


MODULE = Path(__file__).resolve().parents[1] / "fastfile.py"
SPEC = importlib.util.spec_from_file_location("fastfile", MODULE)
assert SPEC and SPEC.loader
fastfile = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(fastfile)


def test_decompresses_iwff_envelope() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        source = root / "fixture.ff"
        payload = b"typed export still pending\0"
        source.write_bytes(fastfile.HEADER.pack(fastfile.MAGIC, 5) + zlib.compress(payload))
        report, decoded = fastfile.decompress(source)
        assert decoded == payload
        assert report["magic"] == "IWffu100"
        assert report["version"] == 5
        assert report["status"] == "decompressed-x86-db-pending-typed-export"


if __name__ == "__main__":
    test_decompresses_iwff_envelope()
    print("fastfile tests: ok")
