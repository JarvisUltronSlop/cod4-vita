import unittest
import test_weapon_pipeline
from analyze_render_passes import analyze_render_passes


class WeaponVertexTests(unittest.TestCase):
    def fixture(self):
        rows = test_weapon_pipeline.WeaponPipelineTests().fixture()
        for row in rows:
            name = row.get('name', '')
            if name.startswith('cpu-diagnostic-'):
                row['value0'] = 21
            if name == 'render-probe-kind':
                row['value0'] = 18
            if name in ('render-probe-weapon-family', 'render-probe-weapon-cache-use'):
                row['value0'] = row['value1']
        for row in list(rows):
            if row.get('name') == 'render-probe-frame':
                candidate = row['value0'] == 2
                for name, a, b in [('vertices', 2 if candidate else 0, 2), ('vertex-count', 1000, 44000 if candidate else 84000), ('vertex-table', 1, 65537)]:
                    rows.append({**row, 'name': 'render-probe-weapon-' + name, 'value0': a, 'value1': b})
        return rows

    def test_exact_vertex_path_with_retained_pipeline(self):
        result = analyze_render_passes(self.fixture())[0]
        self.assertTrue(result['captureComplete'])
        self.assertTrue(result['weaponVertexComparison']['candidateActuallyUsed'])
        self.assertIsNone(result['weaponPipelineComparison'])
        for phase in result['phases']:
            self.assertEqual(phase['weaponFamilyDraws'], phase['weaponFamilyEligible'])
            self.assertEqual(phase['weaponLightCache']['candidateUpdates'], phase['weaponLightCache']['updates'])
        self.assertEqual(result['phases'][1]['name'], 'packed-weapon-vertices')
        self.assertEqual(result['phases'][0]['weaponVertices']['candidateUpdates'], 0)
        self.assertGreater(result['phases'][1]['weaponVertices']['candidateUpdates'], 0)

    def test_incomplete_or_inconsistent_vertex_path_rejected(self):
        for name in ('vertices', 'vertex-count', 'vertex-table'):
            rows = [r for r in self.fixture() if r.get('name') != 'render-probe-weapon-' + name]
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        for name, field, value in [('vertices', 'value0', 1), ('vertices', 'value1', 0), ('vertex-count', 'value0', 0), ('vertex-count', 'value1', 44000), ('vertex-table', 'value0', 0), ('vertex-table', 'value1', 0), ('family', 'value0', 0), ('cache-use', 'value0', 0)]:
            rows = self.fixture()
            next(r for r in rows if r.get('name') == 'render-probe-weapon-' + name)[field] = value
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'], name)


if __name__ == '__main__':
    unittest.main()
