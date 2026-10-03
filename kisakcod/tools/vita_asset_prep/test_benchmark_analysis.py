import copy
import unittest
from analyze_benchmark import analyze, CASES

def fixture():
    records=[{'type':'run','schema':1,'round':'resolution-1'}]; identity={'actualFiles':{},'round':'resolution-1'}
    for i in range(5):
        path=f'file-{i}'; identity['actualFiles'][path]=str(i)
        records.append({'type':'identity','path':path,'sha256':str(i)})
    for i,name in enumerate(CASES):
        scene=[720,408] if i==2 else [480,272] if i==4 else [960,544]
        records.append({'type':'case','index':i,'id':name,'scene':scene})
        interval=1000000 if i==0 else 750000 if i in (2,4) else 1500000
        count=10 if i==0 else 80 if i in (2,4) else 40
        for n in range(1,count+1):
            records.append({'type':'frame','case':i,'n':n,'intervalUs':interval,'endUs':n*interval,
                'accountingValid':True,'scene':scene,'clocks':[333,222,111,111],
                'charging':0,'origin':[1,2,3],'angles':[0,90,0],'weapon':2,
                'draws':500,'triangles':100000,'queueUs':20000,'gcWaitUs':100})
        records.append({'type':'result','case':i,'samples':count,'complete':True})
    records.append({'type':'end','allSegmentsWritten':True})
    return records,identity

class AnalysisTests(unittest.TestCase):
    def test_normal_round_adaptive_fallback(self):
        old,identity=fixture()
        rows=[r for r in old if r['type'] in ('run','identity')]
        rows[0]['round']=identity['round']='normal-dispatch-4'
        for i,name in enumerate(('native-a','adaptive-lights','native-b','no-normal-map','native-c')):
            mode=5 if i==1 else 6 if i==3 else 0
            rows.append({'type':'case','index':i,'id':name,'scene':[960,544],'shaderMode':mode})
            for n in range(1,61):
                rows.append({'type':'frame','case':i,'n':n,'intervalUs':1000000,'endUs':n*1000000,
                    'accountingValid':True,'scene':[960,544],'clocks':[333,222,111,111],
                    'charging':0,'origin':[1,2,3],'angles':[0,90,0],'weapon':2,
                    'draws':500,'triangles':100000,'shaderMode':mode,'sceneShaderDraws':300,
                    'simpleMaterialDraws':0,'postCopyDraws':0,'authoredPostDraws':1,
                    'noLocalLightDraws':300 if mode==5 else 0,'noSpecularDraws':0,
                    'specularEligibleDraws':20,'nonSunLightInputs':0,
                    'normalEligibleDraws':30,'adaptiveFullLightDraws':0,
                    'noNormalMapDraws':300 if mode==6 else 0})
            rows.append({'type':'result','case':i,'samples':60,'complete':True})
        rows.append({'type':'end','allSegmentsWritten':True})
        self.assertTrue(analyze(rows,identity)['validComparison'])
        for r in rows:
            if r['type']=='frame': r['nonSunLightInputs']=1
        self.assertFalse(analyze(rows,identity)['validComparison'])
        for r in rows:
            if r['type']=='frame' and r['case']==1:
                r['noLocalLightDraws']=0; r['adaptiveFullLightDraws']=300
        self.assertTrue(analyze(rows,identity)['validComparison'])
        frame=next(r for r in rows if r['type']=='frame' and r['case']==3)
        frame['noNormalMapDraws']=0
        self.assertFalse(analyze(rows,identity)['validComparison'])

    def test_material_features_and_actual_programs(self):
        old,identity=fixture()
        rows=[r for r in old if r['type'] in ('run','identity')]
        rows[0]['round']=identity['round']='material-3'
        for i,name in enumerate(('native-a','no-local-lights','native-b','no-specular','native-c')):
            mode=3 if i==1 else 4 if i==3 else 0
            rows.append({'type':'case','index':i,'id':name,'scene':[960,544],'shaderMode':mode})
            for n in range(1,61):
                rows.append({'type':'frame','case':i,'n':n,'intervalUs':1000000,'endUs':n*1000000,
                    'accountingValid':True,'scene':[960,544],'clocks':[333,222,111,111],
                    'charging':0,'origin':[1,2,3],'angles':[0,90,0],'weapon':2,
                    'draws':500,'triangles':100000,'shaderMode':mode,'sceneShaderDraws':300,
                    'simpleMaterialDraws':0,'postCopyDraws':0,'authoredPostDraws':1,
                    'noLocalLightDraws':300 if mode==3 else 0,
                    'noSpecularDraws':300 if mode==4 else 0,
                    'specularEligibleDraws':20,'nonSunLightInputs':1})
            rows.append({'type':'result','case':i,'samples':60,'complete':True})
        rows.append({'type':'end','allSegmentsWritten':True})
        result=analyze(rows,identity)
        self.assertTrue(result['validComparison'],result['errors'])
        for field in ('noLocalLightDraws','noSpecularDraws','specularEligibleDraws','nonSunLightInputs'):
            broken=copy.deepcopy(rows)
            frame=next(r for r in broken if r['type']=='frame' and r['case']==1)
            del frame[field]
            self.assertFalse(analyze(broken,identity)['validComparison'],field)
        for r in rows:
            if r['type']=='frame': r['nonSunLightInputs']=0
        result=analyze(rows,identity)
        self.assertTrue(result['validComparison'],result['errors'])
        self.assertEqual(result['comparisons'][0]['featureActivity'],'not-exercised')
        self.assertTrue(result['comparisons'][0]['programCostMeasured'])

    def test_blocked_input_requires_explicit_policy_and_no_source_delivery(self):
        rows,identity=fixture()
        rows[0]['inputPolicy']='blocked-gameplay-v1'
        rows.insert(-1,{'type':'blocked-input','deliveredToSource':False})
        self.assertTrue(analyze(rows,identity)['validComparison'])
        rows[-2]['deliveredToSource']=True
        self.assertFalse(analyze(rows,identity)['validComparison'])
        rows[-2]['deliveredToSource']=False
        del rows[0]['inputPolicy']
        self.assertFalse(analyze(rows,identity)['validComparison'])
    def test_shading_draw_evidence(self):
        records,identity=fixture()
        identity['round']='shading-2'
        rows=[r for r in records if r['type'] in ('run','identity')]
        rows[0]['round']='shading-2'
        for i,name in enumerate(('native-a','post-copy','native-b','simple-material','native-c')):
            mode=1 if i==1 else 2 if i==3 else 0
            rows.append({'type':'case','index':i,'id':name,'scene':[960,544],'shaderMode':mode})
            for n in range(1,61):
                rows.append({'type':'frame','case':i,'n':n,'intervalUs':1000000,'endUs':n*1000000,
                    'accountingValid':True,'scene':[960,544],'clocks':[333,222,111,111],
                    'charging':0,'origin':[1,2,3],'angles':[0,90,0],'weapon':2,
                    'draws':500,'triangles':100000,'shaderMode':mode,'sceneShaderDraws':300,
                    'simpleMaterialDraws':300 if mode==2 else 0,'postCopyDraws':1 if mode==1 else 0,
                    'authoredPostDraws':0 if mode==1 else 1})
            rows.append({'type':'result','case':i,'samples':60,'complete':True})
        rows.append({'type':'end','allSegmentsWritten':True})
        result=analyze(rows,identity)
        self.assertTrue(result['validComparison'],result['errors'])
        for field,value in (('shaderMode',0),('sceneShaderDraws',0),('simpleMaterialDraws',0),
                            ('postCopyDraws',1),('authoredPostDraws',0),('scene',[480,272])):
            with self.subTest(field=field):
                broken=copy.deepcopy(rows)
                frame=next(r for r in broken if r['type']=='frame' and r['case']==3)
                frame[field]=value
                self.assertFalse(analyze(broken,identity)['validComparison'])

    def test_clean_round(self):
        records,identity=fixture(); result=analyze(records,identity)
        self.assertTrue(result['validComparison'],result['errors'])
        self.assertTrue(all(c['passesScreen'] for c in result['comparisons']))
        self.assertFalse(result['hardwareQualified'])
    def test_failures_cannot_pass(self):
        def frames(rows): return [r for r in rows if r['type']=='frame']
        mutations=[
            lambda r,i: r.pop(),
            lambda r,i: r.append({'type':'abort','reason':'suspend'}),
            lambda r,i: i['actualFiles'].update({'file-0':'wrong'}),
            lambda r,i: frames(r)[-1].update({'origin':[200,0,0]}),
            lambda r,i: frames(r)[-1].update({'clocks':[444,222,222,111]}),
            lambda r,i: frames(r)[-1].update({'charging':1}),
            lambda r,i: frames(r)[-1].update({'n':10000}),
            lambda r,i: frames(r)[-1].update({'accountingValid':False}),
            lambda r,i: frames(r)[-1].update({'scene':[480,272]}),
        ]
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                rows,identity=fixture(); mutation(rows,identity); result=analyze(rows,identity)
                self.assertFalse(result['validComparison'])
                self.assertFalse(any(c['passesScreen'] for c in result['comparisons']))
    def test_baseline_drift(self):
        rows,identity=fixture()
        for r in rows:
            if r['type']=='frame' and r['case']==3:
                r['intervalUs']*=2; r['endUs']*=2
        result=analyze(rows,identity)
        self.assertFalse(result['validComparison'])
        self.assertTrue(any('drift' in e for e in result['errors']))
    def test_small_gain_rejected(self):
        rows,identity=fixture()
        for r in rows:
            if r['type']=='frame' and r['case'] in (2,4):
                r['intervalUs']=1480000; r['endUs']=r['n']*1480000
        result=analyze(rows,identity)
        self.assertTrue(result['validComparison'])
        self.assertFalse(any(c['passesScreen'] for c in result['comparisons']))
if __name__=='__main__': unittest.main()
