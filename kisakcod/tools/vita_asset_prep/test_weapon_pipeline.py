import unittest
import test_weapon_opaque
from analyze_render_passes import analyze_render_passes


class WeaponPipelineTests(unittest.TestCase):
    def fixture(self):
        rows = test_weapon_opaque.WeaponOpaqueTests().fixture()
        for row in rows:
            if row.get('name', '').startswith('cpu-diagnostic-'):
                row['value0'] = 20
            if row.get('name') == 'render-probe-kind':
                row['value0'] = 17
        for row in list(rows):
            if row.get('name') == 'render-probe-frame':
                candidate = row['value0'] == 2
                for name, a, b in [('family', int(candidate), 1), ('family-valid', 0, 3), ('cache-use', 2 if candidate else 0, 2), ('cache-hits', 200, 10), ('cache-resets', int(candidate), 229620)]:
                    rows.append({**row, 'name': 'render-probe-weapon-' + name, 'value0': a, 'value1': b})
        return rows

    def test_shader_and_cache_observed(self):
        result = analyze_render_passes(self.fixture())[0]
        self.assertTrue(result['captureComplete'])
        self.assertTrue(result['weaponPipelineComparison']['candidateActuallyUsed'])
        self.assertIsNone(result['weaponOpaqueComparison'])
        self.assertIsNone(result['fastColorClearComparison'])
        self.assertEqual(result['phases'][1]['name'], 'weapon-pipeline')
        self.assertGreater(result['phases'][1]['weaponLightCache']['hits'], 0)
        self.assertEqual(result['phases'][0]['weaponLightCache']['candidateUpdates'], 0)

    def test_missing_or_inconsistent_dispatch_rejected(self):
        for name in ('family', 'family-valid', 'cache-use', 'cache-hits', 'cache-resets'):
            rows = [r for r in self.fixture() if r.get('name') != 'render-probe-weapon-' + name]
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        for name, field, value in [('family', 'value0', 1), ('family', 'value1', 0), ('family-valid', 'value0', 1), ('family-valid', 'value1', 0), ('cache-use', 'value0', 1), ('cache-use', 'value1', 0), ('cache-hits', 'value0', -1), ('cache-resets', 'value1', 999999), ('cache-resets', 'value0', 3)]:
            rows = self.fixture()
            next(r for r in rows if r.get('name') == 'render-probe-weapon-' + name)[field] = value
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'], name)
        rows = self.fixture()
        rows[0]['dropped'] = 1
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])


if __name__ == '__main__':
    unittest.main()
