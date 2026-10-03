"""Separate diagnostic validity, native timeline evidence and optional GPU counters."""
from collections import defaultdict
import re,struct,math
from analyze_render_passes import summary

def analyze_graphics_profile(cpu, tests):
    result=[]
    for test in tests:
        if test['mode']!=33:continue
        generation=test['generation'];rows=[r for r in cpu if r.get('generation')==generation]
        errors=[r for r in rows if r.get('name')=='graphics-counter-error']
        batches=[];current=None
        for r in rows:
            n=r.get('name','')
            if n=='graphics-counter-batch':
                current=dict(id=r['value0'],group=r['value1'],sequence=r.get('sequence'),jobs={},other={},parameters=[],frames={},problems=[]);batches.append(current)
            elif current and n.startswith('graphics-'):
                m=re.fullmatch(r'graphics-job-(\d+)-(id|time|values)',n)
                if m:
                    job=current['jobs'].setdefault(int(m[1]),{})
                    if m[2] in job:current['problems'].append('duplicate job field')
                    job[m[2]]=[r['value0'],r['value1']]
                elif re.fullmatch(r'graphics-parameter-\d+',n):
                    current['parameters'].append(dict(peakBytes=r['value0'],partialRender=r['value1']>>32,vertexPaused=r['value1']&0xffffffff))
                elif (fm:=re.fullmatch(r'graphics-hw-frame-(\d+)(-time)?',n)):
                    current['frames'].setdefault(int(fm[1]),{})['time' if fm[2] else 'data']=[r['value0'],r['value1']]
                elif n in ('graphics-counter-buffer','graphics-counter-jobs','graphics-counter-raw-truncated','graphics-counter-window'):
                    current['other'][n]=[r['value0'],r['value1']]
        jobs=[];issues=[];parameters=[];hardware_frames=[]
        for batch in batches:
            issues.extend(batch['problems']);other=batch['other']
            if any(other.get('graphics-counter-raw-truncated',[0,0])):issues.append('raw hardware timeline truncated')
            if other.get('graphics-counter-buffer',[0,0])[1]:issues.append('hardware buffer overflow')
            declared=other.get('graphics-counter-jobs',[None])[0]
            if declared is not None and declared!=len(batch['jobs']):issues.append('missing hardware jobs')
            parameters.extend(dict(group=batch['group'],**v) for v in batch['parameters'])
            for frame in batch['frames'].values():
                if set(frame)!=set(('data','time')):issues.append('incomplete hardware frame');continue
                hardware_frames.append(dict(group=batch['group'],frame=frame['data'][0],activityUs=frame['data'][1],startUs=frame['time'][0],endUs=frame['time'][1],processScope='unavailable'))
            for data in batch['jobs'].values():
                if set(data)!=set(('id','time','values')):issues.append('incomplete hardware job');continue
                packed,kind=data['id'];start,end=data['time'];ab,c=data['values'];a,b=ab>>32,ab&0xffffffff
                group=kind>>32;jobtype=(kind>>16)&0xffff
                if end<start or group!=batch['group'] or group>3 or jobtype not in {0:(0,7,8),1:(0,1,2),2:(0,3,4),3:(0,5,6)}.get(group,()):issues.append('invalid hardware job identity/timing');continue
                entry=dict(group=group,frame=packed>>32,scene=packed&0xffffffff,type=jobtype,core=kind&0xffff,startUs=start,endUs=end,elapsedUs=end-start)
                if jobtype in (1,2):
                    values=[struct.unpack('<f',struct.pack('<I',v))[0] for v in ([a] if jobtype==1 else [a,b,c])]
                    if not all(math.isfinite(v) and 0<=v<=100 for v in values):issues.append('invalid hardware metric percentage');continue
                    entry['reportedPercentages']=values
                elif jobtype in (3,4,5,6):entry['rawCounters']=[a,b,c]
                jobs.append(entry)
        groups=[]
        for group in range(4):
            selected=[j for j in jobs if j['group']==group]
            params=[v for v in parameters if v['group']==group]
            groups.append(dict(group=group,parameterRecords=len(params),parameterPeakBytes=max((v['peakBytes'] for v in params),default=None),partialRenderRecords=sum(v['partialRender'] for v in params),vertexPausedRecords=sum(v['vertexPaused'] for v in params),jobs=len(selected),vertexJobElapsed=summary([j['elapsedUs'] for j in selected if j['type'] in (1,3,5,7)]),fragmentJobElapsed=summary([j['elapsedUs'] for j in selected if j['type'] in (2,4,6,8)]),hardwareFrames=len({j['frame'] for j in selected}),scenes=len({(j['frame'],j['scene']) for j in selected})))
        by_sequence=defaultdict(list)
        for r in rows:by_sequence[r.get('sequence')].append(r)
        timeline_issues=[];timeline=[];phases=[]
        for phase in test['phases']:
            seqs={r.get('sequence') for r in rows if r.get('name')=='render-probe-frame' and r.get('value0')==phase['phase']}
            observations=[];health=[]
            for seq in sorted(seqs):
                sample=by_sequence[seq]
                for name in ('graphics-profile-frame','graphics-profile-health','graphics-profile-scenes','graphics-profile-observer-us'):
                    values=[r for r in sample if r.get('name')==name]
                    if len(values)!=1:timeline_issues.append('missing or duplicate '+name)
                health.extend(r for r in sample if r.get('name')=='graphics-profile-health')
                observations.extend(r['value0'] for r in sample if r.get('name')=='graphics-profile-observer-us')
                for r in sample:
                    if re.fullmatch(r'graphics-scene-\d+-cpu-time',r.get('name','')):
                        if r['value1']<r['value0']:timeline_issues.append('reversed CPU scene time')
                        prefix=r['name'].removesuffix('-cpu-time')
                        details={name:[v for v in sample if v.get('name')==prefix+'-'+name] for name in ('state','result','begin-cpu','target','format','load-store')}
                        if len(details['state'])!=1 or len(details['result'])!=1:timeline_issues.append('missing scene state or status')
                        if any(v['value0'] for v in details['result']):timeline_issues.append('native EndScene failure')
                        if any(v['value1']&0xffffffff for v in details['load-store']):timeline_issues.append('native BeginScene failure')
                        timeline.append(dict(sequence=seq,phase=phase['phase'],name=r['name'],startUs=r['value0'],endUs=r['value1'],metadata={k:[[v['value0'],v['value1']] for v in values] for k,values in details.items()},beginObserved=bool(details['begin-cpu'])))
                    if r.get('name')=='graphics-scene-overflow':timeline_issues.append('scene timeline overflow')
            phases.append(dict(phase=phase['phase'],name=phase['name'],observerCpuElapsed=summary(observations[2:]),reportedStates=sorted({r['value1'] for r in health}),frameMeanUs=phase['renderElapsed']['meanUs'],graphicsCompletionMeanUs=phase['displayQueueElapsed']['meanUs']))
        if not timeline:timeline_issues.append('no native scene submissions observed')
        measured=bool(jobs and not errors and not issues and groups[0]['parameterRecords']>0 and all(g['jobs'] for g in groups[1:]))
        baseline=[phases[i]['frameMeanUs'] for i in (0,5)]
        result.append(dict(generation=generation,captureComplete=test['captureComplete'],timelineComplete=bool(test['captureComplete'] and not timeline_issues),counterEvidenceComplete=measured,counterAvailability='available' if measured else 'unavailable' if errors else 'unverified',counterErrors=errors,counterProblems=issues,timelineProblems=timeline_issues,groups=groups,jobs=jobs,parameterRecords=parameters,hardwareFrames=hardware_frames,cpuSceneTimeline=timeline,phases=phases,baselineFrameMeanUs=baseline,hardwareToCpuSceneMappingVerified=False,hardwareClockCorrelationVerified=False,interpretation='Job times may overlap across GPU cores/stages. Reported percentages retain SDK metric meanings and are not whole-GPU utilization. CPU scene submission time is not GPU execution time. Baseline windows measure observer overhead and drift, not an optimization speedup.'))
    return result
