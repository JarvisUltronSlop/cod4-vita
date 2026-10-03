"""Validate authored-mip selection and unchanged texture/geometry workload."""
from collections import Counter, defaultdict

NAMES=('native-a','world-mip','native-b','viewmodel-mip','native-c','both-mips','native-d')
MODES=(0,1,0,2,0,3,0)

def mip_bytes(fmt,w,h):
    if fmt==21: return w*h*4
    if fmt==50: return w*h
    if fmt==0x31545844: return ((w+3)//4)*((h+3)//4)*8
    if fmt in (0x33545844,0x35545844): return ((w+3)//4)*((h+3)//4)*16
    return 0

def validate_textures(records, frames, cases, reject):
    uploads=defaultdict(list); totals={}; summary={}; layouts=[]
    for r in records:
        if r.get('type')=='texture-upload': uploads[r.get('case')].append(r)
        if r.get('type')=='texture-frame':
            key=(r.get('case'),r.get('n'))
            if key in totals: reject('Duplicate texture frame')
            totals[key]=r
    if set(uploads)!=set(range(7)): reject('Missing/extra texture upload cases')
    if set(totals)!={(i,f['n']) for i,ff in frames.items() for f in ff}:
        reject('Missing/extra texture frame evidence')
    for i,mode in enumerate(MODES):
        counts=[0]*3; reduced=[0]*3; source=[0]*3; selected=[0]*3; layout=[]
        if cases.get(i,{}).get('textureMode')!=mode: reject('Requested texture mode mismatch')
        for r in uploads[i]:
            fields=('mode','nameHash','role','format','flags','sourceBytes','selectedBytes','offset')
            if any(type(r.get(k))!=int or r[k]<0 for k in fields) or any(
                not isinstance(r.get(k),list) or len(r[k])!=3 or any(type(v)!=int or v<=0 for v in r[k])
                for k in ('source','selected')):
                reject('Malformed texture upload'); continue
            role=r['role']; w,h,levels=r['source']
            if role not in (0,1,2) or w>4096 or h>4096 or levels>16:
                reject('Invalid texture role/dimensions'); continue
            if r['mode']!=mode or r.get('valid') is not True or r.get('success') is not True:
                reject('Texture mode/upload failure')
            requested=(role==1 and mode&1) or (role==2 and mode&2)
            expected=bool(requested and not (r['flags']&4) and levels>=2 and w>=8 and h>=8)
            offset=mip_bytes(r['format'],w,h) if expected else 0
            dimensions=[max(1,w//2),max(1,h//2),levels-1] if expected else [w,h,levels]
            chain=sum(mip_bytes(r['format'],max(1,w>>level),max(1,h>>level)) for level in range(levels))
            if chain<=0 or chain*(6 if r['flags']&4 else 1)>r['sourceBytes']:
                reject('Incomplete/unsupported texture source chain')
            if r.get('reduced') is not expected or r['offset']!=offset or r['selected']!=dimensions or r['selectedBytes']!=r['sourceBytes']-offset:
                reject('Incorrect authored mip selection')
            counts[role]+=1; reduced[role]+=expected
            source[role]+=r['sourceBytes']; selected[role]+=r['selectedBytes']
            layout.append((r['nameHash'],role,w,h,levels,r['format'],r['flags'],r['sourceBytes']))
        layouts.append(Counter(layout))
        for f in frames[i]:
            t=totals.get((i,f['n']),{})
            if t.get('mode')!=mode or t.get('failures')!=0: reject('Active texture mode/failure mismatch')
            for key,expected in (('attempts',counts),('reduced',reduced),('sourceBytes',source),('selectedBytes',selected)):
                if t.get(key)!=expected: reject('Texture staging changed during measurement or totals mismatch: '+key)
            for key in ('audioMode','audioErrors','geometryMode','noLocalLightDraws','noSpecularDraws','noNormalMapDraws','adaptiveFullLightDraws'):
                if f.get(key)!=0: reject('Texture experiment changed another factor: '+key)
            if f.get('staticDrawFailures')!=0 or f.get('staticArchivePasses')!=1:
                reject('Texture experiment lost native static geometry')
        exercised=(not mode) or all(reduced[r]>0 for r in (1,2) if mode&(1<<(r-1)))
        summary[i]={'uploadAttempts':counts,'reducedUploads':reduced,'sourcePayloadBytes':source,
                    'selectedPayloadBytes':selected,'exercised':exercised}
    if any(layout!=layouts[0] for layout in layouts[1:]): reject('Texture source inventory changed between cases')
    return summary
