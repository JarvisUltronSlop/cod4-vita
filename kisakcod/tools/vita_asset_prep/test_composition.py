import unittest
import test_world_math
from analyze_render_passes import analyze_render_passes

class CompositionTests(unittest.TestCase):
    def fixture(self):
        rows=[r for r in test_world_math.WorldMathTests().fixture() if not r.get('name','').startswith('render-probe-world-math')]
        phases={r['sequence']:r['value0'] for r in rows if r.get('name')=='render-probe-frame'}
        for r in rows:
            name=r.get('name','');phase=phases.get(r.get('sequence'))
            if name.startswith('cpu-diagnostic-'):r['value0']=34
            if name=='render-probe-kind':r['value0']=31
            if name=='render-probe-fast-clear' and phase==2:r['value0']=r['value1']=1
            if name=='render-pass-composition-geometry':r['value0']=1 if phase==2 else 2;r['value1']=2 if phase==2 else 3
        for r in list(rows):
            if r.get('name')=='render-probe-frame':
                rows += [{**r,'name':'render-probe-composition-copy','value0':1,'value1':int(r['value0']==2)},
                         {**r,'name':'render-probe-composition-valid','value0':0,'value1':960*544}]
        return rows
    def test_complete(self):
        a=analyze_render_passes(self.fixture())[0]
        self.assertTrue(a['captureComplete'],a['invalidFrames'])
        self.assertEqual([p['composition']['redundantClearPixelsRemovedPerFrame'] for p in a['phases']],[0,522240,0])
    def test_reject_missing_wrong_depth_draws_or_shader(self):
        for name,field in [('render-probe-composition-copy','value1'),('render-probe-composition-valid','value0'),
                ('render-pass-composition-geometry','value0'),('render-probe-fast-clear','value1'),('render-probe-world-alpha','value0')]:
            rows=self.fixture();next(r for r in rows if r.get('name')==name)[field]+=1
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'],name)
        rows=[r for r in self.fixture() if r.get('name')!='render-probe-composition-valid']
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])

if __name__=='__main__':unittest.main()
