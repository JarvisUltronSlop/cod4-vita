import io
import json
from pathlib import Path
import tempfile
import unittest
import zlib

from cpu_compressed_log import lines
from analyze_cpu import read, analyze
import test_render_passes


def packed(data):
    return b''.join(zlib.compress(data[i:i+32768], 1) for i in range(0, len(data), 32768))


class CompressedLogTests(unittest.TestCase):
    def test_roundtrip_and_records_crossing_members(self):
        raw=b''.join(json.dumps({'sequence': i, 'value': 'sample'*16}).encode()+b'\n' for i in range(5000))
        self.assertEqual(b''.join(lines(io.BytesIO(packed(raw)))), raw)
        self.assertEqual(list(lines(io.BytesIO(b''))), [])

    def test_damage_is_not_accepted(self):
        raw=b'{"name":"cpu-diagnostic-capture-complete"}\n'
        block=packed(raw)
        for damaged in (block[:-1], block+b'garbage', block[:-1]+bytes([block[-1]^1]), zlib.compress(b'x'*32769)):
            with self.assertRaises((ValueError,zlib.error)):
                list(lines(io.BytesIO(damaged)))
        with self.assertRaisesRegex(ValueError,'incomplete compressed JSON'):
            list(lines(io.BytesIO(packed(b'{"incomplete":1}'))))

    def test_only_checksummed_prefix_survives(self):
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'cpu-perf.jsonl.z'
            path.write_bytes(packed(b'{"first":1}\n')+packed(b'{"second":2}\n')[:-1])
            problems=[]
            self.assertEqual(read(path,problems),[{'first':1}])
            self.assertTrue(problems)

    def test_plain_and_compressed_ambiguity_is_reported(self):
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'cpu-perf.jsonl';path.write_bytes(b'{"old":1}\n')
            path.with_name(path.name+'.z').write_bytes(packed(b'{"new":1}\n'))
            problems=[];self.assertEqual(read(path,problems),[{'new':1}]);self.assertTrue(problems)

    def test_complete_analysis_identical_and_overflow_still_invalid(self):
        with tempfile.TemporaryDirectory() as temp:
            a=Path(temp)/'plain';b=Path(temp)/'packed';a.mkdir();b.mkdir()
            rows=test_render_passes.RenderPassTests().opaque_fixture()
            for name,values in [('cpu-perf.jsonl',rows),('loading-stages.jsonl',[]),('vita-perf.jsonl',[]),('script-stages.jsonl',[])]:
                raw=b''.join(json.dumps(v).encode()+b'\n' for v in values)
                (a/name).write_bytes(raw);(b/(name+'.z')).write_bytes(packed(raw))
            before=analyze(a);after=analyze(b);before.pop('folder');after.pop('folder')
            self.assertEqual(before,after)
            self.assertTrue(after['renderPassTests'][0]['captureComplete'])
            rows[0]['dropped']=1
            (b/'cpu-perf.jsonl.z').write_bytes(packed(b''.join(json.dumps(v).encode()+b'\n' for v in rows)))
            self.assertFalse(analyze(b)['renderPassTests'][0]['captureComplete'])


if __name__ == '__main__':
    unittest.main()
