#!/usr/bin/env python3
"""Build and stage the isolated CPU reference or candidate; never restage Balanced."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import zipfile


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def verify_tls_link(root, elf):
    symbols = subprocess.check_output([str(Path(root) / '.tools/vitasdk/bin/arm-vita-eabi-nm.exe'),
                                       str(elf)], text=True)
    matches = [line.split() for line in symbols.splitlines() if line.endswith(' pthread_cancel')]
    if len(matches) != 1 or len(matches[0]) != 3 or matches[0][1] != 'T':
        raise ValueError('C++ thread_local would use shared storage: pthread_cancel is not linked')
    return {'pthreadCancelAddress': matches[0][0], 'gccEmutlsThreadProxyResolved': True,
            'hardwareIsolationCheck': 'required at each launch before recorder/workers'}


def stage(root, kind, build=True, content_dir=None, delivery_dir=None):
    root = Path(root).resolve()
    source = root / 'kisakcod'
    buildroot = root / 'build/vita' / ('cpu-' + kind)
    destination = Path(delivery_dir).resolve() if delivery_dir else root / 'deploy' / ('cpu-' + kind)
    content_dir=Path(content_dir).resolve() if content_dir else None
    if content_dir:
        if kind!='candidate' or not delivery_dir:raise ValueError('New content requires a separate candidate delivery directory')
        content_dir.relative_to(root);destination.relative_to(root)
        if destination.exists():raise ValueError('Map expansion requires a new delivery directory')
        from validate_vita_candidate import validate_content
        content_manifest=validate_content(content_dir)
    stream_index = None
    replay_tape = None
    if kind == 'candidate':
        from cpu_audio_index import prepare
        stream_index, stream_provenance = (prepare(root,content_dir/'stream',destination/'cpu-stream-index.bin') if content_dir else prepare(root))
        from cpu_replay_tape import prepare as prepare_replay
        replay_tape, replay_provenance = prepare_replay(root)
    if kind == 'reference' and (destination / 'validation.json').exists():
        raise ValueError('The captured CPU reference is frozen. It must not be overwritten by later source.')
    env = dict(os.environ, VITASDK=str(root / '.tools/vitasdk'))
    env['PATH'] = str(root / '.tools/vitasdk/bin') + os.pathsep + env['PATH']
    if build:
        for args in (['cmake', '--preset', 'vita-cpu-' + kind],
                     ['cmake', '--build', '--preset', 'vita-cpu-' + kind]):
            subprocess.run(args, cwd=source, env=env, check=True)
    target = buildroot / 'scripts/platform/vita'
    cache = (buildroot / 'CMakeCache.txt').read_text()
    for setting in ('CMAKE_BUILD_TYPE:STRING=Release', 'KISAK_VITA_CPU_PROFILE:BOOL=ON',
                    'KISAK_VITA_CPU_RUNTIME:BOOL=' + ('ON' if kind == 'candidate' else 'OFF'),
                    'KISAK_VITA_SOURCE_RUNTIME:BOOL=ON', 'KISAK_VITA_BALANCED_CANDIDATE:BOOL=ON',
                    'KISAK_VITA_PROFILE:BOOL=OFF', 'KISAK_VITA_ALLOW_SOFTWARE_FALLBACK:BOOL=OFF',
                    'KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT:BOOL=OFF'):
        if setting not in cache.splitlines():
            raise ValueError('Incorrect CPU build configuration: ' + setting)
    ownership = json.loads((target / 'vita-source-ownership.json').read_text())
    if not ownership['passed']:
        raise ValueError('Source ownership audit failed')
    pins = json.loads((source / 'dependencies/vita/vita-dependencies.json').read_text())
    dependencies = {}
    for key, directory, report, library in (
        ('vitaGL', 'vitaGL-cpu-paging' if kind == 'candidate' else 'vitaGL-nosplash', 'vitaGL-build.json', 'libvitaGL.a'),
        ('openalSoft', 'openal-soft', 'openal-build.json', 'libopenal.a')):
        folder = root / 'build/vita' / directory
        provenance = json.loads((folder / report).read_text())
        if provenance['librarySha256'] != digest(folder / library):
            raise ValueError('Dependency archive identity differs: ' + key)
        if key == 'vitaGL' and provenance['patchSha256'] != pins[key]['patchSha256']:
            raise ValueError('VitaGL patch provenance differs')
        if key == 'openalSoft' and provenance['pin'] != pins[key]:
            raise ValueError('OpenAL patch provenance differs')
        if key == 'vitaGL' and kind == 'candidate':
            if provenance['flags'] != {'NO_SPLASHSCREEN': 1, 'HAVE_SHADER_CACHE': 1, 'STORE_DEPTH_STENCIL': 1}:
                raise ValueError('CPU texture paging requires depth preservation across scene restarts')
            cache_pin = json.loads((source / 'dependencies/vita/vita-cpu-cache.json').read_text())
            if provenance.get('cpuCache') != cache_pin:
                raise ValueError('CPU shader-cache dependency provenance differs')
            init_pin = json.loads((source / 'dependencies/vita/vita-cpu-init.json').read_text())
            if (provenance.get('cpuInitialization') != init_pin or
                    digest(source / 'dependencies/vita' / init_pin['patch']) != init_pin['patchSha256']):
                raise ValueError('CPU initialization dependency provenance differs')
            padding_pin = json.loads((source / 'dependencies/vita/vita-cpu-cache-padding.json').read_text())
            if (provenance.get('cpuCachePadding') != padding_pin or
                    digest(source / 'dependencies/vita' / padding_pin['patch']) != padding_pin['patchSha256']):
                raise ValueError('CPU cache padding dependency provenance differs')
            failure_pin = json.loads((source / 'dependencies/vita/vita-cpu-shader-failure.json').read_text())
            if (provenance.get('cpuShaderFailure') != failure_pin or
                    digest(source / 'dependencies/vita' / failure_pin['patch']) != failure_pin['patchSha256']):
                raise ValueError('CPU shader failure dependency provenance differs')

        setting = 'KISAK_VITA_VITAGL_LIBRARY:FILEPATH=' if key == 'vitaGL' else 'KISAK_VITA_OPENAL_LIBRARY:FILEPATH='
        actual = next((line.split('=', 1)[1] for line in cache.splitlines() if line.startswith(setting)), None)
        if not actual or Path(actual).resolve() != (folder / library).resolve():
            raise ValueError('Build linked an unexpected dependency: ' + key)
        dependencies[key] = provenance
    files = {p.relative_to(root).as_posix(): digest(p)
             for p in sorted((source / 'src').rglob('*'))
             if p.is_file() and p.suffix in ('.cpp', '.c', '.h', '.inc')}
    selfpath = target / 'KisakCOD-vita-bootstrap.self'
    elfpath = target / 'KisakCOD-vita-bootstrap'
    tls_link = verify_tls_link(root, elfpath)
    icon = None
    icon_setting = next((line.split('=', 1)[1] for line in cache.splitlines()
                         if line.startswith('KISAK_VITA_APP_ICON:FILEPATH=')), '')
    if icon_setting:
        icon = Path(icon_setting).resolve()
        icon.relative_to(root)
        with zipfile.ZipFile(target / 'KisakCOD-vita-bootstrap.vpk') as original:
            if hashlib.sha256(original.read('sce_sys/icon0.png')).hexdigest() != digest(icon):
                raise ValueError('Input VPK icon differs from configured source')
    identity = {
        'schema': 1, 'kind': 'cpu-' + kind, 'hardwareQualified': False,
        'threadLocalLinkage': tls_link,
        'sourceCheckpoint': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
        'sourceFiles': files,
        'sourceTreeSha256': hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest(),
        'configurationSha256': digest(buildroot / 'CMakeCache.txt'),
        'dependencies': dependencies, 'elfSha256': digest(elfpath),
        'ownershipSha256': digest(target / 'vita-source-ownership.json'),
        'actualFiles': {'app0:eboot.bin': digest(selfpath)},
        'defaults': {'launch': 'main', 'overlay': True, 'startSelect': 'overlay-only',
                     'world': [720, 408], 'viewmodel': [960, 544], 'serverHz': 20,
                     'worldLighting': 'simple-experimental' if kind == 'candidate' else 'original',
                     'renderPipeline': 'shared-gpu-test-baseline' if kind == 'candidate' else 'original',
                     'clocks': 'unchanged', 'assets': 'unchanged'},
    }
    pack_root=content_dir if content_dir else root / 'Data/CODVIT001/paks'
    if icon:
        identity['actualFiles']['app0:sce_sys/icon0.png'] = digest(icon)
        identity['applicationIcon'] = json.loads(icon.with_suffix('.json').read_text())
        if identity['applicationIcon']['outputSha256'] != digest(icon):
            raise ValueError('Application icon provenance differs')
    names=sorted(content_manifest['packages']) if content_dir else sorted(p.name for p in pack_root.glob('*.kvap'))
    pack_layout = None
    if kind == 'candidate':
        from cpu_pack_layout import prepare as prepare_layout
        destination.mkdir(parents=True, exist_ok=True)
        pack_layout = destination / 'cpu-pack-layout.bin'
        pack_layout.write_bytes(prepare_layout(pack_root,names))
        identity['actualFiles']['app0:cpu-pack-layout.bin'] = digest(pack_layout)
        identity['packLayoutPolicy'] = 'Exact installed size and header/TOC CRC before map eligibility; full typed payload CRCs remain required.'
    for name in names:
        identity['actualFiles']['ux0:data/CODVIT001/paks/' + name] = digest(pack_root/name)
    if content_dir:
        identity['defaults']['assets']='retained run76 Shipment recovery' if content_manifest.get('deliveryScope','').startswith('Shipment recovery;') else 'original-map content expansion'
        identity['selectedMaps']=content_manifest.get('selectedMaps',[])
        identity['contentManifestSha256']=digest(content_dir/'vertical-slice-manifest.json')
    if stream_index:
        identity['actualFiles']['app0:cpu-stream-index.bin'] = digest(stream_index)
        identity['streamIndex'] = stream_provenance
        identity['shaderCompatibility'] = json.loads((target / 'vita_cpu_shader_key.json').read_text())
        identity['shaderCompilerModule'] = 'Actual libshacccg bytes additionally hashed on the Vita; unavailable identity disables persistence.'
        identity['actualFiles']['app0:cpu-source-replay.bin'] = digest(replay_tape)
        identity['replayTape'] = replay_provenance
    identity['embeddedDependencies'] = {
        p.relative_to(source).as_posix(): digest(p)
        for folder in ('dr_libs', 'zlib', 'ode')
        for p in sorted((source / 'deps' / folder).rglob('*'))
        if p.is_file() and p.suffix in ('.c', '.cpp', '.h', '.hpp', '.inc')}
    protected = root / 'build/cpu-preservation/protected-sha256.json'
    if protected.exists():
        for relative, expected in json.loads(protected.read_text()).items():
            if digest(root / relative) != expected:
                raise ValueError('A protected file changed: ' + relative)
    reference = root / 'deploy/cpu-reference'
    if kind == 'candidate':
        frozen = json.loads((reference / 'validation.json').read_text())
        for file, key in (('KisakCOD-Vita-CPU-Reference.vpk', 'vpkSha256'),
                          ('KisakCOD-vita-bootstrap.elf', 'elfSha256'), ('source.zip', 'sourceArchiveSha256')):
            if digest(reference / file) != frozen[key]:
                raise ValueError('Frozen reference identity differs: ' + file)
        with zipfile.ZipFile(reference / 'KisakCOD-Vita-CPU-Reference.vpk') as archive:
            reference_identity = json.loads(archive.read('cpu-identity.json'))
        for key, folder, name in (('vitaGL', 'vitaGL-nosplash', 'libvitaGL.a'),
                                   ('openalSoft', 'openal-soft', 'libopenal.a')):
            if digest(root / 'build/vita' / folder / name) != reference_identity['dependencies'][key]['librarySha256']:
                raise ValueError('An original pinned dependency changed: ' + key)
        replay_reference = root / 'deploy/cpu-replay-reference'
        replay_validation = json.loads((replay_reference / 'validation.json').read_text())
        for name, key in (('KisakCOD-Vita-CPU-Replay-Reference.vpk', 'vpkSha256'),
                          ('KisakCOD-vita-bootstrap.elf', 'elfSha256'), ('source.zip', 'sourceArchiveSha256')):
            if digest(replay_reference / name) != replay_validation[key]:
                raise ValueError('Replay reference identity differs: ' + name)
        if (replay_validation['replayTape'] != replay_provenance
                or replay_validation['baseSourceArchiveSha256'] != frozen['sourceArchiveSha256']):
            raise ValueError('Replay reference does not use the same tape and frozen source')
        with zipfile.ZipFile(replay_reference / 'source.zip') as archive:
            for name in ('vita_cpu_replay.h', 'vita_cpu_replay.cpp', 'vita_cpu_replay_tape.h',
                         'vita_cpu_replay_frontend.inc', 'vita_cpu_profile.h', 'vita_cpu_profile.cpp', 'vita_cpu_recorder_start.h', 'vita_cpu_tls_probe.h'):
                relative = 'src/vita/' + name
                if name == 'vita_cpu_profile.cpp':
                    from cpu_recorder_contract import verify
                    identity['recorderObservationContract'] = verify(
                        archive.read('kisakcod/' + relative).decode('utf-8'),
                        (source / relative).read_text(encoding='utf-8'))
                    continue
                if hashlib.sha256(archive.read('kisakcod/' + relative)).hexdigest() != digest(source / relative):
                    raise ValueError('Replay observation/tape implementation differs: ' + name)
        identity['replayReference'] = replay_validation
        from build_cpu_test_reference import SHARED
        test_reference = root / 'deploy/cpu-test-reference'
        test_validation = json.loads((test_reference / 'validation.json').read_text())
        for name, key in (('KisakCOD-Vita-CPU-Test-Reference.vpk', 'vpkSha256'),
                          ('KisakCOD-vita-bootstrap.elf', 'elfSha256'), ('source.zip', 'sourceArchiveSha256')):
            if digest(test_reference / name) != test_validation[key]:
                raise ValueError('Test reference identity differs: ' + name)
        if test_validation['baseSourceArchiveSha256'] != frozen['sourceArchiveSha256']:
            raise ValueError('Test reference must use the frozen performance source')
        shared = {name: digest(source / 'src/vita' / name) for name in SHARED}
        if shared != test_validation['shipmentTest']['sharedFiles']:
            raise ValueError('Three-minute test implementation differs from reference')
        identity['shipmentTest'] = test_validation['shipmentTest']
        identity['testReference'] = test_validation
    destination.mkdir(parents=True, exist_ok=True)
    identity_bytes = (json.dumps(identity, sort_keys=True, separators=(',', ':')) + '\n').encode()
    (destination / 'cpu-identity.json').write_bytes(identity_bytes)
    output = destination / ('KisakCOD-Vita-Maps-Batch-1.vpk' if content_dir
                            else 'KisakCOD-Vita-CPU-' + kind.title() + '.vpk')
    pending = destination / 'cpu.pending.vpk'
    with zipfile.ZipFile(target / 'KisakCOD-vita-bootstrap.vpk') as original:
        if hashlib.sha256(original.read('eboot.bin')).hexdigest() != digest(selfpath):
            raise ValueError('Input VPK SELF mismatch')
        with zipfile.ZipFile(pending, 'w', zipfile.ZIP_DEFLATED) as archive:
            for name in sorted(original.namelist()):
                if name.endswith('.kvap') or 'libshacccg' in name.lower():
                    raise ValueError('Unexpected pack or runtime in VPK')
                if name in ('cpu-identity.json', 'benchmark-identity.json'):
                    continue
                info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, original.read(name))
            for name in ('cpu-identity.json', 'benchmark-identity.json'):
                info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, identity_bytes)
            if stream_index:
                info = zipfile.ZipInfo('cpu-stream-index.bin', (1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, stream_index.read_bytes())
            if replay_tape:
                info = zipfile.ZipInfo('cpu-source-replay.bin', (1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, replay_tape.read_bytes())
            if pack_layout:
                info = zipfile.ZipInfo('cpu-pack-layout.bin', (1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, pack_layout.read_bytes())
    with zipfile.ZipFile(pending) as archive:
        if archive.testzip() or hashlib.sha256(archive.read('eboot.bin')).hexdigest() != digest(selfpath):
            raise ValueError('Staged VPK verification failed')
    if output.exists() and digest(output) != digest(pending):
        previous = destination / ('previous-' + digest(output)[:16] + '.vpk')
        if not previous.exists():
            shutil.copy2(output, previous)
    pending.replace(output)
    for sourcefile, name in ((elfpath, 'KisakCOD-vita-bootstrap.elf'),
                             (target / 'vita-source-ownership.json', 'vita-source-ownership.json'),
                             (buildroot / 'CMakeCache.txt', 'CMakeCache.txt')):
        shutil.copy2(sourcefile, destination / name)
    # Capture the exact build inputs before the next implementation step.
    with zipfile.ZipFile(destination / 'source.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
        for folder in ('src', 'scripts', 'dependencies', 'tools/vita_asset_prep',
                       'deps/dr_libs', 'deps/zlib', 'deps/ode', 'tests/vita_portable'):
            for path in sorted((source / folder).rglob('*')):
                if path.is_file() and '__pycache__' not in path.parts:
                    archive.write(path, path.relative_to(root))
        for name in ('CMakeLists.txt', 'CMakePresets.json'):
            archive.write(source / name, 'kisakcod/' + name)
        archive.write(source / 'docs/vita/CPU_BASELINE.md', 'kisakcod/docs/vita/CPU_BASELINE.md')
        if content_dir:
            archive.write(source / 'docs/vita/ORIGINAL_MAPS.md', 'kisakcod/docs/vita/ORIGINAL_MAPS.md')
        if icon:
            archive.write(icon, icon.relative_to(root))
            archive.write(icon.with_suffix('.json'), icon.with_suffix('.json').relative_to(root))
    validation = {'schema': 1, 'kind': kind, 'vpkSha256': digest(output),
                  'elfSha256': digest(elfpath), 'selfSha256': digest(selfpath),
                  'sourceArchiveSha256': digest(destination / 'source.zip'),
                  'ownershipPassed': True, 'archiveVerified': True, 'protectedFilesVerified': True,
                  'hardwareQualified': False, 'cpuReady': False,
                  'physicalMeasurements': 'pending', 'crossfire': 'blocked-with-current-packs'}
    if kind == 'candidate' and not content_dir:
        validation['implementationStatus'] = 'experimental CPU candidate; qualification incomplete'
        hardware_findings = root / 'build/cpu-first-hardware-findings.json'
        if hardware_findings.exists():
            validation['priorHardwareCapture'] = 'Reference ran; prior candidate crashed before Main. Recorder priority repaired; crash cause remains unconfirmed.'
            shutil.copy2(hardware_findings, destination / hardware_findings.name)
        second_findings = root / 'build/cpu-second-hardware-findings.json'
        if second_findings.exists():
            validation['priorHardwareCapture'] = 'Second candidate crashed in GXM startup after a failed allocation; shared emutls also misrouted renderer ownership. Repaired candidate awaits hardware startup.'
            shutil.copy2(second_findings, destination / second_findings.name)
        for capture in ('third', 'fourth', 'fifth', 'sixth', 'seventh', 'eighth', 'ninth', 'tenth', 'eleventh', 'twelfth', 'thirteenth', 'fourteenth', 'fifteenth', 'sixteenth', 'seventeenth', 'eighteenth', 'nineteenth', 'twentieth', 'twenty-first', 'twenty-second', 'twenty-third', 'twenty-fourth', 'twenty-fifth', 'twenty-sixth', 'twenty-seventh', 'twenty-eighth', 'twenty-ninth', 'thirtieth', 'thirty-first', 'thirty-second', 'thirty-third', 'thirty-fourth', 'thirty-fifth', 'thirty-sixth', 'thirty-seventh', 'thirty-eighth', 'thirty-ninth', 'fortieth', 'forty-first', 'forty-second', 'forty-third', 'forty-fourth', 'forty-fifth', 'forty-sixth', 'forty-seventh', 'forty-eighth', 'forty-ninth', 'fiftieth', 'fifty-first'):
            findings = root / ('build/cpu-' + capture + '-hardware-findings.json')
            if findings.exists():
                shutil.copy2(findings, destination / findings.name)
                validation['priorHardwareCapture'] = {
                    'fifty-first': 'Run51 valid60s comparison214.168/183.558/219.823ms:14.3-16.5% lower candidate frame time. Graphics wait145.128ms;shader321/321 eligible draws. No drops/underruns, correct viewports; visual/HUD/CPU/loading qualification incomplete.',
                    'fiftieth': 'Run50 valid60s comparison214.715/220.077/219.756ms; no FPS gain. CPU vertex preparation20.216/14.563/21.712ms improved5.7-7.1ms. Actual dispatch and viewports passed; no drops. HUD/CPU/loading qualification incomplete.',
                    'forty-ninth': 'Run49 valid60s comparison235.981/219.457/240.623ms; candidate7.0-8.8% lower frame elapsed, queue173.967ms. Conversion6.102ms and packing5.965ms remain. No loss, correct observed viewports; repeat/appearance/CPU/loading qualification incomplete.',
                    'forty-eighth': 'Run48 valid180s draw probe3671samples;352/354stable world/prop signatures sampled>=3times. Large weapon group37.0ms intrusive mean;world58830.4ms,post16.6ms,grass12.9ms. Weapon CPU-side pass21-27ms. No new gain; HUD and CPU qualification incomplete.',
                    'forty-seventh': 'Run47 two complete GPU captures; early alpha gave no robust gain. Candidate waits190.115/190.891ms. Actual dispatch and viewports passed; zero recording loss. Run48 refreshes draw attribution on the successful fast grass/color/clear baseline. HUD and CPU qualification incomplete.',
                    'forty-sixth': 'Run46 238.034/241.171/246.144ms with193.009ms middle-phase graphics wait; no robust HUD frame gain. Missing source HUD menus confirmed by user, source packet rejects up to23; HUD and CPU qualification incomplete. Run47 targets alpha-tested scene arithmetic.',
                    'forty-fifth': 'Run45 valid weapon test255.758/247.113/286.075ms with11.85% baseline drift; no robust gain established. Source19.386/29.088Hz,p9541.802ms. Shipment176.362s and19 shader-cache rejects remain unresolved. Run46 fixes duplicate HUD and tests a dedicated UI pipeline.',
                    'forty-fourth': 'Run44 valid combined fast-color/clear285.982ms versus393.990/422.056ms baselines,27-32% lower frame time; no loss, correct viewports. Appearance and CPU/hardware qualification incomplete.',
                    'forty-third': 'Run43 valid2094 samples; largest repeated draw waits map to instanced grass and the native clear. Baseline398.925ms. Intrusive waits are not additive normal-frame costs; CPU/hardware qualification incomplete.',
                    'forty-second': 'Run42 early reads gave no gain. Current baseline408.323ms:354.719ms queue wait and53.364ms non-swap work. Draw-level attribution pending, CPU qualification incomplete.',
                    'forty-first': 'Run41 confirms pending graphics completion: AddEntry354-387ms falls to0.054ms after Finish411ms. Callback0.07ms, zero VBlank calls. Total frames worsen with forced completion; keep asynchronous presentation. Clean recording, CPU qualification incomplete.',
                    'fortieth': 'Run 40 fixed view and both shaders verified; 406.5/424.5/438.4 ms, no specialization gain beyond drift. Zero recording drops/audio underruns. Source 19.35/29.03 Hz; CPU and visual qualification incomplete.',
                    'thirty-ninth': 'Run 39 compact recording verified: 3.78 MB copied versus 25.63 MB decoded, zero recording drops or audio underruns. About 614 ms render / 565 ms display queue; different initial spawn prevents cross-run FPS attribution. CPU qualification incomplete.',
                    'thirty-eighth': 'Run 38 finished: 451/472/479 ms baseline/candidate/baseline; no specular rewrite gain beyond drift. 9567 telemetry drops invalidate qualification; overflow starts in baseline. Recording repair required.',
                    'thirty-seventh': 'Run 37 material test completed: specular/reflection removal saved about 45 ms, base-color/fog about 91 ms; no normal-map gain beyond variation established. Diagnostic quality reductions only. Later CPU tail truncated after completion; CPU/appearance qualification incomplete.',
                    'thirty-sixth': 'Run 36 completed: faster-shader baselines 450/482 ms, depth-order candidate 481 ms. No gain; adds 1.562 ms source sorting and 3.704 ms renderer world submission. Later menu tail truncated after durable completion. Appearance and CPU/hardware qualification incomplete.',
                    'thirty-fifth': 'Run 35 shader comparison completed: baselines 650/687 ms, opaque candidate 527 ms. About 141 ms (21%) less frame time with actual use and close geometry. Later menu tail truncated after durable completion. Appearance and CPU/hardware qualification remain incomplete.',
                    'thirty-fourth': 'Run 34 completed its 100-second workload test. Baselines 675/698/685 ms, tiny prop coverage 429 ms, no prop draws 407 ms. Coverage-dependent work is a major target; not isolated shader cost or a playable gain. Later menu tail truncated after durable completion. CPU/hardware qualification incomplete.',
                    'thirty-third': 'Run 33 completed its 60-second shader comparison. Baselines averaged 662.014 and 695.990 ms; candidate 676.794 ms, inside baseline variation. No improvement accepted. Later menu tail truncated after durable completion. CPU/hardware qualification incomplete.',
                    'thirty-second': 'Run 32 completed its 60-second pass probe with a durable marker. Props/effects mean serialized completion wait 511.486 ms, world 297.827 ms: together about 82 percent. Normal phases averaged 817/847 ms. Later CPU menu tail truncated; CPU/hardware qualification incomplete.',
                    'thirty-first': 'Run 31 completed all three map loads. Ordinary Shipment was about 18.85/28.28 Hz with roughly 667 ms median frame intervals, 611 ms display queue and 57 ms renderer preparation. Actual viewports matched; CPU log ended mid-record, qualification incomplete.',
                    'thirtieth': 'Run 30 completed normal/stall diagnostics at 18.007/27.026 and 18.018/27.042 Hz. No audio underruns or recording loss. Third ordinary Shipment load aborted on a 14465226-byte model-chunk allocation. Run 31 uses bounded model/LOD reading and local allocation-failure recovery; hardware retest pending.',
                    'twenty-ninth': 'Run 29 completed two diagnostics at 12.073/18.119 and 11.674/17.521 Hz with zero underruns or telemetry loss. Focused audio transition repair passed; CPU readiness remains incomplete. Run 30 moves copied viewmodel conversion and lighting to the render owner; hardware measurement pending.',
                    'twenty-eighth': 'Run 28 completed two diagnostics at 12.000/18.010 and 11.788/17.692 Hz. Renderer packing averaged 7.066 and 6.818 ms; total rates did not improve. Four stale-channel underruns occurred at second map-load start after Main. Run 29 stops source sounds before teardown waits; physical repair verification pending.',
                    'twenty-seventh': 'Run 27 completed at 12.052/18.088 Hz. Only 1.966 percent of lighting misses matched older surviving compatible responses; no reuse enabled. Run 28 removes the probe and uses exact integer classification for renderer byte packing. Physical improvement pending.',
                    'twenty-sixth': 'Run 26 completed at 11.391/17.095 Hz; 254 KiB static scratch saved, speed benefit unproven. All copied logs parse, no recorded loss/audio underruns. Runs 24 and 26 have different recorded positions/RNG and collision workloads. Run 27 measures exact cross-sample lighting reuse without returning older cached responses.',
                    'twenty-fifth': 'Run 25 regressed to 10.515/15.781 Hz; lighting rose to 17.638 ms from 12.327. Durable timed completion, no recorded losses/audio underruns; later final CPU-log line truncated. Run 26 restores run-24 lighting and streams original skinning/conversion through 64-vertex blocks. Physical effectiveness pending.',
                    'twenty-fourth': 'Run 24 completed at 12.094/18.152 Hz, essentially level with run 22. Mean brush time decreased, while renderer preparation increased. Weapon/hands lighting still averages 12.327 ms per update. Final CPU-log tail truncated after durable completion; no recorded losses or audio underruns. Run 25 changes exact lighting color division only. Physical effectiveness pending.',
                    'twenty-third': 'Run 23 completed at 11.403/17.113 Hz, slower overall than run 22. Combat-phase slowdown confirmed; final CPU-log tail truncated after durable completion, with no recorded losses or audio underruns. Run 24 restores the run-22 decoder and refines ordered brush groups. Physical effectiveness pending.',
                    'twenty-second': 'Run 22 completed at 12.162/18.253 Hz with combined preparation p95 91.552 ms, clean completion marker and no recorded audio underruns or telemetry loss. Run 23 uses exact read-only packed-basis decoding tables in source skinning and vertex conversion. Physical effectiveness pending.',
                    'twenty-first': 'Run 21 completed at 11.782/17.682 Hz with combined preparation p95 94.787 ms, durable completion marker and no recorded audio underruns or telemetry loss. Run 22 simplifies the exact source lighting stencil and separates presentation/overlay bookkeeping. Physical effectiveness pending.',
                    'twentieth': 'Run 20 completed at 11.813/17.729 Hz with p95 preparation 96.445 ms, no recorded audio underruns or telemetry loss, and a truncated post-test CPU-log tail. Run 21 accelerates exact byte packing, preallocates upload scratch and persists an explicit diagnostic completion boundary. Physical effectiveness pending.',
                    'nineteenth': 'Run 19 completed at 10.744/16.125 Hz with p95 combined preparation 105.185 ms and no recorded telemetry loss. Two loading audio underruns and a truncated post-test CPU-log tail remain. Current candidate accelerates material lookup and exact lighting palette preparation, and services playback through material audits. Hardware effectiveness pending.',
                    'eighteenth': 'Run 18 completed but is invalid: 4671 telemetry losses during diagnostic, low geometry-cache hit rate, 233.419-second map load and one subsequent audio underrun. Candidate removes that cache, strengthens buffered recording/audio servicing and skips redundant file seeks. Hardware repair effectiveness pending.',
                    'seventeenth': 'Seventeenth diagnostic completed at 10.312/15.476 Hz without recorded audio underruns or telemetry loss. Lighting averaged 13.697 ms and vertex conversion 6.900 ms per update; combined preparation p95 remains 105.021 ms. CPU readiness incomplete. Current candidate caches pure normal geometry and avoids redundant vertex initialization/colour division; hardware measurement pending.',
                    'sixteenth': 'Sixteenth diagnostic completed at 9.895/14.851 Hz; brush search averaged 3.026 ms per update, source preparation p95 73.678 ms. No audio underruns or recorder loss. CPU readiness still fails. Current candidate inlines exact scalar skin/vertex math and splits weapon preparation timing; hardware measurement pending.',
                    'third': 'Third capture verified startup; candidate gameplay remains unqualified.',
                    'fourth': 'Fourth capture reached Main but Shipment failed at the contiguous registry allocation before the timed test. CPU registry backing changed; hardware map retest pending.',
                    'fifth': 'Fifth capture passed registry initialization but reached the new 32768-block cap during model construction. Small objects now share pages; hardware Shipment retest pending.',
                    'sixth': 'Sixth capture exhausted the backing allowance through unused page tails during model construction. The allocator now reuses the active small page across larger allocations; hardware Shipment retest pending.',
                    'seventh': 'Seventh capture completed the three-minute Shipment fixture; CPU rates and preparation failed targets.',
                    'tenth': 'Tenth diagnostic completed in 84.98 seconds with total world collision time 25.99 seconds. CPU readiness incomplete.',
                    'fifteenth': 'Fifteenth diagnostic: renderer median 29.287 ms and Shipment load 107.579 s; source rates essentially unchanged at 9.45/14.18 Hz. Zero recorded audio underruns. Current candidate adds ordered world-brush group rejection on the source owner; hardware measurement pending.',
                    'fourteenth': 'Fourteenth diagnostic completed at 9.43/14.15 Hz with zero recorded audio underruns. Renderer median 66.34 ms and Shipment load 131.92 seconds were slower. CPU readiness failed. Current candidate batches consecutive plain draws and accelerates exact package CRC; hardware measurement pending.',
                    'thirteenth': 'Thirteenth diagnostic completed at 8.91 Hz server and 13.37 Hz client, about 32 percent above run 12. Two initial audio underruns; CPU readiness failed. Current candidate adds prepared lighting, exact vertex packing and playback servicing during resource waits; hardware measurement pending.',
                    'twelfth': 'Twelfth capture isolated 17.91 seconds terrain collision and 38.40 seconds native weapon/hands preparation. Candidate now adds exact lighting-response reuse and finer ordered terrain groups; hardware improvement pending.',
                    'eleventh': 'Eleventh repeat completed in 87.66 seconds at 6.83 Hz server and 10.26 Hz client; total world collision time 25.57 seconds. CPU readiness incomplete. New candidate separates brush/terrain, native weapon preparation and renderer setup/upload timings; hardware capture pending.',
                    'ninth': 'Ninth CPU diagnostic completed at 6.48 Hz server and 9.72 Hz client, with p95 source preparation 131.22 ms. World collision queries took 31.20 seconds across 17544 calls. Conservative brush prefilter added; hardware improvement and CPU qualification pending.',
                    'eighth': 'Eighth CPU diagnostic completed but still ran at 6.55 Hz server and 9.83 Hz client with p95 source preparation 126.60 ms. Finer movement/collision/source-preparation timing added; hardware diagnostic retest pending.',
                }[capture]
        validation['deterministicReferenceReplay'] = 'shared input-tape harness implemented; execution/comparison on Vita pending'
        validation['replayTape'] = replay_provenance
        validation['replayReferenceVerified'] = True
        validation['frozenReferenceVerified'] = True
        validation['testReferenceVerified'] = True
        validation['shipmentTest'] = identity['shipmentTest']
        validation['originalDependenciesVerified'] = True
        validation['protectedFileCount'] = len(json.loads(protected.read_text()))
        for name in ('portable-release.xml', 'portable-debug-cpu.xml', 'cpu-tool-tests.log',
                     'portable-final-focused.xml', 'portable-replay-debug.xml',
                     'portable-replay-release.xml', 'portable-render-final-debug.xml',
                     'portable-render-final-release.xml', 'shader-equations-unchanged.json',
                     'menu-autotest-debug.xml', 'menu-autotest-release.xml', 'menu-autotest-tools.log',
                     'recorder-startup-debug.xml', 'recorder-startup-release.xml',
                     'second-fix-debug.xml', 'second-fix-release.xml', 'second-fix-linked-tls.json',
                     'fourth-fix-debug.xml', 'fourth-fix-release.xml', 'fourth-fix-tool-tests.log',
                     'fifth-fix-debug.xml', 'fifth-fix-release.xml', 'fifth-fix-tool-tests.log',
                     'sixth-fix-before.xml', 'sixth-fix-debug.xml', 'sixth-fix-release.xml', 'sixth-fix-tool-tests.log',
                     'eighth-instrumentation-source-check.json', 'eighth-instrumentation-debug.xml',
                     'eighth-instrumentation-release.xml', 'eighth-instrumentation-tool-tests.log',
                     'ninth-collision-source-check.json', 'ninth-collision-debug.xml', 'ninth-collision-release.xml',
                     'eleventh-timing-source-check.json', 'eleventh-timing-debug.xml', 'eleventh-timing-release.xml',
                     'twelfth-detail-source-check.json', 'twelfth-detail-debug.xml', 'twelfth-detail-release.xml',
                     'twelfth-detail-measurements.json', 'thirteenth-cpu-optimization.json',
                     'thirteenth-optimization-debug.xml', 'thirteenth-optimization-release.xml', 'thirteenth-registry-contracts.log',
                     'fourteenth-cpu-optimization.json', 'fourteenth-optimization-debug.xml',
                     'fourteenth-optimization-release.xml', 'fourteenth-tool-tests.log',
                     'fifteenth-cpu-optimization.json', 'fifteenth-optimization-debug.xml', 'fifteenth-optimization-release.xml',
                     'fifteenth-frontend-debug.xml', 'fifteenth-frontend-release.xml', 'fifteenth-tool-tests.log',
                     'sixteenth-cpu-optimization.json', 'sixteenth-optimization-debug.xml', 'sixteenth-optimization-release.xml',
                     'sixteenth-tool-tests.log', 'sixteenth-registry-contracts.log',
                     'seventeenth-cpu-optimization.json', 'seventeenth-optimization-debug.xml',
                     'seventeenth-optimization-release.xml', 'seventeenth-skin-debug.xml', 'seventeenth-skin-release.xml',
                     'seventeenth-native-skin-calls.json', 'seventeenth-tool-tests.log',
                     'eighteenth-cpu-optimization.json', 'eighteenth-optimization-debug.xml',
                     'eighteenth-optimization-release.xml', 'eighteenth-native-storage.json', 'eighteenth-tool-tests.log',
                     'nineteenth-cpu-repairs.json', 'nineteenth-optimization-debug.xml', 'nineteenth-optimization-release.xml',
                     'nineteenth-native-storage.json', 'nineteenth-tool-tests.log',
                     'twentieth-cpu-optimization.json', 'twentieth-native-analysis.json',
                     'twentieth-optimization-debug.xml', 'twentieth-optimization-release.xml',
                     'twentieth-material-final-debug.xml', 'twentieth-material-final-release.xml',
                     'twentieth-tool-tests.log',
                     'twenty-first-cpu-optimization.json', 'twenty-first-native-analysis.json',
                     'twenty-first-optimization-debug.xml', 'twenty-first-optimization-release.xml',
                     'twenty-first-tool-tests.log',
                     'twenty-second-cpu-optimization.json', 'twenty-second-native-analysis.json',
                     'twenty-second-optimization-debug.xml', 'twenty-second-optimization-release.xml',
                     'twenty-second-tool-tests.log',
                     'twenty-third-cpu-optimization.json', 'twenty-third-native-analysis.json',
                     'twenty-third-optimization-debug.xml', 'twenty-third-optimization-release.xml',
                     'twenty-third-tool-tests.log',
                     'twenty-fourth-cpu-optimization.json', 'twenty-fourth-native-analysis.json',
                     'twenty-fourth-optimization-debug.xml', 'twenty-fourth-optimization-release.xml',
                     'twenty-fourth-tool-tests.log', 'twenty-fourth-brush-parity.log',
                     'twenty-fifth-cpu-optimization.json', 'twenty-fifth-native-analysis.json',
                     'twenty-fifth-optimization-debug.xml', 'twenty-fifth-optimization-release.xml',
                     'twenty-fifth-tool-tests.log',
                     'twenty-sixth-cpu-optimization.json', 'twenty-sixth-native-analysis.json',
                     'twenty-sixth-optimization-debug.xml', 'twenty-sixth-optimization-release.xml',
                     'twenty-sixth-tool-tests.log', 'twenty-sixth-skin-parity.log',
                     'twenty-seventh-cpu-optimization.json', 'twenty-seventh-native-analysis.json',
                     'twenty-seventh-optimization-debug.xml', 'twenty-seventh-optimization-release.xml',
                     'twenty-seventh-tool-tests.log', 'twenty-seventh-light-parity.log',
                     'twenty-eighth-cpu-optimization.json', 'twenty-eighth-native-analysis.json',
                     'twenty-eighth-optimization-debug.xml', 'twenty-eighth-optimization-release.xml',
                     'twenty-eighth-tool-tests.log', 'twenty-eighth-pack-parity.log',
                     'twenty-ninth-cpu-optimization.json', 'twenty-ninth-native-analysis.json',
                     'twenty-ninth-optimization-debug.xml', 'twenty-ninth-optimization-release.xml',
                     'twenty-ninth-tool-tests.log',
                     'thirtieth-cpu-optimization.json', 'thirtieth-native-analysis.json',
                     'thirtieth-optimization-debug.xml', 'thirtieth-optimization-release.xml',
                     'thirtieth-tool-tests.log', 'thirtieth-skin-parity.log', 'thirtieth-light-parity.log',
                     'thirty-first-cpu-optimization.json', 'thirty-first-model-loader-analysis.json',
                     'thirty-first-optimization-debug.xml', 'thirty-first-optimization-release.xml',
                     'thirty-first-tool-tests.log', 'thirty-first-model-parity-debug.log', 'thirty-first-model-parity-release.log'):
            report = root / 'build/cpu-preservation' / name
            if report.exists():
                shutil.copy2(report, destination / name)
        measurement_details = root / 'build/cpu-preservation/twelfth-detail-measurements.json'
        if measurement_details.exists():
            validation['measurementDetails'] = 'twelfth-detail-measurements.json'
            validation['nextAction'] = json.loads(measurement_details.read_text(encoding='utf-8'))['nextAction']
        optimization_details = root / 'build/cpu-preservation/thirteenth-cpu-optimization.json'
        if optimization_details.exists():
            validation['optimizationDetails'] = 'thirteenth-cpu-optimization.json'
            validation['nextAction'] = json.loads(optimization_details.read_text(encoding='utf-8'))['nextAction']
        optimization_details = root / 'build/cpu-preservation/fourteenth-cpu-optimization.json'
        if optimization_details.exists():
            validation['optimizationDetails'] = optimization_details.name
            validation['nextAction'] = json.loads(optimization_details.read_text(encoding='utf-8'))['nextAction']
        optimization_details = root / 'build/cpu-preservation/fifteenth-cpu-optimization.json'
        if optimization_details.exists():
            validation['optimizationDetails'] = optimization_details.name
            validation['nextAction'] = json.loads(optimization_details.read_text(encoding='utf-8'))['nextAction']
        optimization_details = root / 'build/cpu-preservation/sixteenth-cpu-optimization.json'
        if optimization_details.exists():
            validation['optimizationDetails'] = optimization_details.name
            validation['nextAction'] = json.loads(optimization_details.read_text(encoding='utf-8'))['nextAction']
        optimization_details = root / 'build/cpu-preservation/seventeenth-cpu-optimization.json'
        if optimization_details.exists():
            validation['optimizationDetails'] = optimization_details.name
            validation['nextAction'] = json.loads(optimization_details.read_text(encoding='utf-8'))['nextAction']
        optimization_details = root / 'build/cpu-preservation/eighteenth-cpu-optimization.json'
        if optimization_details.exists():
            validation['optimizationDetails'] = optimization_details.name
            validation['nextAction'] = json.loads(optimization_details.read_text(encoding='utf-8'))['nextAction']
        optimization_details = root / 'build/cpu-preservation/nineteenth-cpu-repairs.json'
        if optimization_details.exists():
            validation['optimizationDetails'] = optimization_details.name
            validation['nextAction'] = json.loads(optimization_details.read_text(encoding='utf-8'))['nextAction']
        optimization_details = root / 'build/cpu-preservation/twentieth-cpu-optimization.json'
        if optimization_details.exists():
            validation['optimizationDetails'] = optimization_details.name
            validation['nextAction'] = json.loads(optimization_details.read_text(encoding='utf-8'))['nextAction']
        optimization_details = root / 'build/cpu-preservation/twenty-first-cpu-optimization.json'
        if optimization_details.exists():
            validation['optimizationDetails'] = optimization_details.name
            validation['nextAction'] = json.loads(optimization_details.read_text(encoding='utf-8'))['nextAction']
        optimization_details = root / 'build/cpu-preservation/twenty-second-cpu-optimization.json'
        if optimization_details.exists():
            validation['optimizationDetails'] = optimization_details.name
            validation['nextAction'] = json.loads(optimization_details.read_text(encoding='utf-8'))['nextAction']
        optimization_details = root / 'build/cpu-preservation/twenty-third-cpu-optimization.json'
        if optimization_details.exists():
            validation['optimizationDetails'] = optimization_details.name
            validation['nextAction'] = json.loads(optimization_details.read_text(encoding='utf-8'))['nextAction']
        optimization_details = root / 'build/cpu-preservation/twenty-fourth-cpu-optimization.json'
        if optimization_details.exists():
            validation['optimizationDetails'] = optimization_details.name
            validation['nextAction'] = json.loads(optimization_details.read_text(encoding='utf-8'))['nextAction']
        optimization_details = root / 'build/cpu-preservation/twenty-fifth-cpu-optimization.json'
        if optimization_details.exists():
            validation['optimizationDetails'] = optimization_details.name
            validation['nextAction'] = json.loads(optimization_details.read_text(encoding='utf-8'))['nextAction']
        optimization_details = root / 'build/cpu-preservation/twenty-sixth-cpu-optimization.json'
        if optimization_details.exists():
            validation['optimizationDetails'] = optimization_details.name
            validation['nextAction'] = json.loads(optimization_details.read_text(encoding='utf-8'))['nextAction']
        optimization_details = root / 'build/cpu-preservation/twenty-seventh-cpu-optimization.json'
        if optimization_details.exists():
            validation['optimizationDetails'] = optimization_details.name
            validation['nextAction'] = json.loads(optimization_details.read_text(encoding='utf-8'))['nextAction']
        optimization_details = root / 'build/cpu-preservation/twenty-eighth-cpu-optimization.json'
        if optimization_details.exists():
            validation['optimizationDetails'] = optimization_details.name
            validation['nextAction'] = json.loads(optimization_details.read_text(encoding='utf-8'))['nextAction']
        optimization_details = root / 'build/cpu-preservation/twenty-ninth-cpu-optimization.json'
        if optimization_details.exists():
            validation['optimizationDetails'] = optimization_details.name
            validation['nextAction'] = json.loads(optimization_details.read_text(encoding='utf-8'))['nextAction']
        optimization_details = root / 'build/cpu-preservation/thirtieth-cpu-optimization.json'
        if optimization_details.exists():
            validation['optimizationDetails'] = optimization_details.name
            validation['nextAction'] = json.loads(optimization_details.read_text(encoding='utf-8'))['nextAction']
        optimization_details = root / 'build/cpu-preservation/thirty-first-cpu-optimization.json'
        if optimization_details.exists():
            validation['optimizationDetails'] = optimization_details.name
            validation['nextAction'] = json.loads(optimization_details.read_text(encoding='utf-8'))['nextAction']
        probe_details = root / 'build/cpu-preservation/thirty-second-render-probe.json'
        if probe_details.exists():
            shutil.copy2(probe_details, destination / probe_details.name)
            validation['optimizationDetails'] = probe_details.name
            validation['nextAction'] = json.loads(probe_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run 31 third-load success and ordinary Shipment timings recorded; run 32 render-pass probe hardware measurement pending. CPU qualification incomplete.'
        prop_details = root / 'build/cpu-preservation/thirty-third-prop-shader.json'
        if prop_details.exists():
            shutil.copy2(prop_details, destination / prop_details.name)
            validation['optimizationDetails'] = prop_details.name
            validation['nextAction'] = json.loads(prop_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run 32 pass probe reviewed; run 33 prop shader comparison pending. No graphics gain or CPU qualification claimed.'
        workload_details = root / 'build/cpu-preservation/thirty-fourth-prop-workload.json'
        if workload_details.exists():
            shutil.copy2(workload_details, destination / workload_details.name)
            validation['optimizationDetails'] = workload_details.name
            validation['nextAction'] = json.loads(workload_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run 33 showed no shader-specialization improvement beyond variation. Run 34 prop workload diagnostic pending; no graphics gain or CPU qualification claimed.'
        opaque_details = root / 'build/cpu-preservation/thirty-fifth-opaque-shader.json'
        if opaque_details.exists():
            shutil.copy2(opaque_details, destination / opaque_details.name)
            validation['optimizationDetails'] = opaque_details.name
            validation['nextAction'] = json.loads(opaque_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run 34 confirms substantial coverage-dependent prop work. Run 35 opaque shader comparison pending; no graphics gain or CPU qualification claimed.'
        depth_details = root / 'build/cpu-preservation/thirty-sixth-world-depth-order.json'
        if depth_details.exists():
            shutil.copy2(depth_details, destination / depth_details.name)
            validation['optimizationDetails'] = depth_details.name
            validation['nextAction'] = json.loads(depth_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run 35 opaque shader comparison saved about 141 ms (21%) versus baseline midpoint. Run 36 world depth ordering pending; no additional gain or CPU qualification claimed.'
        material_details = root / 'build/cpu-preservation/thirty-seventh-material-cost.json'
        if material_details.exists():
            shutil.copy2(material_details, destination / material_details.name)
            validation['optimizationDetails'] = material_details.name
            validation['nextAction'] = json.loads(material_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run 35 opaque shaders saved about 141 ms (21%); run 36 depth ordering had no gain. Run 37 material cost diagnostic pending; no additional quality-preserving gain or CPU qualification claimed.'
        menu_details = root / 'build/cpu-preservation/menu-cleanup.json'
        if menu_details.exists():
            shutil.copy2(menu_details, destination / menu_details.name)
            validation['optimizationDetails'] = menu_details.name
            validation['nextAction'] = json.loads(menu_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run 37 completed: about 45 ms specular/reflection and 91 ms broader fragment-lighting diagnostic reductions. Menu-only update; no measurement repeat needed. CPU and visual qualification incomplete.'
        specular_details = root / 'build/cpu-preservation/thirty-eighth-specular-math.json'
        if specular_details.exists():
            shutil.copy2(specular_details, destination / specular_details.name)
            validation['optimizationDetails'] = specular_details.name
            validation['nextAction'] = json.loads(specular_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run 37 feature-removal diagnostic identified about 45 ms of specular/reflection-related elapsed cost. Run 38 preserves the features and compares shared dot-product / power-five math against faster opaque baselines. Hardware measurements pending; no additional gain or CPU qualification claimed.'
        compact_details = root / 'build/cpu-preservation/thirty-ninth-compact-recording.json'
        if compact_details.exists():
            shutil.copy2(compact_details, destination / compact_details.name)
            validation['optimizationDetails'] = compact_details.name
            validation['nextAction'] = json.loads(compact_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run 38 has no established shader gain and 9567 lost telemetry records. Compact recording, fair drain and coalesced flush repair pending physical verification. Host lossless replay reduces captures by about 85%; no GPU gain or CPU qualification claimed.'
            validation['logTransport'] = {'format': 'zlib-members-32k-v1', 'suffix': '.jsonl.z', 'startup': 'cpu-startup.jsonl', 'lossless': True}
        family_details = root / 'build/cpu-preservation/fortieth-material-family.json'
        if family_details.exists():
            shutil.copy2(family_details, destination / family_details.name)
            validation['optimizationDetails'] = family_details.name
            validation['nextAction'] = json.loads(family_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run 39 compact logs verified, 85.25% smaller with no telemetry drops/audio underruns; about 614 ms render / 565 ms display queue at a different initial view. Run 40 fixed authored view and exact material specialization comparison pending. No new GPU gain, visual qualification or CPU readiness claimed.'
        display_details = root / 'build/cpu-preservation/forty-first-display-queue.json'
        if display_details.exists():
            shutil.copy2(display_details, destination / display_details.name)
            validation['optimizationDetails'] = display_details.name
            validation['nextAction'] = json.loads(display_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run 40 fixed-view comparison valid with no shader-specialization gain (406.5/424.5/438.4 ms). Run 41 splits original AddEntry wait, preceding GXM Finish and display callback timings; attribution pending hardware. No frame-rate gain or CPU qualification claimed.'
        prefetch_details = root / 'build/cpu-preservation/forty-second-texture-prefetch.json'
        if prefetch_details.exists():
            shutil.copy2(prefetch_details, destination / prefetch_details.name)
            validation['optimizationDetails'] = prefetch_details.name
            validation['nextAction'] = json.loads(prefetch_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run41 confirms GPU-completion/buffer synchronization dominates display queue blocking; callback0.07ms and zero VBlank waits. Run42 compares earlier texture reads with matched material-family baselines and normal asynchronous presentation. Hardware gain and visuals pending; CPU qualification incomplete.'
        draw_details = root / 'build/cpu-preservation/forty-third-draw-probe.json'
        if draw_details.exists():
            shutil.copy2(draw_details, destination / draw_details.name)
            validation['optimizationDetails'] = draw_details.name
            validation['nextAction'] = json.loads(draw_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run42 baseline408.323ms, queue354.719ms, non-swap53.364ms. Run43 adds rotating intrusive draw-completion samples with prefix and empty controls; attribution pending. Same resolution, material shaders, compiler settings, source gameplay and compact recorder. No performance gain or CPU qualification claimed.'
        fast_details = root / 'build/cpu-preservation/forty-fourth-color-clear.json'
        if fast_details.exists():
            shutil.copy2(fast_details, destination / fast_details.name)
            validation['optimizationDetails'] = fast_details.name
            validation['nextAction'] = json.loads(fast_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run43 valid draw attribution: largest repeated instanced grass batches75.049/56.719/50.259ms intrusive waits; native clear45.981ms. Run44 compares minimal color/clear fragment paths together against unchanged baselines. No gain, visual or CPU qualification yet.'
        weapon_details = root / 'build/cpu-preservation/forty-fifth-weapon-opaque.json'
        if weapon_details.exists():
            shutil.copy2(weapon_details, destination / weapon_details.name)
            validation['optimizationDetails'] = weapon_details.name
            validation['nextAction'] = json.loads(weapon_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run44 combined shaders285.982ms,27-32% lower frame time than baselines. Run45 retains those shaders across all test phases and tests existing no-discard programs on opaque source weapon/hands draws. New gain and appearance pending; CPU/hardware qualification incomplete.'
        hud_details = root / 'build/cpu-preservation/forty-sixth-source-hud.json'
        if hud_details.exists():
            shutil.copy2(hud_details, destination / hud_details.name)
            validation['optimizationDetails'] = hud_details.name
            validation['nextAction'] = json.loads(hud_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run45 255.758/247.113/286.075ms; baseline drift11.85% prevents a robust weapon-gain claim. Run46 corrects the duplicate HUD and measures dedicated HUD shaders on the retained run44 graphics-test baseline. Appearance, new performance, CPU and loading qualification pending.'
        alpha_details = root / 'build/cpu-preservation/forty-seventh-early-alpha.json'
        if alpha_details.exists():
            shutil.copy2(alpha_details, destination / alpha_details.name)
            validation['optimizationDetails'] = alpha_details.name
            validation['nextAction'] = json.loads(alpha_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run46 middle-phase graphics wait193.009ms; no robust HUD frame gain and missing source menu HUD unresolved. Run47 tests early alpha rejection before normal arithmetic on the retained run44 baseline. No performance or visual gain claimed before hardware.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/46th run'
            validation['latestHardwareFindings'] = 'cpu-forty-sixth-hardware-findings.json'
        current_draw_details = root / 'build/cpu-preservation/forty-eighth-current-draw-probe.json'
        if current_draw_details.exists():
            shutil.copy2(current_draw_details, destination / current_draw_details.name)
            validation['optimizationDetails'] = current_draw_details.name
            validation['nextAction'] = json.loads(current_draw_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run47 two complete early-alpha tests gave no robust gain; candidate waits190.115/190.891ms. Run48 records bounded draw completion against the retained fast grass/color/clear baseline. No new performance gain or hardware qualification claimed. Missing source HUD unresolved.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/47th run'
            validation['latestHardwareFindings'] = 'cpu-forty-seventh-hardware-findings.json'
        weapon_pipeline_details = root / 'build/cpu-preservation/forty-ninth-weapon-pipeline.json'
        if weapon_pipeline_details.exists():
            shutil.copy2(weapon_pipeline_details, destination / weapon_pipeline_details.name)
            validation['optimizationDetails'] = weapon_pipeline_details.name
            validation['nextAction'] = json.loads(weapon_pipeline_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run48 weapon pass21-27ms CPU-side elapsed with large sampled weapon draws. Run49 tests exact lighting memoization plus existing baked-model shader dispatch. No gain before hardware measurement; missing HUD, CPU/loading/visual qualification remain unresolved.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/48th run'
            validation['latestHardwareFindings'] = 'cpu-forty-eighth-hardware-findings.json'
        weapon_vertex_details = root / 'build/cpu-preservation/fiftieth-weapon-vertices.json'
        if weapon_vertex_details.exists():
            shutil.copy2(weapon_vertex_details, destination / weapon_vertex_details.name)
            validation['optimizationDetails'] = weapon_vertex_details.name
            validation['nextAction'] = json.loads(weapon_vertex_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run49 positive single-run7.0-8.8% gain; candidate219.457ms, queue173.967ms. Run50 tests fused vertex preparation with the run49 pipeline retained all phases. No new gain or hardware/visual/CPU qualification claimed. Source HUD unresolved.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/49th run'
            validation['latestHardwareFindings'] = 'cpu-forty-ninth-hardware-findings.json'
        weapon_shader_details = root / 'build/cpu-preservation/fifty-first-weapon-shader.json'
        if weapon_shader_details.exists():
            shutil.copy2(weapon_shader_details, destination / weapon_shader_details.name)
            validation['optimizationDetails'] = weapon_shader_details.name
            validation['nextAction'] = json.loads(weapon_shader_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run50 CPU vertex preparation improved5.7-7.1ms; no FPS gain. Run51 tests authored colour/specular weapon specialization, retaining prior CPU/GPU paths all phases. No new gain before measurement; HUD/CPU/loading/visual qualification incomplete.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/50th run'
            validation['latestHardwareFindings'] = 'cpu-fiftieth-hardware-findings.json'
        world_shader_details = root / 'build/cpu-preservation/fifty-second-world-shader.json'
        if world_shader_details.exists():
            shutil.copy2(world_shader_details, destination / world_shader_details.name)
            validation['optimizationDetails'] = world_shader_details.name
            validation['nextAction'] = json.loads(world_shader_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run51 positive single-run14.3-16.5% frame gain. Run52 tests transparent world material specialization with original alpha predicates and all authored texture inputs; prior weapon/CPU changes active all phases. No new gain before measurement; HUD/CPU/loading/visual qualification incomplete.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/51st run'
            validation['latestHardwareFindings'] = 'cpu-fifty-first-hardware-findings.json'
        current_draw_details = root / 'build/cpu-preservation/fifty-third-current-draw-probe.json'
        if current_draw_details.exists():
            shutil.copy2(current_draw_details, destination / current_draw_details.name)
            validation['optimizationDetails'] = current_draw_details.name
            validation['nextAction'] = json.loads(current_draw_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run52 valid60s comparison181.372/172.911/184.178ms:4.7-6.1% mean frame reduction in one capture. Run53 profiles the retained candidate with corrected surface/fullscreen identity. No new hardware measurement; camera/HUD/CPU/loading/visual qualification incomplete.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/52nd run'
            validation['latestHardwareFindings'] = 'cpu-fifty-second-hardware-findings.json'
            validation['cameraHardwareVerified'] = False
            findings = root / 'build/cpu-fifty-second-hardware-findings.json'
            if findings.exists(): shutil.copy2(findings, destination / findings.name)
        post_details = root / 'build/cpu-preservation/fifty-fourth-post-shader.json'
        if post_details.exists():
            shutil.copy2(post_details, destination / post_details.name)
            validation['optimizationDetails'] = post_details.name
            validation['nextAction'] = json.loads(post_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run53 healthy profile: post16.590ms sampled completion with film on/glow off. Unfenced frames170.578/169.640ms. Run54 tests exact post feature specialization without probe fences; no new gain before measurement. Camera/HUD/CPU/loading/visual qualification incomplete.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/53rd run'
            validation['latestHardwareFindings'] = 'cpu-fifty-third-hardware-findings.json'
            validation['cameraHardwareVerified'] = False
            findings = root / 'build/cpu-fifty-third-hardware-findings.json'
            if findings.exists(): shutil.copy2(findings, destination / findings.name)
        world_lifetime_details = root / 'build/cpu-preservation/fifty-fifth-world-lifetime.json'
        if world_lifetime_details.exists():
            shutil.copy2(world_lifetime_details, destination / world_lifetime_details.name)
            validation['optimizationDetails'] = world_lifetime_details.name
            validation['nextAction'] = json.loads(world_lifetime_details.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run54 post specialization rejected:174.490ms versus170.778/172.491ms. Run55 tests opaque-world temporary lifetime with original post restored and prior improvements fixed. No new gain before hardware measurement; camera/HUD/CPU/loading qualification incomplete.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/54th run'
            validation['latestHardwareFindings'] = 'cpu-fifty-fourth-hardware-findings.json'
            validation['cameraHardwareVerified'] = False
            findings = root / 'build/cpu-fifty-fourth-hardware-findings.json'
            if findings.exists(): shutil.copy2(findings, destination / findings.name)
        resource_audit = root / 'build/cpu-preservation/fifty-sixth-resource-audit.json'
        if resource_audit.exists():
            shutil.copy2(resource_audit, destination / resource_audit.name)
            validation['optimizationDetails'] = resource_audit.name
            validation['nextAction'] = json.loads(resource_audit.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run55 no meaningful gain:171.030/171.998/172.048ms. Run56 captures compiled-program and actual bound-texture metadata with unchanged shaders/settings/cache namespace. No performance gain or qualification claimed.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/55th run'
            validation['latestHardwareFindings'] = 'cpu-fifty-fifth-hardware-findings.json'
            findings = root / 'build/cpu-fifty-fifth-hardware-findings.json'
            if findings.exists(): shutil.copy2(findings, destination / findings.name)
        lightmap_layout = root / 'build/cpu-preservation/fifty-seventh-lightmap-layout.json'
        if lightmap_layout.exists():
            shutil.copy2(lightmap_layout, destination / lightmap_layout.name)
            validation['optimizationDetails'] = lightmap_layout.name
            validation['nextAction'] = json.loads(lightmap_layout.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run56 unit4 sampler bug confirmed; lightmaps uncompressed/linear. Run57 fixes sampler state in both lanes and compares byte-preserving swizzled lightmap copies. No hardware performance or visual gain claimed.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/56th run'
            validation['latestHardwareFindings'] = 'cpu-fifty-sixth-hardware-findings.json'
            findings = root / 'build/cpu-fifty-sixth-hardware-findings.json'
            if findings.exists(): shutil.copy2(findings, destination / findings.name)
        loading_recovery = root / 'build/cpu-preservation/fifty-eighth-loading-recovery.json'
        if loading_recovery.exists():
            shutil.copy2(loading_recovery, destination / loading_recovery.name)
            validation['optimizationDetails'] = loading_recovery.name
            validation['nextAction'] = json.loads(loading_recovery.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run57 reached Main57.667s; Shipment failed its required specular residency gate after222.413s. Both3MiB lightmap clones byte-verified; no GPU phases. Allocation pressure suspected, exact prior upload error unavailable. Run58 defers diagnostic copies and reserves larger required semantics earlier. No hardware recovery or performance gain claimed.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/57th run'
            validation['latestHardwareFindings'] = 'cpu-fifty-seventh-hardware-findings.json'
            findings = root / 'build/cpu-fifty-seventh-hardware-findings.json'
            if findings.exists(): shutil.copy2(findings, destination / findings.name)
        inplace_lightmaps = root / 'build/cpu-preservation/fifty-ninth-inplace-lightmaps.json'
        if inplace_lightmaps.exists():
            shutil.copy2(inplace_lightmaps, destination / inplace_lightmaps.name)
            validation['optimizationDetails'] = inplace_lightmaps.name
            validation['nextAction'] = json.loads(inplace_lightmaps.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run58 required materials/HUD staging passed,91model specular aliases loaded. Primary1MiB clone verified;secondary2MiB clone failed after4.001s. No GPU results. Run59 replaces duplicate texture allocations with reversible in-place conversion using128KiB workspace; loading, visuals and performance await hardware.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/58th run'
            validation['latestHardwareFindings'] = 'cpu-fifty-eighth-hardware-findings.json'
            findings = root / 'build/cpu-fifty-eighth-hardware-findings.json'
            if findings.exists(): shutil.copy2(findings, destination / findings.name)
        world_math = root / 'build/cpu-preservation/sixtieth-world-math.json'
        if world_math.exists():
            shutil.copy2(world_math, destination / world_math.name)
            validation['optimizationDetails'] = world_math.name
            validation['nextAction'] = json.loads(world_math.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run59 completed loading/layout verification but no gain beyond variation. Final linear173.121ms/5.776FPS, graphics wait132.776ms. Run60 compares opaque/transparent world arithmetic with original textures and equations; native code generation, appearance and speed pending.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/59th run'
            validation['latestHardwareFindings'] = 'cpu-fifty-ninth-hardware-findings.json'
            findings = root / 'build/cpu-fifty-ninth-hardware-findings.json'
            if findings.exists(): shutil.copy2(findings, destination / findings.name)
        world_basis = root / 'build/cpu-preservation/sixty-first-world-basis.json'
        if world_basis.exists():
            shutil.copy2(world_basis, destination / world_basis.name)
            validation['optimizationDetails'] = world_basis.name
            validation['nextAction'] = json.loads(world_basis.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run60 complete:170.676/170.225/172.505ms;small gain not promoted. Run61 moves certified constant basis construction to vertex shading while preserving material equations, geometry and textures. Hardware code generation/coverage/appearance/speed pending.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/60th run'
            validation['latestHardwareFindings'] = 'cpu-sixtieth-hardware-findings.json'
            findings = root / 'build/cpu-sixtieth-hardware-findings.json'
            if findings.exists(): shutil.copy2(findings, destination / findings.name)
        basis_load_audit = root / 'build/cpu-preservation/sixty-second-basis-load-audit.json'
        if basis_load_audit.exists():
            shutil.copy2(basis_load_audit, destination / basis_load_audit.name)
            validation['optimizationDetails'] = basis_load_audit.name
            validation['nextAction'] = json.loads(basis_load_audit.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run61 complete:169.683ms candidate frame,128.202ms graphics completion;modest gain. Heavy507/588 fall back. Main112.483s,Shipment232.551s with same read bytes but longer waits. Run62 audits final batch rejection/run counts and sparse read execution counters;unchanged shaders/cache identity. No new performance gain claimed.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/61st run'
            validation['latestHardwareFindings'] = 'cpu-sixty-first-hardware-findings.json'
            findings = root / 'build/cpu-sixty-first-hardware-findings.json'
            if findings.exists(): shutil.copy2(findings, destination / findings.name)
            investigation = root / 'build/cpu-preservation/sixty-second-loading-investigation.json'
            if investigation.exists(): shutil.copy2(investigation, destination / investigation.name)
        cache_padding = root / 'build/cpu-preservation/sixty-third-cache-padding.json'
        if cache_padding.exists():
            shutil.copy2(cache_padding, destination / cache_padding.name)
            validation['optimizationDetails'] = cache_padding.name
            validation['nextAction'] = json.loads(cache_padding.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run62 complete:169.645ms candidate frame,128.261ms graphics wait. Main59.187s,Shipment100.957s;23cached-shader rejections/compiles cost35.287s. Run63 corrects compiler/GXM size mismatch with bounded padding acceptance;unchanged shaders/cache namespace. Hardware cache reuse/loading improvement pending.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/62nd run'
            validation['latestHardwareFindings'] = 'cpu-sixty-second-hardware-findings.json'
            findings = root / 'build/cpu-sixty-second-hardware-findings.json'
            if findings.exists(): shutil.copy2(findings, destination / findings.name)
            material_audit = root / 'build/cpu-preservation/sixty-third-material-inputs.json'
            if material_audit.exists(): shutil.copy2(material_audit, destination / material_audit.name)
        world_gloss = root / 'build/cpu-preservation/sixty-fourth-world-gloss.json'
        if world_gloss.exists():
            shutil.copy2(world_gloss, destination / world_gloss.name)
            validation['optimizationDetails'] = world_gloss.name
            validation['nextAction'] = json.loads(world_gloss.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run63 confirmed54 cached shaders loaded,23 padding cases accepted,zero rejects/recompiles;Main19.924s,Shipment108.037s,candidate169.959ms/5.88FPS and128.664ms graphics wait. Run64 tests certified constant ground gloss;hardware precision/visual/speed evidence pending.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/63rd run'
            validation['latestHardwareFindings'] = 'cpu-sixty-third-hardware-findings.json'
            findings = root / 'build/cpu-sixty-third-hardware-findings.json'
            if findings.exists(): shutil.copy2(findings, destination / findings.name)
        world_components = root / 'build/cpu-preservation/sixty-fifth-world-components.json'
        if world_components.exists():
            shutil.copy2(world_components, destination / world_components.name)
            validation['optimizationDetails'] = world_components.name
            validation['nextAction'] = json.loads(world_components.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run64 valid:171.114/172.183/172.735ms frame,130.987/130.739/131.270ms graphics completion;constant gloss570/570eligible draws and6fewer instructions but no useful gain. Run65 isolates secondary-lightmap and specular/reflection costs on certified gravel decal in five20-second windows. Diagnostic reductions only;no gain or quality qualification claimed.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/64th run'
            validation['latestHardwareFindings'] = 'cpu-sixty-fourth-hardware-findings.json'
            findings = root / 'build/cpu-sixty-fourth-hardware-findings.json'
            if findings.exists(): shutil.copy2(findings, destination / findings.name)
        specular_split = root / 'build/cpu-preservation/sixty-sixth-specular-split.json'
        if specular_split.exists():
            shutil.copy2(specular_split, destination / specular_split.name)
            validation['optimizationDetails'] = specular_split.name
            validation['nextAction'] = json.loads(specular_split.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run65: specular/reflection bypass saved 5.768-8.244ms graphics completion and 6.558-8.702ms frame time against neighbors. Secondary-lightmap bypass had no consistent frame gain. Run66 separates highlight and reflection contributions on the same certified decal; diagnostic-only reductions, no new performance or visual qualification.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/65th run'
            validation['latestHardwareFindings'] = 'cpu-sixty-fifth-hardware-findings.json'
            findings = root / 'build/cpu-sixty-fifth-hardware-findings.json'
            if findings.exists(): shutil.copy2(findings, destination / findings.name)
        graphics_profile = root / 'build/cpu-preservation/sixty-seventh-graphics-profile.json'
        if graphics_profile.exists():
            shutil.copy2(graphics_profile, destination / graphics_profile.name)
            validation['optimizationDetails'] = graphics_profile.name
            validation['nextAction'] = json.loads(graphics_profile.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run66 two valid GPU Tests: reflection removal saved1.190-3.882ms graphics wait; highlight had no consistent full-frame gain. Run67 adds Graphics Profile with unchanged shaders: optional native GPU counters, passive scene/target timeline and display callbacks. Hardware counter availability/correlation and profiling overhead remain unverified.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/66th run'
            validation['latestHardwareFindings'] = 'cpu-sixty-sixth-hardware-findings.json'
            findings = root / 'build/cpu-sixty-sixth-hardware-findings.json'
            if findings.exists(): shutil.copy2(findings, destination / findings.name)
        composition = root / 'build/cpu-preservation/sixty-eighth-composition.json'
        if composition.exists():
            shutil.copy2(composition, destination / composition.name)
            validation['optimizationDetails'] = composition.name
            validation['nextAction'] = json.loads(composition.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run67 completed120s but optional Razor module returned0x805A1000 and actual transparent-world selection differed between windows. Only phase2 used the intended pipeline:133.132ms graphics completion;CPU scene calls0.603ms,display callback0.070ms,no VBlank wait. No counter attribution or constant-pipeline comparison. Run68 removes a redundant native fullscreen clear by writing far depth in the existing world-copy draw;actual state, coverage, visual parity and gain need hardware validation.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/67th run'
            validation['latestHardwareFindings'] = 'cpu-sixty-seventh-hardware-findings.json'
            findings = root / 'build/cpu-sixty-seventh-hardware-findings.json'
            if findings.exists(): shutil.copy2(findings, destination / findings.name)
        pass_isolation = root / 'build/cpu-preservation/sixty-ninth-pass-isolation.json'
        if pass_isolation.exists():
            shutil.copy2(pass_isolation, destination / pass_isolation.name)
            validation['optimizationDetails'] = pass_isolation.name
            validation['nextAction'] = json.loads(pass_isolation.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run68 completed60s/346frames but aggregate depth check incorrectly expected0..1, while world range is1/64..1. Graphics completion132.003/132.781/132.343ms;no observed gain;clear not promoted. Run69 corrects depth validation and adds nine20s pass-isolation windows through the existing Graphics Profile menu, retaining three scene boundaries and baseline neighbors. Passive timeline without Razor. Physical validation pending; no playable improvement claimed.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/68th run'
            validation['latestHardwareFindings'] = 'cpu-sixty-eighth-hardware-findings.json'
            findings = root / 'build/cpu-sixty-eighth-hardware-findings.json'
            if findings.exists(): shutil.copy2(findings, destination / findings.name)
        world_precision = root / 'build/cpu-preservation/seventieth-world-precision.json'
        if world_precision.exists():
            shutil.copy2(world_precision, destination / world_precision.name)
            validation['optimizationDetails'] = world_precision.name
            validation['nextAction'] = json.loads(world_precision.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run69 valid: world omission reduced graphics completion by74.775-75.175ms; props/FX38.763-42.189ms. Effects are nonadditive and are not playable gains. Run70 GPU Test measures explicit mixed-precision world colour arithmetic across both full material families; all textures/effects retained. Native compilation, appearance and speed remain unverified.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/69th run'
            validation['latestHardwareFindings'] = 'cpu-sixty-ninth-hardware-findings.json'
            for report in ('cpu-sixty-ninth-hardware-findings.json',):
                findings = root / 'build' / report
                if findings.exists(): shutil.copy2(findings, destination / findings.name)
            numeric = root / 'build/cpu-preservation/seventieth-precision-numerical.json'
            if numeric.exists(): shutil.copy2(numeric, destination / numeric.name)
        shader_failure = root / 'build/cpu-preservation/seventy-sixth-render-pipeline.json'
        if shader_failure.exists():
            shutil.copy2(shader_failure, destination / shader_failure.name)
            validation['optimizationDetails'] = shader_failure.name
            validation['nextAction'] = json.loads(shader_failure.read_text(encoding='utf-8'))['nextAction']
            validation['physicalMeasurements'] = 'Run75: ordinary-play median490ms versus fixed GPU simple-lighting test100ms; views differ and optimized non-world paths were diagnostic-gated. Twelve SubData calls took1.18-1.74seconds. Run76 shares rendering feature policy with ordinary play and moves physical pose-buffer rotation from source snapshots to renderer swaps. Hardware effectiveness and visual quality remain unqualified.'
            validation['latestHardwareCapture'] = 'crash logs/cpu-candidate/75th run'
            validation['latestHardwareFindings'] = 'cpu-seventy-fifth-hardware-findings.json'
            findings = root / 'build/cpu-seventy-fifth-hardware-findings.json'
            if findings.exists(): shutil.copy2(findings, destination / findings.name)
        shutil.copy2(source / 'docs/vita/CPU_BASELINE.md', destination / 'CPU_BASELINE.md')
    if content_dir:
        validation.update(implementationStatus='original-map expansion; hardware qualification incomplete',
            optimizationDetails='map-expansion.json',latestHardwareCapture=None,
            physicalMeasurements='Run76 user reported approximately 10 FPS on Shipment. New map performance is unmeasured.',
            nextAction='Use the batch-specific install and ordinary-play instructions.',crossfire='unqualified-new-content')
        (destination/'map-expansion.json').write_text(json.dumps(dict(contentManifestSha256=identity['contentManifestSha256'],packages=names,hardwareQualified=False),indent=2)+'\n')
        copies=[]
        for name,entry in content_manifest['packages'].items():
            copies.append(dict(source=str(content_dir/name),destination='ux0:data/CODVIT001/paks/'+name,**entry))
        for name,entry in content_manifest.get('streams',{}).items():
            copies.append(dict(source=str(content_dir/name),destination='ux0:data/CODVIT001/'+name,**entry))
        for name,sha in content_manifest.get('media',{}).items():
            copies.append(dict(source=str(content_dir/'media'/name),destination='ux0:data/CODVIT001/media/'+name,sha256=sha))
        manifest_file=content_dir/'vertical-slice-manifest.json'
        copies.append(dict(source=str(manifest_file),destination='ux0:data/CODVIT001/vertical-slice-manifest.json',
                           bytes=manifest_file.stat().st_size,sha256=digest(manifest_file)))
        (destination/'copy-list.json').write_text(json.dumps(dict(
            installVpk=str(output),files=copies,
            instruction='Install this VPK over CODVIT001. Copy every listed file as one coherent set; map packs from another common-pack generation will be rejected.',
            removeFiles=[],hardwareQualified=False),indent=2)+'\n')
    (destination / 'validation.json').write_text(json.dumps(validation, indent=2) + '\n')
    print(json.dumps({'vpk': str(output), **validation}, indent=2))
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument('--kind', choices=('reference', 'candidate'), required=True)
    parser.add_argument('--skip-build', action='store_true')
    parser.add_argument('--content-dir',type=Path)
    parser.add_argument('--delivery-dir',type=Path)
    args = parser.parse_args()
    stage(args.workspace, args.kind, not args.skip_build,args.content_dir,args.delivery_dir)
