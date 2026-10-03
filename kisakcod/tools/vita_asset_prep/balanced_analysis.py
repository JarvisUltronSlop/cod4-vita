"""Validate the combined profile without treating it as a one-factor trial."""
def validate_balanced(records,frames,cases,reject):
    auxiliary={}
    for r in records:
        if r.get('type')=='balanced-frame':
            key=(r.get('case'),r.get('n'))
            if key in auxiliary: reject('Duplicate balanced profile sample')
            auxiliary[key]=r
    reference=None
    for i,ff in frames.items():
        candidate=i==1
        world=[720,408] if candidate else [960,544]
        expected_bytes=960*544*6+(720*408*6 if candidate else 0)
        if cases.get(i,{}).get('shaderMode')!=(5 if candidate else 0):
            reject('Incorrect combined case policy')
        for f in ff:
            a=auxiliary.get((i,f['n']),{})
            if a.get('world')!=world or a.get('viewmodel')!=[960,544]:
                reject('Missing/wrong native viewmodel or world dimensions')
            if a.get('adaptiveLighting') is not candidate or a.get('balancedGrass') is not candidate:
                reject('Combined shader/grass policy differs from case')
            if a.get('targetLogicalBytes')!=expected_bytes: reject('Unexpected target generation residency')
            available=a.get('grassOptionalCandidates'); omitted=a.get('grassOmissions')
            if type(available)!=int or type(omitted)!=int or not 0<=omitted<=available:
                reject('Invalid grass conservation evidence')
            elif not candidate and omitted: reject('Baseline omitted grass')
            if type(a.get('compositionUs'))!=int: reject('Missing composition timing')
            if f.get('authoredPostDraws')!=1 or f.get('postCopyDraws')!=0:
                reject('Combined scene must receive exactly one authored grade')
            if f.get('staticDrawFailures',0): reject('Static draw failure')
            if f.get('simpleMaterialDraws',0) or f.get('noNormalMapDraws',0) or f.get('noSpecularDraws',0):
                reject('Unrequested material reduction')
            workload=[f.get(k) for k in ('textureBytes','meshBytes','worldGeometry','staticCandidates','staticCandidateInstances')]
            if any(v is None for v in workload): reject('Missing unchanged workload evidence')
            if reference is None: reference=workload
            elif workload!=reference: reject('Texture/mesh/world/static candidate workload changed')
    if len(auxiliary)!=sum(map(len,frames.values())): reject('Missing/extra combined profile samples')
