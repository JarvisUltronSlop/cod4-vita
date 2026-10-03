"""Build an isolated replay adapter from the frozen reference, never today's gameplay.

The original reference VPK/build/source archive are read-only inputs. The new
delivery is exclusively for the common-clock correctness comparison; use the
original reference for ordinary loading/performance measurements.
"""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import zipfile

from cpu_replay_tape import prepare
from stage_cpu import digest, verify_tls_link


def build(root, compile=True):
    root = Path(root).resolve()
    original = root / 'deploy/cpu-reference'
    frozen = json.loads((original / 'validation.json').read_text())
    source_zip = original / 'source.zip'
    if digest(source_zip) != frozen['sourceArchiveSha256']:
        raise ValueError('Frozen reference source archive identity differs')
    if digest(original / 'KisakCOD-Vita-CPU-Reference.vpk') != frozen['vpkSha256']:
        raise ValueError('Frozen reference VPK identity differs')
    tree = root / 'build/cpu-replay-reference-source'
    source = tree / 'kisakcod'
    current = root / 'kisakcod'
    original_bytes = {}
    with zipfile.ZipFile(source_zip) as archive:
        for name in archive.namelist():
            path = Path(name)
            if path.is_absolute() or '..' in path.parts or path.parts[0] != 'kisakcod':
                raise ValueError('Unsafe source archive path: ' + name)
            if name.endswith('/'):
                continue
            data = archive.read(name)
            target = tree / path
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists() or target.read_bytes() != data:
                target.write_bytes(data)
            original_bytes[name] = data
    def normalized(data):
        return data.decode('utf-8').replace('\r\r\n', '\n').replace('\r\n', '\n')
    def edit(name, replacements, include=False):
        path = source / name
        text = normalized(path.read_bytes())
        if include:
            text = '#include <vita/vita_cpu_replay.h>\n' + text
        for old, new in replacements:
            if text.count(old) != 1:
                raise ValueError('Frozen-source replay patch anchor differs: ' + name + ': ' + old[:96])
            text = text.replace(old, new, 1)
        path.write_bytes(text.encode())
    # Only the observation/recording/tape implementation is shared. No candidate
    # graphics, audio, source adapter, scheduling, asset or memory code is copied.
    shared = ('vita_cpu_profile.h', 'vita_cpu_profile.cpp', 'vita_cpu_queue.h', 'vita_cpu_recorder_start.h', 'vita_cpu_tls_probe.h',
              'vita_cpu_replay.h', 'vita_cpu_replay.cpp', 'vita_cpu_replay_tape.h',
              'vita_cpu_replay_frontend.inc', 'vita_crt_random.h')
    for name in shared:
        shutil.copy2(current / 'src/vita' / name, source / 'src/vita' / name)
    edit('src/vita/vita_source_session.cpp', [
        ('    registry_ = registry;', '    registry_ = registry;\n    VitaCpuReplay_SourceInitialize();'),
        ('    if (elapsedMs > kMaximumRunningFrameMs)',
         '    if(VitaCpuReplay_TapeActive() && !VitaCpuReplay_NextFrame(elapsedMs))\n'
         '        return Fail("source replay tape exhausted");\n    if (elapsedMs > kMaximumRunningFrameMs)'),
        ('            CL_SendCmd(0);', '            CL_SendCmd(0);\n'
         '            VitaCpuReplay_Command(clients[0].cmds[clients[0].cmdNumber & 127],clients[0].cmdNumber);'),
        ('        ++status_.frameServerTicks;', '        ++status_.frameServerTicks;\n'
         '        if(level.clients)VitaCpuReplay_State("server",level.clients[0].ps,status_.serverTicks);\n'
         '        VitaCpuReplay_ServerWorld(status_.serverTicks);'),
        ('        UpdatePresentation();', '        UpdatePresentation();\n'
         '        VitaCpuReplay_State("client",CG_GetLocalClientGlobals(0)->predictedPlayerState,status_.completedClientFrames);'),
    ], include=True)
    edit('src/vita/vita_lowlevel_services.cpp', [
        ('    static const auto start = std::chrono::steady_clock::now();',
         '    static const auto start = std::chrono::steady_clock::now();\n    uint32_t replayTime=0;\n'
         '    if(kisak::vita::VitaCpuReplay_Clock(&replayTime))return replayTime;')], include=True)
    edit('src/vita/vita_input.cpp', [
        ('bool VitaInput_ReadSourceSample(VitaInputSample* sample) {',
         'bool VitaInput_ReadSourceSample(VitaInputSample* sample) {\n'
         '    if(kisak::vita::VitaCpuReplay_Input(sample))return true;')], include=True)
    edit('src/vita/vita_frontend.h', [
        ('    bool comparisonRequested_=false;', '    bool sourceReplayRequested_=false;\n    bool comparisonRequested_=false;'),
        ('    bool ConsumeComparisonRequest()', '    bool ConsumeSourceReplayRequest() {const bool v=sourceReplayRequested_;sourceReplayRequested_=false;return v;}\n    bool ConsumeComparisonRequest()')])
    edit('src/vita/vita_frontend.cpp', [
        ('    RunComparison,', '    RunComparison,\n    RunSourceReplay,'),
        ('    {"VITA_COMPARISON", "RUN PERFORMANCE COMPARISON", true, VitaFrontendAction::RunComparison},',
         '    {"VITA_COMPARISON", "RUN PERFORMANCE COMPARISON", true, VitaFrontendAction::RunComparison},\n'
         '    {"VITA_SOURCE_REPLAY", "RUN SOURCE REPLAY", true, VitaFrontendAction::RunSourceReplay},'),
        ('void VitaFrontend::Reset() {', 'void VitaFrontend::Reset() {\n    sourceReplayRequested_=false;'),
        ('    case VitaFrontendAction::RunComparison:', '    case VitaFrontendAction::RunSourceReplay:\n'
         '        sourceReplayRequested_=true;screen_=VitaFrontendScreen::Main;selection_=1;return true;\n'
         '    case VitaFrontendAction::RunComparison:')])
    edit('src/vita/vita_bootstrap.cpp', [
        ('        TraceGameplayFrame("input_polled");',
         '        TraceGameplayFrame("input_polled");\n#include <vita/vita_cpu_replay_frontend.inc>')], include=True)
    edit('src/game_mp/g_utils_mp.cpp', [
        ('void __cdecl G_AddEvent(gentity_s *ent, uint32_t event, uint32_t eventParm)\n{',
         'void __cdecl G_AddEvent(gentity_s *ent, uint32_t event, uint32_t eventParm)\n{\n'
         '    kisak::vita::VitaCpuReplay_Event("server-entity",ent->s.number,event,eventParm);')], include=True)
    edit('src/bgame/bg_misc.cpp', [
        ('void __cdecl BG_AddPredictableEventToPlayerstate(entity_event_t newEvent, uint32_t eventParm, playerState_s *ps)\n{',
         'void __cdecl BG_AddPredictableEventToPlayerstate(entity_event_t newEvent, uint32_t eventParm, playerState_s *ps)\n{\n'
         '    kisak::vita::VitaCpuReplay_Event("predictable",ps->clientNum,uint32_t(newEvent),eventParm);')], include=True)
    edit('src/universal/com_math.cpp', [
        ('static uint32_t holdrand;', 'static uint32_t holdrand;\nuint32_t VitaCpuGameplayRandomState(){return holdrand;}')])
    edit('scripts/platform/vita/CMakeLists.txt', [
        ('target_link_options(KisakCOD-vita-bootstrap PRIVATE -Wl,--gc-sections',
         'target_link_options(KisakCOD-vita-bootstrap PRIVATE -Wl,--gc-sections\n    -Wl,--undefined=pthread_cancel'),
        ('    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_source_session.cpp',
         '    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_source_session.cpp\n'
         '    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_cpu_replay.cpp')])
    # Dependencies were not included in the original source archive. Use exactly
    # the retained local embedded sources; identities are recorded below.
    for name in ('dr_libs', 'zlib', 'ode'):
        shutil.copytree(current / 'deps' / name, source / 'deps' / name, dirs_exist_ok=True)
    delta = []
    for path in sorted(source.rglob('*')):
        if not path.is_file() or 'deps' in path.relative_to(source).parts:
            continue
        name = path.relative_to(tree).as_posix()
        before, after = original_bytes.get(name, b''), path.read_bytes()
        if before != after:
            delta.extend(difflib.unified_diff(normalized(before).splitlines(True), normalized(after).splitlines(True),
                                             fromfile='a/' + name, tofile='b/' + name))
    delivery = root / 'deploy/cpu-replay-reference'
    delivery.mkdir(parents=True, exist_ok=True)
    patch = delivery / 'replay-adapter.patch'
    patch.write_text(''.join(delta), encoding='utf-8')
    fixture, fixture_identity = prepare(root)
    buildroot = root / 'build/vita/cpu-replay-reference'
    with zipfile.ZipFile(original / 'KisakCOD-Vita-CPU-Reference.vpk') as archive:
        base_identity = json.loads(archive.read('cpu-identity.json'))
    for key, folder, library in (('vitaGL', 'vitaGL-nosplash', 'libvitaGL.a'), ('openalSoft', 'openal-soft', 'libopenal.a')):
        if digest(root / 'build/vita' / folder / library) != base_identity['dependencies'][key]['librarySha256']:
            raise ValueError('Original dependency changed: ' + key)
    env = dict(os.environ, VITASDK=str(root / '.tools/vitasdk'))
    env['PATH'] = str(root / '.tools/vitasdk/bin') + os.pathsep + env['PATH']
    if compile:
        subprocess.run(['cmake', '-S', str(source), '-B', str(buildroot), '-G', 'Ninja',
                        '-DCMAKE_TOOLCHAIN_FILE=' + str(root / '.tools/vitasdk/share/vita.toolchain.cmake'),
                        '-DCMAKE_BUILD_TYPE=Release', '-DKISAK_PLATFORM=vita', '-DKISAK_VITA_CPU_PROFILE=ON',
                        '-DKISAK_VITA_CPU_RUNTIME=OFF', '-DKISAK_VITA_BALANCED_CANDIDATE=ON',
                        '-DKISAK_VITA_BENCHMARK=ON', '-DKISAK_VITA_BENCHMARK_ROUND=9', '-DKISAK_VITA_PROFILE=OFF',
                        '-DKISAK_VITA_VITAGL_LIBRARY=' + str(root / 'build/vita/vitaGL-nosplash/libvitaGL.a'),
                        '-DKISAK_VITA_VITASHARK_LIBRARY=' + str(root / 'build/vita/vitaShaRK/libvitashark.a'),
                        '-DKISAK_VITA_OPENAL_LIBRARY=' + str(root / 'build/vita/openal-soft/libopenal.a'),
                        '-DPython3_EXECUTABLE=' + str(root / '.tools/vita-python-tests/Scripts/python.exe')],
                       cwd=root, env=env, check=True)
        subprocess.run(['cmake', '--build', str(buildroot), '--target', 'KisakCOD-vita-bootstrap.vpk-vpk',
                        'KisakCOD-vita-source-ownership-report', '--parallel', '2'], cwd=root, env=env, check=True)
    target = buildroot / 'scripts/platform/vita'
    tls_link = verify_tls_link(root, target / 'KisakCOD-vita-bootstrap')
    ownership = json.loads((target / 'vita-source-ownership.json').read_text())
    if not ownership['passed']:
        raise ValueError('Replay reference source ownership failed')
    identity = dict(base_identity, kind='cpu-replay-reference', hardwareQualified=False,
                    threadLocalLinkage=tls_link,
                    baseSourceArchiveSha256=frozen['sourceArchiveSha256'], replayAdapterSha256=digest(patch),
                    replayTape=fixture_identity, elfSha256=digest(target / 'KisakCOD-vita-bootstrap'))
    identity['sourceFiles'] = {p.relative_to(source).as_posix(): digest(p) for p in sorted(source.rglob('*'))
                               if p.is_file() and p.suffix in ('.cpp', '.c', '.h', '.inc', '.hpp')}
    identity['sourceTreeSha256'] = hashlib.sha256(json.dumps(identity['sourceFiles'], sort_keys=True).encode()).hexdigest()
    identity['configurationSha256'] = digest(buildroot / 'CMakeCache.txt')
    identity['ownershipSha256'] = digest(target / 'vita-source-ownership.json')
    identity['actualFiles'] = dict(base_identity['actualFiles'], **{
        'app0:eboot.bin': digest(target / 'KisakCOD-vita-bootstrap.self'),
        'app0:cpu-source-replay.bin': digest(fixture)})
    identity_bytes = (json.dumps(identity, sort_keys=True, separators=(',', ':')) + '\n').encode()
    (delivery / 'cpu-identity.json').write_bytes(identity_bytes)
    output = delivery / 'KisakCOD-Vita-CPU-Replay-Reference.vpk'
    pending = delivery / 'replay.pending.vpk'
    with zipfile.ZipFile(target / 'KisakCOD-vita-bootstrap.vpk') as source_vpk, zipfile.ZipFile(pending, 'w') as archive:
        if hashlib.sha256(source_vpk.read('eboot.bin')).hexdigest() != identity['actualFiles']['app0:eboot.bin']:
            raise ValueError('Replay reference input SELF mismatch')
        entries = {name: source_vpk.read(name) for name in source_vpk.namelist()}
        entries.update({'cpu-source-replay.bin': fixture.read_bytes(), 'cpu-identity.json': identity_bytes,
                        'benchmark-identity.json': identity_bytes})
        for name, data in sorted(entries.items()):
            if name.endswith('.kvap') or 'libshacccg' in name.lower():
                raise ValueError('Unexpected content in replay VPK')
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0));info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)
    with zipfile.ZipFile(pending) as archive:
        if archive.testzip() or hashlib.sha256(archive.read('eboot.bin')).hexdigest() != identity['actualFiles']['app0:eboot.bin']:
            raise ValueError('Replay reference VPK verification failed')
    if output.exists() and digest(output) != digest(pending):
        shutil.copy2(output, delivery / ('previous-' + digest(output)[:16] + '.vpk'))
    pending.replace(output)
    for path, name in ((target / 'KisakCOD-vita-bootstrap', 'KisakCOD-vita-bootstrap.elf'),
                       (target / 'vita-source-ownership.json', 'vita-source-ownership.json'),
                       (buildroot / 'CMakeCache.txt', 'CMakeCache.txt')):
        shutil.copy2(path, delivery / name)
    with zipfile.ZipFile(delivery / 'source.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(source.rglob('*')):
            if path.is_file() and '__pycache__' not in path.parts:
                archive.write(path, path.relative_to(tree))
    report = {'kind': 'cpu-replay-reference', 'vpkSha256': digest(output), 'elfSha256': identity['elfSha256'],
              'selfSha256': identity['actualFiles']['app0:eboot.bin'], 'sourceArchiveSha256': digest(delivery / 'source.zip'),
              'baseSourceArchiveSha256': frozen['sourceArchiveSha256'], 'replayAdapterSha256': digest(patch),
              'replayTape': fixture_identity, 'ownershipPassed': True, 'archiveVerified': True,
              'hardwareQualified': False, 'gameplayParityQualified': False,
              'scope': 'Frozen source plus explicit tape/clock/observer adapter; original source frame order preserved. Not the performance reference.'}
    (delivery / 'validation.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument('--skip-build', action='store_true')
    args = parser.parse_args()
    build(args.workspace, not args.skip_build)
