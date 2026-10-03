import copy
from analyze_benchmark import analyze
from test_benchmark_analysis import fixture as resolution_fixture

def fixture():
    old,identity=resolution_fixture()
    rows=[r for r in old if r['type'] in ('run','identity')]
    rows[0]['round']=identity['round']='balanced-combined-9'
    for r in old:
        case=r.get('case',r.get('index',-1))
        if case not in (1,2,3): continue
        r=copy.deepcopy(r); i=case-1
        if r['type']=='case':
            r.update(index=i,id=('native-a','balanced-combined','native-b')[i],shaderMode=5 if i==1 else 0)
        else: r['case']=i
        rows.append(r)
        if r['type']=='frame':
            r.update(textureBytes=10000,meshBytes=20000,worldGeometry=[150,14000],staticCandidates=[8,163],
                     staticCandidateInstances=[133,269],authoredPostDraws=1,postCopyDraws=0)
            rows.append(dict(type='balanced-frame',case=i,n=r['n'],world=r['scene'],viewmodel=[960,544],
                             compositionUs=100 if i==1 else 0,grassOptionalCandidates=20,grassOmissions=5 if i==1 else 0,
                             balancedGrass=i==1,adaptiveLighting=i==1,targetLogicalBytes=960*544*6+(720*408*6 if i==1 else 0)))
    rows.append({'type':'end','allSegmentsWritten':True})
    return rows,identity

def test_combined_compares_directly():
    rows,identity=fixture(); result=analyze(rows,identity)
    assert result['validComparison'],result['errors']
    assert len(result['comparisons'])==1
    assert result['comparisons'][0]['gainPercent']==50

def test_combined_rejects_degraded_weapon_or_wrong_targets():
    for key,value in [('viewmodel',[720,408]),('targetLogicalBytes',0),('adaptiveLighting',False),('grassOmissions',21)]:
        rows,identity=fixture()
        next(r for r in rows if r['type']=='balanced-frame' and r['case']==1)[key]=value
        assert not analyze(rows,identity)['validComparison']

def test_missing_profile_evidence_cannot_certify():
    rows,identity=fixture()
    rows=[r for r in rows if r['type']!='balanced-frame']
    assert not analyze(rows,identity)['validComparison']
