import unittest
from analyze_world_basis_audit import analyze


class BasisAuditTests(unittest.TestCase):
    def fixture(self):
        rows = []
        for batch in (261, 507, 588):
            rows.append(dict(stage='world-basis-audit', subject='material', generation=1,
                detail=f'schema=1 batch={batch} triangles=10 constant=6 normalVaries=4 '
                       'tangentVaries=0 bothVary=0 nonfinite=0 degenerate=0 invalidIndex=0 '
                       'runs=3 constantRuns=2 longestConstantRun=4 certificate=0 inputValid=1'))
        return rows

    def test_mixed_coverage_and_unavailable_counter(self):
        r = analyze(self.fixture(), [dict(name='package-read-sample-unavailable', generation=1)])
        self.assertTrue(r['complete'])
        self.assertEqual(r['batches'][0]['extraDrawsIfSplit'], 2)
        self.assertTrue(r['batches'][0]['splitCanRecoverTriangles'])
        self.assertEqual(r['readSamples'][0]['available'], 0)
        self.assertIsNone(r['readSamples'][0]['utilization'])

    def test_missing_duplicate_malformed_and_wrong_certificate(self):
        rows = self.fixture()
        self.assertFalse(analyze(rows[:-1], [])['complete'])
        self.assertFalse(analyze(rows+[rows[0]], [])['complete'])
        self.assertFalse(analyze([], [])['complete'])
        for old, new in [('constant=6', 'constant=-1'), ('certificate=0', 'certificate=1'),
                         ('runs=3', 'runs=1'), ('inputValid=1', 'inputValid=0')]:
            bad = self.fixture();bad[0]['detail'] = bad[0]['detail'].replace(old, new)
            self.assertFalse(analyze(bad, [])['complete'])

    def test_raw_counter_not_converted_to_time(self):
        report = analyze(self.fixture(), [dict(name='package-read-sample-raw', generation=1,
                                              elapsedUs=10, value0=9999, value1=65536)])
        self.assertEqual(report['readSamples'][0]['executionCounterDeltaRaw'], 9999)
        self.assertEqual(report['readSamples'][0]['sampledReadWallUs'], 10)
        self.assertIsNone(report['readSamples'][0]['utilization'])


if __name__ == '__main__':
    unittest.main()
