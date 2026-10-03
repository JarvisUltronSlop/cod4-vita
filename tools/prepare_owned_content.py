"""Build an offline exporter, convert owned Steam COD4 data, and build a local VPK.

Never downloads game data or writes into the supplied installation. Each run gets
a new workspace-local directory; incomplete conversion is never an install set.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import urllib.request
import uuid
import zipfile

WORKSPACE = Path(__file__).resolve().parents[1]
ASSET_TOOLS = WORKSPACE / 'kisakcod/tools/vita_asset_prep'
sys.path.insert(0, str(ASSET_TOOLS))

def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')

def check_install(owned, maps):
    owned = Path(owned).resolve(strict=True)
    # Isolation works in both directions: an output must not be inside the game.
    if owned.is_relative_to(WORKSPACE) or WORKSPACE.is_relative_to(owned):
        raise ValueError('Keep the Steam installation and this checkout in separate directories.')
    required = ['iw3mp.exe', 'localization.txt', 'main', 'zone/english']
    required += [f'zone/english/{name}.ff' for name in maps]
    for name in required:
        if not (owned / name).exists():
            raise ValueError('Missing Steam COD4 input: ' + name + ' (English 2007 PC release required)')
    if not list((owned / 'main').glob('*.iwd')):
        raise ValueError('No COD4 IWD archives found under main.')
    if 'mp_shipment' not in maps:
        raise ValueError('Include mp_shipment; it supplies the common frontend closure.')
    return owned

def command(args, *, cwd=WORKSPACE, env=None, log=None):
    print('Running ' + str(args[0]) + ' ' + ' '.join(str(a) for a in args[1:3]), flush=True)
    if log:
        with Path(log).open('w', encoding='utf-8') as stream:
            result = subprocess.run([str(a) for a in args], cwd=cwd, env=env,
                                    stdout=stream, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError(f'Command failed; see {log}')
    else:
        subprocess.run([str(a) for a in args], cwd=cwd, env=env, check=True)

def build_exporter(run):
    dx = WORKSPACE / '.tools/dxsdk-d3dx'
    if not (dx / 'build/native/include/d3dx9.h').is_file():
        dx.mkdir(parents=True, exist_ok=True)
        package = dx / 'Microsoft.DXSDK.D3DX.9.29.952.8.nupkg'
        # Microsoft's redistributable headers/import libraries; no game or Sony files.
        urllib.request.urlretrieve(
            'https://www.nuget.org/api/v2/package/Microsoft.DXSDK.D3DX/9.29.952.8', package)
        with zipfile.ZipFile(package) as archive:
            for entry in archive.infolist():
                if not (dx / entry.filename).resolve().is_relative_to(dx.resolve()):
                    raise ValueError('Unsafe DirectX package member')
            archive.extractall(dx)
    build = WORKSPACE / 'build/public-exporter'
    command(['cmake', '-S', WORKSPACE / 'kisakcod', '-B', build,
             '-G', 'Visual Studio 17 2022', '-A', 'Win32',
             '-DKISAK_VITA_ASSET_EXPORTER=ON', '-DCMAKE_BUILD_TYPE=Release',
             '-DCICD=ON', '-DDXSDK_DIR=' + (dx / 'build/native').as_posix()],
            log=run / 'exporter-configure.log')
    command(['cmake', '--build', build, '--config', 'Release',
             '--target', 'KisakCOD-mp', '--parallel', '4'], log=run / 'exporter-build.log')
    exporter = build / 'bin/Release/KisakCOD-vita-exporter.exe'
    if not exporter.is_file():
        raise ValueError('Offline exporter was not produced')
    for name in ('D3DX9_43.dll', 'D3DCompiler_43.dll'):
        runtime = dx / 'build/native/release/bin/x86' / name
        if not runtime.is_file():
            raise ValueError('Missing Microsoft D3DX package runtime: ' + name)
        shutil.copy2(runtime, exporter.parent / name)
    return exporter

def export_map(exporter, owned, name, run, timeout):
    home = run / name / 'home'
    runtime = run / name / 'runtime'
    for folder in (home / 'main', home / 'profiles', home / 'players/profiles/vitaexport', runtime):
        folder.mkdir(parents=True, exist_ok=True)
    (home / 'main/language.cfg').write_text('// local export language shim\n', encoding='ascii')
    (home / 'profiles/active.txt').write_text('vitaexport', encoding='ascii')
    (home / 'players/profiles/vitaexport/config_mp.cfg').write_text('seta name "vitaexport"\n', encoding='ascii')
    shutil.copy2(owned / 'localization.txt', runtime)
    # The Microsoft D3DX runtime is obtained from the user's OS/game installation.
    # The exporter has no Bink, Miles or Steam SDK link or runtime dependency.
    env = dict(os.environ, KISAK_VITA_EXPORT='1', KISAK_VITA_EXPORT_HEADLESS='1',
               KISAK_PROFILE_NAME='vitaexport', ALSOFT_DRIVERS='null')
    args = [str(exporter), '+set', 'fs_basepath', str(owned), '+set', 'fs_homepath', str(home),
            '+set', 'fs_cdpath', '', '+set', 'r_fullscreen', '0', '+set', 'in_mouse', '0',
            '+set', 'snd_volume', '0', '+set', 'com_maxfps', '30',
            '+wait', '10', '+map', name, '+dump_vita_assets']
    console = home / 'main/console_mp.log'
    print('Exporting ' + name + ' from your owned installation...', flush=True)
    with (run / name / 'stdout.log').open('w', encoding='utf-8') as output:
        process = subprocess.Popen(args, cwd=runtime, env=env, stdout=output,
                                   stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if console.exists():
                    text = console.read_text(errors='replace')
                    if 'Vita font export: ok' in text:
                        return home / 'main'
                if process.poll() is not None:
                    raise RuntimeError(f'{name} exporter exited ({process.returncode}); see {run / name}')
                time.sleep(0.5)
            raise RuntimeError(f'{name} export timed out; see {run / name}')
        finally:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=30)

def convert(exporter, owned, maps, run, timeout):
    from shipment_typed_export import build_package
    from source_sound_closure import prepare as sounds
    from prepare_map_closure import prepare as images
    from map_catalog import map_index
    from vertical_slice_packs import build, load_inputs
    from validate_vita_candidate import validate_content
    inputs = {}
    for name in maps:
        exported = export_map(exporter, owned, name, run, timeout)
        typed = run / name / 'typed.kvap'
        report = build_package(exported / 'vita_assets.csv', WORKSPACE, typed, owned)
        if not report.get('playable'):
            raise ValueError(f'{name} typed export is not runtime-ready; see its report in {typed.parent}')
        closure = run / name / 'audio'
        sounds(exported, WORKSPACE / 'kisakcod/src', owned, closure, WORKSPACE)
        image_root = run / name / 'images'
        images(typed, map_index(name), owned, image_root, WORKSPACE)
        inputs[name] = {'source': str(typed), 'soundClosure': str(closure / 'sound-closure.json'),
                        'supplement': str(image_root / 'supplement.kvap')}
    spec = run / 'map-inputs.json'
    write_json(spec, inputs)
    content = run / 'content'
    build(None, content, WORKSPACE, None, None, source_runtime_profile=True,
          retail_root=owned, map_inputs=load_inputs(spec), texture_paging=len(maps) > 1)
    validate_content(content)
    return content

def build_vpk(content, delivery, run):
    from cpu_audio_index import prepare as audio_index
    from cpu_replay_tape import prepare as replay
    from cpu_pack_layout import prepare as pack_layout
    from record_vita_build import validate_vpk
    from stage_cpu import verify_tls_link
    sdk = WORKSPACE / '.tools/vitasdk'
    env = dict(os.environ, VITASDK=str(sdk))
    env['PATH'] = str(sdk / 'bin') + os.pathsep + env['PATH']
    for script, flags in [('build_vitagl.py', ['--cpu-cache', '--cache-padding', '--shader-failure', '--store-depth']),
                          ('build_openal.py', [])]:
        command([sys.executable, ASSET_TOOLS / script, '--workspace', WORKSPACE, *flags],
                log=run / (script + '.log'))
    source = WORKSPACE / 'kisakcod'
    # Keep retail icon pixels out of the application; users' assets stay external.
    command(['cmake', '--preset', 'vita-cpu-candidate', '-DKISAK_VITA_APP_ICON='],
            cwd=source, env=env, log=run / 'vita-configure.log')
    command(['cmake', '--build', '--preset', 'vita-cpu-candidate'],
            cwd=source, env=env, log=run / 'vita-build.log')
    target = WORKSPACE / 'build/vita/cpu-candidate/scripts/platform/vita'
    ownership = json.loads((target / 'vita-source-ownership.json').read_text())
    if not ownership.get('passed'):
        raise ValueError('Native source ownership audit failed')
    verify_tls_link(WORKSPACE, target / 'KisakCOD-vita-bootstrap')
    indices = run / 'indices'
    indices.mkdir()
    audio, _ = audio_index(WORKSPACE, content / 'stream', indices / 'cpu-stream-index.bin')
    tape, _ = replay(WORKSPACE)
    layout = indices / 'cpu-pack-layout.bin'
    names = sorted(p.name for p in content.glob('*.kvap'))
    layout.write_bytes(pack_layout(content, names))
    vpk = delivery / 'COD4-Vita.vpk'
    original = target / 'KisakCOD-vita-bootstrap.vpk'
    validate_vpk(original, target / 'KisakCOD-vita-bootstrap.self')
    with zipfile.ZipFile(original) as incoming, zipfile.ZipFile(vpk, 'w', zipfile.ZIP_DEFLATED) as output:
        members = {name: incoming.read(name) for name in incoming.namelist()}
        for file in (audio, tape, layout):
            members[file.name] = file.read_bytes()
        for name, data in sorted(members.items()):
            entry = zipfile.ZipInfo(name, (2000, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            output.writestr(entry, data)
    validate_vpk(vpk, target / 'KisakCOD-vita-bootstrap.self')
    write_json(delivery / 'build-info.json', {
        'hardwareQualified': False, 'kind': 'public-source-development',
        'sourceCommit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=WORKSPACE, text=True).strip(),
        'vpkSha256': digest(vpk), 'selfSha256': digest(target / 'KisakCOD-vita-bootstrap.self'),
        'contentManifestSha256': digest(content / 'vertical-slice-manifest.json'),
        'sourceOwnershipPassed': True})

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cod4-root', type=Path, required=True)
    parser.add_argument('--maps', nargs='+', choices=('mp_shipment', 'mp_crash', 'mp_crossfire'), default=['mp_shipment'])
    parser.add_argument('--exporter', type=Path)
    parser.add_argument('--export-timeout', type=int, default=600)
    parser.add_argument('--content-only', action='store_true')
    parser.add_argument('--check-only', action='store_true')
    args = parser.parse_args()
    try:
        if sys.version_info < (3, 11) or os.name != 'nt':
            raise ValueError('The current exporter workflow requires Windows and Python 3.11 or newer.')
        maps = list(dict.fromkeys(args.maps))
        owned = check_install(args.cod4_root, maps)
        for tool in ('git', 'cmake'):
            if not shutil.which(tool): raise ValueError('Install required build tool: ' + tool)
        if not args.content_only:
            if not (WORKSPACE / '.tools/vitasdk/share/vita.toolchain.cmake').is_file():
                raise ValueError('Install the Windows VitaSDK toolchain and libraries in .tools/vitasdk; see BUILDING.md.')
            if not shutil.which('ninja'): raise ValueError('Install Ninja and add it to PATH.')
        if args.exporter and not args.exporter.resolve().is_file():
            raise ValueError('The supplied offline exporter does not exist.')
        if args.check_only:
            print('Owned installation and tool paths are valid. No files were converted.')
            return 0
        run_id = time.strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:8]
        run = WORKSPACE / 'build/owned-content' / run_id
        run.mkdir(parents=True)
        exporter = args.exporter.resolve() if args.exporter else build_exporter(run)
        content = convert(exporter, owned, maps, run, args.export_timeout)
        delivery = WORKSPACE / 'deploy/owned-content' / run_id
        delivery.mkdir(parents=True)
        data = delivery / 'ux0/data/CODVIT001'
        data.mkdir(parents=True)
        shutil.copytree(content / 'stream', data / 'stream', dirs_exist_ok=True)
        (data / 'paks').mkdir()
        for file in content.glob('*.kvap'): shutil.copy2(file, data / 'paks' / file.name)
        shutil.copy2(content / 'vertical-slice-manifest.json', data)
        if not args.content_only: build_vpk(content, delivery, run)
        (delivery / 'INSTALL.txt').write_text(
            'Development build; hardware qualification is incomplete.\n'
            'Install COD4-Vita.vpk with VitaShell, then copy ux0/data/CODVIT001 to ux0:data/CODVIT001.\n'
            'Install your separately obtained libshacccg.suprx at ur0:data/libshacccg.suprx.\n'
            'Keep the entire matching data set together. Do not upload this folder.\n', encoding='utf-8')
        print('Prepared your local install set: ' + str(delivery))
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print('Owned-content preparation failed: ' + str(error), file=sys.stderr)
        return 2

if __name__ == '__main__':
    raise SystemExit(main())
