import unittest
import test_world_math
import test_gpu_resources
from analyze_render_passes import analyze_render_passes
from analyze_gpu_resources import analyze_resources

class WorldComponentTests(unittest.TestCase):
    def fixture(self):
        rows=test_world_math.WorldMathTests().fixture()
        phase_of={r['sequence']:r['value0'] for r in rows if r.get('name')=='render-probe-frame'}
        for r in list(rows):
            phase=phase_of.get(r.get('sequence'))
            if phase in (2,3):
                extra={**r,'sequence':r['sequence']+200,'startUs':r['startUs']+40000000}
                if extra.get('name')=='render-probe-frame':extra['value0']=phase+2
                rows.append(extra)
        for r in rows:
            name=r.get('name','')
            if name.startswith('cpu-diagnostic-'):
                r['value0']=31
                if name!='cpu-diagnostic-begin':r['startUs']+=40000000
            if name=='render-probe-kind':r['value0']=28
        phase_of={r['sequence']:r['value0'] for r in rows if r.get('name')=='render-probe-frame'}
        for r in rows:
            name=r.get('name','');phase=phase_of.get(r.get('sequence'));variant=1 if phase==2 else 2 if phase==4 else 0
            if name.startswith('render-probe-world-math'):
                r['name']=name.replace('world-math','world-components').replace('-targets','-target')
                if name.endswith('-valid'):r['value0']=0;r['value1']=3
                elif name.endswith('-targets'):r['value0']=4;r['value1']=variant
                else:r['value0']=int(bool(variant));r['value1']=1
        return rows
    def resources(self):
        rows=[r for r in test_gpu_resources.ResourceAuditTests().fixture() if not r['name'].startswith('gpu-audit-p2-')]
        for r in list(rows):
            if r['name'].startswith('gpu-audit-p3-'):
                for number in (9,10):
                    new={**r,'name':r['name'].replace('p3-',f'p{number}-')}
                    if r['name'].endswith('-program'):new['value0']=number+10
                    if r['name'].endswith('-f-instructions'):new['value0']-=20
                    rows.append(new)
            if r['name'].startswith('gpu-audit-s'):
                for phase in (2,3):
                    if f'-p{phase}-' in r['name']:rows.append({**r,'name':r['name'].replace(f'-p{phase}-',f'-p{phase+2}-')})
        for r in list(rows):
            name=r['name']
            if name.startswith('gpu-audit-s') and name.endswith('-program'):
                target='-s588-' in name
                variant=(1 if '-p2-' in name else 2 if '-p4-' in name else 0) if target else 0
                r['value0']=r['value1']=(19 if variant==1 else 20 if variant==2 else 13) if target else 11
                rows.append({**r,'name':name.replace('-program','-component'),'value0':variant,'value1':int(target)})
        rows.append(dict(name='render-probe-world-components',generation=1,value0=1,value1=1))
        rows.append(dict(name='world-basis-ready',generation=0,value0=3,value1=1))
        return rows
    def test_five_phase_complete(self):
        a=analyze_render_passes(self.fixture())[0];self.assertTrue(a['captureComplete'],a['invalidFrames'])
        self.assertEqual(len(a['phases']),5)
        self.assertEqual([x['phases'] for x in a['worldComponentComparisons']],[[1,2,3],[3,4,5]])
        self.assertEqual([x['worldComponents']['variant'] for x in a['phases']],[0,1,0,2,0])
        b=analyze_resources(self.resources());self.assertTrue(b['complete'],b['problems']);self.assertEqual(len(b['captures']),15)
    def test_incomplete_second_comparison(self):
        for phase in (4,5):
            rows=self.fixture();sequences={r['sequence'] for r in rows if r.get('name')=='render-probe-frame' and r['value0']==phase}
            self.assertFalse(analyze_render_passes([r for r in rows if r.get('sequence') not in sequences])[0]['captureComplete'])
        rows=self.fixture();next(r for r in rows if r.get('name')=='cpu-diagnostic-end')['startUs']-=40000000
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
    def test_wrong_dispatch_and_binding(self):
        for suffix in ('','-valid','-target'):
            self.assertFalse(analyze_render_passes([r for r in self.fixture() if r.get('name')!='render-probe-world-components'+suffix])[0]['captureComplete'])
        for name,field in [('render-probe-world-components','value0'),('render-probe-world-components-valid','value0'),('render-probe-world-components-target','value1')]:
            rows=self.fixture();next(r for r in rows if r.get('name')==name)[field]+=1
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        for name in ['gpu-audit-s588-p4-component','gpu-audit-s588-p4-program','gpu-audit-s588-p5-u2-filter','gpu-audit-p10-v-code-hash0']:
            rows=self.resources();next(r for r in rows if r['name']==name)['value0']+=1
            self.assertFalse(analyze_resources(rows)['complete'],name)
        self.assertFalse(analyze_resources([r for r in self.resources() if not r['name'].startswith('gpu-audit-p10-')])['complete'])
if __name__=='__main__':unittest.main()
