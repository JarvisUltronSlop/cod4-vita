"""Seven-phase world diagnostic. Omissions and pixel scaling are not playable gains."""
from collections import defaultdict
import math
import struct
from analyze_render_passes import PASSES, summary

LABELS=('baseline-before','sky-off','baseline','cheap-world-lighting','baseline','world-props-480x272','baseline-after')


def scalar(value):
    if not isinstance(value,int) or not 0<=value<=0xffffffff:return float('nan')
    return struct.unpack('<f',struct.pack('<I',value))[0]


def analyze_world_scaling(cpu, parse_problems=()):
    results=[]
    for begin in cpu:
        if begin.get('name')!='cpu-diagnostic-begin' or begin.get('value0')!=37:continue
        gen=begin['generation'];start=begin['startUs']
        rows=[r for r in cpu if r.get('generation')==gen and r.get('startUs',0)>=start]
        endings=[r for r in rows if r.get('name')=='cpu-diagnostic-end' and r.get('value0')==37]
        done=[r for r in rows if r.get('name')=='cpu-diagnostic-capture-complete' and r.get('value0')==37 and r.get('value1')==0]
        elapsed=endings[0]['startUs']-start if len(endings)==1 else 0
        health=[r for r in cpu if r.get('type') in ('recording-health','end')]
        problems=list(parse_problems)
        if not health or any(r.get('dropped') or r.get('ioFailed') for r in health):problems.append('recording health unavailable or invalid')
        if len(endings)!=1 or len(done)!=1 or not 139000000<=elapsed<=145000000:problems.append('140-second diagnostic completion unavailable')
        anchors={};position=None;camera_reference=None
        for name in ('gpu-anchor-origin-xy','gpu-anchor-origin-z-yaw','gpu-anchor-angles','gpu-anchor-source-ready'):
            values=[r for r in cpu if r.get('name')==name and r.get('generation')==gen and r.get('startUs',0)<start]
            if len(values)==1:anchors[name]=values[0]
        if len(anchors)!=4 or not all(anchors.get('gpu-anchor-source-ready',{}).get(v,0)>0 for v in ('value0','value1')):problems.append('source diagnostic anchor unavailable')
        if len(anchors)==4:
            xyz=[anchors['gpu-anchor-origin-xy']['value0'],anchors['gpu-anchor-origin-xy']['value1'],anchors['gpu-anchor-origin-z-yaw']['value0']]
            position=[scalar(v) for v in xyz]
            if not all(math.isfinite(v) and abs(v)<=131072 for v in position):problems.append('invalid camera anchor')
            angles=[scalar(anchors['gpu-anchor-angles']['value0']),scalar(anchors['gpu-anchor-origin-z-yaw']['value1']),scalar(anchors['gpu-anchor-angles']['value1'])]
            if not all(math.isfinite(v) for v in angles):problems.append('invalid anchor angles')
            if anchors['gpu-anchor-source-ready']['startUs']<max(anchors[n]['startUs'] for n in anchors if n!='gpu-anchor-source-ready')+1900000:problems.append('source anchor not settled')
        frames=defaultdict(lambda:defaultdict(list))
        for r in rows:
            if r.get('name','').startswith('render-'):frames[r.get('sequence')][r['name']].append(r)
        required=['render-probe-frame','render-probe-kind','render-probe-total-us','render-probe-swap-us','render-probe-viewports',
                  'render-world-scale-sky','render-world-scale-draws','render-world-scale-valid','render-world-scale-targets',
                  'render-world-scale-world','render-world-scale-size']
        required += ['render-probe-camera-xy','render-probe-camera-z']
        required += [f'render-world-scale-viewport-{i}' for i in range(3)]
        required += ['render-pass-'+p for p in PASSES]+['render-pass-'+p+'-geometry' for p in PASSES]
        valid=defaultdict(list);invalid=[];order=[]
        for seq,items in sorted(frames.items()):
            if 'render-probe-frame' not in items:continue
            reason=None
            if any(len(items[n])!=1 for n in required):reason='missing or duplicate frame evidence'
            else:
                f={n:items[n][0] for n in required};phase=f['render-probe-frame']['value0'];order.append(phase)
                pair=lambda name:(f[name]['value0'],f[name]['value1'])
                if phase not in range(1,8):reason='invalid phase'
                elif pair('render-probe-kind')!=(34,0) or f['render-probe-frame']['value1']!=127:reason='wrong probe or missing pass'
                elif pair('render-probe-viewports')!=(7,0):reason='viewport category missing or incorrect'
                elif any(items.get(n) for n in ('render-probe-gl-error',)):reason='graphics error'
                elif f['render-probe-total-us']['value1'] or any(f['render-pass-'+p]['value1'] for p in PASSES):reason='unexpected fenced pass'
                else:
                    camera=[scalar(v) for v in (*pair('render-probe-camera-xy'),pair('render-probe-camera-z')[0])]
                    # The anchor is the authored player spawn, not the rendered
                    # eye. Source settling and view height change Z. Match the
                    # established fixed-view bounds, then require a stationary
                    # eye across all seven phases as a separate check.
                    if (position is None or pair('render-probe-camera-z')[1]!=1
                            or not all(math.isfinite(v) for v in camera)
                            or any(abs(camera[i]-position[i])>(128 if i==2 else 4) for i in range(3))):
                        reason='camera unavailable or outside source anchor bounds'
                    elif camera_reference is None:camera_reference=camera
                    elif any(abs(camera[i]-camera_reference[i])>0.01 for i in range(3)):
                        reason='camera moved between diagnostic frames'
                    size=(480,272) if phase==6 else (720,408)
                    if pair('render-world-scale-size')!=size or any(pair(f'render-world-scale-viewport-{i}')!=(size if i<2 else (960,544)) for i in range(3)):reason='wrong actual viewport or target size'
                    sky,suppressed=pair('render-world-scale-sky');actual,eligible=pair('render-world-scale-draws')
                    if sky<=0 or suppressed!=(sky if phase==2 else 0):reason='sky draw suppression not observed as requested'
                    if f['render-world-scale-valid']['value0'] or f['render-world-scale-valid']['value1'] not in range(4) or (phase==4 and f['render-world-scale-valid']['value1']!=3) or eligible<=0 or actual!=(eligible if phase==4 else 0):reason='cheap shader unavailable, unused, or unexpected dispatch'
                    if pair('render-world-scale-targets')!=((7 if phase==4 else 0),7):reason='heavy world target coverage unavailable'
                    world,low=pair('render-world-scale-world')
                    if world<eligible or low!=(1 if phase==6 else 0):reason='world geometry or target switch unavailable'
                    if f['render-probe-total-us']['value0']<=0 or not 0<=f['render-probe-swap-us']['value1']<=f['render-probe-swap-us']['value0']<=f['render-probe-total-us']['value0']:reason='invalid timing values'
            if reason:invalid.append({'sequence':seq,'phase':items['render-probe-frame'][0].get('value0'),'reason':reason})
            else:valid[phase].append(f)
        if order!=sorted(order):problems.append('phase order regressed')
        common_valid=not problems
        phases=[]
        for phase in range(1,8):
            observed=valid[phase];kept=observed[2:]
            if len(kept)<3:problems.append(f'phase{phase}: insufficient valid frames')
            phases.append(dict(phase=phase,name=LABELS[phase-1],frames=len(observed),retainedFrames=len(kept),
                renderElapsed=summary([f['render-probe-total-us']['value0'] for f in kept]),
                graphicsCompletion=summary([f['render-probe-swap-us']['value1'] for f in kept]),
                passes={p:dict(submissionElapsed=summary([f['render-pass-'+p]['value0'] for f in kept]),
                               meanDraws=sum(f['render-pass-'+p+'-geometry']['value0'] for f in kept)/len(kept) if kept else None,
                               meanTriangles=sum(f['render-pass-'+p+'-geometry']['value1'] for f in kept)/len(kept) if kept else None) for p in PASSES},
                cheapDraws=sum(f['render-world-scale-draws']['value0'] for f in kept),
                skySuppressed=sum(f['render-world-scale-sky']['value1'] for f in kept)))
        complete=not problems and not invalid
        comparisons=[]
        if common_valid:
            for phase in (2,4,6):
                a,b,c=[phases[i-1] for i in (phase-1,phase,phase+1)]
                if any(x['retainedFrames']<3 for x in (a,b,c)) or any(x.get('phase') in (phase-1,phase,phase+1) for x in invalid):
                    comparisons.append(dict(phase=phase,name=b['name'],available=False));continue
                comparisons.append(dict(phase=phase,name=b['name'],available=True,
                    frameReductionVsBeforeUs=a['renderElapsed']['meanUs']-b['renderElapsed']['meanUs'],
                    frameReductionVsAfterUs=c['renderElapsed']['meanUs']-b['renderElapsed']['meanUs'],
                    completionReductionVsBeforeUs=a['graphicsCompletion']['meanUs']-b['graphicsCompletion']['meanUs'],
                    completionReductionVsAfterUs=c['graphicsCompletion']['meanUs']-b['graphicsCompletion']['meanUs']))
        programs={}
        for label,pid in [('opaque-baseline',1),('alpha-baseline',3),('opaque-cheap',15),('alpha-cheap',16)]:
            stages={}
            for stage in ('v','f'):
                def unique(suffix):
                    values=[r for r in cpu if r.get('name')==f'gpu-audit-p{pid}-{stage}-{suffix}']
                    return values[0] if len(values)==1 else None
                instructions,registers,buffers=unique('instructions'),unique('registers'),unique('buffers')
                if all(x is not None for x in (instructions,registers,buffers)):
                    stages[stage]=dict(primaryInstructions=instructions['value0'],secondaryInstructions=instructions['value1'],temporaryRegisters=registers['value1']&0xffffffff,scratchBuffers=buffers['value0'],threadBuffers=buffers['value1'])
            programs[label]=dict(available=len(stages)==2,stages=stages)
        results.append(dict(generation=gen,mode=37,elapsedUs=elapsed,captureComplete=complete,problems=problems,invalidFrames=invalid,phases=phases,comparisons=comparisons,shaderPrograms=programs,
            diagnosticAnchor=position,diagnosticCamera=camera_reference,
            hardwareQualified=False,ordinaryPlayQualified=False,
            limitations=['Sky omission and simplified lighting are diagnostic quality reductions.',
                        '480x272 changes the shared map/props target; weapon/HUD remain native. It does not isolate world-only pixel cost.',
                        'Shader program metadata is static, not executed cycles or occupancy. Cheap shading can also change linked vertex work.',
                        'Marginal reductions are not additive, isolated GPU pass costs or promised playable gains.']))
    return results
