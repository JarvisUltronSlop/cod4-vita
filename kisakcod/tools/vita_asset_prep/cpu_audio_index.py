"""Stage decoder metadata for unchanged loose streams; never rewrite audio."""
import hashlib
import json
from pathlib import Path
import subprocess


def digest(path):
    with Path(path).open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest()


def prepare(root, stream_root=None, output_path=None):
    root = Path(root).resolve()
    source = root / 'kisakcod'
    build = root / 'build/host-tests'
    output = Path(output_path) if output_path else root / 'build/cpu-preservation/cpu-stream-index.bin'
    output.parent.mkdir(parents=True,exist_ok=True)
    stream_root=Path(stream_root) if stream_root else root / 'Data/CODVIT001/stream'
    report = output.with_suffix('.json')
    inputs = {'streams': {p.relative_to(stream_root).as_posix(): digest(p)
                          for p in sorted((stream_root / 'sound').rglob('*'))
                          if p.is_file() and p.suffix in ('.mp3', '.wav')},
              'decoder': {name: digest(source / 'deps/dr_libs' / name) for name in ('dr_mp3.h', 'dr_wav.h')},
              'tool': digest(source / 'tools/vita_asset_prep/cpu_audio_index.cpp'),
              'format': digest(source / 'src/vita/vita_cpu_audio_index.h')}
    if output.exists() and report.exists():
        old = json.loads(report.read_text())
        if old.get('inputs') == inputs and old.get('sha256') == digest(output):
            return output, old
    for command in (
        ['cmake', '-S', str(source / 'tests/vita_portable'), '-B', str(build)],
        ['cmake', '--build', str(build), '--config', 'Release', '--target', 'vita-cpu-audio-index'],
        [str(build / 'Release/vita-cpu-audio-index.exe'), str(stream_root), str(output)],
    ):
        subprocess.run(command, check=True)
    result = {'schema': 1, 'inputs': inputs, 'sha256': digest(output),
              'quality': 'Original files and formats unchanged; metadata only',
              'runtimeValidation': 'Size, first/last 4096-byte CRC, decoder format; full stream identities recorded here'}
    report.write_text(json.dumps(result, indent=2) + '\n')
    return output, result


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, default=Path(__file__).resolve().parents[3])
    args = parser.parse_args()
    output, report = prepare(args.workspace)
    print(json.dumps({'output': str(output), 'sha256': report['sha256']}))
