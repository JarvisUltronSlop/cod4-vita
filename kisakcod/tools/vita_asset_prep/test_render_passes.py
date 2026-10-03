import unittest
import struct
from analyze_render_passes import analyze_render_passes, PASSES


class RenderPassTests(unittest.TestCase):
    def fixture(self):
        rows = [{'type': 'recording-health', 'dropped': 0, 'ioFailed': False},
                {'name': 'cpu-diagnostic-begin', 'generation': 5, 'value0': 3, 'startUs': 100}]
        for phase in (1, 2, 3):
            for frame in range(6):
                sequence = phase*100+frame
                def add(name, a, b):
                    rows.append(dict(name=name, value0=a, value1=b, generation=5, sequence=sequence, startUs=sequence*100))
                wait = 1000 if phase == 2 else 0
                for p in PASSES:
                    add('render-pass-'+p, 100, wait)
                    add('render-pass-'+p+'-geometry', 10, 100)
                add('render-probe-frame', phase, 127)
                add('render-probe-total-us', 10000, 7*wait)
                add('render-probe-swap-us', 1000, 900)
                add('render-probe-geometry', 70, 700)
                add('render-probe-viewports', 7, 0)
        rows += [dict(name='cpu-diagnostic-end', generation=5, value0=3, startUs=60000100),
                 dict(name='cpu-diagnostic-capture-complete', generation=5, value0=3, value1=0, startUs=60000200)]
        return rows

    def test_complete_and_missing_evidence(self):
        rows = self.fixture()
        result = analyze_render_passes(rows)[0]
        self.assertTrue(result['captureComplete'])
        self.assertFalse(result['hardwareQualified'])
        self.assertEqual(result['phases'][1]['passes']['world']['completionWaitElapsed']['meanUs'], 1000)
        self.assertEqual(result['phases'][0]['renderElapsed']['samples'], 4)
        self.assertFalse(analyze_render_passes(rows, ['truncated'])[0]['captureComplete'])
        for name in ('render-pass-world', 'render-probe-frame', 'cpu-diagnostic-capture-complete'):
            self.assertFalse(analyze_render_passes([r for r in rows if r.get('name') != name])[0]['captureComplete'])

    def test_invalid_baseline_and_loss(self):
        rows = self.fixture()
        rows[0]['dropped'] = 1
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        rows = self.fixture()
        next(r for r in rows if r.get('name') == 'render-pass-world')['value1'] = 100
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        rows = self.fixture()
        rows.append(next(r.copy() for r in rows if r.get('name') == 'render-pass-world'))
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])

    def test_missing_viewport_not_a_pass(self):
        rows = self.fixture()
        for r in rows:
            if r.get('name') == 'render-probe-viewports': r['value0'] = 3
        self.assertFalse(analyze_render_passes(rows)[0]['phases'][1]['viewportsAllObservedAndCorrect'])

    def comparison(self):
        rows = self.fixture()
        for r in rows:
            if r.get('name', '').startswith('cpu-diagnostic-'): r['value0'] = 4
            if r.get('name', '').startswith('render-pass-') and not r['name'].endswith('-geometry'): r['value1'] = 0
            if r.get('name') == 'render-probe-total-us': r['value1'] = 0
        for r in list(rows):
            if r.get('name') == 'render-probe-frame':
                rows += [{**r, 'name': 'render-probe-kind', 'value0': 1, 'value1': 0},
                         {**r, 'name': 'render-probe-prop-shader', 'value0': 10 if r['value0'] == 2 else 0, 'value1': 10}]
        return rows

    def test_comparison_requires_unfenced_actual_program_use(self):
        rows = self.comparison()
        result = analyze_render_passes(rows)[0]
        self.assertTrue(result['captureComplete'])
        self.assertTrue(result['propShaderComparison']['candidateActuallyUsed'])
        self.assertEqual(result['phases'][1]['specializedPropDraws'], 40)
        next(r for r in rows if r.get('name') == 'render-pass-world')['value1'] = 1
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        rows = self.comparison()
        for r in rows:
            if r.get('name') == 'render-probe-prop-shader': r['value0'] = 0
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        rows = self.comparison()
        next(r for r in rows if r.get('name') == 'render-probe-prop-shader')['value0'] = 10
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])

    def opaque_fixture(self):
        rows=self.comparison()
        for row in rows:
            if row.get('name','').startswith('cpu-diagnostic-'):row['value0']=6
            if row.get('name')=='render-probe-kind':row['value0']=3
            if row.get('name')=='render-probe-prop-shader':row['value0']=0
        for row in list(rows):
            if row.get('name')=='render-probe-frame':
                for name,count in [('world',20),('props',10)]:
                    rows.append({**row,'name':'render-probe-opaque-'+name,'value0':count if row['value0']==2 else 0,'value1':count})
                rows.append({**row,'name':'render-probe-opaque-invalid','value0':0,'value1':0})
        return rows

    def test_opaque_comparison(self):
        result=analyze_render_passes(self.opaque_fixture())[0]
        self.assertTrue(result['captureComplete'])
        self.assertTrue(result['opaqueShaderComparison']['candidateActuallyUsed'])
        self.assertEqual(result['phases'][1]['opaqueDraws'],{'world':80,'props':40})
        for name,value in [('render-probe-opaque-invalid',1),('render-probe-prop-shader',1),('render-probe-opaque-world',1),('render-probe-viewports',5)]:
            rows=self.opaque_fixture();next(v for v in rows if v.get('name')==name)['value0']=value
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        for missing in ['render-probe-opaque-world','render-probe-opaque-props','render-probe-opaque-invalid','cpu-diagnostic-capture-complete']:
            self.assertFalse(analyze_render_passes([v for v in self.opaque_fixture() if v.get('name')!=missing])[0]['captureComplete'])

    def test_opaque_unused_or_failed(self):
        rows=self.opaque_fixture()
        for v in rows:
            if v.get('name') in ('render-probe-opaque-world','render-probe-opaque-props'):v['value0']=v['value1']=0
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        rows=self.opaque_fixture();rows[0]['dropped']=1
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        rows=self.opaque_fixture();next(v for v in rows if v.get('name')=='render-pass-world')['value1']=1
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])

    def depth_fixture(self):
        rows=self.opaque_fixture()
        for row in rows:
            if row.get('name','').startswith('cpu-diagnostic-'):row['value0']=7
            if row.get('name')=='render-probe-kind':row['value0']=4
            if row.get('name') in ('render-probe-opaque-world','render-probe-opaque-props'):row['value0']=row['value1']
        for row in list(rows):
            if row.get('name')=='render-probe-frame':
                for name,a,b in [('order',4 if row['value0']==2 else 0,12 if row['value0']==2 else 0),('eligible',12,0),('membership',100,100),('sort-us',400,row['value0'])]:
                    rows.append({**row,'name':'render-probe-depth-'+name,'value0':a,'value1':b})
        return rows

    def test_depth_order_comparison(self):
        result=analyze_render_passes(self.depth_fixture())[0]
        self.assertTrue(result['captureComplete'])
        self.assertTrue(result['depthOrderComparison']['candidateActuallyUsed'])
        self.assertEqual(result['phases'][1]['depthOrderSwaps'],16)
        for name,field,value in [('render-probe-depth-order','value0',1),('render-probe-depth-membership','value1',101),('render-probe-depth-eligible','value1',1),('render-probe-depth-sort-us','value1',3),('render-probe-opaque-world','value0',0)]:
            rows=self.depth_fixture();next(v for v in rows if v.get('name')==name)[field]=value
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        rows=self.depth_fixture()
        for v in rows:
            if v.get('name')=='render-probe-depth-order':v['value0']=0
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        for missing in ('render-probe-depth-order','render-probe-depth-membership','render-probe-depth-sort-us'):
            self.assertFalse(analyze_render_passes([v for v in self.depth_fixture() if v.get('name')!=missing])[0]['captureComplete'])

    def specular_fixture(self):
        rows=self.opaque_fixture()
        for row in rows:
            if row.get('name','').startswith('cpu-diagnostic-'):row['value0']=9
            if row.get('name')=='render-probe-kind':row['value0']=6
            if row.get('name') in ('render-probe-opaque-world','render-probe-opaque-props'):row['value0']=row['value1']
        for row in list(rows):
            if row.get('name')=='render-probe-frame':
                for name,a,b in [('ready',3,3),('world',20 if row['value0']==2 else 0,20),('props',10 if row['value0']==2 else 0,10),('inputs',25,15)]:
                    rows.append({**row,'name':'render-probe-specular-'+name,'value0':a,'value1':b})
        return rows

    def test_specular_comparison_requires_real_program_and_inputs(self):
        report=analyze_render_passes(self.specular_fixture())[0]
        self.assertTrue(report['captureComplete'])
        self.assertTrue(report['specularMathComparison']['candidateActuallyUsed'])
        self.assertIsNone(report['opaqueShaderComparison'])
        self.assertEqual(report['phases'][1]['specularMathDraws'],{'world':80,'props':40})
        for name,field,value in [('ready','value0',1),('world','value0',1),('props','value1',1),('inputs','value0',31),('inputs','value1',26)]:
            rows=self.specular_fixture();next(v for v in rows if v.get('name')=='render-probe-specular-'+name)[field]=value
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        for missing in ('ready','world','props','inputs'):
            self.assertFalse(analyze_render_passes([v for v in self.specular_fixture() if v.get('name')!='render-probe-specular-'+missing])[0]['captureComplete'])
        rows=self.specular_fixture()
        for v in rows:
            if v.get('name')=='render-probe-specular-inputs':v['value0']=v['value1']=0
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        self.assertFalse(analyze_render_passes(self.specular_fixture(),['truncated'])[0]['captureComplete'])

    @staticmethod
    def bits(value):
        return struct.unpack('<I', struct.pack('<f', value))[0]

    def family_fixture(self):
        rows=self.opaque_fixture()
        for row in rows:
            if 'startUs' in row:row['startUs']+=3000000
            if row.get('name','').startswith('cpu-diagnostic-'):row['value0']=11
            if row.get('name')=='render-probe-kind':row['value0']=8
            if row.get('name') in ('render-probe-opaque-world','render-probe-opaque-props'):row['value0']=row['value1']
        for row in list(rows):
            if row.get('name')=='render-probe-frame':
                for name,a,b in [('family-ready',3,3),('family-lightmapped',20 if row['value0']==2 else 0,20),
                                 ('family-baked',10 if row['value0']==2 else 0,10),('family-invalid',0,0),
                                 ('camera-xy',self.bits(158),self.bits(735)),('camera-z',self.bits(250),1)]:
                    rows.append({**row,'name':'render-probe-'+name,'value0':a,'value1':b})
        for name,a,b,t in [('origin-xy',self.bits(158),self.bits(735),1),
                            ('origin-z-yaw',self.bits(196),self.bits(90),2),
                            ('angles',self.bits(0),self.bits(0),3),('source-ready',100,150,2000003)]:
            rows.append(dict(name='gpu-anchor-'+name,generation=5,value0=a,value1=b,startUs=t))
        return rows

    def test_material_family_dispatch_and_evidence(self):
        report=analyze_render_passes(self.family_fixture())[0]
        self.assertTrue(report['captureComplete'])
        self.assertTrue(report['materialFamilyComparison']['candidateActuallyUsed'])
        self.assertIsNone(report['opaqueShaderComparison'])
        self.assertEqual(report['diagnosticAnchor']['origin'],[158,735,196])
        self.assertEqual(report['phases'][1]['materialFamilyDraws'],{'lightmapped':80,'baked':40})
        for name,field,value in [('family-ready','value0',1),('family-invalid','value0',1),
                                 ('family-lightmapped','value0',1),('family-baked','value1',11),
                                 ('camera-z','value1',0),('camera-xy','value0',self.bits(170)),
                                 ('camera-z','value0',self.bits(400)),('camera-z','value0',self.bits(float('nan')))]:
            rows=self.family_fixture();next(v for v in rows if v.get('name')=='render-probe-'+name)[field]=value
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'],name)
        for name in ('family-lightmapped','family-baked'):
            rows=self.family_fixture()
            for v in rows:
                if v.get('name')=='render-probe-'+name:v['value0']=v['value1']=0
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        for name in ('family-ready','family-lightmapped','family-baked','family-invalid','camera-xy','camera-z'):
            rows=[r for r in self.family_fixture() if r.get('name')!='render-probe-'+name]
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        rows=self.family_fixture();rows[0]['dropped']=1
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        self.assertFalse(analyze_render_passes(self.family_fixture(),['truncated'])[0]['captureComplete'])

    def test_material_family_requires_settled_source_anchor(self):
        for name in ('origin-xy','origin-z-yaw','angles','source-ready'):
            rows=self.family_fixture()
            self.assertFalse(analyze_render_passes([r for r in rows if r.get('name')!='gpu-anchor-'+name])[0]['captureComplete'])
            rows.append(next(r.copy() for r in rows if r.get('name')=='gpu-anchor-'+name))
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        for name,field,value in [('source-ready','startUs',1000000),('source-ready','value0',0),
                                 ('source-ready','value1',0),('origin-xy','value0',self.bits(float('inf'))),
                                 ('origin-z-yaw','value1',self.bits(float('nan'))),('origin-xy','value0',-1),
                                 ('origin-xy','value0',self.bits(200000))]:
            rows=self.family_fixture();next(r for r in rows if r.get('name')=='gpu-anchor-'+name)[field]=value
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'],name)

    def test_opaque_baseline_recording(self):
        rows=self.opaque_fixture()
        for row in rows:
            if row.get('name','').startswith('cpu-diagnostic-'):row['value0']=10
            if row.get('name')=='render-probe-kind':row['value0']=7
            if row.get('name') in ('render-probe-opaque-world','render-probe-opaque-props'):row['value0']=row['value1']
        result=analyze_render_passes(rows)[0]
        self.assertTrue(result['captureComplete'])
        self.assertIsNone(result['opaqueShaderComparison'])
        self.assertTrue(result['opaqueBaselineRecording']['allPhasesSameShader'])
        next(v for v in rows if v.get('name')=='render-probe-opaque-world')['value0']=0
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])

if __name__ == '__main__': unittest.main()
