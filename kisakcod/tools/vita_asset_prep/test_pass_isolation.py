import unittest
import struct
import test_world_math
from analyze_render_passes import analyze_render_passes

class PassIsolationTests(unittest.TestCase):
    def fixture(self):
        rows=[r for r in test_world_math.WorldMathTests().fixture() if not r.get('name','').startswith('render-probe-world-math')]
        phases={r['sequence']:r['value0'] for r in rows if r.get('name')=='render-probe-frame'}
        for r in list(rows):
            if phases.get(r.get('sequence'))==3:
                for phase in range(4,10):
                    e={**r,'sequence':r['sequence']+(phase-3)*100,'startUs':r['startUs']+(phase-3)*20000000}
                    if e.get('name')=='render-probe-frame':e['value0']=phase
                    rows.append(e)
        phases={r['sequence']:r['value0'] for r in rows if r.get('name')=='render-probe-frame'}
        for r in rows:
            name=r.get('name','')
            if name.startswith('cpu-diagnostic-'):
                r['value0']=35
                if name!='cpu-diagnostic-begin':r['startUs']+=120000000
            if name=='render-probe-kind':r['value0']=32
            if name=='render-pass-composition-geometry':r['value0']=2;r['value1']=3
            if name=='render-pass-post-geometry':r['value0']=1;r['value1']=2
        bits=lambda n:struct.unpack('<I',struct.pack('<f',n))[0]
        geometry={(r.get('sequence'),r.get('name')):r.get('value0') for r in rows}
        for r in list(rows):
            if r.get('name')!='render-probe-frame':continue
            phase=r['value0'];seq=r['sequence'];at=r['startUs']
            def add(name,a,b):rows.append({**r,'name':name,'value0':a,'value1':b})
            add('render-isolation-health',0,phase);add('render-isolation-post',int(phase==8),0)
            add('render-probe-composition-copy',1,0);add('render-probe-composition-valid',0,960*544)
            for name in ('before','during','after'):add('render-composition-depth-'+name,bits(1/64),bits(1))
            add('render-composition-depth-state',15,0x0203)
            for i,p in enumerate(('setup','world','props-fx','composition','weapon','post','hud')):
                count=geometry[seq,'render-pass-'+p+'-geometry'];add('render-isolation-draws-'+str(i),count,count-(1 if phase==2 else 0) if i=={2:1,4:2,6:4}.get(phase) else 0)
            add('graphics-profile-frame',phase,1);add('graphics-profile-scenes',3,phase);add('graphics-profile-health',0,1);add('display-probe-health',1,0)
            for i in range(3):
                pre='graphics-scene-'+str(i)+'-';width,height=(720,408) if i==0 else (960,544)
                add(pre+'begin-cpu',at+i*100,at+i*100+1);add(pre+'cpu-time',at+i*100+50,at+i*100+51)
                add(pre+'state',[3,5,7][i]<<32,(width<<32)|height);add(pre+'result',0,phase)
                add(pre+'format',0,width);add(pre+'load-store',0,0)
        return rows
    def test_all_four_comparisons(self):
        a=analyze_render_passes(self.fixture())[0]
        self.assertTrue(a['captureComplete'],a['invalidFrames'])
        self.assertEqual([v['phases'] for v in a['passIsolationComparisons']],[[1,2,3],[3,4,5],[5,6,7],[7,8,9]])
        self.assertEqual([v['component'] for v in a['passIsolationComparisons']],['world','props-fx','weapon','post'])
    def test_failed_coverage_or_state(self):
        for name,field in [('render-isolation-draws-0','value1'),('render-isolation-post','value0'),('graphics-profile-scenes','value0'),
                ('graphics-scene-1-result','value0'),('graphics-scene-0-state','value1'),('render-composition-depth-before','value0'),
                ('render-composition-depth-after','value0'),('render-composition-depth-state','value0'),('render-probe-world-alpha','value0')]:
            rows=self.fixture();next(r for r in rows if r.get('name')==name)[field]+=1
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'],name)
        rows=[r for r in self.fixture() if r.get('name')!='render-composition-depth-during'];self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
    def test_world_clear_is_required_not_unchecked_leakage(self):
        for delta in (-1,1):
            rows=self.fixture();phases={r['sequence']:r['value0'] for r in rows if r.get('name')=='render-probe-frame'}
            next(r for r in rows if r.get('name')=='render-isolation-draws-1' and phases[r['sequence']]==2)['value1']+=delta
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        rows=self.fixture();next(r for r in rows if r.get('name')=='render-probe-fast-clear')['value0']=1
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
    def test_reject_missing_last_window_and_razor_calls(self):
        rows=self.fixture();seq={r['sequence'] for r in rows if r.get('name')=='render-probe-frame' and r['value0']==9}
        self.assertFalse(analyze_render_passes([r for r in rows if r.get('sequence') not in seq])[0]['captureComplete'])
        rows=self.fixture();rows.append(dict(name='graphics-counter-error',generation=5,startUs=10000000,value0=1,value1=0))
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
if __name__=='__main__':unittest.main()
