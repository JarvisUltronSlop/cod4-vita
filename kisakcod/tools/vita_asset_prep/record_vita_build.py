#!/usr/bin/env python3
"""Record and validate one exact native Vita executable/package generation."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile


DIAGNOSTIC_FLAGS = (
    "ALLOW_SOFTWARE_FALLBACK", "CAPTURE_SOFTWARE_FRAME", "CAPTURE_OVERLAY_ONLY",
    "VITA3K_NO_AUDIO", "EXPERIMENTAL_NATIVE_ENTITY_RUNTIME", "MENU_ONLY",
    "SMOKE_AUTOSTART_SHIPMENT", "SMOKE_GAMEPLAY_ONLY", "SMOKE_AUTHORED_FX_EVENT",
    "SMOKE_LITE_ASSETS", "DEBUG_NO_FRAME_EFFECTS", "NATIVE_FRAME_PROBE",
    "NATIVE_DIRECT_DISPLAY_PROBE", "NATIVE_SKIP_CLEAR_PROBE",
    "NATIVE_SKIP_VIEWMODEL_PROBE", "NATIVE_SKIP_MODEL_COLOR_PROBE",
    "NATIVE_CPU_TEXTURE_UPLOAD", "CLEAN_VIEWMODEL",
)
DIAGNOSTIC_VALUES = ("MENU_SCREEN", "SMOKE_MAP_INDEX", "NATIVE_TRIANGLE_BUDGET")
FORBIDDEN_PAYLOADS = {".kvap", ".iwd", ".ff", ".pak", ".bik", ".mp4", ".at9", ".wav", ".dds"}


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def read_cache(path: Path) -> dict[str, str]:
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line and not line.startswith(("#", "//")) and ":" in line and "=" in line:
            key, value = line.split("=", 1)
            values[key.split(":", 1)[0]] = value
    return values


def validate_configuration(cache: dict[str, str], configuration: str) -> None:
    expected_type = "RelWithDebInfo" if configuration == "Profile" else "Release"
    if cache.get("CMAKE_BUILD_TYPE") != expected_type:
        raise ValueError("compiler optimization configuration does not match the requested build")
    flags = cache.get("CMAKE_CXX_FLAGS_RELWITHDEBINFO" if configuration == "Profile" else "CMAKE_CXX_FLAGS_RELEASE", "").split()
    if "-O3" not in flags or "-DNDEBUG" not in flags:
        raise ValueError("Release and Profile must share -O3/NDEBUG code generation")
    profile_enabled = cache.get("KISAK_VITA_PROFILE", "OFF").upper() in ("ON", "TRUE", "1")
    if profile_enabled != (configuration == "Profile"):
        raise ValueError("profiling instrumentation does not match the requested configuration")
    for key in DIAGNOSTIC_FLAGS:
        if cache.get("KISAK_VITA_" + key, "OFF").upper() not in ("OFF", "FALSE", "0", ""):
            raise ValueError(f"native package contains diagnostic option {key}")
    for key in DIAGNOSTIC_VALUES:
        if cache.get("KISAK_VITA_" + key, "") != "":
            raise ValueError(f"native package contains diagnostic override {key}")
    for key in ("SOURCE_RUNTIME", "EXTERNAL_CONTENT"):
        if cache.get("KISAK_VITA_" + key, "").upper() not in ("ON", "TRUE", "1"):
            raise ValueError(f"native package requires {key}")


def validate_vpk(vpk: Path, self_file: Path) -> str:
    with zipfile.ZipFile(vpk) as archive:
        names = [entry.filename.replace("\\", "/").lower() for entry in archive.infolist()]
        if names.count("eboot.bin") != 1:
            raise ValueError("VPK must contain exactly one eboot.bin")
        for name in names:
            if "libshacccg" in name or Path(name).suffix in FORBIDDEN_PAYLOADS:
                raise ValueError(f"VPK contains an external content/runtime payload: {name}")
        executable = archive.read(next(entry for entry in archive.namelist()
                                       if entry.replace("\\", "/").lower() == "eboot.bin"))
    embedded = hashlib.sha256(executable).hexdigest()
    if embedded != sha256(self_file):
        raise ValueError("VPK eboot.bin does not match the current SELF")
    return embedded


def validate_release_symbols(symbols: str) -> None:
    # A false CMake definition is still 'defined' to the preprocessor. Inspect
    # the linked implementations, not just the cache, to catch that mistake.
    for name in ("GfxVita_QueueProfileCapture(", "GfxVita_CaptureProfileStage(",
                 "CaptureNativeSceneFrame(", "CaptureNativeInputContracts(",
                 "VitaSourceInputSmoke::"):
        if name in symbols:
            raise ValueError(f"Release retains diagnostic implementation {name}")


def source_identity(workspace: Path) -> dict[str, object]:
    def git(*args: str) -> str:
        return subprocess.check_output(["git", "-C", str(workspace), *args], text=True).strip()
    names = git("ls-files", "--cached", "--others", "--exclude-standard", "-z").split("\0")
    extensions = {".cpp", ".c", ".h", ".hpp", ".inc", ".cmake", ".ps1", ".py", ".patch"}
    files = {}
    for name in sorted(set(names)):
        path = workspace / name
        if path.is_file() and (path.suffix in extensions or path.name in ("CMakeLists.txt", "CMakePresets.json", "vita-dependencies.json")):
            files[name] = sha256(path)
    encoded = json.dumps(files, sort_keys=True, separators=(",", ":")).encode()
    return {"head": git("rev-parse", "HEAD"), "branch": git("branch", "--show-current"),
            "sourceSha256": hashlib.sha256(encoded).hexdigest(), "sourceFiles": files}


def record(workspace: Path, build: Path, configuration: str) -> dict[str, object]:
    cache = read_cache(build / "CMakeCache.txt")
    validate_configuration(cache, configuration)
    directory = build / "scripts/platform/vita"
    files = {"elf": directory / "KisakCOD-vita-bootstrap",
             "self": directory / "KisakCOD-vita-bootstrap.self",
             "vpk": directory / "KisakCOD-vita-bootstrap.vpk",
             "ownership": directory / "vita-source-ownership.json",
             "linkMap": directory / "vita-source-link.map"}
    hashes = {name: sha256(path) for name, path in files.items()}
    embedded = validate_vpk(files["vpk"], files["self"])
    if configuration == "Release":
        nm = workspace / ".tools/vitasdk/bin/arm-vita-eabi-nm.exe"
        symbols = subprocess.check_output([str(nm), "-C", "--defined-only", str(files["elf"])], text=True)
        validate_release_symbols(symbols)
    ownership = json.loads(files["ownership"].read_text(encoding="utf-8"))
    if ownership.get("binarySha256") != hashes["elf"]:
        raise ValueError("ownership report does not match the current ELF")
    if ownership.get("linkMapSha256") != hashes["linkMap"]:
        raise ValueError("ownership report does not match the current link map")
    library = Path(cache["KISAK_VITA_VITAGL_LIBRARY"])
    dependency = json.loads((library.parent / "vitaGL-build.json").read_text())
    pins = json.loads((workspace / "kisakcod/dependencies/vita/vita-dependencies.json").read_text())
    if (dependency["librarySha256"] != sha256(library) or
            dependency["revision"] != pins["vitaGL"]["revision"] or
            dependency["patchSha256"] != pins["vitaGL"]["patchSha256"]):
        raise ValueError("VitaGL dependency provenance does not match the pinned build")
    expected_flags = dict(pins["vitaGL"]["flags"])
    if configuration == "Profile":
        expected_flags.update(pins["vitaGL"]["profileFlags"])
    if dependency["flags"] != expected_flags:
        raise ValueError("VitaGL flags do not match the release/profile configuration")
    shark = Path(cache["KISAK_VITA_VITASHARK_LIBRARY"])
    if (sha256(shark) != dependency["vitaShaRK"]["librarySha256"] or
            dependency["vitaShaRK"]["revision"] != pins["vitaShaRK"]["revision"]):
        raise ValueError("VitaShaRK does not match the VitaGL dependency build")
    openal_library = Path(cache["KISAK_VITA_OPENAL_LIBRARY"])
    openal = json.loads((openal_library.parent / "openal-build.json").read_text())
    if openal["pin"] != pins["openalSoft"] or openal["librarySha256"] != sha256(openal_library):
        raise ValueError("OpenAL does not match its pinned source and Vita patches")
    compiler = workspace / ".tools/vitasdk/bin/arm-vita-eabi-g++.exe"
    compiler_version = subprocess.check_output([str(compiler), "--version"], text=True).splitlines()[0]
    return {"schemaVersion": 1, "configuration": configuration,
            "artifacts": hashes, "ebootSha256": embedded,
            "nativeConfigurationValidated": True,
            "releaseDiagnosticsAbsent": True if configuration == "Release" else None,
            "sourceOwnershipPassed": bool(ownership.get("passed", False)),
            "sourceSimulationLinkPassed": bool(ownership.get("simulationLinkPassed", False)),
            "hardwareQualified": False, "compiler": compiler_version,
            "vitaGlSha256": sha256(library), "dependencies": dependency, "openal": openal,
            "source": source_identity(workspace),
            "cache": {key: value for key, value in cache.items()
                      if key.startswith("KISAK_VITA_") or key in ("CMAKE_BUILD_TYPE", "CMAKE_CXX_FLAGS_RELEASE", "CMAKE_CXX_FLAGS_RELWITHDEBINFO")}}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--configuration", choices=("Release", "Profile"), required=True)
    args = parser.parse_args()
    try:
        report = record(args.workspace.resolve(), args.build.resolve(), args.configuration)
        output = args.build / "scripts/platform/vita/vita-build-manifest.json"
        output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"Verified matching native ELF/SELF/VPK generation: {output}")
        return 0
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        print(f"Vita build provenance failed: {error}")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
