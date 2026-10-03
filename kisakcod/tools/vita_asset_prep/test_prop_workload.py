import unittest
from analyze_prop_workload import analyze_prop_workload
from analyze_render_passes import PASSES


class PropWorkloadTests(unittest.TestCase):
    def fixture(self):
        rows=[dict(type='recording-health',dropped=0,ioFailed=False),
              dict(name='cpu-diagnostic-begin',value0=5,generation=1,startUs=100)]
        for phase in range(1,6):
            for frame in range(6):
                seq=phase*100+frame
                def add(name,a,b=0):rows.append(dict(name=name,value0=a,value1=b,generation=1,sequence=seq,startUs=seq*100))
                for p in PASSES:add('render-pass-'+p,100)
                add('render-probe-frame',phase,127);add('render-probe-kind',2)
                add('render-probe-total-us',10000);add('render-probe-swap-us',9000,8900)
                add('render-probe-prop-attempts',100,20000)
                add('render-probe-prop-submitted',0 if phase==4 else 100,0 if phase==4 else 20000)
                add('render-probe-prop-clip',3,100 if phase==2 else 0)
                add('render-probe-props-group-us',700)
                add('render-probe-viewports',5 if phase==4 else 7)
                add('render-probe-prop-shader',0,100)
        rows.extend([dict(name='cpu-diagnostic-end',value0=5,generation=1,startUs=100000100),
                     dict(name='cpu-diagnostic-capture-complete',value0=5,value1=0,generation=1,startUs=100001000)])
        return rows
    def test_complete_and_missing(self):
        rows=self.fixture();result=analyze_prop_workload(rows)[0]
        self.assertTrue(result['captureComplete']);self.assertFalse(result['hardwareQualified'])
        self.assertEqual(result['phases'][3]['meanAttemptedPropDraws'],100)
        self.assertEqual(result['phases'][3]['meanSubmittedPropDraws'],0)
        self.assertFalse(analyze_prop_workload(rows,['truncated'])[0]['captureComplete'])
        for n in ['cpu-diagnostic-capture-complete','render-probe-prop-clip','render-probe-props-group-us']:
            self.assertFalse(analyze_prop_workload([r for r in rows if r.get('name')!=n])[0]['captureComplete'])
    def test_wrong_workload_and_leaked_state(self):
        for name,field,value,sequence in [('render-probe-prop-submitted','value0',100,400),
                ('render-probe-prop-clip','value1',0,200),('render-probe-prop-clip','value0',1,200),
                ('render-probe-prop-shader','value0',10,200),('render-pass-world','value1',100,200),
                ('render-probe-viewports','value0',7,400)]:
            rows=self.fixture();next(r for r in rows if r.get('name')==name and r.get('sequence')==sequence)[field]=value
            self.assertFalse(analyze_prop_workload(rows)[0]['captureComplete'],(name,field))
        rows=self.fixture();rows[0]['dropped']=1
        self.assertFalse(analyze_prop_workload(rows)[0]['captureComplete'])

if __name__=='__main__':unittest.main()
