#!/usr/bin/env python3
"""Build the pinned VitaSDK OpenAL Soft port without modifying the SDK."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import tarfile
import urllib.request

from build_vitagl import digest, publish, run


def build(workspace: Path) -> Path:
    directory = workspace / "kisakcod/dependencies/vita"
    config = json.loads((directory / "vita-dependencies.json").read_text())["openalSoft"]
    patch = directory / config["patch"]
    if digest(patch) != config["patchSha256"]:
        raise ValueError("OpenAL Vita patch differs from its pin")
    host_patch = directory / config["hostToolsPatch"]
    if digest(host_patch) != config["hostToolsPatchSha256"]:
        raise ValueError("OpenAL host-tools patch differs from its pin")
    voice_patch = directory / config["voiceMemoryPatch"]
    if digest(voice_patch) != config["voiceMemoryPatchSha256"]:
        raise ValueError("OpenAL voice-memory patch differs from its pin")
    root = workspace / "build/vita/dependencies" / ("openal-1.19.1-" + digest(patch)[:12] + "-" + digest(host_patch)[:12] + "-" + digest(voice_patch)[:12])
    root.mkdir(parents=True, exist_ok=True)
    archive = root / "openal-source.tar.gz"
    if not archive.exists():
        urllib.request.urlretrieve(config["sourceUrl"], archive)
    if digest(archive) != config["sourceSha256"]:
        raise ValueError("OpenAL source archive hash differs from its pin")
    source = root / "source"
    if not source.exists():
        source.mkdir()
        with tarfile.open(archive) as packed:
            for entry in packed.getmembers():
                relative = Path(*Path(entry.name).parts[1:])
                if not relative.parts:
                    continue
                entry.name = relative.as_posix()
                packed.extract(entry, source, filter="data")
        run("git", "init", "--quiet", str(source))
        run("git", "apply", "--check", str(patch), cwd=source)
        run("git", "apply", str(patch), cwd=source)
        run("git", "apply", "--check", str(host_patch), cwd=source)
        run("git", "apply", str(host_patch), cwd=source)
        run("git", "apply", "--check", str(voice_patch), cwd=source)
        run("git", "apply", str(voice_patch), cwd=source)
        hashes = {path.relative_to(source).as_posix(): digest(path) for path in source.rglob("*")
                  if path.is_file() and ".git" not in path.relative_to(source).parts}
        (root / "source-hashes.json").write_text(json.dumps(hashes, indent=2) + "\n")
    hashes = json.loads((root / "source-hashes.json").read_text())
    for name, expected in hashes.items():
        if digest(source / name) != expected:
            raise ValueError(f"Pinned OpenAL source changed: {name}")
    sdk = workspace / ".tools/vitasdk"
    env = dict(os.environ, VITASDK=str(sdk))
    env["PATH"] = str(sdk / "bin") + os.pathsep + env["PATH"]
    target = root / "build"
    native = root / "host-tools"
    generator = ["-G", "Visual Studio 17 2022", "-A", "x64"] if os.name == "nt" else ["-G", "Ninja"]
    run("cmake", "-S", str(source / "native-tools"), "-B", str(native), *generator,
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5")
    run("cmake", "--build", str(native), "--config", "Release", "-j", "4")
    options = ["-DCMAKE_BUILD_TYPE=Release", "-DLIBTYPE=STATIC", "-DALSOFT_UTILS=OFF",
               "-DALSOFT_EXAMPLES=OFF", "-DALSOFT_TESTS=OFF", "-DALSOFT_CONFIG=OFF",
               "-DALSOFT_INSTALL=ON", "-DALSOFT_EMBED_HRTF_DATA=OFF", "-DALSOFT_BACKEND_WAVE=OFF",
               "-DALSOFT_BACKEND_VITA=ON", "-DALSOFT_REQUIRE_VITA=ON",
               "-DKISAK_OPENAL_NATIVE_TOOLS_DIR=" + str(native), "-DCMAKE_DISABLE_FIND_PACKAGE_Git=TRUE",
               "-DCMAKE_POLICY_VERSION_MINIMUM=3.5", "-DALSOFT_CPUEXT_NEON=ON",
               "-DCMAKE_C_FLAGS=-std=gnu11 -Wno-error=implicit-function-declaration -Wno-error=int-conversion "
               "-ffunction-sections -fdata-sections -ffile-prefix-map=" + source.as_posix() + "=/openal-soft"]
    run("cmake", "-S", str(source), "-B", str(target), "-G", "Ninja",
        "-DCMAKE_TOOLCHAIN_FILE=" + str(sdk / "share/vita.toolchain.cmake"), *options, env=env)
    run("cmake", "--build", str(target), "--target", "OpenAL", "-j", "4", env=env)
    output = workspace / "build/vita/openal-soft"
    publish(target / "libopenal.a", output / "libopenal.a")
    for header in (source / "include/AL").glob("*.h"):
        publish(header, output / "include/AL" / header.name)
    compiler = sdk / "bin/arm-vita-eabi-gcc.exe"
    provenance = {"schemaVersion": 1, "pin": config, "options": options, "sourceFiles": hashes,
                  "compilerSha256": digest(compiler), "librarySha256": digest(output / "libopenal.a")}
    (output / "openal-build.json").write_text(json.dumps(provenance, indent=2) + "\n")
    print(f"Built pinned OpenAL Soft Vita backend: {output / 'libopenal.a'}")
    return output / "libopenal.a"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    args = parser.parse_args()
    try:
        build(args.workspace.resolve())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"OpenAL build failed: {error}")
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stdout or "", error.stderr or "")
        raise SystemExit(2)
