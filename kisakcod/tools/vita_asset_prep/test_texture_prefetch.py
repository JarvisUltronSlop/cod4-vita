import unittest
import test_render_passes
from analyze_render_passes import analyze_render_passes


class TexturePrefetchTests(unittest.TestCase):
    def fixture(self):
        rows=test_render_passes.RenderPassTests().family_fixture()
        for row in rows:
            if row.get('name','').startswith('cpu-diagnostic-'):row['value0']=13
            if row.get('name')=='render-probe-kind':row['value0']=10
            if row.get('name') in ('render-probe-family-lightmapped','render-probe-family-baked'):row['value0']=row['value1']
        for row in list(rows):
            if row.get('name')=='render-probe-frame':
                for name,a,b in [('ready',3,3),('lightmapped',20 if row['value0']==2 else 0,20),('baked',10 if row['value0']==2 else 0,10),('invalid',0,0)]:
                    rows.append({**row,'name':'render-probe-prefetch-'+name,'value0':a,'value1':b})
        return rows

    def test_prefetch_requires_same_family_baseline_and_actual_candidate(self):
        result=analyze_render_passes(self.fixture())[0]
        self.assertTrue(result['captureComplete']);self.assertTrue(result['texturePrefetchComparison']['candidateActuallyUsed'])
        self.assertIsNone(result['materialFamilyComparison']);self.assertIsNone(result['displayQueueProbe'])
        self.assertEqual(result['phases'][0]['materialFamilyDraws'],{'lightmapped':80,'baked':40})
        self.assertEqual(result['phases'][1]['texturePrefetchDraws'],{'lightmapped':80,'baked':40})
        for name,field,value in [('prefetch-ready','value0',1),('prefetch-invalid','value0',1),('prefetch-lightmapped','value0',1),('prefetch-baked','value1',11),('family-lightmapped','value0',0)]:
            rows=self.fixture();next(v for v in rows if v.get('name')=='render-probe-'+name)[field]=value
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'],name)

    def test_prefetch_missing_unused_or_lost_evidence(self):
        for name in ('prefetch-ready','prefetch-lightmapped','prefetch-baked','prefetch-invalid','camera-z'):
            rows=[v for v in self.fixture() if v.get('name')!='render-probe-'+name]
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'],name)
        rows=self.fixture()
        for v in rows:
            if v.get('name')=='render-probe-prefetch-baked':v['value0']=0
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        rows=self.fixture();rows[0]['dropped']=1;self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        self.assertFalse(analyze_render_passes(self.fixture(),['truncated'])[0]['captureComplete'])

if __name__=='__main__':unittest.main()
