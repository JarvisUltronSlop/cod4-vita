import hashlib
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
NORMALIZE = ROOT / "normalize_vpk.py"
CMAKE = ROOT.parent.parent / "scripts" / "platform" / "vita" / "CMakeLists.txt"


def make_archive(path, timestamp):
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED) as archive:
        for name, payload in (("sce_sys/param.sfo", b"param"),
                              ("eboot.bin", b"self"),
                              ("data/test.kvap", b"assets")):
            info = zipfile.ZipInfo(name, timestamp)
            info.compress_type = zipfile.ZIP_STORED
            info.external_attr = 0o644 << 16
            archive.writestr(info, payload)


def normalize(path):
    result = subprocess.run([sys.executable, str(NORMALIZE), str(path)],
                            capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    cmake = CMAKE.read_text(encoding="utf-8")
    assert "find_package(Python3 COMPONENTS Interpreter REQUIRED)" in cmake
    assert "KISAK_VITA_VPK_RAW_TARGET" in cmake
    assert "KISAK_VITA_MODEL_SEMANTIC_PACKAGE" in cmake
    assert "shipment-db-model-semantics.kvap" in cmake
    assert "normalize_vpk.py" in cmake
    assert "add_custom_target(KisakCOD-vita-bootstrap.vpk-vpk" in cmake
    with tempfile.TemporaryDirectory() as directory:
        first = Path(directory) / "first.vpk"
        second = Path(directory) / "second.vpk"
        make_archive(first, (2024, 1, 2, 3, 4, 6))
        make_archive(second, (2025, 6, 7, 8, 9, 10))
        assert digest(first) != digest(second)
        normalize(first)
        normalize(second)
        assert digest(first) == digest(second)
        with zipfile.ZipFile(first) as archive:
            assert archive.namelist() == [
                "sce_sys/param.sfo", "eboot.bin", "data/test.kvap"
            ]
            assert archive.read("data/test.kvap") == b"assets"
    print("vita package reproducibility tests: ok")


if __name__ == "__main__":
    main()
