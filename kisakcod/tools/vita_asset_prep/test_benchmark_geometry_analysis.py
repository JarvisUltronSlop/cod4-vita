import copy
import unittest
from analyze_benchmark import analyze
from test_benchmark_analysis import fixture


def geometry_fixture(foliage=True):
    old, identity=fixture()
    rows=[r for r in old if r['type'] in ('run','identity')]
    rows[0]['round']=identity['round']='geometry-5'
    names=('native-a','no-foliage-models','native-b','no-static-models','native-c')
    for i,name in enumerate(names):
        mode=1 if i==1 else 2 if i==3 else 0
        rows.append({'type':'case','index':i,'id':name,'scene':[960,544],'shaderMode':0,'geometryMode':mode})
        for n in range(1,61):
            candidates=[10 if foliage else 0,20]
            triangles=[1000 if foliage else 0,2000]
            skipped=[v if mode==2 or (mode==1 and j==0) else 0 for j,v in enumerate(candidates)]
            skipped_triangles=[v if mode==2 or (mode==1 and j==0) else 0 for j,v in enumerate(triangles)]
            submitted=[a-b for a,b in zip(candidates,skipped)]
            submitted_triangles=[a-b for a,b in zip(triangles,skipped_triangles)]
            interval=1000000 if mode else 1500000
            rows.append({'type':'frame','case':i,'n':n,'endUs':n*interval,'intervalUs':interval,
                'accountingValid':True,'scene':[960,544],'clocks':[333,222,111,111],
                'charging':0,'origin':[1,2,3],'angles':[0,90,0],'weapon':2,
                'draws':100+sum(submitted),'triangles':5000+sum(submitted_triangles),
                'shaderMode':0,'sceneShaderDraws':50+sum(submitted),'simpleMaterialDraws':0,
                'postCopyDraws':0,'authoredPostDraws':1,'noLocalLightDraws':0,'noSpecularDraws':0,
                'noNormalMapDraws':0,'adaptiveFullLightDraws':0,'geometryMode':mode,
                'staticArchivePasses':1,'staticDrawFailures':0,'staticModelUs':1000,
                'staticCandidates':candidates,'staticCandidateTriangles':triangles,
                'staticCandidateInstances':[20 if foliage else 0,40],
                'staticSkipped':skipped,'staticSkippedTriangles':skipped_triangles,
                'staticSubmitted':submitted,'staticSubmittedTriangles':submitted_triangles,
                'worldGeometry':[40,4000],'viewmodelGeometry':[10,1000]})
        rows.append({'type':'result','case':i,'samples':60,'complete':True})
    rows.append({'type':'end','allSegmentsWritten':True})
    return rows,identity


class GeometryAnalysisTests(unittest.TestCase):
    def test_expected_omission_and_lower_total_workload_are_valid(self):
        rows,identity=geometry_fixture(); r=analyze(rows,identity)
        self.assertTrue(r['validComparison'],r['errors'])
        self.assertTrue(all(c['passesScreen'] for c in r['comparisons']))

    def test_inactive_category_cannot_pass(self):
        rows,identity=geometry_fixture(False); r=analyze(rows,identity)
        self.assertTrue(r['validComparison'],r['errors'])
        self.assertEqual(r['comparisons'][0]['featureActivity'],'not-exercised')
        self.assertFalse(r['comparisons'][0]['passesScreen'])

    def test_incorrect_or_missing_category_evidence_rejected(self):
        mutations=[('geometryMode',0),('staticArchivePasses',0),('staticDrawFailures',1),
                   ('staticSkipped',[0,0]),('staticSkippedTriangles',[0,0]),
                   ('staticSubmitted',[10,20]),('staticSubmittedTriangles',[1000,2000]),
                   ('staticCandidateInstances',[20,1]),('worldGeometry',[1,20]),
                   ('viewmodelGeometry',[0,0]),('noLocalLightDraws',50),
                   ('staticCandidateTriangles',None),('staticSubmitted',[])]
        for key,value in mutations:
            rows,identity=geometry_fixture()
            # Whole segment mutation also exercises stable-workload checks.
            for f in rows:
                if f['type']=='frame' and f['case']==1: f[key]=copy.deepcopy(value)
            r=analyze(rows,identity)
            self.assertFalse(r['validComparison'],key)
            self.assertFalse(any(c['passesScreen'] for c in r['comparisons']),key)


if __name__=='__main__': unittest.main()
