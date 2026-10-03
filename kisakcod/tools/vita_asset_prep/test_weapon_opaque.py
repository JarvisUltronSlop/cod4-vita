import unittest
import test_color_fast
from analyze_render_passes import analyze_render_passes

class WeaponOpaqueTests(unittest.TestCase):
    def fixture(self):
        rows=test_color_fast.FastColorTests().fixture()
        for row in rows:
            if row.get('name','').startswith('cpu-diagnostic-'):row['value0']=16
            if row.get('name')=='render-probe-kind':row['value0']=13
            if row.get('name') in ('render-probe-fast-color','render-probe-fast-clear'):row['value0']=row['value1']
        for row in list(rows):
            if row.get('name')=='render-probe-frame':
                for name,a,b in [('ready',1,1),('opaque',1 if row['value0']==2 else 0,1),('maps',1,1),('invalid',0,0)]:
                    rows.append({**row,'name':'render-probe-weapon-'+name,'value0':a,'value1':b})
        return rows

    def test_candidate_with_retained_fast_baseline(self):
        result=analyze_render_passes(self.fixture())[0]
        self.assertTrue(result['captureComplete'])
        self.assertTrue(result['weaponOpaqueComparison']['candidateActuallyUsed'])
        self.assertIsNone(result['fastColorClearComparison'])
        self.assertIsNone(result['materialFamilyComparison'])
        self.assertEqual(result['phases'][1]['weaponOpaqueDraws'],4)
        self.assertEqual(result['phases'][0]['fastColorClearDraws'],dict(color=80,clear=8))

    def test_missing_wrong_or_lost_evidence_rejected(self):
        for name in ('ready','opaque','invalid','maps'):
            rows=[v for v in self.fixture() if v.get('name')!='render-probe-weapon-'+name]
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        for name,field,value in [('weapon-ready','value0',0),('weapon-ready','value1',0),('weapon-invalid','value0',1),('weapon-opaque','value0',1),('weapon-opaque','value1',0),('weapon-maps','value0',2),('fast-color','value0',0),('family-baked','value0',0)]:
            rows=self.fixture();next(v for v in rows if v.get('name')=='render-probe-'+name)[field]=value
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'],name)
        rows=self.fixture()
        for v in rows:
            if v.get('name')=='render-probe-weapon-maps':v['value1']=0
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        rows=self.fixture();rows[0]['dropped']=1;self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        self.assertFalse(analyze_render_passes(self.fixture(),['truncated'])[0]['captureComplete'])

if __name__=='__main__':unittest.main()
