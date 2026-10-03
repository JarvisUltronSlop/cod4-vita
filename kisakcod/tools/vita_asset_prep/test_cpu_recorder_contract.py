import unittest
from pathlib import Path
from cpu_recorder_contract import verify


class RecorderContractTests(unittest.TestCase):
    def setUp(self):
        self.source = (Path(__file__).resolve().parents[2] / 'src/vita/vita_cpu_profile.cpp').read_text(encoding='utf-8')

    def test_sink_changes_allowed(self):
        altered = self.source.replace('flushMaxUs=0, flushCount=0', 'flushMaxUs=1, flushCount=0')
        self.assertNotEqual(altered, self.source)
        result = verify(self.source, altered)
        self.assertNotEqual(result['referenceTextSha256'], result['candidateTextSha256'])

    def test_record_encoding_change_rejected(self):
        altered = self.source.replace('(unsigned long long)event.startUs,', '(unsigned long long)(event.startUs+1),')
        self.assertNotEqual(altered, self.source)
        with self.assertRaisesRegex(ValueError, 'WriteEvent'):
            verify(self.source, altered)

    def test_aggregation_change_rejected(self):
        altered = self.source.replace('++total.value0;', 'total.value0+=2;')
        self.assertNotEqual(altered, self.source)
        with self.assertRaisesRegex(ValueError, 'VitaCpuProfile_Record'):
            verify(self.source, altered)


    def test_compressed_startup_preserves_observation_contract(self):
        from cpu_recorder_contract import startup_observation
        plain=startup_observation(self.source)
        self.assertNotEqual(plain,self.source)
        verify(plain,self.source)

    def test_compressed_startup_cannot_change_tls_or_header(self):
        for before,after in [('VitaCpuVerifyThreadLocals','DifferentTlsCheck'),('CODVIT-CPU-1','CODVIT-CPU-2')]:
            altered=self.source.replace(before,after)
            with self.assertRaisesRegex(ValueError,'VitaCpuProfile_Start'):
                verify(self.source,altered)

if __name__ == '__main__':
    unittest.main()
