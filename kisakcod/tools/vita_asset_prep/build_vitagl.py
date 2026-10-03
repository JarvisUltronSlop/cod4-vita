#!/usr/bin/env python3
"""Build the pinned, patched VitaGL dependency in an isolated source directory.

Uses CMake/Ninja so Windows does not need an unrecorded GNU make installation.
The source list and flags mirror the pinned upstream Makefile. No files in the
SDK or the user's VitaGL checkout are edited.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import zipfile


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(*args: str, cwd: Path | None = None, env: dict | None = None) -> str:
    completed = subprocess.run(args, cwd=cwd, env=env, check=True,
                               capture_output=True, text=True)
    return completed.stdout.strip()


def publish(source: Path, destination: Path) -> None:
    """Retain any previous local dependency before replacing it."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() and digest(destination) != digest(source):
        backup = destination.parent / "previous" / (digest(destination) + "-" + destination.name)
        backup.parent.mkdir(exist_ok=True)
        if not backup.exists():
            shutil.copy2(destination, backup)
    shutil.copy2(source, destination)


def build_shark(workspace: Path, sdk: Path, env: dict) -> tuple[Path, dict]:
    pin = json.loads((workspace / "kisakcod/dependencies/vita/vita-dependencies.json").read_text())["vitaShaRK"]
    repository = workspace / "build/vita/dependencies/vitaShaRK-repository"
    if not repository.exists():
        run("git", "clone", "--no-checkout", pin["repository"], str(repository))
    revision = run("git", "-C", str(repository), "rev-parse", pin["revision"] + "^{commit}")
    if revision != pin["revision"]:
        raise ValueError("VitaShaRK revision does not match its pin")
    root = repository.parent / ("vitaShaRK-" + revision[:12])
    source = root / "source"
    if not source.exists():
        archive = subprocess.check_output(["git", "-C", str(repository), "archive", "--format=zip", revision])
        source.mkdir(parents=True)
        with zipfile.ZipFile(io.BytesIO(archive)) as zipped:
            for member in zipped.infolist():
                if not (source / member.filename).resolve().is_relative_to(source.resolve()):
                    raise ValueError("Invalid path in VitaShaRK archive")
            zipped.extractall(source)
        names = run("git", "-C", str(repository), "ls-tree", "-r", "--name-only", revision).splitlines()
        hashes = {name: digest(source / name) for name in names if (source / name).is_file()}
        (root / "source-hashes.json").write_text(json.dumps(hashes, indent=2) + "\n")
    hashes = json.loads((root / "source-hashes.json").read_text())
    for name, expected in hashes.items():
        if digest(source / name) != expected:
            raise ValueError(f"Pinned VitaShaRK source has changed: {name}")
    project = root / "project"
    project.mkdir(exist_ok=True)
    (project / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
project(COD4PinnedVitaShaRK LANGUAGES C)
add_library(vitashark STATIC "{(source / 'source/vitashark.c').as_posix()}")
target_compile_options(vitashark PRIVATE -g -O2 -ffast-math -mtune=cortex-a9 -mfpu=neon -ftree-vectorize
  "-ffile-prefix-map={source.as_posix()}=/vitaShaRK")
''')
    target = root / "build"
    run("cmake", "-S", str(project), "-B", str(target), "-G", "Ninja",
        "-DCMAKE_TOOLCHAIN_FILE=" + str(sdk / "share/vita.toolchain.cmake"), env=env)
    run("cmake", "--build", str(target), "-j", "4", env=env)
    output = workspace / "build/vita/vitaShaRK"
    publish(target / "libvitashark.a", output / "libvitashark.a")
    publish(source / "source/vitashark.h", output / "include/vitashark.h")
    provenance = {"revision": revision, "librarySha256": digest(output / "libvitashark.a"),
                  "headerSha256": digest(output / "include/vitashark.h"), "sourceFiles": hashes}
    (output / "vitaShaRK-build.json").write_text(json.dumps(provenance, indent=2) + "\n")
    return output, provenance


def build(workspace: Path, profile: bool, source_repository: Path, cpu_cache: bool = False, cache_padding: bool = False, shader_failure: bool = False, store_depth: bool = False) -> Path:
    if store_depth and (not shader_failure or profile):
        raise ValueError("Depth preservation requires the patched CPU release dependency")
    config_dir = workspace / "kisakcod/dependencies/vita"
    config = json.loads((config_dir / "vita-dependencies.json").read_text())["vitaGL"]
    if not source_repository.exists():
        run("git", "clone", "--no-checkout", config["repository"], str(source_repository))
    patch = config_dir / config["patch"]
    if digest(patch) != config["patchSha256"]:
        raise ValueError("VitaGL patch does not match the dependency pin")
    cache_pin = None
    cache_patch = None
    init_pin = None
    init_patch = None
    if cpu_cache:
        cache_pin = json.loads((config_dir / "vita-cpu-cache.json").read_text())
        cache_patch = config_dir / cache_pin["patch"]
        init_pin = json.loads((config_dir / "vita-cpu-init.json").read_text())
        init_patch = config_dir / init_pin["patch"]
        if (init_pin['baseRevision'] != config['revision'] or
                init_pin['baseCachePatchSha256'] != digest(cache_patch) or
                init_pin['patchSha256'] != digest(init_patch)):
            raise ValueError('CPU initialization patch provenance differs')
        if cache_pin["baseRevision"] != config["revision"] or cache_pin["basePatchSha256"] != digest(patch) or digest(cache_patch) != cache_pin["patchSha256"]:
            raise ValueError("CPU cache patch provenance differs from the pinned base")
    padding_pin = None
    padding_patch = None
    if cache_padding:
        if not cpu_cache:
            raise ValueError('Cache padding correction requires the CPU cache/init patches')
        padding_pin = json.loads((config_dir / 'vita-cpu-cache-padding.json').read_text())
        padding_patch = config_dir / padding_pin['patch']
        if (padding_pin['baseRevision'] != config['revision']
                or padding_pin['baseCachePatchSha256'] != digest(cache_patch)
                or padding_pin['baseInitializationPatchSha256'] != digest(init_patch)
                or padding_pin['patchSha256'] != digest(padding_patch)):
            raise ValueError('Cache padding correction provenance differs')
    failure_pin = failure_patch = None
    if shader_failure:
        if not cache_padding:
            raise ValueError('Shader failure correction requires cache padding patch')
        failure_pin = json.loads((config_dir / 'vita-cpu-shader-failure.json').read_text())
        failure_patch = config_dir / failure_pin['patch']
        if (failure_pin['baseRevision'] != config['revision']
                or failure_pin['basePaddingPatchSha256'] != digest(padding_patch)
                or failure_pin['patchSha256'] != digest(failure_patch)):
            raise ValueError('Shader failure patch provenance differs')
    revision = run("git", "-C", str(source_repository), "rev-parse", config["revision"] + "^{commit}")
    if revision != config["revision"]:
        raise ValueError("VitaGL revision does not match the dependency pin")
    mode = "profile" if profile else "release"
    root = workspace / "build/vita/dependencies" / (revision[:12] + "-" + digest(patch)[:12])
    if cache_patch:
        root = root.with_name(root.name + "-cpu-" + digest(cache_patch)[:12])
    if init_patch:
        root = root.with_name(root.name + "-init-" + digest(init_patch)[:12])
    if padding_patch:
        root = root.with_name(root.name + '-padding-' + digest(padding_patch)[:12])
    if failure_patch:
        root = root.with_name(root.name + "-failure-" + digest(failure_patch)[:12])
    if store_depth:
        root = root.with_name("paging-" + hashlib.sha256(root.name.encode()).hexdigest()[:16])
    source = root / "source"
    if not source.exists():
        archive = subprocess.check_output(["git", "-C", str(source_repository), "archive", "--format=zip", revision])
        source.mkdir(parents=True)
        with zipfile.ZipFile(io.BytesIO(archive)) as zipped:
            for member in zipped.infolist():
                target = (source / member.filename).resolve()
                if not target.is_relative_to(source.resolve()):
                    raise ValueError("Invalid path in VitaGL source archive")
            zipped.extractall(source)
        # A dependency-only repository makes git apply independent of the
        # outer workspace prefix. It is not the implementation repository.
        run("git", "init", "--quiet", str(source))
        run("git", "apply", "--check", str(patch), cwd=source)
        run("git", "apply", str(patch), cwd=source)
        if cache_patch:
            run("git", "apply", "--check", str(cache_patch), cwd=source)
            run("git", "apply", str(cache_patch), cwd=source)
        if init_patch:
            run("git", "apply", "--check", str(init_patch), cwd=source)
            run("git", "apply", str(init_patch), cwd=source)
        if padding_patch:
            run("git", "apply", "--check", str(padding_patch), cwd=source)
            run("git", "apply", str(padding_patch), cwd=source)
        if failure_patch:
            run("git", "apply", "--check", str(failure_patch), cwd=source)
            run("git", "apply", str(failure_patch), cwd=source)
        tracked = run("git", "-C", str(source_repository), "ls-tree", "-r", "--name-only", revision).splitlines()
        source_hashes = {name: digest(source / name) for name in tracked if (source / name).is_file()}
        (root / "source-hashes.json").write_text(json.dumps(source_hashes, indent=2) + "\n")
    source_hashes = json.loads((root / "source-hashes.json").read_text())
    for name, expected in source_hashes.items():
        if digest(source / name) != expected:
            raise ValueError(f"Pinned dependency source has changed: {name}")
    sdk = workspace / ".tools/vitasdk"
    env = dict(os.environ, VITASDK=str(sdk))
    env["PATH"] = str(sdk / "bin") + os.pathsep + env["PATH"]
    shark, shark_provenance = build_shark(workspace, sdk, env)
    compiler = sdk / "bin/arm-vita-eabi-gcc.exe"
    if not compiler.is_file():
        compiler = sdk / "bin/arm-vita-eabi-gcc"
    target = root / mode
    project = root / (mode + "-project")
    project.mkdir(exist_ok=True)
    flags = dict(config["flags"])
    if profile:
        flags.update(config["profileFlags"])
    definitions = ["SKIP_SPLASHSCREEN", "HAVE_SHADER_CACHE"]
    if profile:
        definitions.extend(("HAVE_PROFILING", "LOG_ERRORS"))
    expected_flags = {"NO_SPLASHSCREEN": 1, "HAVE_SHADER_CACHE": 1}
    if profile:
        expected_flags["HAVE_PROFILING"] = 1
        expected_flags["LOG_ERRORS"] = 1
    if flags != expected_flags:
        raise ValueError("Dependency flags changed; update and qualify the explicit build recipe")
    if store_depth:
        flags["STORE_DEPTH_STENCIL"] = 1
        definitions.append("STORE_DEPTH_STENCIL")
    # Upstream Makefile SOURCES: source source/utils source/utils/preprocessor.
    files = sorted(path for directory in (source / "source", source / "source/utils", source / "source/utils/preprocessor")
                   for path in directory.iterdir() if path.suffix in (".c", ".cpp"))
    if any(path.relative_to(source).as_posix() not in source_hashes for path in files):
        raise ValueError("An unpinned source file was added to the dependency build")
    cmake_sources = "\n".join(f'  "{path.as_posix()}"' for path in files)
    cmake = f'''cmake_minimum_required(VERSION 3.20)
project(COD4PinnedVitaGL LANGUAGES C CXX)
add_library(vitaGL STATIC
{cmake_sources}
)
target_include_directories(vitaGL PRIVATE "{(source / 'source').as_posix()}" "{(shark / 'include').as_posix()}")
target_compile_options(vitaGL PRIVATE -g -O3 -ffast-math -mtune=cortex-a9 -mfpu=neon
  -Wno-incompatible-pointer-types -Wno-stringop-overflow -mfp16-format=ieee
  "-ffile-prefix-map={source.as_posix()}=/vitaGL"
  $<$<COMPILE_LANGUAGE:CXX>:-fexceptions;-std=gnu++11;-Wno-write-strings>)
target_compile_definitions(vitaGL PRIVATE {' '.join(definitions)} VGL_GIT_HASH="{revision[:7]}")
'''
    (project / "CMakeLists.txt").write_text(cmake)
    run("cmake", "-S", str(project), "-B", str(target), "-G", "Ninja",
        "-DCMAKE_TOOLCHAIN_FILE=" + str(sdk / "share/vita.toolchain.cmake"), cwd=workspace, env=env)
    run("cmake", "--build", str(target), "-j", "4", env=env)
    library = target / "libvitaGL.a"
    output = workspace / "build/vita" / ("vitaGL-profile" if profile else "vitaGL-nosplash")
    if cpu_cache:
        output = workspace / "build/vita" / ("vitaGL-cpu-shader-input" if shader_failure else "vitaGL-cpu-cache-padding" if cache_padding else "vitaGL-cpu")
    if store_depth:
        output = workspace / "build/vita/vitaGL-cpu-paging"
    output.mkdir(parents=True, exist_ok=True)
    publish(library, output / library.name)
    publish(source / "source/vitaGL.h", output / "include/vitaGL.h")
    provenance = {"schemaVersion": 1, "revision": revision, "patchSha256": digest(patch),
                  "flags": flags, "compiler": run(str(compiler), "--version").splitlines()[0],
                  "compilerSha256": digest(compiler), "librarySha256": digest(library),
                  "makefileSha256": source_hashes["Makefile"], "sourceFiles": source_hashes,
                  "vitaShaRK": shark_provenance}
    if cache_pin:
        provenance["cpuCache"] = cache_pin
    if init_pin:
        provenance['cpuInitialization'] = init_pin
    if padding_pin:
        provenance['cpuCachePadding'] = padding_pin
    if failure_pin:
        provenance['cpuShaderFailure'] = failure_pin
    (output / "vitaGL-build.json").write_text(json.dumps(provenance, indent=2) + "\n")
    print(f"Built pinned VitaGL {mode}: {output / library.name}")
    return output / library.name


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--repository", type=Path)
    parser.add_argument("--profile", action="store_true")
    parser.add_argument("--cpu-cache", action="store_true")
    parser.add_argument("--cache-padding", action="store_true")
    parser.add_argument("--shader-failure", action="store_true")
    parser.add_argument("--store-depth", action="store_true")
    args = parser.parse_args()
    try:
        workspace = args.workspace.resolve()
        build(workspace, args.profile, (args.repository or workspace / ".tools/vitaGL").resolve(), args.cpu_cache, args.cache_padding, args.shader_failure, args.store_depth)
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"VitaGL build failed: {error}")
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stdout or "", error.stderr or "")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
