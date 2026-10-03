import unittest
import test_gpu_draws
from analyze_render_passes import analyze_render_passes


class FastDrawProbeTests(unittest.TestCase):
    def fixture(self):
        rows = test_gpu_draws.GpuDrawTests().fixture()
        for row in rows:
            if row.get('name', '').startswith('cpu-diagnostic-'):
                row['value0'] = 19
            if row.get('name') == 'render-probe-kind':
                row['value0'] = 16
        for row in list(rows):
            if row.get('name') == 'render-probe-frame':
                for name, a, b in [('ready', 1, 1), ('color', 9, 9), ('clear', 2, 2), ('invalid', 0, 0)]:
                    rows.append({**row, 'name': 'render-probe-fast-' + name, 'value0': a, 'value1': b})
        return rows

    def test_current_baseline_attribution(self):
        result = analyze_render_passes(self.fixture())[0]
        self.assertTrue(result['captureComplete'])
        self.assertTrue(result['drawWaitBreakdown']['coverageComplete'])
        self.assertFalse(result['drawWaitBreakdown']['normalFrameImprovementClaimed'])
        self.assertIn('Early-alpha and weapon-opaque candidates disabled', result['drawWaitBreakdown']['baselineContract'])
        self.assertIsNone(result['earlyAlphaComparison'])
        self.assertIsNone(result['fastColorClearComparison'])
        self.assertIsNone(result['materialFamilyComparison'])
        self.assertIn('instrumentedElapsed', result['phases'][1]['passes']['world'])
        self.assertIn('missing source HUD', result['sourceHudEvidence']['qualificationLimit'])

    def test_baseline_program_loss_and_missing_draws_rejected(self):
        for name in ('ready', 'color', 'clear', 'invalid'):
            rows = [r for r in self.fixture() if r.get('name') != 'render-probe-fast-' + name]
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        for name, field, value in [('fast-ready', 'value0', 0), ('fast-clear', 'value0', 0), ('fast-color', 'value0', 0), ('fast-invalid', 'value0', 1), ('family-baked', 'value0', 0), ('kind', 'value0', 11)]:
            rows = self.fixture()
            next(r for r in rows if r.get('name') == 'render-probe-' + name)[field] = value
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'], name)
        rows = [r for r in self.fixture() if r.get('name') != 'gpu-draw-wait']
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        rows = self.fixture()
        rows[0]['dropped'] = 1
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])


if __name__ == '__main__':
    unittest.main()
