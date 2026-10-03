#!/usr/bin/env python3
"""Rebuild and stage the explicitly selected physical-Vita resolution benchmark."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

def digest(path):
    with path.open('rb') as f: return hashlib.file_digest(f,'sha256').hexdigest()

def run(*args, cwd, env):
    subprocess.run([str(a) for a in args],cwd=cwd,env=env,check=True)

def stage(root, build=True, round_number=1):
    if round_number not in (1,2,3,4,5,6,7,8,9): raise ValueError("Unsupported benchmark round")
    root=root.resolve(); source=root/'kisakcod'; scripts=source/'tools/vita_asset_prep'
    audio_inputs=None
    if round_number==6:
        from prepare_benchmark_audio import prepare
        audio_inputs=prepare(root,build)
    env=dict(os.environ,VITASDK=str(root/'.tools/vitasdk'))
    env['PATH']=str(root/'.tools/vitasdk/bin')+os.pathsep+env['PATH']
    pins=json.loads((source/'dependencies/vita/vita-dependencies.json').read_text())
    pin=pins['vitaGL']
    gl=root/'build/vita/vitaGL-nosplash'; provenance=gl/'vitaGL-build.json'
    al=root/'build/vita/openal-soft'; alprovenance=al/'openal-build.json'
    if build:
        if not alprovenance.exists() or json.loads(alprovenance.read_text())['pin']!=pins['openalSoft']:
            run(sys.executable,scripts/'build_openal.py','--workspace',root,cwd=root,env=env)
        if not provenance.exists() or json.loads(provenance.read_text())['patchSha256']!=pin['patchSha256']:
            run(sys.executable,scripts/'build_vitagl.py','--workspace',root,cwd=root,env=env)
        run('cmake','--preset','vita-balanced' if round_number==9 else 'vita-benchmark',f'-DKISAK_VITA_BENCHMARK_ROUND:STRING={round_number}',cwd=source,env=env)
        run('cmake','--build','--preset','vita-balanced' if round_number==9 else 'vita-benchmark',cwd=source,env=env)
    buildroot=root/('build/vita/balanced' if round_number==9 else 'build/vita/benchmark-round1')
    target=buildroot/'scripts/platform/vita'
    cache=(buildroot/'CMakeCache.txt').read_text()
    required=[f'KISAK_VITA_BENCHMARK_ROUND:STRING={round_number}','CMAKE_BUILD_TYPE:STRING=Release','KISAK_VITA_BENCHMARK:BOOL=ON',
              'KISAK_VITA_PROFILE:BOOL=OFF','KISAK_VITA_SOURCE_RUNTIME:BOOL=ON',
              'KISAK_VITA_ALLOW_SOFTWARE_FALLBACK:BOOL=OFF','KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT:BOOL=OFF']
    if round_number==9: required.append('KISAK_VITA_BALANCED_CANDIDATE:BOOL=ON')
    for setting in required:
        if setting not in cache: raise ValueError('Incorrect benchmark configuration: '+setting)
    linked_al=next((line.split('=',1)[1] for line in cache.splitlines()
                    if line.startswith('KISAK_VITA_OPENAL_LIBRARY:FILEPATH=')),None)
    if linked_al is None or Path(linked_al).resolve()!=(al/'libopenal.a').resolve():
        raise ValueError('Benchmark does not link the pinned OpenAL library')
    owner=target/'vita-source-ownership.json'
    if not json.loads(owner.read_text())['passed']: raise ValueError('Source ownership audit failed')
    glinfo=json.loads(provenance.read_text())
    alinfo=json.loads(alprovenance.read_text())
    if alinfo['pin']!=pins['openalSoft'] or alinfo['librarySha256']!=digest(al/'libopenal.a'):
        raise ValueError('OpenAL build provenance mismatch')
    if glinfo['patchSha256']!=pin['patchSha256'] or glinfo['librarySha256']!=digest(gl/'libvitaGL.a'):
        raise ValueError('VitaGL build provenance mismatch')
    selfpath=target/'KisakCOD-vita-bootstrap.self'
    inputvpk=target/'KisakCOD-vita-bootstrap.vpk'
    with zipfile.ZipFile(inputvpk) as archive:
        if hashlib.sha256(archive.read('eboot.bin')).hexdigest()!=digest(selfpath):
            raise ValueError('VPK does not contain the matching SELF')
        if any(n.endswith('.kvap') or 'libshacccg' in n.lower() for n in archive.namelist()):
            raise ValueError('Unexpected content/runtime in VPK')
    destination=root/('deploy/balanced-candidate' if round_number==9 else f'deploy/benchmark-round{round_number}'); destination.mkdir(parents=True,exist_ok=True)
    # Record actual source bytes, including new untracked implementation files.
    sources={p.relative_to(root).as_posix():digest(p) for p in sorted((source/'src').rglob('*'))
             if p.is_file() and p.suffix in ('.cpp','.c','.h','.inc')}
    identity={
        'schema':1,'kind':'playable-balanced-not-qualified' if round_number==9 else 'hardware-diagnostic-not-qualified','round':({1:'resolution-1',2:'shading-2',3:'material-3',4:'normal-dispatch-4',5:'geometry-5',6:'audio-6',7:'textures-7',8:'debris-fog-8',9:'balanced-combined-9'}[round_number]),
        'sourceCheckpoint':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),
        'inputPolicy':'blocked-gameplay-v1',
        'sourceTreeSha256':hashlib.sha256(json.dumps(sources,sort_keys=True).encode()).hexdigest(),
        'sourceFiles':sources,
        'configurationSha256':digest(buildroot/'CMakeCache.txt'),
        'scenarioSha256':digest(source/'src/vita/vita_benchmark.h'),
        'vitaGL':glinfo,'openalSoft':alinfo,'ownershipSha256':digest(owner),
        'actualFiles':{'app0:eboot.bin':digest(selfpath)},
        'memoryAccounting':'Runtime budget queries may be unavailable; target bytes are logical sizes, not physical allocation totals.'}
    if round_number==9: identity['ordinaryInputPolicy']='real-controller'; identity['defaults']={'world':[720,408],'viewmodel':[960,544],'hud':[960,544],'grass':'balanced','adaptiveLighting':True,'ordinaryLaunch':'main','startSelect':'overlay-only','settingsApply':'next-map-load','textureMips':'unchanged','audio':'unchanged','serverHz':20}
    for name in ('common.kvap','mp_shipment.kvap','mp_crossfire.kvap'):
        pack=root/'Data/CODVIT001/paks'/name
        identity['actualFiles']['ux0:data/CODVIT001/paks/'+name]=digest(pack)
    if audio_inputs:
        identity['actualFiles'].update(audio_inputs['actualFiles'])
        identity['audioConversion']=audio_inputs
    # Identity is external to the SELF, avoiding a circular self-hash.
    identitybytes=(json.dumps(identity,sort_keys=True,separators=(',',':'))+'\n').encode()
    (destination/'benchmark-identity.json').write_bytes(identitybytes)
    output=destination/('KisakCOD-Vita-Balanced.vpk' if round_number==9 else f'KisakCOD-Vita-Benchmark-Round{round_number}.vpk')
    temporary=destination/'benchmark.pending.vpk'
    with zipfile.ZipFile(inputvpk) as original, zipfile.ZipFile(temporary,'w',zipfile.ZIP_DEFLATED) as archive:
        for name in sorted(original.namelist()):
            if name=='benchmark-identity.json': continue
            info=zipfile.ZipInfo(name,(1980,1,1,0,0,0));info.compress_type=zipfile.ZIP_DEFLATED
            archive.writestr(info,original.read(name))
        info=zipfile.ZipInfo('benchmark-identity.json',(1980,1,1,0,0,0)); info.compress_type=zipfile.ZIP_DEFLATED
        archive.writestr(info,identitybytes)
    with zipfile.ZipFile(temporary) as archive:
        if archive.testzip() is not None: raise ValueError('VPK archive CRC failure')
        if hashlib.sha256(archive.read('eboot.bin')).hexdigest()!=identity['actualFiles']['app0:eboot.bin']:
            raise ValueError('Staged executable differs')
    if output.exists() and digest(output)!=digest(temporary):
        previous=destination/('previous-'+digest(output)[:16]+'.vpk')
        if not previous.exists(): shutil.copy2(output,previous)
    temporary.replace(output)
    shutil.copy2(owner,destination/owner.name)
    shutil.copy2(source/('docs/vita/BALANCED_CANDIDATE.md' if round_number==9 else f'docs/vita/BENCHMARK_ROUND{round_number}.md'),destination/'READ-ME.md')
    shutil.copy2(source/'docs/vita/BENCHMARK_RELOAD_FIX.md',destination/'BENCHMARK_RELOAD_FIX.md')
    sums={p.name:digest(p) for p in (output,destination/'benchmark-identity.json',destination/owner.name)}
    (destination/'checksums.json').write_text(json.dumps(sums,indent=2)+'\n')
    print(json.dumps({'vpk':str(output),'sha256':sums[output.name],'hardwareQualified':False},indent=2))
    return output

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--workspace',type=Path,default=Path(__file__).resolve().parents[3])
    ap.add_argument('--skip-build',action='store_true',help='Package an already freshly verified build')
    ap.add_argument('--round',type=int,choices=(1,2,3,4,5,6,7,8,9),default=1,dest='round_number')
    args=ap.parse_args(); stage(args.workspace,not args.skip_build,args.round_number)
