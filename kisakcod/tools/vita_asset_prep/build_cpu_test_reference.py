"""Add the three-minute menu test to frozen reference source, in a separate build.

Only test UI/input/recording boundaries change. Reference scheduling, gameplay,
graphics, audio, startup decoding and pinned dependencies remain the reference.
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

from stage_cpu import digest, verify_tls_link

SHARED = ('vita_shipment_autotest.h', 'vita_shipment_autotest_runtime.h',
          'vita_shipment_autotest_frontend.inc', 'vita_shipment_autotest_overlay.inc')
PATCHED = ('vita_frontend.h', 'vita_frontend.cpp', 'vita_bootstrap.cpp', 'vita_cpu_profile.h', 'vita_cpu_profile.cpp')
SUPPORT = ('vita_cpu_recorder_start.h', 'vita_cpu_tls_probe.h')


def normalized(data):
    return data.decode('utf-8').replace('\r\r\n', '\n').replace('\r\n', '\n')


def build(root, compile=True):
    root = Path(root).resolve()
    original = root / 'deploy/cpu-reference'
    frozen = json.loads((original / 'validation.json').read_text())
    for name, key in (('source.zip', 'sourceArchiveSha256'),
                      ('KisakCOD-Vita-CPU-Reference.vpk', 'vpkSha256'),
                      ('KisakCOD-vita-bootstrap.elf', 'elfSha256')):
        if digest(original / name) != frozen[key]:
            raise ValueError('Frozen reference identity differs: ' + name)
    tree = root / 'build/cpu-test-reference-source'
    source = tree / 'kisakcod'
    current = root / 'kisakcod'
    before = {}
    with zipfile.ZipFile(original / 'source.zip') as archive:
        for name in archive.namelist():
            path = Path(name)
            if path.is_absolute() or '..' in path.parts or path.parts[0] != 'kisakcod':
                raise ValueError('Unsafe source archive path: ' + name)
            if name.endswith('/'):
                continue
            data = archive.read(name)
            target = tree / path
            target.parent.mkdir(parents=True, exist_ok=True)
            if name not in {'kisakcod/src/vita/' + p for p in PATCHED} and (not target.exists() or target.read_bytes() != data):
                target.write_bytes(data)
            before[name] = data

    def edit(name, replacements):
        path = source / name
        text = normalized(before['kisakcod/' + name])
        for old, new in replacements:
            if text.count(old) != 1:
                raise ValueError('Frozen test patch anchor differs: ' + name + ': ' + old[:96])
            text = text.replace(old, new, 1)
        if not path.exists() or path.read_bytes() != text.encode():
            path.write_bytes(text.encode())

    for name in SHARED + SUPPORT:
        shutil.copy2(current / 'src/vita' / name, source / 'src/vita' / name)
    frontend = normalized((current / 'src/vita/vita_frontend.cpp').read_bytes())
    def fragment(start, end):
        if frontend.count(start) != 1 or frontend.count(end) != 1:
            raise ValueError('Shared test UI anchors differ')
        return frontend[frontend.index(start):frontend.index(end)]
    edit('src/vita/vita_frontend.h', [
        ('    Graphics,', '    Graphics,\n    Tests,'),
        ('    bool comparisonRequested_=false;', '    bool comparisonRequested_=false;\n    bool shipmentTestRequested_=false;'),
        ('    bool ConsumeComparisonRequest()',
         '    bool ConsumeShipmentTestRequest() {const bool value=shipmentTestRequested_;shipmentTestRequested_=false;return value;}\n    bool ConsumeComparisonRequest()')])
    edit('src/vita/vita_frontend.cpp', [
        ('    OpenGraphics,', '    OpenGraphics,\n    OpenTests,\n    RunShipmentTest,'),
        ('    {"MENU_QUIT", "QUIT", false, VitaFrontendAction::Disabled},',
         '    {"VITA_TESTS", "TESTS", true, VitaFrontendAction::OpenTests},\n    {"MENU_QUIT", "QUIT", false, VitaFrontendAction::Disabled},'),
        ('constexpr VitaFrontendRouteItem kControlsRoute[] = {',
         fragment('constexpr VitaFrontendRouteItem kTestsRoute[] = {', 'constexpr VitaFrontendRouteItem kControlsRoute[] = {')
         + 'constexpr VitaFrontendRouteItem kControlsRoute[] = {'),
        ('    case kisak::vita::VitaFrontendScreen::Graphics:',
         '    case kisak::vita::VitaFrontendScreen::Tests:\n        if (count) *count = static_cast<int>(std::size(kTestsRoute));\n'
         '        return kTestsRoute;\n    case kisak::vita::VitaFrontendScreen::Graphics:'),
        ('    if (screen == kisak::vita::VitaFrontendScreen::Graphics) return nullptr;',
         '    if (screen == kisak::vita::VitaFrontendScreen::Graphics || screen == kisak::vita::VitaFrontendScreen::Tests) return nullptr;'),
        ('void VitaFrontend::Reset() {', 'void VitaFrontend::Reset() {\n    shipmentTestRequested_=false;'),
        ('    case VitaFrontendScreen::Graphics:\n',
         '    case VitaFrontendScreen::Tests:\n        screen_ = VitaFrontendScreen::Main; selection_=3; break;\n    case VitaFrontendScreen::Graphics:\n'),
        ('    case VitaFrontendAction::RunComparison:',
         fragment('    case VitaFrontendAction::OpenTests:', '    case VitaFrontendAction::RunSourceReplay:')
         + '    case VitaFrontendAction::RunComparison:'),
        ('    case VitaFrontendScreen::Graphics: {',
         fragment('    case VitaFrontendScreen::Tests: {', '    case VitaFrontendScreen::Graphics: {')
         + '    case VitaFrontendScreen::Graphics: {')])
    edit('src/vita/vita_bootstrap.cpp', [
        ('#include <vita/vita_perf.h>', '#include <vita/vita_perf.h>\n#include <vita/vita_shipment_autotest_runtime.h>'),
        ('        if (platformEvent.suspend || platformEvent.resume) {',
         '        if (platformEvent.suspend || platformEvent.resume) {\n            kisak::vita::shipmentAutoTest.Interrupt();'),
        ('        TraceGameplayFrame("input_polled");',
         '        TraceGameplayFrame("input_polled");\n#include <vita/vita_shipment_autotest_frontend.inc>'),
        ('            developerPerfVisible = false;', '            // Preserve overlay choice across menu transitions.'),
        ('            benchmarkLoadIssued=false; developerPerfVisible=false;',
         '            benchmarkLoadIssued=false; developerPerfVisible=true;'),
        ('        if (frontend.InGame() && inputSample.performanceTogglePressed) {',
         '        if (inputSample.performanceTogglePressed) {'),
        ('        gfx->EndFrame();\n#if KISAK_VITA_BENCHMARK',
         '#include <vita/vita_shipment_autotest_overlay.inc>\n        gfx->EndFrame();\n#if KISAK_VITA_BENCHMARK')])
    # An explicit completion acknowledgement must follow draining the records
    # published before the request. No producer or file ownership is changed.
    profile = normalized((current / 'src/vita/vita_cpu_profile.cpp').read_bytes())
    startup = profile[profile.index('void VitaCpuProfile_Startup('):profile.index('VitaCpuRole VitaCpuCurrentRole()')]
    recorder_start = profile[profile.index('    writerThread=VitaCpuStartRecorder('):profile.index('    if(writerThread<0) {')]
    edit('src/vita/vita_cpu_profile.h', [
        ('void VitaCpuProfile_Flush();', 'void VitaCpuProfile_Flush(bool wait=false);'),
        ('bool VitaCpuProfile_Start(const char* kind);',
         'bool VitaCpuProfile_Start(const char* kind);\nvoid VitaCpuProfile_Startup(const char* stage,int result,int priority=0,int affinity=0);')])
    edit('src/vita/vita_cpu_profile.cpp', [
        ('#include <vita/vita_cpu_profile.h>', '#include <vita/vita_cpu_profile.h>\n#include <vita/vita_cpu_recorder_start.h>\n#include <vita/vita_cpu_tls_probe.h>'),
        ('    if (affinity) {', '    if (affinity && sceKernelGetThreadCpuAffinityMask(id)!=affinity) {'),
        ('VitaCpuRole VitaCpuCurrentRole()', startup + 'VitaCpuRole VitaCpuCurrentRole()'),
        ('    sceIoMkdir("ux0:data/CODVIT001/logs",0777);',
         '    sceIoMkdir("ux0:data/CODVIT001/logs",0777);\n'
         '    if(FILE* file=std::fopen("ux0:data/CODVIT001/logs/cpu-startup.jsonl","wb"))std::fclose(file);\n'
         '    VitaCpuProfile_Startup("recorder-initialize",0,kVitaCpuRecorderPriority,SCE_KERNEL_CPU_MASK_USER_2);\n'
         '    if(!VitaCpuVerifyThreadLocals([](const char* stage,int result,int affinity){\n'
         '        VitaCpuProfile_Startup(stage,result,160,affinity);\n'
         '    })) {failed.store(true);return false;}'),
        ('    writerThread=sceKernelCreateThread("cod4-cpu-recorder",Writer,0x10000120,65536,0,SCE_KERNEL_CPU_MASK_USER_2,nullptr);\n'
         '    if(writerThread<0 || sceKernelStartThread(writerThread,0,nullptr)<0) {',
         recorder_start + '    if(writerThread<0) {'),
        ('    for (;;) {\n        bool any=false;',
         '    for (;;) {\n        const auto request=flushRequest.load(std::memory_order_acquire);\n        bool any=false;'),
        ('        const auto request=flushRequest.load(std::memory_order_acquire);\n        if (request!=flushed.load()) {',
         '        if (request!=flushed.load()) {\n            char health[192];\n'
         '            std::snprintf(health,sizeof(health),"{\\"type\\":\\"recording-health\\",\\"atUs\\":%llu,\\"dropped\\":%llu,\\"ioFailed\\":%s}\\n",\n'
         '                (unsigned long long)VitaCpuNow(),(unsigned long long)dropped.load(),failed.load()?"true":"false");\n'
         '            Write(perf,health);Write(loading,health);'),
        ('void VitaCpuProfile_Flush() { if(active.load())flushRequest.fetch_add(1,std::memory_order_release); }',
         'void VitaCpuProfile_Flush(bool wait) {\n    if(!active.load())return;\n'
         '    const auto request=flushRequest.fetch_add(1,std::memory_order_release)+1;\n'
         '    while(wait && flushed.load(std::memory_order_acquire)<request && active.load())sceKernelDelayThread(1000);\n}')])
    edit('scripts/platform/vita/CMakeLists.txt', [
        ('target_link_options(KisakCOD-vita-bootstrap PRIVATE -Wl,--gc-sections',
         'target_link_options(KisakCOD-vita-bootstrap PRIVATE -Wl,--gc-sections\n    -Wl,--undefined=pthread_cancel')])
    for name in ('dr_libs', 'zlib', 'ode'):
        shutil.copytree(current / 'deps' / name, source / 'deps' / name, dirs_exist_ok=True)
    delivery = root / 'deploy/cpu-test-reference'
    delivery.mkdir(parents=True, exist_ok=True)
    delta = []
    changed = []
    for path in sorted(source.rglob('*')):
        if not path.is_file() or 'deps' in path.relative_to(source).parts:
            continue
        name = path.relative_to(tree).as_posix()
        old, new = before.get(name, b''), path.read_bytes()
        if old != new:
            changed.append(name)
            delta.extend(difflib.unified_diff(normalized(old).splitlines(True), normalized(new).splitlines(True),
                                             fromfile='a/' + name, tofile='b/' + name))
    allowed = {'kisakcod/src/vita/' + name for name in SHARED + SUPPORT + PATCHED}
    allowed.add('kisakcod/scripts/platform/vita/CMakeLists.txt')
    if set(changed) != allowed:
        raise ValueError('Unexpected changes in frozen source: ' + repr(set(changed) ^ allowed))
    patch = delivery / 'test-adapter.patch'
    patch.write_bytes(''.join(delta).encode())
    with zipfile.ZipFile(original / 'KisakCOD-Vita-CPU-Reference.vpk') as archive:
        base_identity = json.loads(archive.read('cpu-identity.json'))
    for key, folder, name in (('vitaGL', 'vitaGL-nosplash', 'libvitaGL.a'), ('openalSoft', 'openal-soft', 'libopenal.a')):
        if digest(root / 'build/vita' / folder / name) != base_identity['dependencies'][key]['librarySha256']:
            raise ValueError('Original dependency changed: ' + key)
    buildroot = root / 'build/vita/cpu-test-reference'
    env = dict(os.environ, VITASDK=str(root / '.tools/vitasdk'))
    env['PATH'] = str(root / '.tools/vitasdk/bin') + os.pathsep + env['PATH']
    if compile:
        subprocess.run(['cmake', '-S', str(source), '-B', str(buildroot), '-G', 'Ninja',
                        '-DCMAKE_TOOLCHAIN_FILE=' + str(root / '.tools/vitasdk/share/vita.toolchain.cmake'),
                        '-DCMAKE_BUILD_TYPE=Release', '-DKISAK_PLATFORM=vita', '-DKISAK_VITA_CPU_PROFILE=ON',
                        '-DKISAK_VITA_CPU_RUNTIME=OFF', '-DKISAK_VITA_BALANCED_CANDIDATE=ON',
                        '-DKISAK_VITA_BENCHMARK=ON', '-DKISAK_VITA_BENCHMARK_ROUND=9', '-DKISAK_VITA_PROFILE=OFF',
                        '-DKISAK_VITA_SMOKE_AUTOSTART_SHIPMENT=OFF', '-DKISAK_VITA_ALLOW_SOFTWARE_FALLBACK=OFF',
                        '-DKISAK_VITA_VITAGL_LIBRARY=' + str(root / 'build/vita/vitaGL-nosplash/libvitaGL.a'),
                        '-DKISAK_VITA_VITASHARK_LIBRARY=' + str(root / 'build/vita/vitaShaRK/libvitashark.a'),
                        '-DKISAK_VITA_OPENAL_LIBRARY=' + str(root / 'build/vita/openal-soft/libopenal.a'),
                        '-DPython3_EXECUTABLE=' + str(root / '.tools/vita-python-tests/Scripts/python.exe')],
                       cwd=root, env=env, check=True)
        subprocess.run(['cmake', '--build', str(buildroot), '--target', 'KisakCOD-vita-bootstrap.vpk-vpk',
                        'KisakCOD-vita-source-ownership-report', '--parallel', '2'], cwd=root, env=env, check=True)
    target = buildroot / 'scripts/platform/vita'
    tls_link = verify_tls_link(root, target / 'KisakCOD-vita-bootstrap')
    if not json.loads((target / 'vita-source-ownership.json').read_text())['passed']:
        raise ValueError('Test reference source ownership failed')
    identity = dict(base_identity, kind='cpu-test-reference', hardwareQualified=False,
                    threadLocalLinkage=tls_link,
                    baseSourceArchiveSha256=frozen['sourceArchiveSha256'], testAdapterSha256=digest(patch),
                    elfSha256=digest(target / 'KisakCOD-vita-bootstrap'))
    identity['sourceFiles'] = {p.relative_to(source).as_posix(): digest(p) for p in sorted(source.rglob('*'))
                               if p.is_file() and p.suffix in ('.cpp', '.c', '.h', '.inc', '.hpp')}
    identity['sourceTreeSha256'] = hashlib.sha256(json.dumps(identity['sourceFiles'], sort_keys=True).encode()).hexdigest()
    identity['configurationSha256'] = digest(buildroot / 'CMakeCache.txt')
    identity['ownershipSha256'] = digest(target / 'vita-source-ownership.json')
    identity['actualFiles'] = dict(base_identity['actualFiles'], **{'app0:eboot.bin': digest(target / 'KisakCOD-vita-bootstrap.self')})
    identity['defaults'] = dict(base_identity['defaults'], startSelect='overlay-only', overlay=True)
    identity['shipmentTest'] = {'menu': 'Main > Tests > Run Shipment Test (3 min)', 'durationUs': 180000000,
                                'clock': 'real wall time after source player spawn', 'normalRenderingAndAudio': True,
                                'sharedFiles': {name: digest(source / 'src/vita' / name) for name in SHARED}}
    payload = (json.dumps(identity, sort_keys=True, separators=(',', ':')) + '\n').encode()
    (delivery / 'cpu-identity.json').write_bytes(payload)
    output = delivery / 'KisakCOD-Vita-CPU-Test-Reference.vpk'
    pending = delivery / 'test.pending.vpk'
    with zipfile.ZipFile(target / 'KisakCOD-vita-bootstrap.vpk') as original_vpk, zipfile.ZipFile(pending, 'w') as archive:
        if hashlib.sha256(original_vpk.read('eboot.bin')).hexdigest() != identity['actualFiles']['app0:eboot.bin']:
            raise ValueError('Input SELF mismatch')
        entries = {name: original_vpk.read(name) for name in original_vpk.namelist()}
        entries.update({'cpu-identity.json': payload, 'benchmark-identity.json': payload})
        for name, data in sorted(entries.items()):
            if name.endswith('.kvap') or 'libshacccg' in name.lower():
                raise ValueError('Unexpected content in test reference VPK')
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0));info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)
    with zipfile.ZipFile(pending) as archive:
        if archive.testzip() or hashlib.sha256(archive.read('eboot.bin')).hexdigest() != identity['actualFiles']['app0:eboot.bin']:
            raise ValueError('Test reference VPK verification failed')
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
    report = {'kind': 'cpu-test-reference', 'vpkSha256': digest(output), 'elfSha256': identity['elfSha256'],
              'selfSha256': identity['actualFiles']['app0:eboot.bin'], 'sourceArchiveSha256': digest(delivery / 'source.zip'),
              'baseSourceArchiveSha256': frozen['sourceArchiveSha256'], 'testAdapterSha256': digest(patch),
              'shipmentTest': identity['shipmentTest'], 'changedReferenceFiles': changed,
              'ownershipPassed': True, 'archiveVerified': True, 'hardwareQualified': False,
              'scope': 'Frozen performance reference with shared three-minute test/menu, overlay, and repaired recorder startup/flush instrumentation.'}
    (delivery / 'validation.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument('--skip-build', action='store_true')
    args = parser.parse_args()
    build(args.workspace, not args.skip_build)
