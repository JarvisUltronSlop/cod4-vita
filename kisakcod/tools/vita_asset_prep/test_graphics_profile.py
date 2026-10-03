import unittest
import test_world_components
from analyze_render_passes import analyze_render_passes
from analyze_graphics_profile import analyze_graphics_profile
class GraphicsProfileTests(unittest.TestCase):
    def fixture(self):
        rows=test_world_components.WorldComponentTests().fixture();phases={r['sequence']:r['value0'] for r in rows if r.get('name')=='render-probe-frame'}
        for r in list(rows):
            if phases.get(r.get('sequence'))==5:
                e={**r,'sequence':r['sequence']+100,'startUs':r['startUs']+20000000}
                if e.get('name')=='render-probe-frame':e['value0']=6
                rows.append(e)
        for r in rows:
            if r.get('name','').startswith('cpu-diagnostic-'):
                r['value0']=33
                if r['name']!='cpu-diagnostic-begin':r['startUs']+=20000000
            if r.get('name')=='render-probe-kind':r['value0']=30
        rows=[r for r in rows if not r.get('name','').startswith('render-probe-world-components')]
        for r in list(rows):
            if r.get('name')=='render-probe-frame':
                for n,a,b in [('graphics-profile-frame',r['value0'],3),('graphics-profile-health',1,3),('graphics-profile-scenes',1,r['value0']),('graphics-profile-observer-us',50,0),('graphics-scene-0-cpu-time',r['startUs'],r['startUs']+10),('graphics-scene-0-state',0,0),('graphics-scene-0-result',0,r['value0'])]:rows.append({**r,'name':n,'value0':a,'value1':b})
        rows.append(dict(name='graphics-counter-error',generation=5,value0=1,value1=0x80000001))
        return rows
    def test_fallback_and_six_windows(self):
        rows=self.fixture();t=analyze_render_passes(rows);self.assertTrue(t[0]['captureComplete'],t[0]['invalidFrames'])
        p=analyze_graphics_profile(rows,t)[0];self.assertTrue(p['timelineComplete'],p['timelineProblems']);self.assertEqual(p['counterAvailability'],'unavailable');self.assertFalse(p['counterEvidenceComplete']);self.assertEqual(len(p['phases']),6)
    def test_missing_timeline_or_window(self):
        rows=[r for r in self.fixture() if r.get('name')!='graphics-profile-scenes'];p=analyze_graphics_profile(rows,analyze_render_passes(rows))[0];self.assertFalse(p['timelineComplete'])
        rows=self.fixture();seq={r['sequence'] for r in rows if r.get('name')=='render-probe-frame' and r['value0']==6};rows=[r for r in rows if r.get('sequence') not in seq];self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
    def test_valid_metrics_and_corrupt_batches(self):
        rows=[r for r in self.fixture() if r.get('name')!='graphics-counter-error']
        def add(n,a,b):rows.append(dict(name=n,generation=5,sequence=1000,value0=a,value1=b))
        for group in range(4):
            add('graphics-counter-batch',group+1,group);add('graphics-counter-buffer',1,0)
            if group==0:
                add('graphics-parameter-0',1024,1<<32);add('graphics-counter-jobs',0,0)
            else:
                add('graphics-counter-jobs',1,0);add('graphics-job-0-id',123<<32,(group<<32)|((group*2)<<16))
                add('graphics-job-0-time',100,200);add('graphics-job-0-values',0,0)
            add('graphics-counter-raw-truncated',0,0)
        p=analyze_graphics_profile(rows,analyze_render_passes(rows))[0];self.assertTrue(p['counterEvidenceComplete'],p['counterProblems']);self.assertEqual(p['groups'][0]['parameterPeakBytes'],1024)
        next(r for r in rows if r.get('name')=='graphics-counter-raw-truncated')['value0']=1
        self.assertFalse(analyze_graphics_profile(rows,analyze_render_passes(rows))[0]['counterEvidenceComplete'])
    def test_missing_counter_data_is_unverified(self):
        rows=[r for r in self.fixture() if r.get('name')!='graphics-counter-error'];p=analyze_graphics_profile(rows,analyze_render_passes(rows))[0];self.assertEqual(p['counterAvailability'],'unverified');self.assertFalse(p['counterEvidenceComplete'])
if __name__=='__main__':unittest.main()
