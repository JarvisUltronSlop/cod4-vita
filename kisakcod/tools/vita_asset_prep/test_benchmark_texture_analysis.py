import copy
import unittest
from analyze_benchmark import analyze
from benchmark_texture_analysis import NAMES, MODES
from test_benchmark_geometry_analysis import geometry_fixture

def fixture(mips=True):
    old,identity=geometry_fixture()
    rows=[r for r in old if r['type'] in ('run','identity')]
    rows[0]['round']=identity['round']='textures-7'
    template=next(r for r in old if r['type']=='frame' and r['case']==0)
    for i,mode in enumerate(MODES):
        rows.append(dict(type='case',index=i,id=NAMES[i],scene=[960,544],shaderMode=0,geometryMode=0,audioMode=0,textureMode=mode))
        reductions=[False,bool(mode&1) and mips,bool(mode&2) and mips]
        source=20480 if mips else 16384
        selected=[source-16384 if r else source for r in reductions]
        for role in range(3):
            reduced=reductions[role]
            rows.append(dict(type='texture-upload',case=i,mode=mode,nameHash=100+role,role=role,
                source=[64,64,2 if mips else 1],format=21,flags=0,sourceBytes=source,
                selected=[32,32,1] if reduced else [64,64,2 if mips else 1],
                offset=16384 if reduced else 0,selectedBytes=selected[role],valid=True,reduced=reduced,success=True))
        for n in range(1,61):
            f=copy.deepcopy(template); interval=1000000 if mode else 1500000
            f.update(case=i,n=n,endUs=n*interval,intervalUs=interval,audioMode=0,audioErrors=0)
            rows.append(f)
            rows.append(dict(type='texture-frame',case=i,n=n,mode=mode,failures=0,attempts=[1,1,1],
                reduced=[int(r) for r in reductions],sourceBytes=[source]*3,selectedBytes=selected))
        rows.append(dict(type='result',case=i,samples=60,complete=True))
    rows.append(dict(type='end',allSegmentsWritten=True))
    return rows,identity

class TextureAnalysisTests(unittest.TestCase):
    def test_all_three_combinations(self):
        rows,id=fixture(); r=analyze(rows,id)
        self.assertTrue(r['validComparison'],r['errors'])
        self.assertEqual(len(r['comparisons']),3)
        self.assertTrue(all(c['passesScreen'] for c in r['comparisons']))

    def test_protected_or_wrong_mip_and_failed_upload_rejected(self):
        for key,value in [('role',0),('selected',[16,16,1]),('offset',0),('mode',0),('valid',False),
                          ('success',False),('selectedBytes',10),('sourceBytes',10),('format',123),('reduced',False)]:
            rows,id=fixture()
            next(r for r in rows if r['type']=='texture-upload' and r['case']==1 and r['role']==1)[key]=value
            self.assertFalse(analyze(rows,id)['validComparison'],key)

    def test_missing_extra_changing_uploads_rejected(self):
        for change in ('missing','extra','changed'):
            rows,id=fixture()
            target=next(r for r in rows if r['type']=='texture-upload' and r['case']==1)
            if change=='missing': rows.remove(target)
            elif change=='extra': rows.append(copy.deepcopy(target))
            else: target['nameHash']+=1
            self.assertFalse(analyze(rows,id)['validComparison'],change)

    def test_no_mips_not_exercised(self):
        rows,id=fixture(False); r=analyze(rows,id)
        self.assertTrue(r['validComparison'],r['errors'])
        self.assertTrue(all(c['featureActivity']=='not-exercised' and not c['passesScreen'] for c in r['comparisons']))

    def test_main_frame_other_factors_and_missing_totals(self):
        for key,value in [('audioMode',1),('geometryMode',2),('staticDrawFailures',1)]:
            rows,id=fixture(); next(r for r in rows if r['type']=='frame')[key]=value
            self.assertFalse(analyze(rows,id)['validComparison'],key)
        rows,id=fixture(); rows.remove(next(r for r in rows if r['type']=='texture-frame'))
        self.assertFalse(analyze(rows,id)['validComparison'])

if __name__=='__main__': unittest.main()
