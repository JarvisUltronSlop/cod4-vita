"""Intrusive draw completion samples; never an additive normal-frame GPU profile."""
from collections import defaultdict,Counter
import math
import re

PASSES=('setup','world','props-fx','composition','weapon','post','hud')
def stats(values):
    v=sorted(values)
    return dict(samples=len(v),meanUs=sum(v)/len(v) if v else None,
                medianUs=v[len(v)//2] if v else None,p95Us=v[max(0,math.ceil(len(v)*.95)-1)] if v else None,
                maxUs=max(v) if v else None)

def analyze_gpu_draws(cpu,generation,start_us):
    rows=[r for r in cpu if r.get('generation')==generation and r.get('startUs',0)>=start_us]
    chunks=defaultdict(dict);sites={};frames=defaultdict(list);errors=[]
    def fail(sequence,reason):errors.append(dict(sequence=sequence,reason=reason))
    for r in rows:
        name=r.get('name','');seq=r.get('sequence')
        if name.startswith('gpu-'):frames[seq].append(r)
        if name.startswith('gpu-key-'):
            m=re.fullmatch(r'gpu-key-([012])-([0-9a-f]{64})',name)
            ident=r.get('value0')
            if not m or not isinstance(ident,int) or not 1<=ident<=2048:fail(seq,'invalid descriptor');continue
            chunk=int(m[1])
            if chunk in chunks[ident]:fail(seq,'duplicate descriptor chunk')
            chunks[ident][chunk]=[int(m[2][i:i+8],16) for i in range(0,64,8)]
        if name.startswith('gpu-site-'):
            ident=r.get('value0')
            if ident in sites:fail(seq,'duplicate call site')
            sites[ident]=(name[len('gpu-site-'):],r.get('value1'))
    catalog={}
    for ident,parts in chunks.items():
        if set(parts)!={0,1,2} or ident not in sites:fail(None,'incomplete descriptor '+str(ident));continue
        key=sum((parts[i] for i in range(3)),[])
        if key[0]>6 or key[1]!=sites[ident][1] or key[2] not in (1,2,3) or key[5]==0:fail(None,'invalid descriptor fields');continue
        catalog[ident]=dict(pipeline=PASSES[key[0]],callSite=sites[ident][0],line=key[1],api={1:'glDrawArrays',2:'glDrawElements',3:'glDrawElementsInstanced'}[key[2]],
            mode=key[3],elementCount=key[4],instances=key[5],shader=key[6],framebuffer=key[7],vertexBuffer=key[8],indexBuffer=key[9],indexOffset=key[10],
            sourceSurface=None if key[11]==0xffffffff else key[11],textures=dict(zip(('color','normal','specular','reflection','primaryLightmap','secondaryLightmap'),key[12:18])),
            stateBits=key[18:20],techniqueFlags=key[20:22],pathFlags=key[22],samplers=key[23])
    for descriptor in catalog.values():
        if descriptor['pathFlags']&256:
            descriptor['fullscreenInputs']={'scene':descriptor['textures']['color'],'dither':descriptor['textures']['normal']}
            descriptor['textures']={}
    render={r['sequence']:r for r in rows if r.get('name')=='render-probe-frame'}
    observed=Counter();baseline=Counter();waits=defaultdict(list);submits=defaultdict(list);ordinals=defaultdict(set)
    frame_results=[];previous_draws=0;step=0
    for seq in sorted(set(frames)|set(render)):
        records=frames[seq];names=defaultdict(list)
        for r in records:names[r['name']].append(r)
        if seq not in render:fail(seq,'draw evidence without renderer frame');continue
        phase=render[seq]['value0']
        required=('frame','window','prefix','empty','health','report-us')
        if any(len(names['gpu-draw-'+n])!=1 for n in required):fail(seq,'missing/duplicate draw frame records');continue
        def pair(n):
            r=names['gpu-draw-'+n][0];return r['value0'],r['value1']
        count,total=pair('frame');first,measured=pair('window');prefix,prefix_first=pair('prefix');empty,batch=pair('empty')
        if not 0<count<=2048 or total>2048 or pair('health')!=(0,2048):fail(seq,'capacity/GL health failure');continue
        order=[]
        for at in range(0,count,4):
            values=names[f'gpu-order-{at:04d}']
            if len(values)!=1:fail(seq,'missing/duplicate draw order');break
            x,y=values[0]['value0'],values[0]['value1'];ids=[x&0xffffffff,x>>32,y&0xffffffff,y>>32]
            if count-at<4 and any(ids[count-at:]):fail(seq,'nonzero order padding')
            order.extend(ids[:min(4,count-at)])
        if len(order)!=count or any(i not in catalog for i in order):fail(seq,'draw order lacks descriptor');continue
        if any(len(v)!=1 for k,v in names.items() if k.startswith('gpu-order-')):fail(seq,'duplicate draw order')
        groups=(previous_draws+7)//8
        expected_first=(step%groups)*8 if phase==2 and groups else 0
        if phase==2 and groups:step+=1
        previous_draws=count
        expected_ordinals=list(range(first,min(count,first+8))) if phase==2 else []
        if first!=expected_first or measured!=len(expected_ordinals) or batch!=8 or prefix_first!=first:fail(seq,'rotating sample window differs')
        if phase!=2 and (prefix or empty):fail(seq,'baseline completion fence')
        sample_maps=[]
        for suffix in ('submit','wait'):
            values={}
            for row in names['gpu-draw-'+suffix]:
                ident=row['value0']&0xffffffff;ordinal=row['value0']>>32
                if ordinal in values or ordinal>=count or order[ordinal]!=ident or row['value1']<0:fail(seq,'invalid draw sample pairing')
                values[ordinal]=(ident,row['value1'])
            if sorted(values)!=expected_ordinals:fail(seq,'missing/duplicate sampled draw')
            sample_maps.append(values)
        expected_pass=[0]*7
        if expected_ordinals:expected_pass[PASSES.index(catalog[order[first]]['pipeline'])]+=prefix+empty
        for ordinal,(ident,wait) in sample_maps[1].items():
            if ordinal not in sample_maps[0]:continue
            waits[ident].append(wait);submits[ident].append(sample_maps[0][ordinal][1]);ordinals[ident].add(ordinal)
            expected_pass[PASSES.index(catalog[ident]['pipeline'])]+=wait
        actual_pass={}
        for row in names['gpu-draw-pass-wait']:
            if row['value0'] in actual_pass:fail(seq,'duplicate pass wait')
            actual_pass[row['value0']]=row['value1']
        if actual_pass!={i:v for i,v in enumerate(expected_pass)}:fail(seq,'pass/prefix/control wait accounting differs')
        observed.update(order)
        if phase!=2:baseline.update(order)
        frame_results.append(dict(sequence=seq,phase=phase,draws=count,sampled=measured,prefixWaitUs=prefix,emptyControlUs=empty,
                                  totalProbeWaitUs=sum(expected_pass),passWaitUs=expected_pass,reportUs=pair('report-us')[0]))
    ranked=[dict(id=i,**catalog[i],observations=observed[i],baselineObservations=baseline[i],sampledOrdinals=sorted(ordinals[i]),
                 completionWait=stats(waits[i]),cpuSubmit=stats(submits[i])) for i in catalog if observed[i]]
    ranked.sort(key=lambda r:r['completionWait']['meanUs'] if r['completionWait']['meanUs'] is not None else -1,reverse=True)
    grouped=defaultdict(list)
    for row in ranked:grouped[(row['pipeline'],row['callSite'],row['line'])].append(row['id'])
    functions=[]
    for (pipeline,site,line),ids in grouped.items():
        functions.append(dict(pipeline=pipeline,callSite=site,line=line,drawSignatures=len(ids),
            completionWaitPerSampleDraw=stats([v for i in ids for v in waits[i]]),
            cpuSubmitPerSampleDraw=stats([v for i in ids for v in submits[i]])))
    functions.sort(key=lambda v:v['completionWaitPerSampleDraw']['meanUs'] if v['completionWaitPerSampleDraw']['meanUs'] is not None else -1,reverse=True)
    stable=[i for i in catalog if baseline[i]>=3 and catalog[i]['pipeline'] in ('world','props-fx')]
    missing=[i for i in stable if not waits[i]];thin=[i for i in stable if len(waits[i])<3]
    phase_summary=[]
    for phase in (1,2,3):
        selected=[f for f in frame_results if f['phase']==phase]
        phase_summary.append(dict(phase=phase,frames=len(selected),sampledDraws=sum(f['sampled'] for f in selected),
                                  prefixWait=stats([f['prefixWaitUs'] for f in selected]),emptyControl=stats([f['emptyControlUs'] for f in selected]),
                                  totalProbeWait=stats([f['totalProbeWaitUs'] for f in selected]),report=stats([f['reportUs'] for f in selected])))
    return dict(captureComplete=not errors and all(p['frames']>=3 for p in phase_summary) and phase_summary[1]['sampledDraws']>0,
                coverageComplete=bool(stable) and not thin,stableWorldPropSignatures=len(stable),unmeasuredStableSignatures=missing,fewerThanThreeSamples=thin,
                invalidFrames=errors,phases=phase_summary,draws=ranked,callSites=functions,frameWaits=frame_results,
                interpretation='Draw completion samples include forced scene completion, tile store/load and draw prerequisites. Prefix wait includes earlier queued work. Empty control is reported separately, not subtracted as an exact overhead estimate. Do not sum ranked draw means or treat them as normal-frame GPU execution/utilization. Unmeasured and sparsely sampled signatures remain incomplete evidence.')
