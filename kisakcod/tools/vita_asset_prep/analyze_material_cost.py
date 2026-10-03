"""Counterfactual opaque material costs; visual reductions are diagnostic only."""
from collections import defaultdict
from analyze_render_passes import PASSES, summary

NAMES = {1:'baseline-before',2:'no-normal-maps',3:'baseline-middle-1',4:'no-specular-reflections',5:'baseline-middle-2',6:'base-color-and-fog',7:'baseline-after'}

def analyze_material_cost(cpu, problems=()):
    results=[]
    for begin in cpu:
        if begin.get('name')!='cpu-diagnostic-begin' or begin.get('value0')!=8:continue
        rows=[r for r in cpu if r.get('generation')==begin['generation'] and r.get('startUs',0)>=begin['startUs']]
        ends=[r for r in rows if r.get('name')=='cpu-diagnostic-end' and r.get('value0')==8]
        markers=[r for r in rows if r.get('name')=='cpu-diagnostic-capture-complete' and r.get('value0')==8 and r.get('value1')==0]
        sequences=defaultdict(lambda:defaultdict(list))
        for r in rows:
            if r.get('name','').startswith('render-'):sequences[r.get('sequence')][r['name']].append(r)
        required=['render-probe-'+n for n in ('frame','kind','total-us','swap-us','viewports','geometry','prop-shader','opaque-world','opaque-props','opaque-invalid','material-mode','material-world','material-props','material-features','material-lightmaps','material-reflections')]
        required+=['render-pass-'+p for p in PASSES]+['render-pass-'+p+'-geometry' for p in PASSES]
        phases=defaultdict(list);invalid=[];order=[]
        for seq,names in sorted(sequences.items()):
            if not any(n.startswith(('render-pass-','render-probe-')) for n in names):continue
            if (any(len(names.get(n,[]))!=1 for n in required)
                    or any(n in names for n in ('render-probe-incomplete','render-probe-gl-error'))
                    or any(n.startswith(('render-probe-depth-','render-probe-prop-clip')) for n in names)):
                invalid.append({'sequence':seq,'reason':'missing, duplicate or conflicting evidence'});continue
            s={n:names[n][0] for n in required};phase=s['render-probe-frame']['value0'];variant={2:1,4:2,6:3}.get(phase,0)
            opaque=[s['render-probe-opaque-'+n] for n in ('world','props')]
            used=[s['render-probe-material-'+n] for n in ('world','props')]
            total=sum(v['value1'] for v in opaque)
            valid=(phase in NAMES and s['render-probe-kind']['value0']==5 and s['render-probe-frame']['value1']==127
                and s['render-probe-viewports']['value0']==7 and s['render-probe-viewports']['value1']==0
                and s['render-probe-prop-shader']['value0']==0 and s['render-probe-opaque-invalid']['value0']==0
                and s['render-probe-total-us']['value1']==0 and all(s['render-pass-'+p]['value1']==0 for p in PASSES)
                and s['render-probe-material-mode']['value0']==1<<variant and s['render-probe-material-mode']['value1']==31
                and total>0 and all(v['value0']==v['value1'] and v['value0']>=0 for v in opaque)
                and all(u['value1']==o['value1'] and u['value0']==(o['value1'] if variant else 0) for u,o in zip(used,opaque))
                and all(0<=s['render-probe-material-'+n][v]<=total for n in ('features','lightmaps','reflections') for v in ('value0','value1')))
            if not valid:invalid.append({'sequence':seq,'reason':'material variant, eligibility, state or viewport mismatch'});continue
            phases[phase].append(s);order.append(phase)
        reports=[]
        for phase in NAMES:
            samples=phases[phase][2:]
            mean=lambda n,v:sum(s[n][v] for s in samples)/len(samples) if samples else None
            reports.append({'phase':phase,'name':NAMES[phase],'frames':len(phases[phase]),'settlingFramesExcluded':min(2,len(phases[phase])),
                'renderElapsed':summary([s['render-probe-total-us']['value0'] for s in samples]),
                'queueElapsed':summary([s['render-probe-swap-us']['value1'] for s in samples]),
                'meanOpaqueDraws':{n:mean('render-probe-opaque-'+n,'value0') for n in ('world','props')},
                'meanDiagnosticDraws':{n:mean('render-probe-material-'+n,'value0') for n in ('world','props')},
                'meanFeatureDraws':{n:mean('render-probe-material-'+key,field) for n,key,field in [('normal','features','value0'),('specular','features','value1'),('primaryLightmap','lightmaps','value0'),('secondaryLightmap','lightmaps','value1'),('reflection','reflections','value0'),('localLights','reflections','value1')]},
                'passes':{p:{'submissionElapsed':summary([s['render-pass-'+p]['value0'] for s in samples]),'meanDraws':mean('render-pass-'+p+'-geometry','value0'),'meanTriangles':mean('render-pass-'+p+'-geometry','value1')} for p in PASSES}})
        health=[r for r in cpu if r.get('type') in ('recording-health','end')]
        elapsed=ends[0]['startUs']-begin['startUs'] if ends else None
        complete=bool(elapsed is not None and elapsed>=140000000 and markers and health and not problems and not invalid
            and order==sorted(order) and all(p['renderElapsed']['samples']>=3 for p in reports)
            and not any(r.get('dropped',0) or r.get('ioFailed',False) for r in health))
        comparisons=[]
        for phase,feature in ((2,'normal'),(4,'specular'),(6,None)):
            before,test,after=reports[phase-2:phase+1]
            means=[p['renderElapsed']['meanUs'] for p in (before,test,after)]
            available=bool(test['renderElapsed']['samples'] and (feature is None or test['meanFeatureDraws'][feature]>0))
            midpoint=(means[0]+means[2])/2 if all(v is not None for v in means) else None
            comparisons.append({'phase':phase,'featureObserved':available,'baselineMidpointUs':midpoint,
                'reductionUs':midpoint-means[1] if midpoint is not None else None,
                'fractionLowerElapsed':1-means[1]/midpoint if midpoint else None,
                'baselineDriftFraction':means[2]/means[0]-1 if means[0] and means[2] else None})
        results.append({'generation':begin['generation'],'mode':8,'elapsedUs':elapsed,'captureComplete':complete,'phases':reports,'comparisons':comparisons,'invalidFrames':invalid,'hardwareQualified':False,'qualityPreservingImprovement':False,
            'interpretation':'Diagnostic-only changes on eligible opaque static world/props. Texture-feature counts describe available draw inputs, not hardware sampling counts. Base-color phase retains vertex lighting and fog. Compare neighboring baselines and geometry; differences overlap and cannot be summed or labeled isolated GPU utilization. Ordinary quality is unchanged.'})
    return results
