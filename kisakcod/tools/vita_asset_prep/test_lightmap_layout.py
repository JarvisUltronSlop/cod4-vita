import copy
import unittest
import test_world_lifetime
import test_gpu_resources
from analyze_render_passes import analyze_render_passes
from analyze_gpu_resources import analyze_resources


class LightmapLayoutTests(unittest.TestCase):
    def fixture(self):
        rows = [r for r in test_world_lifetime.WorldLifetimeTests().fixture() if not r.get('name', '').startswith('render-probe-world-lifetime')]
        for r in rows:
            if r.get('name', '').startswith('cpu-diagnostic-'): r['value0'] = 27
            if r.get('name') == 'render-probe-kind': r['value0'] = 24
        for r in list(rows):
            if r.get('name') == 'render-probe-frame':
                for name, a, b in [('', 2 if r['value0'] == 2 else 0, 2), ('-valid', 0, 0), ('-storage', 3*1024*1024, 8*1024*1024)]:
                    rows.append({**r, 'name': 'render-probe-lightmap-layout'+name, 'value0': a, 'value1': b})
        return rows

    def test_complete_layout_comparison(self):
        a = analyze_render_passes(self.fixture())[0]
        self.assertTrue(a['captureComplete'], a['invalidFrames'])
        self.assertTrue(a['lightmapLayoutComparison']['candidateActuallyUsed'])
        self.assertIsNone(a['worldLifetimeComparison'])
        self.assertIsNone(a['worldAlphaComparison'])

    def test_wrong_sampler_fallback_failure_or_memory_invalidates(self):
        for suffix, field, value in [('', 'value0', 1), ('-valid', 'value0', 1), ('-valid', 'value1', 1), ('-storage', 'value0', 0), ('-storage', 'value0', 9*1024*1024)]:
            rows = self.fixture()
            next(r for r in rows if r.get('name') == 'render-probe-lightmap-layout'+suffix)[field] = value
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])

    def resources(self):
        rows = [r for r in test_gpu_resources.ResourceAuditTests().fixture() if not r['name'].startswith('gpu-audit-p2-')]
        rows.append(dict(name='render-probe-lightmap-layout', generation=1, value0=2, value1=2))
        for r in rows:
            name = r['name']
            if name.startswith('gpu-audit-s') and name.endswith('-program'):
                r['value0'] = r['value1'] = 13 if '-s588-' in name else 11
            if '-u3-' not in name and '-u4-' not in name: continue
            if name.endswith('-format-type'): r['value0'], r['value1'] = (0 if '-u3-' in name else 0x0c000000), (0 if '-p2-' in name else 0x60000000)
            if name.endswith('-mips'): r['value0'], r['value1'] = 1, 0
            if name.endswith('-address'): r['value0'], r['value1'] = 2, 2
            if '-p2-' in name and name.endswith('-binding'): r['value0'] += 100; r['value1'] += 100
        for ident, size in [(242,1024*1024),(243,2*1024*1024)]:
            for name,a,b in [('upload',ident,ident+100),('verified-bytes',size,3*1024*1024),('memory-types',1,1)]:
                rows.append(dict(name='lightmap-layout-'+name,generation=1,value0=a,value1=b))
        return rows

    def test_failed_startup_keeps_layout_identity_without_claiming_capture(self):
        rows = [r for r in self.resources() if not r['name'].startswith(('render-probe-', 'gpu-audit-s'))]
        result = analyze_resources(rows)
        self.assertFalse(result['complete'])
        self.assertEqual(result['layoutStorage']['verifiedBytes'], 3*1024*1024)
        self.assertNotIn('world-lifetime', result['programs'])
        self.assertIn('No actual world texture observations', result['problems'])

    def inplace_resources(self):
        rows=[r for r in self.resources() if not r['name'].startswith('lightmap-layout-')]
        for r in rows:
            if r['name'].startswith('gpu-audit-s') and '-p2-' in r['name'] and ('-u3-' in r['name'] or '-u4-' in r['name']) and r['name'].endswith('-binding'):
                r['value0']-=100; r['value1']-=100
        def add(name,a,b): rows.append(dict(name='lightmap-layout-'+name,generation=1,value0=a,value1=b))
        add('inplace-schema',1,128*1024)
        for ident,size in [(242,1024*1024),(243,2*1024*1024)]:add('registered',ident,size)
        for state in (1,0):
            for ident,size in [(242,1024*1024),(243,2*1024*1024)]:
                add('content-verified',ident,size);add('content-state',ident,state)
            add('transition-us',100000,state);add('transition-valid',1,2)
        return rows

    def test_inplace_identity_roundtrip_and_workspace_required(self):
        rows=self.inplace_resources();result=analyze_resources(rows)
        self.assertTrue(result['complete'],result['problems']);self.assertEqual(result['layoutStorage']['extraGpuBytes'],0)
        for name,key,value in [('lightmap-layout-inplace-schema','value1',1024),('lightmap-layout-content-verified','value1',0),('lightmap-layout-content-state','value1',2),('lightmap-layout-transition-valid','value0',0),('gpu-audit-s261-p2-u3-binding','value0',999)]:
            broken=copy.deepcopy(rows);next(r for r in broken if r['name']==name)[key]=value
            self.assertFalse(analyze_resources(broken)['complete'],name)
        broken=[r for r in rows if r['name']!='lightmap-layout-content-state'];self.assertFalse(analyze_resources(broken)['complete'])

    def test_inplace_phase_clock_excludes_conversion(self):
        rows=self.fixture();begin=next(r for r in rows if r.get('name')=='cpu-diagnostic-begin')
        def add(name,a,b,at=None): rows.append(dict(type='counter',name=name,generation=begin['generation'],sequence=0,startUs=begin['startUs'] if at is None else at,value0=a,value1=b))
        add('lightmap-layout-inplace-schema',1,128*1024,begin['startUs']-1)
        add('lightmap-layout-transition-us',100000,1);add('lightmap-layout-transition-us',100000,0)
        add('lightmap-layout-diagnostic-paused-us',200000,0)
        for row in list(rows):
            if row.get('name')=='render-probe-frame':rows.append({**row,'name':'render-probe-lightmap-inplace','value0':128*1024,'value1':0})
            if row.get('name') in ('cpu-diagnostic-end','cpu-diagnostic-capture-complete'):row['startUs']+=200000
        result=analyze_render_passes(rows)[0];self.assertTrue(result['captureComplete'],result['invalidFrames'])
        for name,key,value in [('render-probe-lightmap-inplace','value1',3145728),('lightmap-layout-diagnostic-paused-us','value0',0)]:
            broken=copy.deepcopy(rows);next(r for r in broken if r.get('name')==name)[key]=value
            self.assertFalse(analyze_render_passes(broken)[0]['captureComplete'],name)

    def test_actual_layout_and_texels_inputs_required(self):
        rows = self.resources(); a = analyze_resources(rows)
        self.assertTrue(a['complete'], a['problems'])
        for suffix, field, value in [('address', 'value0', 0), ('format-type', 'value1', 0x60000000), ('dimensions', 'value0', 128), ('binding', 'value0', 4)]:
            broken = copy.deepcopy(rows)
            next(r for r in broken if r['name'] == 'gpu-audit-s261-p2-u3-'+suffix)[field] = value
            self.assertFalse(analyze_resources(broken)['complete'])


if __name__ == '__main__': unittest.main()
