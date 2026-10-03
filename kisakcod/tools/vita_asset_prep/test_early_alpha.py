import unittest
import test_color_fast
from analyze_render_passes import analyze_render_passes


class EarlyAlphaTests(unittest.TestCase):
    def fixture(self):
        rows = test_color_fast.FastColorTests().fixture()
        for row in rows:
            if row.get('name', '').startswith('cpu-diagnostic-'):
                row['value0'] = 18
            if row.get('name') == 'render-probe-kind':
                row['value0'] = 15
            if row.get('name') in ('render-probe-fast-color', 'render-probe-fast-clear'):
                row['value0'] = row['value1']
        for row in list(rows):
            if row.get('name') == 'render-probe-frame':
                for name, a, b in [('ready', 3, 3), ('world', int(row['value0'] == 2), 1), ('props', 0, 0), ('invalid', 0, 0)]:
                    rows.append({**row, 'name': 'render-probe-alpha-' + name, 'value0': a, 'value1': b})
        return rows

    def test_retained_baseline_and_actual_dispatch(self):
        result = analyze_render_passes(self.fixture())[0]
        self.assertTrue(result['captureComplete'])
        self.assertTrue(result['earlyAlphaComparison']['candidateActuallyUsed'])
        self.assertIsNone(result['fastColorClearComparison'])
        self.assertFalse(result['hardwareQualified'])
        self.assertIn('missing source HUD', result['sourceHudEvidence']['qualificationLimit'])
        self.assertEqual(result['phases'][1]['name'], 'early-alpha')

    def test_missing_invalid_or_unseen_rejected(self):
        for name in ('ready', 'world', 'props', 'invalid'):
            rows = [r for r in self.fixture() if r.get('name') != 'render-probe-alpha-' + name]
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        for name, field, value in [('alpha-ready', 'value0', 1), ('alpha-ready', 'value1', 2), ('alpha-world', 'value0', 1), ('alpha-world', 'value1', 0), ('alpha-props', 'value1', -1), ('alpha-world', 'value1', 999999), ('alpha-invalid', 'value0', 1), ('fast-color', 'value0', 0), ('family-baked', 'value0', 0)]:
            rows = self.fixture()
            next(r for r in rows if r.get('name') == 'render-probe-' + name)[field] = value
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'], name)
        rows = self.fixture()
        rows[0]['dropped'] = 1
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        self.assertFalse(analyze_render_passes(self.fixture(), ['truncated'])[0]['captureComplete'])


if __name__ == '__main__':
    unittest.main()
