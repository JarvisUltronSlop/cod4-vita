import unittest
import test_world_components
from analyze_render_passes import analyze_render_passes
from analyze_gpu_resources import analyze_resources

class SpecularSplitTests(test_world_components.WorldComponentTests):
    def fixture(self):
        rows=super().fixture()
        for r in rows:
            if r.get('name','').startswith('cpu-diagnostic-'):r['value0']=32
            if r.get('name')=='render-probe-kind':r['value0']=29
        return rows
    def resources(self):
        rows=super().resources()
        for r in rows:
            r['name']=r['name'].replace('gpu-audit-p9-','gpu-audit-p11-').replace('gpu-audit-p10-','gpu-audit-p12-')
        rows.append(dict(name='world-specular-split-ready',generation=0,value0=3,value1=3))
        return rows
    def test_wrong_dispatch_and_binding(self):
        for suffix in ('','-valid','-target'):
            self.assertFalse(analyze_render_passes([r for r in self.fixture() if r.get('name')!='render-probe-world-components'+suffix])[0]['captureComplete'])
        for name in ['gpu-audit-s588-p4-component','gpu-audit-s588-p4-program','gpu-audit-s588-p5-u2-filter','gpu-audit-p12-v-code-hash0']:
            rows=self.resources();next(r for r in rows if r['name']==name)['value0']+=1
            self.assertFalse(analyze_resources(rows)['complete'],name)
        for number in (11,12):
            self.assertFalse(analyze_resources([r for r in self.resources() if not r['name'].startswith(f'gpu-audit-p{number}-')])['complete'])
    def test_historical_names_and_schema(self):
        old=test_world_components.WorldComponentTests()
        for rows,names in [(old.fixture(),['secondary-lightmap','specular-reflection']),(self.fixture(),['highlight','reflection'])]:
            self.assertEqual([c['component'] for c in analyze_render_passes(rows)[0]['worldComponentComparisons']],names)
        self.assertIn('decal-highlight-off',analyze_resources(self.resources())['programs'])
        self.assertIn('decal-secondary-off',analyze_resources(old.resources())['programs'])
        for wrong in (28,30):
            rows=self.fixture();next(r for r in rows if r.get('name')=='render-probe-kind')['value0']=wrong
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])

if __name__=='__main__':unittest.main()
