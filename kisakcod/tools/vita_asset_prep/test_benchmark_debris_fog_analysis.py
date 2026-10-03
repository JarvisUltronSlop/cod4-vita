import copy
import unittest
from analyze_benchmark import analyze
from test_benchmark_geometry_analysis import geometry_fixture

def fixture(debris=True,fog=True):
    old,identity=geometry_fixture(); rows=[r for r in old if r['type'] in ('run','identity')]
    rows[0]['round']=identity['round']='debris-fog-8'
    template=next(r for r in old if r['type']=='frame' and r['case']==0)
    names=('native-a','no-ground-clutter','native-b','no-fog','native-c')
    for i,name in enumerate(names):
        mode=int(i==1); shader=7 if i==3 else 0
        rows.append(dict(type='case',index=i,id=name,scene=[960,544],shaderMode=shader,geometryMode=3 if mode else 0))
        for n in range(1,61):
            f=copy.deepcopy(template); interval=1000000 if i in (1,3) else 1500000
            f.update(case=i,n=n,endUs=n*interval,intervalUs=interval,audioMode=0,audioErrors=0,
                     shaderMode=shader,geometryMode=3 if mode else 0,textureBytes=1024,meshBytes=2048,targetLogicalBytes=4096)
            rows.append(f)
            rows.append(dict(type='debris-fog-frame',case=i,n=n,debrisMode=mode,failures=0,
                noFogDraws=f['sceneShaderDraws'] if i==3 else 0,fogEligibleDraws=f['sceneShaderDraws'] if fog else 0,
                fogEnabled=fog,fogStart=100 if fog else 0,fogDensity=.001 if fog else 0,textureMode=0))
            for c in range(4):
                candidate=10 if debris or c in (1,3) else 0
                omit=mode and c in (0,2)
                rows.append(dict(type='debris-category-frame',case=i,n=n,category=c,candidates=candidate,
                    candidateTriangles=candidate*100,candidateInstances=candidate*2,
                    skipped=candidate if omit else 0,skippedTriangles=candidate*100 if omit else 0,
                    submitted=0 if omit else candidate,submittedTriangles=0 if omit else candidate*100))
        rows.append(dict(type='result',case=i,samples=60,complete=True))
    rows.append(dict(type='end',allSegmentsWritten=True))
    return rows,identity

class DebrisFogAnalysisTests(unittest.TestCase):
    def test_valid_round(self):
        rows,id=fixture(); result=analyze(rows,id)
        self.assertTrue(result['validComparison'],result['errors'])
        self.assertTrue(all(c['passesScreen'] for c in result['comparisons']))

    def test_inactive_features_cannot_pass(self):
        rows,id=fixture(False,False); result=analyze(rows,id)
        self.assertTrue(result['validComparison'],result['errors'])
        self.assertTrue(all(not c['passesScreen'] and c['featureActivity']=='not-exercised' for c in result['comparisons']))

    def test_missing_duplicate_categories_and_failure_rejected(self):
        for key,value in [('skipped',0),('skippedTriangles',0),('candidateInstances',1),('submitted',1),('candidates',None)]:
            rows,id=fixture()
            next(r for r in rows if r['type']=='debris-category-frame' and r['case']==1 and r['category']==0)[key]=value
            self.assertFalse(analyze(rows,id)['validComparison'],key)
        for duplicate in (False,True):
            rows,id=fixture(); row=next(r for r in rows if r['type']=='debris-category-frame')
            if duplicate: rows.append(row)
            else: rows.remove(row)
            self.assertFalse(analyze(rows,id)['validComparison'])

    def test_fog_variant_inputs_and_other_factors_must_match(self):
        for key,value in [('failures',1),('noFogDraws',0),('fogDensity',0),('fogStart',200),('fogEnabled',False),('textureMode',1)]:
            rows,id=fixture()
            next(r for r in rows if r['type']=='debris-fog-frame' and r['case']==3)[key]=value
            self.assertFalse(analyze(rows,id)['validComparison'],key)
        for key,value in [('audioMode',1),('textureBytes',512),('meshBytes',1024),('viewmodelGeometry',[1,1]),('geometryMode',1)]:
            rows,id=fixture(); next(r for r in rows if r['type']=='frame')[key]=value
            self.assertFalse(analyze(rows,id)['validComparison'],key)

if __name__=='__main__': unittest.main()
