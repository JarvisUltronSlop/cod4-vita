import unittest
import test_weapon_specular
from analyze_render_passes import analyze_render_passes


class WorldAlphaTests(unittest.TestCase):
    def fixture(self):
        rows = test_weapon_specular.WeaponSpecularTests().fixture()
        for row in rows:
            name = row.get('name', '')
            if name.startswith('cpu-diagnostic-'):
                row['value0'] = 23
            if name == 'render-probe-kind':
                row['value0'] = 20
            if name == 'render-probe-weapon-specular':
                row['value0'] = row['value1']
            if name == 'render-pass-world-geometry':
                row['value0'] = 25
        for row in list(rows):
            if row.get('name') == 'render-probe-frame':
                for name, a, b in [('', 2 if row['value0'] == 2 else 0, 2), ('-valid', 0, 1), ('-target', 1 if row['value0'] == 2 else 0, 1)]:
                    rows.append({**row, 'name': 'render-probe-world-alpha' + name, 'value0': a, 'value1': b})
        return rows

    def test_world_family_and_retained_weapon_paths(self):
        result = analyze_render_passes(self.fixture())[0]
        self.assertTrue(result['captureComplete'])
        self.assertTrue(result['worldAlphaComparison']['candidateActuallyUsed'])
        self.assertIsNone(result['weaponSpecularComparison'])
        self.assertIsNone(result['earlyAlphaComparison'])
        for phase in result['phases']:
            self.assertEqual(phase['weaponSpecular']['draws'], phase['weaponSpecular']['eligible'])
            self.assertGreater(phase['worldAlpha']['targetEligible'], 0)
        self.assertEqual(result['phases'][1]['name'], 'transparent-world-shader')

    def test_missing_ineligible_or_unseen_target_rejected(self):
        for name in ('', '-valid', '-target'):
            rows = [r for r in self.fixture() if r.get('name') != 'render-probe-world-alpha' + name]
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        for name, field, value in [('', 'value0', 1), ('', 'value1', 0), ('', 'value1', 6), ('-valid', 'value0', 1), ('-valid', 'value1', 0), ('-target', 'value0', 1), ('-target', 'value1', 3)]:
            rows = self.fixture()
            next(r for r in rows if r.get('name') == 'render-probe-world-alpha' + name)[field] = value
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'], name)
        rows = self.fixture()
        for row in rows:
            if row.get('name') == 'render-probe-world-alpha-target':
                row['value0'] = row['value1'] = 0
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        rows = self.fixture()
        next(r for r in rows if r.get('name') == 'render-probe-weapon-specular')['value0'] = 0
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])


if __name__ == '__main__':
    unittest.main()
