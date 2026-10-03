import unittest
from analyze_material_cost import analyze_material_cost
from analyze_render_passes import PASSES

class MaterialCostTests(unittest.TestCase):
    def fixture(self):
        rows=[dict(type='recording-health',dropped=0,ioFailed=False),dict(name='cpu-diagnostic-begin',value0=8,generation=1,startUs=100)]
        for phase in range(1,8):
            for frame in range(6):
                seq=phase*100+frame;variant={2:1,4:2,6:3}.get(phase,0)
                def add(n,a,b=0):rows.append(dict(name=n,value0=a,value1=b,generation=1,sequence=seq,startUs=seq*100))
                for p in PASSES:add('render-pass-'+p,100);add('render-pass-'+p+'-geometry',10,200)
                for n,a,b in [('frame',phase,127),('kind',5,0),('total-us',10000 if not variant else 8000,0),('swap-us',7000,6900),('viewports',7,0),('geometry',70,1400),('prop-shader',0,20),('opaque-world',30,30),('opaque-props',20,20),('opaque-invalid',0,0),('material-mode',1<<variant,31),('material-world',30 if variant else 0,30),('material-props',20 if variant else 0,20),('material-features',30,20),('material-lightmaps',20,20),('material-reflections',10,0)]:add('render-probe-'+n,a,b)
        rows.extend([dict(name='cpu-diagnostic-end',value0=8,generation=1,startUs=140000100),dict(name='cpu-diagnostic-capture-complete',value0=8,value1=0,generation=1,startUs=140001000)])
        return rows
    def test_complete_and_features(self):
        result=analyze_material_cost(self.fixture())[0]
        self.assertTrue(result['captureComplete']);self.assertFalse(result['qualityPreservingImprovement'])
        self.assertEqual(result['comparisons'][0]['reductionUs'],2000)
        self.assertEqual(result['phases'][5]['meanDiagnosticDraws'],{'world':30,'props':20})
        rows=self.fixture()
        for v in rows:
            if v.get('name')=='render-probe-material-features':v['value0']=0
        self.assertFalse(analyze_material_cost(rows)[0]['comparisons'][0]['featureObserved'])
    def test_incomplete_and_wrong_dispatch(self):
        for name,field,value in [('render-probe-material-mode','value0',4),('render-probe-material-mode','value1',15),('render-probe-material-world','value0',1),('render-probe-opaque-invalid','value0',1),('render-probe-viewports','value0',5),('render-probe-prop-shader','value0',1),('render-pass-world','value1',10)]:
            rows=self.fixture();next(v for v in rows if v.get('name')==name)[field]=value
            self.assertFalse(analyze_material_cost(rows)[0]['captureComplete'],name)
        for name in ['render-probe-material-features','render-probe-material-mode','cpu-diagnostic-capture-complete']:
            self.assertFalse(analyze_material_cost([v for v in self.fixture() if v.get('name')!=name])[0]['captureComplete'])
    def test_loss_mixed_test_and_short_run(self):
        rows=self.fixture();rows[0]['dropped']=1;self.assertFalse(analyze_material_cost(rows)[0]['captureComplete'])
        rows=self.fixture();rows.append(dict(name='render-probe-depth-order',value0=1,generation=1,sequence=200,startUs=20000));self.assertFalse(analyze_material_cost(rows)[0]['captureComplete'])
        rows=self.fixture();next(v for v in rows if v.get('name')=='cpu-diagnostic-end')['startUs']=60000100;self.assertFalse(analyze_material_cost(rows)[0]['captureComplete'])
        self.assertFalse(analyze_material_cost(self.fixture(),['truncated'])[0]['captureComplete'])

if __name__=='__main__':unittest.main()
