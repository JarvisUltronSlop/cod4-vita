import copy
import unittest
from analyze_gpu_resources import analyze_resources


class ResourceAuditTests(unittest.TestCase):
    def fixture(self):
        rows = []
        def record(generation, name, a, b=0):
            rows.append(dict(name='gpu-audit-' + name, generation=generation, value0=a, value1=b))
        for program in (1, 2, 3):
            record(0, f'p{program}-program', program + 10, 2000)
            for stage in ('v', 'f'):
                prefix = f'p{program}-{stage}'
                for field, a, b in [('size-flags', 1000, int(stage == 'f')), ('registers', 2 | (4 << 32), 8),
                                    ('instructions', 100, 2), ('buffers', 0, 0), ('texture-flags', 0x22222, 0), ('compiler', 0x33dc0, 1),
                                    ('binary-hash0', 1, 2), ('binary-hash1', 3, 4), ('code-hash0', 5, 6), ('code-hash1', 7, 8)]:
                    record(0, prefix + '-' + field, a, b)
        for phase in (1, 2, 3):
            for surface in (261, 507, 588):
                prefix = f's{surface}-p{phase}'
                program = 13 if surface == 588 else 12 if phase == 2 else 11
                record(1, prefix + '-program', program, program)
                for unit in range(6):
                    for field, a, b in [('binding', unit + 1, unit + 1), ('dimensions', 256, 256), ('format-type', 0x85000000, 0),
                                        ('filter', 1, 1), ('mips', 0 if unit == 5 else 4, 0), ('address', 0, 0), ('lod-gamma', 31, 0)]:
                        record(1, prefix + f'-u{unit}-' + field, a, b)
        return rows

    def test_valid_metadata_and_counts(self):
        a = analyze_resources(self.fixture())
        self.assertTrue(a['complete'], a['problems'])
        self.assertEqual(len(a['captures']), 9)
        self.assertEqual(a['programs']['opaque-world']['stages']['f']['secondaryRegisters'], 4)
        self.assertTrue(a['lifetimeComparison']['f']['codeBytesIdentical'])
        self.assertEqual(a['captures'][0]['textures'][0]['baseFormat'], 'BC1')
        self.assertEqual(a['captures'][0]['textures'][5]['gxmMipmapCount'], 0)

    def test_missing_duplicate_wrong_binding_and_program(self):
        rows = self.fixture()
        for field in ['p1-f-instructions', 'p2-f-code-hash1', 's261-p2-program', 's588-p3-u5-mips']:
            self.assertFalse(analyze_resources([r for r in rows if r['name'] != 'gpu-audit-' + field])['complete'])
        self.assertFalse(analyze_resources(rows + [rows[0]])['complete'])
        for name, value in [('s261-p2-u0-binding', 999), ('s507-p3-program', 12), ('p1-f-size-flags', 0)]:
            broken = copy.deepcopy(rows)
            row = next(r for r in broken if r['name'] == 'gpu-audit-' + name)
            row['value1' if name == 'p1-f-size-flags' else 'value0'] = value
            self.assertFalse(analyze_resources(broken)['complete'])
        self.assertFalse(analyze_resources([])['complete'])

    def test_different_code_and_unavailable_are_explicit(self):
        rows = self.fixture()
        next(r for r in rows if r['name'] == 'gpu-audit-p2-f-code-hash0')['value0'] += 1
        self.assertFalse(analyze_resources(rows)['lifetimeComparison']['f']['codeBytesIdentical'])
        rows.append(dict(name='gpu-audit-p2-unavailable', generation=0, value0=2, value1=0))
        self.assertFalse(analyze_resources(rows)['complete'])


if __name__ == '__main__':
    unittest.main()
