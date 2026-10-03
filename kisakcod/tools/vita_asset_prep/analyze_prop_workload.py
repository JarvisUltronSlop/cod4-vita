"""Interpret counterfactual prop-coverage work; never report it as playable FPS."""
from collections import defaultdict
from analyze_render_passes import PASSES, summary


def analyze_prop_workload(cpu, problems=()):
    results=[]
    for begin in cpu:
        if begin.get('name')!='cpu-diagnostic-begin' or begin.get('value0')!=5: continue
        rows=[r for r in cpu if r.get('generation')==begin['generation'] and r.get('startUs',0)>=begin['startUs']]
        ends=[r for r in rows if r.get('name')=='cpu-diagnostic-end' and r.get('value0')==5]
        markers=[r for r in rows if r.get('name')=='cpu-diagnostic-capture-complete' and r.get('value0')==5 and r.get('value1')==0]
        by_seq=defaultdict(lambda:defaultdict(list))
        for r in rows:
            if r.get('name','').startswith('render-'): by_seq[r.get('sequence')][r['name']].append(r)
        phases=defaultdict(list);invalid=[];order=[]
        required=['render-probe-frame','render-probe-kind','render-probe-total-us','render-probe-swap-us',
                  'render-probe-prop-attempts','render-probe-prop-submitted','render-probe-prop-clip',
                  'render-probe-props-group-us','render-probe-viewports','render-probe-prop-shader']
        required+=['render-pass-'+p for p in PASSES]
        for seq,names in sorted(by_seq.items()):
            if not any(n.startswith(('render-pass-','render-probe-')) for n in names):continue
            if (any(len(names.get(n,[]))!=1 for n in required)
                    or any(names.get(n) for n in ['render-probe-incomplete','render-probe-gl-error','render-probe-props-group-invalid','render-probe-props-clip-invalid'])):
                invalid.append({'sequence':seq,'reason':'missing, duplicate or failed evidence'});continue
            s={n:names[n][0] for n in required};phase=s['render-probe-frame']['value0']
            attempts=s['render-probe-prop-attempts'];submitted=s['render-probe-prop-submitted'];clip=s['render-probe-prop-clip'];view=s['render-probe-viewports']
            valid=(phase in range(1,6) and s['render-probe-kind']['value0']==2 and s['render-probe-frame']['value1']==127
                and s['render-probe-prop-shader']['value0']==0 and s['render-probe-total-us']['value1']==0
                and all(s['render-pass-'+p]['value1']==0 for p in PASSES)
                and clip['value0']==3 and attempts['value0']>0 and attempts['value1']>0
                and clip['value1']==(attempts['value0'] if phase==2 else 0)
                and submitted['value0']==(0 if phase==4 else attempts['value0'])
                and submitted['value1']==(0 if phase==4 else attempts['value1'])
                and view['value0']==(5 if phase==4 else 7) and view['value1']==0)
            if not valid:invalid.append({'sequence':seq,'reason':'workload/state did not match phase'});continue
            phases[phase].append(s);order.append(phase)
        reports=[]
        for phase in range(1,6):
            samples=phases[phase][2:]
            reports.append({'phase':phase,'name':{1:'baseline-before',2:'tiny-prop-coverage',3:'baseline-middle',4:'no-prop-gpu-draws',5:'baseline-after'}[phase],
                'frames':len(phases[phase]),'settlingFramesExcluded':min(2,len(phases[phase])),
                'renderElapsed':summary([s['render-probe-total-us']['value0'] for s in samples]),
                'queueElapsed':summary([s['render-probe-swap-us']['value1'] for s in samples]),
                'propCpuSubmission':summary([s['render-probe-props-group-us']['value0'] for s in samples]),
                'meanAttemptedPropDraws':sum(s['render-probe-prop-attempts']['value0'] for s in samples)/len(samples) if samples else None,
                'meanAttemptedPropTriangles':sum(s['render-probe-prop-attempts']['value1'] for s in samples)/len(samples) if samples else None,
                'meanSubmittedPropDraws':sum(s['render-probe-prop-submitted']['value0'] for s in samples)/len(samples) if samples else None})
        health=[r for r in cpu if r.get('type') in ('recording-health','end')]
        elapsed=ends[0]['startUs']-begin['startUs'] if ends else None
        complete=bool(elapsed is not None and elapsed>=100000000 and markers and health and not problems and not invalid
            and order==sorted(order) and all(p['renderElapsed']['samples']>=3 for p in reports)
            and not any(r.get('dropped',0) or r.get('ioFailed',False) for r in health))
        results.append({'generation':begin['generation'],'captureComplete':complete,'elapsedUs':elapsed,'phases':reports,'invalidFrames':invalid,
            'hardwareQualified':False,'interpretation':'Tiny coverage retains geometry/shaders but changes rasterization, tile clipping, visibility and scissor-mask overhead. No-prop draws retain source and CPU preparation. Differences are counterfactual frame costs, not isolated vertex/fragment utilization or playable FPS. Compare all three baselines and attempted geometry.'})
    return results
