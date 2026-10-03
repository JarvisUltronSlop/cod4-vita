"""Execute production text routing/escaping/draining with independent owners."""
from pathlib import Path
import subprocess
import unittest


class RendererTraceTests(unittest.TestCase):
    def test_actual_renderer_text_path(self):
        root = Path(__file__).resolve().parents[3]
        source = (root/'kisakcod/src/vita/vita_cpu_profile.cpp').read_text()
        target = root/'build/host-renderer-trace'
        target.mkdir(exist_ok=True)
        records = source[source.index('struct PerfRecord {'):source.index('struct Channel {')]
        drain = source[source.index('template<uint32_t Capacity>\nbool DrainTextRecords'):source.index('int Writer(SceSize,void*)')]
        append = source[source.index('bool VitaCpuProfile_AppendLog('):source.index('void* VitaCpuProfile_ScopeCheckpoint(')]
        # Keep the exact production append/trace/invalidation functions only.
        append = append[:append.index('\n', append.index('void VitaCpuProfile_Invalidate'))]
        (target/'renderer_trace_functions.inc').write_text(records+drain+append)
        writer = source[source.index('int Writer(SceSize,void*)'):source.index('uint64_t VitaCpuNow()')]
        self.assertIn('renderTextLeft=renderTextRecords.Count()', writer)
        self.assertIn('any|=DrainTextRecords(renderTextRecords,renderTextLeft,requestedSink);', writer)
        self.assertIn('remaining=textLeft || renderTextLeft || sourceLeft;', writer)
        self.assertLess(writer.index('DrainTextRecords(renderTextRecords'), writer.index('stopping.load'))
        test = root/'kisakcod/tests/vita_portable/renderer_trace_tests.cpp'
        (target/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(RendererTraceTests LANGUAGES CXX)
add_executable(renderer-trace "{test.as_posix()}")
target_include_directories(renderer-trace PRIVATE "{target.as_posix()}" "{(root/'kisakcod/src').as_posix()}")
target_compile_features(renderer-trace PRIVATE cxx_std_20)
''')
        commands = [['cmake', '-S', str(target), '-B', str(target/'build')]]
        for config in ('Debug', 'Release'):
            commands += [['cmake', '--build', str(target/'build'), '--config', config],
                         [str(target/f'build/{config}/renderer-trace.exe')]]
        for cmd in commands:
            result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == '__main__':
    unittest.main()
