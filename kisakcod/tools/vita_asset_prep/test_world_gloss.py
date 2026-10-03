import unittest
import test_world_math
import test_gpu_resources
from analyze_render_passes import analyze_render_passes
from analyze_gpu_resources import analyze_resources


class WorldGlossTests(unittest.TestCase):
    def fixture(self):
        rows = test_world_math.WorldMathTests().fixture()
        for r in rows:
            name = r.get('name', '')
            if name.startswith('cpu-diagnostic-'): r['value0'] = 30
            if name == 'render-probe-kind': r['value0'] = 27
            if name.startswith('render-probe-world-math'):
                r['name'] = name.replace('world-math', 'world-gloss')
                if name.endswith('-valid'): r['value1'] = 1
                elif name.endswith('-targets'):
                    r['value0'] = 2 if r['value0'] else 0
                    r['value1'] = 2
                else:
                    r['value0'] = 1 if r['value0'] else 0
                    r['value1'] = 1
        return rows

    def resources(self):
        rows = [r for r in test_gpu_resources.ResourceAuditTests().fixture()
                if not r['name'].startswith('gpu-audit-p2-')]
        for r in list(rows):
            name = r['name']
            if name.startswith('gpu-audit-p1-'):
                new = {**r, 'name': name.replace('p1-', 'p8-')}
                if name.endswith('-program'): new['value0'] = 18
                if name.endswith('-f-instructions'): new['value0'] -= 3
                rows.append(new)
            if name.startswith('gpu-audit-s') and name.endswith('-program'):
                r['value0'] = r['value1'] = 13 if '-s588-' in name else 18 if '-s507-p2-' in name else 11
                rows.append({**r, 'name': name.replace('-program', '-gloss'),
                             'value0': 233 if '-s507-' in name else 0, 'value1': 233})
        rows.append(dict(name='render-probe-world-gloss', generation=1, value0=1, value1=1))
        # Historical shaders may be compiled but are not this diagnostic.
        rows.append(dict(name='world-basis-ready', generation=0, value0=3, value1=1))
        return rows

    def test_complete(self):
        a = analyze_render_passes(self.fixture())[0]
        self.assertTrue(a['captureComplete'], a['invalidFrames'])
        self.assertTrue(a['worldGlossComparison']['candidateActuallyUsed'])
        self.assertIsNone(a['worldAlphaComparison'])
        b = analyze_resources(self.resources())
        self.assertTrue(b['complete'], b['problems'])
        self.assertEqual(b['worldGlossComparison']['f']['deltas']['primaryInstructions'], -3)

    def test_missing_and_wrong_dispatch(self):
        for suffix in ('', '-valid', '-targets'):
            rows = [r for r in self.fixture() if r.get('name') != 'render-probe-world-gloss' + suffix]
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        for suffix, field, value in [('', 'value1', 0), ('', 'value0', 1), ('-valid', 'value0', 1),
                                     ('-valid', 'value1', 0), ('-targets', 'value1', 3)]:
            rows = self.fixture()
            next(r for r in rows if r.get('name') == 'render-probe-world-gloss' + suffix)[field] = value
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])

    def test_resource_certificate_and_inputs(self):
        for name in ['gpu-audit-s507-p2-gloss', 'gpu-audit-p8-f-instructions', 'gpu-audit-s588-p3-u2-binding']:
            self.assertFalse(analyze_resources([r for r in self.resources() if r['name'] != name])['complete'])
        for name in ['gpu-audit-s507-p2-gloss', 'gpu-audit-s261-p1-gloss', 'gpu-audit-s507-p2-program',
                     'gpu-audit-p8-v-code-hash0', 'gpu-audit-s507-p2-u2-filter']:
            rows = self.resources()
            next(r for r in rows if r['name'] == name)['value0'] += 1
            self.assertFalse(analyze_resources(rows)['complete'], name)


if __name__ == '__main__': unittest.main()
