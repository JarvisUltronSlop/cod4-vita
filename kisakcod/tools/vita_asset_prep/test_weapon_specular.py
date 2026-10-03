import unittest
import test_weapon_vertices
from analyze_render_passes import analyze_render_passes


class WeaponSpecularTests(unittest.TestCase):
    def fixture(self):
        rows = test_weapon_vertices.WeaponVertexTests().fixture()
        for row in rows:
            name = row.get('name', '')
            if name.startswith('cpu-diagnostic-'):
                row['value0'] = 22
            if name == 'render-probe-kind':
                row['value0'] = 19
            if name == 'render-probe-weapon-vertices':
                row['value0'] = row['value1']
            if name == 'render-probe-weapon-vertex-count':
                row['value1'] = row['value0'] * 44
            if name == 'render-pass-weapon-geometry':
                row['value0'] = 5
        for row in list(rows):
            if row.get('name') == 'render-probe-frame':
                for name, a, b in [('specular', 1 if row['value0'] == 2 else 0, 1), ('specular-valid', 0, 1), ('specular-maps', 0, 1)]:
                    rows.append({**row, 'name': 'render-probe-weapon-' + name, 'value0': a, 'value1': b})
        return rows

    def test_shader_and_all_phase_vertex_baseline(self):
        result = analyze_render_passes(self.fixture())[0]
        self.assertTrue(result['captureComplete'])
        self.assertTrue(result['weaponSpecularComparison']['candidateActuallyUsed'])
        self.assertIsNone(result['weaponVertexComparison'])
        self.assertIsNone(result['weaponPipelineComparison'])
        for phase in result['phases']:
            self.assertEqual(phase['weaponVertices']['candidateUpdates'], phase['weaponVertices']['updates'])
            self.assertEqual(phase['weaponLightCache']['candidateUpdates'], phase['weaponLightCache']['updates'])
        self.assertEqual(result['phases'][1]['name'], 'authored-weapon-shader')
        self.assertEqual(result['phases'][0]['weaponSpecular']['draws'], 0)

    def test_missing_ineligible_or_baseline_change_rejected(self):
        for name in ('specular', 'specular-valid', 'specular-maps'):
            rows = [r for r in self.fixture() if r.get('name') != 'render-probe-weapon-' + name]
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        for name, field, value in [('specular', 'value0', 1), ('specular', 'value1', 0), ('specular', 'value1', 6), ('specular-valid', 'value0', 1), ('specular-valid', 'value1', 0), ('specular-maps', 'value0', 1), ('specular-maps', 'value1', 0), ('vertices', 'value0', 0), ('vertex-count', 'value1', 84000), ('family', 'value0', 0)]:
            rows = self.fixture()
            next(r for r in rows if r.get('name') == 'render-probe-weapon-' + name)[field] = value
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'], name)


if __name__ == '__main__':
    unittest.main()
