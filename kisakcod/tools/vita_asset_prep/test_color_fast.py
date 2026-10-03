import unittest
import test_render_passes
from analyze_render_passes import analyze_render_passes

class FastColorTests(unittest.TestCase):
    def fixture(self):
        rows=test_render_passes.RenderPassTests().family_fixture()
        for row in rows:
            if row.get('name','').startswith('cpu-diagnostic-'):row['value0']=15
            if row.get('name')=='render-probe-kind':row['value0']=12
            if row.get('name') in ('render-probe-family-lightmapped','render-probe-family-baked'):row['value0']=row['value1']
        for row in list(rows):
            if row.get('name')=='render-probe-frame':
                for name,a,b in [('ready',1,1),('color',20 if row['value0']==2 else 0,20),('clear',2 if row['value0']==2 else 0,2),('invalid',0,0)]:
                    rows.append({**row,'name':'render-probe-fast-'+name,'value0':a,'value1':b})
        return rows

    def test_complete_with_both_actual_programs_and_same_family_baselines(self):
        result=analyze_render_passes(self.fixture())[0]
        self.assertTrue(result['captureComplete'])
        self.assertTrue(result['fastColorClearComparison']['candidateActuallyUsed'])
        self.assertIsNone(result['materialFamilyComparison'])
        self.assertIsNone(result['drawWaitBreakdown'])
        self.assertEqual(result['phases'][1]['fastColorClearDraws'],dict(color=80,clear=8))

    def test_missing_wrong_unavailable_and_lost_evidence_rejected(self):
        for name in ('ready','color','clear','invalid'):
            rows=[r for r in self.fixture() if r.get('name')!='render-probe-fast-'+name]
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        for name,field,value in [('ready','value0',0),('ready','value1',0),('invalid','value0',1),('color','value1',0),('clear','value1',1),('color','value0',1)]:
            rows=self.fixture();next(r for r in rows if r.get('name')=='render-probe-fast-'+name)[field]=value
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'],name)
        rows=self.fixture();rows[0]['dropped']=1
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        self.assertFalse(analyze_render_passes(self.fixture(),['truncated'])[0]['captureComplete'])

if __name__=='__main__':unittest.main()
