import unittest
import test_color_fast
from analyze_render_passes import analyze_render_passes


class HudPipelineTests(unittest.TestCase):
    def fixture(self):
        rows=test_color_fast.FastColorTests().fixture()
        for row in rows:
            if row.get('name','').startswith('cpu-diagnostic-'):row['value0']=17
            if row.get('name')=='render-probe-kind':row['value0']=14
            if row.get('name') in ('render-probe-fast-color','render-probe-fast-clear'):row['value0']=row['value1']
        for row in list(rows):
            if row.get('name')=='render-probe-frame':
                geometry=next(v for v in rows if v.get('name')=='render-pass-hud-geometry' and v['sequence']==row['sequence'])
                draws=geometry['value0']
                for name,a,b in [('draws',draws if row['value0']==2 else 0,draws),('valid',draws,0),('viewport',960<<32|544,0)]:
                    rows.append({**row,'name':'render-probe-hud-'+name,'value0':a,'value1':b})
                for name,a,b in [('errors',0,0),('quads',40,5),('packet',20,0)]:
                    rows.append({**row,'name':'source-ui-'+name,'value0':a,'value1':b})
        return rows

    def test_actual_shader_and_all_baselines(self):
        result=analyze_render_passes(self.fixture())[0]
        self.assertTrue(result['captureComplete'],result['invalidFrames'])
        self.assertTrue(result['hudPipelineComparison']['candidateActuallyUsed'])
        self.assertIsNone(result['fastColorClearComparison'])
        self.assertIsNone(result['weaponOpaqueComparison'])
        self.assertIsNone(result['materialFamilyComparison'])
        self.assertEqual(result['phases'][0]['hudShaderDraws'],0)
        self.assertGreater(result['phases'][1]['hudShaderDraws'],0)
        self.assertEqual(result['phases'][1]['hudShaderDraws'],result['phases'][1]['hudEligibleDraws'])

    def test_unseen_incomplete_wrong_viewport_and_program_rejected(self):
        for name in ['draws','valid','viewport']:
            rows=[r for r in self.fixture() if r.get('name')!='render-probe-hud-'+name]
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        for name,field,value in [('draws','value0',1),('draws','value1',0),('valid','value0',0),('valid','value1',1),('viewport','value0',720<<32|408),('viewport','value1',5)]:
            rows=self.fixture();next(r for r in rows if r.get('name')=='render-probe-hud-'+name)[field]=value
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'],(name,field,value))
        rows=self.fixture();rows[0]['dropped']=1
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        self.assertFalse(analyze_render_passes(self.fixture(),['truncated'])[0]['captureComplete'])

    def test_missing_source_hud_is_not_an_optimization(self):
        for name in ['errors','quads','packet']:
            rows=[r for r in self.fixture() if r.get('name')!='source-ui-'+name]
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        for name,field,value in [('errors','value0',1),('errors','value1',1),('packet','value1',1)]:
            rows=self.fixture();next(r for r in rows if r.get('name')=='source-ui-'+name)[field]=value
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        rows=self.fixture()
        for row in rows:
            if row.get('name')=='source-ui-quads':row['value0']=row['value1']=0
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])


if __name__=='__main__':unittest.main()
