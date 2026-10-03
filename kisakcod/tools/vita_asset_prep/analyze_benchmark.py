#!/usr/bin/env python3
"""Read-only analysis of controlled physical-Vita round-one JSONL captures."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics

CASES = ('clear-control','native-a','scene-720','native-b','scene-480','native-c')
PHASES = ('queueUs','gcWaitUs','sceneEndUs','swapOtherUs','renderUs','postUs',
          'serverUs','clientUs','loopbackUs','audioUs','otherMainUs',
          'audioReadUs','audioDecodeIncludingReadUs','audioUploadUs','audioQueueQueryUs')

def percentile(values, p):
    values=sorted(values)
    return values[max(0,math.ceil(len(values)*p)-1)]

def analyze(records, identity=None):
    errors=[]; results={}; cases={}
    run=next((r for r in records if r.get('type')=='run'),{})
    balanced=run.get('round')=='balanced-combined-9'
    debris_fog=run.get('round')=='debris-fog-8'
    textures=run.get('round')=='textures-7'
    audio=run.get('round')=='audio-6'
    geometry=run.get('round')=='geometry-5'
    normal_round=run.get('round')=='normal-dispatch-4'
    material=run.get('round') in ('material-3','normal-dispatch-4')
    shading=run.get('round') in ('shading-2','material-3','normal-dispatch-4','geometry-5','audio-6','textures-7','debris-fog-8')
    names=('native-a','balanced-combined','native-b') if balanced else ('native-a','no-ground-clutter','native-b','no-fog','native-c') if debris_fog else ('native-a','world-mip','native-b','viewmodel-mip','native-c','both-mips','native-d') if textures else ('native-a','no-background-streams','native-b','ambient-pcm','native-c') if audio else ('native-a','no-foliage-models','native-b','no-static-models','native-c') if geometry else ('native-a','adaptive-lights','native-b','no-normal-map','native-c') if normal_round else ('native-a','no-local-lights','native-b','no-specular','native-c') if material else ('native-a','post-copy','native-b','simple-material','native-c') if shading else CASES
    indices=range(len(names)); frames={i:[] for i in indices}
    def reject(message):
        if message not in errors: errors.append(message)
    headers=[r for r in records if r.get('type')=='run']
    if len(headers)!=1 or headers[0].get('schema')!=1: reject('Missing/unsupported run header')
    if run.get('round') not in ('resolution-1','shading-2','material-3','normal-dispatch-4','geometry-5','audio-6','textures-7','debris-fog-8','balanced-combined-9'): reject('Unsupported experiment round')
    if identity and identity.get('round')!=run.get('round'): reject('Packaged experiment round mismatch')
    if identity and 'inputPolicy' in identity and identity['inputPolicy']!=run.get('inputPolicy'):
        reject('Packaged input policy mismatch')
    if any(r.get('type')=='abort' for r in records): reject('Run was aborted')
    if not any(r.get('type')=='end' and r.get('allSegmentsWritten') for r in records): reject('Run has no completion record')
    for r in records:
        if r.get('type')=='blocked-input':
            if run.get('inputPolicy')!='blocked-gameplay-v1' or r.get('deliveredToSource') is not False:
                reject('Input was not proven blocked before source gameplay')
        if r.get('type')=='case': cases[r['index']]=r
        if r.get('type')=='result': results[r['case']]=r
        if r.get('type')=='frame':
            if r.get('case') in frames: frames[r['case']].append(r)
            else: reject('Unknown case index')
    hashes={r['path']:r['sha256'] for r in records if r.get('type')=='identity'}
    if len(hashes)!=(len(identity['actualFiles']) if audio and identity else 5): reject('Incomplete actual executable/content identities')
    if identity is None: reject('Matching packaged benchmark-identity.json is required')
    else:
        for path, expected in identity['actualFiles'].items():
            if hashes.get(path)!=expected: reject('Identity mismatch: '+path)
    stats={}
    for i,name in enumerate(names):
        ff=frames[i]; result=results.get(i,{})
        if cases.get(i,{}).get('id')!=name: reject('Missing/incorrect case '+name)
        if not ff: reject('No frames for '+name); continue
        times=[f['intervalUs'] for f in ff]
        if not result.get('complete') or result.get('samples')!=len(ff): reject('Incomplete samples: '+name)
        clear=not shading and not balanced and i==0
        minimum=10 if clear else 40
        duration=10000000 if clear else 60000000
        if len(ff)<minimum or sum(times)<duration: reject('Measurement too short: '+name)
        if [f['n'] for f in ff]!=list(range(1,len(ff)+1)): reject('Missing/duplicate frame numbers: '+name)
        if any(f['endUs']<=g['endUs'] for g,f in zip(ff,ff[1:])): reject('Nonmonotonic time: '+name)
        if any(not f.get('accountingValid') for f in ff): reject('Overlapping/unaccounted phase contract: '+name)
        if any(f.get('transientDrops',0) for f in ff): reject('Dropped draw uploads: '+name)
        if any(f.get('scene')!=cases.get(i,{}).get('scene') for f in ff): reject('Actual target differs from requested target: '+name)
        if shading:
            mode={'post-copy':1,'simple-material':2,'no-local-lights':3,'no-specular':4,'adaptive-lights':5,'no-normal-map':6,'no-fog':7}.get(name,0)
            if cases.get(i,{}).get('shaderMode')!=mode: reject('Requested shader mode mismatch: '+name)
            for f in ff:
                if f.get('shaderMode')!=mode: reject('Active shader mode mismatch: '+name)
                if f.get('scene')!=[960,544]: reject('Shading experiment changed resolution')
                scene=f.get('sceneShaderDraws',0); simple=f.get('simpleMaterialDraws',0)
                if scene<=0: reject('Scene shaders not exercised: '+name)
                if (simple!=scene if mode==2 else simple!=0): reject('Wrong scene shader draws: '+name)
                if f.get('postCopyDraws')!=(1 if mode==1 else 0): reject('Wrong post-copy draw count: '+name)
                if f.get('authoredPostDraws')!=(0 if mode==1 else 1): reject('Wrong authored-post draw count: '+name)
                if material:
                    if f.get('noLocalLightDraws')!=(scene if mode==3 or (mode==5 and f.get('nonSunLightInputs')==0) else 0): reject('Wrong local-light variant draws: '+name)
                    if f.get('noSpecularDraws')!=(scene if mode==4 else 0): reject('Wrong specular variant draws: '+name)
                    for key in (('specularEligibleDraws','nonSunLightInputs','normalEligibleDraws') if normal_round else ('specularEligibleDraws','nonSunLightInputs')):
                        if not isinstance(f.get(key),int) or f[key]<0: reject('Missing feature activity: '+key)
                    if normal_round:
                        if f.get('noNormalMapDraws')!=(scene if mode==6 else 0): reject('Wrong normal-map variant draws: '+name)
                        if f.get('adaptiveFullLightDraws')!=(scene if mode==5 and f.get('nonSunLightInputs',0)>0 else 0): reject('Adaptive light fallback mismatch: '+name)
                        if not isinstance(f.get('normalEligibleDraws'),int) or f['normalEligibleDraws']<0: reject('Missing normal-map activity')


        if geometry:
            expected_mode=1 if i==1 else 2 if i==3 else 0
            if cases.get(i,{}).get('geometryMode')!=expected_mode:
                reject('Requested geometry mode mismatch: '+name)
            for f in ff:
                if f.get('geometryMode')!=expected_mode: reject('Active geometry mode mismatch: '+name)
                if f.get('staticArchivePasses')!=1: reject('Native static archive not exercised exactly once: '+name)
                if f.get('staticDrawFailures')!=0: reject('Static model draw failure: '+name)
                for key in ('noLocalLightDraws','noSpecularDraws','noNormalMapDraws','adaptiveFullLightDraws'):
                    if f.get(key)!=0: reject('Geometry test changed shader program: '+key)
                arrays=('staticCandidates','staticCandidateTriangles','staticCandidateInstances',
                        'staticSkipped','staticSkippedTriangles','staticSubmitted','staticSubmittedTriangles',
                        'worldGeometry','viewmodelGeometry')
                if any(not isinstance(f.get(k),list) or len(f[k])!=2
                       or any(type(v)!=int or v<0 for v in f[k]) for k in arrays):
                    reject('Missing/invalid geometry category evidence: '+name); continue
                if not all(v>0 for k in ('worldGeometry','viewmodelGeometry') for v in f[k]):
                    reject('World or viewmodel not exercised: '+name)
                for category in range(2):
                    omit=expected_mode==2 or (expected_mode==1 and category==0)
                    for candidate,skip,submit in (('staticCandidates','staticSkipped','staticSubmitted'),
                                                ('staticCandidateTriangles','staticSkippedTriangles','staticSubmittedTriangles')):
                        count=f[candidate][category]
                        if f[skip][category]!=(count if omit else 0) or f[submit][category]!=(0 if omit else count):
                            reject('Geometry suppression/accounting mismatch: '+name)

        stats[name]={'samples':len(ff),'medianMs':percentile(times,.5)/1000,
                     'p95Ms':percentile(times,.95)/1000,'maxMs':max(times)/1000,
                     'averageFps':1e6/statistics.mean(times),'stalls':sum(t>250000 for t in times),
                     'phasesMs':{k:statistics.mean(f.get(k,0) for f in ff)/1000 for k in PHASES}}
    comparisonIndices=range(0 if shading or balanced else 1,len(names))
    comparisonFrames=[f for i in comparisonIndices for f in frames[i]]
    if comparisonFrames:
        if len({tuple(f['clocks']) for f in comparisonFrames})!=1: reject('Clocks changed within the round')
        if len({f['charging'] for f in comparisonFrames})!=1: reject('Charging state changed within the round')
        anchor=comparisonFrames[0]
        for f in comparisonFrames:
            if any(abs(a-b)>1.0 for a,b in zip(f['origin'],anchor['origin'])):
                reject('Camera/player origin changed by more than one source unit')
            if any(abs((a-b+180)%360-180)>.2 for a,b in zip(f['angles'],anchor['angles'])):
                reject('View angles changed by more than 0.2 degrees')
            if f['weapon']!=anchor['weapon']: reject('Weapon changed between cases')
        for key in (() if geometry or debris_fog or balanced else ('draws','triangles')):
            means=[statistics.mean(f[key] for f in frames[i]) for i in comparisonIndices if frames[i]]
            if means and max(means)>min(means)*1.05: reject('Scene workload changed: '+key)
    if geometry:
        for key in ('staticCandidates','staticCandidateTriangles','staticCandidateInstances','worldGeometry','viewmodelGeometry'):
            for component in range(2):
                means=[statistics.mean(f[key][component] for f in frames[i]) for i in indices if frames[i]
                       and all(isinstance(f.get(key),list) and len(f[key])==2
                               and all(type(v)==int and v>=0 for v in f[key]) for f in frames[i])]
                if means and max(means)>min(means)*1.05:
                    reject('Unchanged geometry workload changed: '+key)
    audio_stats={}
    if audio:
        from benchmark_audio_analysis import validate_audio
        audio_stats=validate_audio(records,frames,cases,reject)
    texture_stats={}
    if textures:
        from benchmark_texture_analysis import validate_textures
        texture_stats=validate_textures(records,frames,cases,reject)
    debris_fog_stats={}
    if debris_fog:
        from benchmark_debris_fog_analysis import validate_debris_fog
        debris_fog_stats=validate_debris_fog(records,frames,cases,reject)
    if balanced:
        from balanced_analysis import validate_balanced
        validate_balanced(records,frames,cases,reject)
    comparisons=[]
    pairs=(('balanced-combined','native-a','native-b'),) if balanced else (('no-ground-clutter','native-a','native-b'),('no-fog','native-b','native-c')) if debris_fog else (('world-mip','native-a','native-b'),('viewmodel-mip','native-b','native-c'),('both-mips','native-c','native-d')) if textures else (('no-background-streams','native-a','native-b'),('ambient-pcm','native-b','native-c')) if audio else (('no-foliage-models','native-a','native-b'),('no-static-models','native-b','native-c')) if geometry else (('adaptive-lights','native-a','native-b'),('no-normal-map','native-b','native-c')) if normal_round else (('no-local-lights','native-a','native-b'),('no-specular','native-b','native-c')) if material else (('post-copy','native-a','native-b'),('simple-material','native-b','native-c')) if shading else (('scene-720','native-a','native-b'),('scene-480','native-b','native-c'))
    for variant,left,right in pairs:
        if not all(k in stats for k in (variant,left,right)): continue
        a,b,v=(stats[k]['medianMs'] for k in (left,right,variant))
        base=(a+b)/2; noise=abs(a-b)/base; gain=(base-v)/base
        stable=noise<=.10
        if not stable: reject('Baseline drift exceeds 10% around '+variant)
        comparisons.append({'case':variant,'savedMs':base-v,'gainPercent':100*gain,
                            'baselineVariationPercent':100*noise,
                            'passesScreen':stable and gain>max(.05,noise)})
    if geometry:
        for c in comparisons:
            i=1 if c['case']=='no-foliage-models' else 3
            exercised=any(sum(f.get('staticSkippedTriangles',[]))>0 for f in frames[i])
            c['featureActivity']='submitted-geometry-removed' if exercised else 'not-exercised'
            c['passesScreen']=c['passesScreen'] and exercised
        for i,name in enumerate(names):
            if name in stats and frames[i]:
                stats[name]['geometry']={k:[statistics.mean(f[k][j] for f in frames[i]) for j in range(2)]
                    for k in ('staticCandidates','staticCandidateTriangles','staticCandidateInstances','staticSkipped',
                              'staticSkippedTriangles','staticSubmitted','staticSubmittedTriangles','worldGeometry','viewmodelGeometry')
                    if all(isinstance(f.get(k),list) and len(f[k])==2
                           and all(type(v)==int and v>=0 for v in f[k]) for f in frames[i])}
                stats[name]['phasesMs']['staticModelUs']=statistics.mean(f.get('staticModelUs',0) for f in frames[i])/1000
    if material:
        for key in (('specularEligibleDraws','nonSunLightInputs','normalEligibleDraws') if normal_round else ('specularEligibleDraws','nonSunLightInputs')):
            means=[statistics.mean(f.get(key,0) for f in frames[i]) for i in indices if frames[i]]
            if means and max(means)>min(means)*1.05: reject('Material input workload changed: '+key)
        for c in comparisons:
            key='normalEligibleDraws' if c['case']=='no-normal-map' else 'nonSunLightInputs' if c['case'] in ('no-local-lights','adaptive-lights') else 'specularEligibleDraws'
            exercised=any(f.get(key,0)>0 for f in comparisonFrames)
            c['featureActivity']='eligible-inputs-present' if exercised else 'not-exercised'
            c['programCostMeasured']=True
            c['interpretation']='Compiled shader cost is measured even when removed feature inputs are inactive; this does not establish visible light contribution.'
    if audio:
        for i,name in enumerate(names):
            if name in stats: stats[name]['audioDetail']=audio_stats.get(i,{})
        for c in comparisons:
            exercised=audio_stats.get(1 if c['case']=='no-background-streams' else 3,{}).get('exercised',False)
            c['featureActivity']='background-streams-exercised' if exercised else 'not-exercised'
            c['passesScreen']=c['passesScreen'] and exercised
    if textures:
        for i,name in enumerate(names):
            if name in stats: stats[name]['textureDetail']=texture_stats.get(i,{})
        for c in comparisons:
            exercised=texture_stats.get(names.index(c['case']),{}).get('exercised',False)
            c['featureActivity']='authored-top-mips-removed' if exercised else 'not-exercised'
            c['passesScreen']=c['passesScreen'] and exercised
    if debris_fog:
        for i,name in enumerate(names):
            if name in stats: stats[name]['debrisFogDetail']=debris_fog_stats.get(i,{})
        for c in comparisons:
            fog=c['case']=='no-fog'
            exercised=debris_fog_stats.get(3 if fog else 1,{}).get('fogInputsExercised' if fog else 'debrisExercised',False)
            c['featureActivity']=('fog-inputs-enabled' if fog else 'clutter-geometry-removed') if exercised else 'not-exercised'
            c['passesScreen']=c['passesScreen'] and exercised
            if fog:
                c['programCostMeasured']=True
                c['interpretation']='Fog shader body cost; enabled inputs do not establish per-fragment visible contribution.'
    valid=not errors
    for c in comparisons: c['passesScreen']=c['passesScreen'] and valid
    nextAction='Repair invalid evidence and repeat the round.'
    if valid:
        base=stats['native-a']['phasesMs']
        control=stats.get('clear-control',{}).get('phasesMs',{})
        if base['gcWaitUs']>max(base['queueUs'],33.3):
            nextAction='Investigate cleanup-worker scheduling, frees and render-target lifetime before reducing quality.'
        elif control.get('queueUs',0)+control.get('gcWaitUs',0)>66.7:
            nextAction='Presentation remains slow in the clear control: isolate queue/synchronization before graphics cuts.'
        elif balanced:
            nextAction='Review combined performance and native weapon quality on both maps; confirm with paired repeats and qualification traversals. Do not add previous individual savings.'
        elif debris_fog:
            nextAction='Broad screening complete: review quality tradeoffs, implement selected shader/resolution/visibility findings and measure the combined candidate.'
        elif textures:
            nextAction='Review world versus viewmodel quality and measured pooled texture bytes; retain reductions only after meaningful frame or memory improvement. Continue debris/fog screening next.'
        elif audio:
            nextAction='Use per-stream read/decode/upload and stopped-queue evidence to isolate buffering, decoding or driver scheduling; retain source audio quality until measured otherwise.'
        elif geometry:
            winners=[c['case'] for c in comparisons if c['passesScreen']]
            nextAction=('Test visibility, batching, distant LOD/density within '+', '.join(winners)+
                        '; confirm before retaining quality reductions.') if winners else 'No geometry suppression passed screening; continue transparency and audio isolation.'
        elif shading:
            winners=[c['case'] for c in comparisons if c['passesScreen']]
            nextAction=('Isolate individual costs within '+', '.join(winners)+'; confirm paired results before retaining changes.') if winners else 'Neither shader simplification passed screening: investigate geometry/transparency and CPU preparation next.'
        elif any(c['passesScreen'] for c in comparisons):
            nextAction='Confirm resolution gains at additional cameras; isolate bloom/post and material shading next.'
        else:
            nextAction='Resolution did not pass screening: isolate native-resolution post-processing, then fixed submission/CPU costs.'
    return {'validComparison':valid,'errors':errors,'stats':stats,'comparisons':comparisons,
            'nextAction':nextAction,'hardwareQualified':False,
            'limits':['Three paired confirmations and visual review remain required.',
                      'GPU utilization, temperature and full memory ownership are not established.',
                      'Nested rendering/audio timings and asynchronous GC duration must not be added together.']}

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('log',type=Path); ap.add_argument('--identity',type=Path,required=True)
    ap.add_argument('--json',action='store_true'); args=ap.parse_args()
    records=[json.loads(line) for line in args.log.read_text().splitlines() if line.strip()]
    identity=json.loads(args.identity.read_text())
    identity['actualFiles']['app0:benchmark-identity.json']=hashlib.sha256(args.identity.read_bytes()).hexdigest()
    report=analyze(records,identity)
    if args.json: print(json.dumps(report,indent=2)); return
    print('VALID SCREENING COMPARISON' if report['validComparison'] else 'COMPARISON REJECTED')
    for error in report['errors']: print('- '+error)
    print('\nCase          Median ms   p95 ms   Average FPS')
    for name,s in report['stats'].items():
        print(f"{name:14} {s['medianMs']:9.2f} {s['p95Ms']:8.2f} {s['averageFps']:13.2f}")
    for c in report['comparisons']:
        print(f"{c['case']}: saved {c['savedMs']:.2f} ms ({c['gainPercent']:.1f}%), baseline variation {c['baselineVariationPercent']:.1f}%; passes screen={c['passesScreen']}")
    print('\nNext: '+report['nextAction'])
    for item in report['limits']: print('- '+item)
if __name__=='__main__': main()
