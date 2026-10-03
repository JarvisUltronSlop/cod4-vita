"""Validate rendering-only ground-clutter suppression and isolated fog shader work."""
import math
import statistics
from collections import defaultdict

def validate_debris_fog(records, frames, cases, reject):
    details={}; categories=defaultdict(dict); summary={}
    for r in records:
        key=(r.get('case'),r.get('n'))
        if r.get('type')=='debris-fog-frame':
            if key in details: reject('Duplicate debris/fog frame')
            details[key]=r
        if r.get('type')=='debris-category-frame':
            c=r.get('category')
            if c in categories[key]: reject('Duplicate debris category')
            categories[key][c]=r
    expected={(i,f['n']) for i,ff in frames.items() for f in ff}
    if set(details)!=expected or set(categories)!=expected: reject('Missing/extra debris/fog frame evidence')
    candidate_layout=None; protected_geometry=None; resources=None; fog_parameters=None
    for i,ff in frames.items():
        mode=1 if i==1 else 0
        if cases.get(i,{}).get('geometryMode')!=(3 if mode else 0): reject('Requested clutter mode mismatch')
        all_categories=[]; fog_rows=[]
        for f in ff:
            key=(i,f['n']); d=details.get(key,{}); cc=categories.get(key,{})
            if d.get('debrisMode')!=mode or d.get('failures')!=0: reject('Clutter mode/draw failure')
            if d.get('textureMode')!=0: reject('Debris/fog changed texture quality')
            if f.get('geometryMode')!=(3 if mode else 0): reject('Active geometry mode mismatch')
            for field in ('audioMode','audioErrors','noLocalLightDraws','noNormalMapDraws','noSpecularDraws','adaptiveFullLightDraws'):
                if f.get(field)!=0: reject('Debris/fog changed another factor: '+field)
            if f.get('staticArchivePasses')!=1 or f.get('staticDrawFailures')!=0:
                reject('Debris/fog lost native static geometry')
            view=f.get('viewmodelGeometry')
            if not isinstance(view,list) or len(view)!=2 or any(type(v)!=int or v<=0 for v in view):
                reject('Missing viewmodel geometry')
            elif protected_geometry is None: protected_geometry=view
            elif view!=protected_geometry: reject('Viewmodel geometry changed')
            resident=tuple(f.get(k) for k in ('textureBytes','meshBytes','targetLogicalBytes'))
            if any(type(v)!=int or v<=0 for v in resident): reject('Missing render resource evidence')
            elif resources is None: resources=resident
            elif resident!=resources: reject('Render resource residency changed')
            if set(cc)!={0,1,2,3}: reject('Missing/extra debris categories'); continue
            valid=True
            fields=('candidates','candidateTriangles','candidateInstances','skipped','skippedTriangles','submitted','submittedTriangles')
            for c,r in cc.items():
                if any(type(r.get(k))!=int or r[k]<0 for k in fields):
                    reject('Malformed debris counts'); valid=False; continue
                omit=bool(mode and c in (0,2))
                for candidate,skip,submit in (('candidates','skipped','submitted'),('candidateTriangles','skippedTriangles','submittedTriangles')):
                    if r[skip]!=(r[candidate] if omit else 0) or r[submit]!=(0 if omit else r[candidate]):
                        reject('Debris geometry conservation/intervention mismatch')
            if valid:
                layout=tuple(tuple(cc[c][k] for k in ('candidates','candidateTriangles','candidateInstances')) for c in range(4))
                if candidate_layout is None: candidate_layout=layout
                elif layout!=candidate_layout: reject('Candidate scene changed between frames/cases')
                all_categories.append(cc)
            if any(type(d.get(k))!=int or d[k]<0 for k in ('noFogDraws','fogEligibleDraws')):
                reject('Missing fog activity'); continue
            if d['noFogDraws']!=(f.get('sceneShaderDraws') if i==3 else 0): reject('Wrong bound fog shader')
            if d['fogEligibleDraws']>f.get('sceneShaderDraws',0): reject('Impossible fog draw count')
            if type(d.get('fogEnabled'))!=bool or d['fogEnabled']!=(d['fogEligibleDraws']>0): reject('Fog enabled/input evidence mismatch')
            if any(type(d.get(k)) not in (int,float) or not math.isfinite(d[k]) for k in ('fogStart','fogDensity')):
                reject('Invalid fog parameters'); continue
            if d['fogEnabled'] and d['fogDensity']<=0: reject('Fog density not active')
            parameters=(d['fogEnabled'],d['fogStart'],d['fogDensity'])
            if fog_parameters is None: fog_parameters=parameters
            elif parameters!=fog_parameters: reject('Source fog parameters changed')
            fog_rows.append(d)
        summary[i]={'categories':{
            str(c):{k:statistics.mean(cc[c][k] for cc in all_categories) for k in fields}
            for c in range(4)} if all_categories else {},
            'debrisExercised':any(cc[c]['skippedTriangles']>0 for cc in all_categories for c in (0,2)),
            'fogInputsExercised':any(d['fogEligibleDraws']>0 for d in fog_rows),
            'meanFogEligibleDraws':statistics.mean(d['fogEligibleDraws'] for d in fog_rows) if fog_rows else None}
    return summary
