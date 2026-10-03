"""Tests of qualification refusals and shader compatibility, using synthetic records."""
import json
from pathlib import Path
import tempfile
import unittest

from analyze_cpu import analyze, compare_loading, percentile, read
from compare_cpu_replay import compare
from cpu_shader_key import compatibility


class CpuTools(unittest.TestCase):
    def test_shipment_timer_requires_completed_rendered_run_and_all_input_phases(self):
        folder = self.root / 'shipment'
        cpu = [{'type': 'recording-health', 'dropped': 0, 'ioFailed': False},
               {'name': 'shipment-test-request', 'value0': 1, 'startUs': 1000},
               {'name': 'shipment-test-begin', 'value0': 1, 'startUs': 10000000, 'generation': 1}]
        cpu += [{'name': 'shipment-test-phase', 'value0': n, 'startUs': 10000000+n*3000000,
                 'generation': 1} for n in range(60)]
        cpu += [{'name': 'shipment-test-complete', 'value0': 1, 'value1': 180000000, 'startUs': 190000000},
                {'name': 'shipment-test-skipped-phases', 'value0': 1, 'value1': 0, 'startUs': 190000001}]
        self.write(folder, 'cpu-perf.jsonl', cpu)
        for name in ('loading-stages.jsonl', 'script-stages.jsonl'):
            self.write(folder, name, [])
        self.write(folder, 'vita-perf.jsonl', [{'type': 'play-frame', 'endUs': 11000000, 'intervalUs': 500000}])
        run = analyze(folder)['shipmentTests'][0]
        self.assertTrue(run['captureComplete'])
        self.assertFalse(run['hardwareQualified'])
        cpu[-2]['name'] = 'shipment-test-cancelled'
        self.write(folder, 'cpu-perf.jsonl', cpu)
        self.assertFalse(analyze(folder)['shipmentTests'][0]['threeMinutesComplete'])
        cpu[-2]['name'] = 'shipment-test-complete'
        cpu[-2]['value1'] = 179999999
        self.write(folder, 'cpu-perf.jsonl', cpu)
        self.assertFalse(analyze(folder)['shipmentTests'][0]['threeMinutesComplete'])
        cpu[-2]['value1'] = 180000000
        cpu.pop(3)
        self.write(folder, 'cpu-perf.jsonl', cpu)
        self.assertFalse(analyze(folder)['shipmentTests'][0]['captureComplete'])

    def test_shipment_loading_failure_and_incomplete_logs_never_pass(self):
        folder = self.root / 'failed'
        self.write(folder, 'cpu-perf.jsonl', [{'name': 'shipment-test-request', 'value0': 1, 'startUs': 1},
                   {'name': 'shipment-test-failed', 'value0': 1, 'value1': 0, 'startUs': 300000001}])
        result = analyze(folder)['shipmentTests'][0]
        self.assertFalse(result['started'])
        self.assertFalse(result['captureComplete'])
        self.assertEqual(result['outcome'], 'failed')

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def write(self, folder, name, rows):
        folder.mkdir(parents=True, exist_ok=True)
        (folder / name).write_text(''.join(json.dumps(r) + '\n' for r in rows))

    def run_fixture(self):
        folder = self.root / 'run'
        cpu = [{'type': 'recording-health', 'dropped': 0, 'ioFailed': False},
               {'name': 'cpu-diagnostic-begin', 'generation': 1, 'startUs': 1000000, 'value0': 1}]
        for i in range(1801):
            at = 1000000 + i * 1000000 // 60
            cpu += [{'name': 'source-progress', 'generation': 1, 'startUs': at, 'value0': i // 3, 'value1': i // 2},
                    {'name': 'source-deadline-lateness', 'generation': 1, 'startUs': at, 'value0': 0}]
            if i % 2 == 0:
                cpu.append({'name': 'cpu-preparation-us', 'generation': 1, 'startUs': at, 'value0': 20000})
                cpu.append({'name': 'render-preparation-us', 'generation': 1, 'startUs': at, 'value0': 1000, 'value1': 1000})
                cpu.append({'name': 'cpu-preparation-latency-us', 'generation': 1, 'startUs': at, 'value0': 22000})
        cpu.append({'name': 'cpu-diagnostic-end', 'generation': 1, 'startUs': 31000000, 'value0': 1})
        self.write(folder, 'cpu-perf.jsonl', cpu)
        for name in ('loading-stages.jsonl', 'vita-perf.jsonl', 'script-stages.jsonl'):
            self.write(folder, name, [])
        return folder, cpu

    def test_missing_evidence_never_passes(self):
        result = analyze(self.root)
        self.assertFalse(result['cpuReady'])
        self.assertTrue(result['problems'])
        self.assertTrue(all(not value['passed'] for value in result['viewports'].values()))

    def test_metric_success_does_not_qualify_hardware(self):
        folder, _ = self.run_fixture()
        result = analyze(folder)
        self.assertTrue(result['diagnostics'][0]['cpuMetricsMet'])
        self.assertFalse(result['hardwareQualified'])
        self.assertEqual(result['diagnostics'][0]['rates']['serverHz'], 20)
        self.assertEqual(result['diagnostics'][0]['preparation']['p99Us'], 20000)

    def test_overflow_invalidates_otherwise_successful_metrics(self):
        folder, cpu = self.run_fixture()
        cpu[0]['dropped'] = 1
        self.write(folder, 'cpu-perf.jsonl', cpu)
        self.assertFalse(analyze(folder)['diagnostics'][0]['cpuMetricsMet'])

    def test_slow_preparation_and_backlog_fail(self):
        folder, cpu = self.run_fixture()
        for row in cpu:
            if row.get('name') == 'cpu-preparation-us':
                row['value0'] = 26000
            if row.get('name') == 'source-deadline-lateness':
                row['value0'] = 100000
        self.write(folder, 'cpu-perf.jsonl', cpu)
        result = analyze(folder)['diagnostics'][0]
        self.assertFalse(result['cpuMetricsMet'])
        self.assertFalse(result['backlogSettled'])

    def test_unseen_viewmodel_is_unavailable(self):
        folder, _ = self.run_fixture()
        self.write(folder, 'vita-perf.jsonl', [{'type': 'play-frame', 'viewportSeen': 3,
                   'world': [720, 408], 'worldViewport': [0, 0, 720, 408], 'propViewport': [0, 0, 960, 544]}])
        result = analyze(folder)['viewports']
        self.assertTrue(result['worldViewport']['passed'])
        self.assertFalse(result['propViewport']['passed'])
        self.assertFalse(result['viewmodelViewport']['available'])

    def test_preparation_gate_includes_both_owners_and_snapshot_queueing(self):
        folder, cpu = self.run_fixture()
        for row in cpu:
            if row.get('name') == 'cpu-preparation-latency-us':
                row['value0'] = 26000
        self.write(folder, 'cpu-perf.jsonl', cpu)
        result = analyze(folder)['diagnostics'][0]
        self.assertEqual(result['preparationLatency']['p95Us'], 26000)
        self.assertFalse(result['cpuMetricsMet'])

    def test_malformed_unicode_and_truncated_records_are_reported(self):
        path = self.root / 'bad.jsonl'
        path.write_bytes(b'{}\n\xff\n{"truncated":\n')
        errors = []
        self.assertEqual(read(path, errors), [{}])
        self.assertEqual(len(errors), 2)

    def test_loading_needs_repetition_and_nonoverlapping_ranges(self):
        def samples(values):
            return [{'loading': {'launchToMainUs': values, 'mapLoads': [{'elapsedUs': v} for v in values]}}]
        self.assertTrue(compare_loading(samples([100, 110, 120]), samples([80, 85, 90]))['launches']['improvementBeyondObservedVariation'])
        self.assertFalse(compare_loading(samples([100, 110, 120]), samples([90, 100, 110]))['launches']['improvementBeyondObservedVariation'])
        self.assertFalse(compare_loading(samples([100]), samples([80]))['launches']['improvementBeyondObservedVariation'])

    def test_source_observer_refuses_different_commands_or_order(self):
        rows = [{'type': 'begin'}, {'type': 'command', 'time': 100, 'buttons': 1},
                {'type': 'event', 'order': 1}, {'type': 'world', 'state': 'aa'},
                {'type': 'state', 'owner': 'server', 'weapon': 'bb'},
                {'type': 'state', 'owner': 'client', 'weapon': 'bb'}, {'type': 'end', 'complete': True}]
        for row in rows:
            row['capture'] = 1
        a, b = self.root / 'a', self.root / 'b'
        for folder in (a, b):
            self.write(folder, 'cpu-replay.jsonl', rows)
            self.write(folder, 'cpu-perf.jsonl', [{'type': 'recording-health', 'dropped': 0}])
        result = compare(a, b)
        self.assertTrue(result['observedFieldsMatch'])
        self.assertFalse(result['gameplayParityQualified'])
        rows[1]['time'] = 101
        self.write(b, 'cpu-replay.jsonl', rows)
        self.assertFalse(compare(a, b)['identicalTimestampedCommands'])
        rows[1]['time'] = 100
        rows[2], rows[3] = rows[3], rows[2]
        self.write(b, 'cpu-replay.jsonl', rows)
        self.assertFalse(compare(a, b)['observedFieldsMatch'])

    def test_shader_identity_tracks_shader_backend_and_bindings_only(self):
        folder = self.root / 'src/gfx_vita'
        folder.mkdir(parents=True)
        path = folder / 'gfx_vita_backend.cpp'
        original = 'const char* shader=R"GLSL(float4 main(){return 1;})GLSL";\nglBindAttribLocation(p,0,"position");\n'
        path.write_text(original)
        (folder / 'gfx_vita_viewmodel.cpp').write_text('')
        provenance = self.root / 'backend.json'
        record = {'revision': 'a', 'patchSha256': 'b', 'cpuCache': {'patchSha256': 'c'},
                  'compilerSha256': 'd', 'flags': {'HAVE_SHADER_CACHE': 1}, 'vitaShaRK': {'revision': 'e'}}
        provenance.write_text(json.dumps(record))
        key, _ = compatibility(self.root, provenance)
        path.write_text(original + '\nvoid unrelated_cpu_work(){}\n')
        self.assertEqual(key, compatibility(self.root, provenance)[0])
        builder = folder / 'gfx_vita_prop_shader.h'
        builder.write_text('uniform specialization contract v1', encoding='utf-8')
        specialized_key = compatibility(self.root, provenance)[0]
        self.assertNotEqual(key, specialized_key)
        builder.write_text('uniform specialization contract v2', encoding='utf-8')
        self.assertNotEqual(specialized_key, compatibility(self.root, provenance)[0])
        builder.unlink()
        hud = folder / 'gfx_vita_hud_shader.h'
        hud.write_text('HUD program v1', encoding='utf-8')
        hud_key = compatibility(self.root, provenance)[0]
        self.assertNotEqual(key, hud_key)
        hud.write_text('HUD program v2', encoding='utf-8')
        self.assertNotEqual(hud_key, compatibility(self.root, provenance)[0])
        early = folder / 'gfx_vita_early_alpha.h'
        early.write_text('original alpha arithmetic order', encoding='utf-8')
        early_key = compatibility(self.root, provenance)[0]
        early.write_text('moved alpha arithmetic order', encoding='utf-8')
        self.assertNotEqual(early_key, compatibility(self.root, provenance)[0])
        early.unlink()

        hud.unlink()
        for change in (original.replace('return 1', 'return 2'), original.replace('p,0', 'p,1')):
            path.write_text(change)
            self.assertNotEqual(key, compatibility(self.root, provenance)[0])
        path.write_text(original)
        record['compilerSha256'] = 'other'
        provenance.write_text(json.dumps(record))
        self.assertNotEqual(key, compatibility(self.root, provenance)[0])

    def test_replay_requires_whole_tape_and_real_source_consumption(self):
        rows = [
            {'type': 'begin', 'stimulus': 'source-input-tape-v1', 'tapeRecords': 1, 'tapeCrc': 123, 'tapeStep': 0},
            {'type': 'replay-step', 'step': 1, 'time': 1033, 'elapsedMs': 33},
            {'type': 'replay-input-read', 'step': 1, 'read': 1},
            {'type': 'command', 'time': 1033, 'buttons': 1},
            {'type': 'event', 'order': 1}, {'type': 'world', 'state': 'aa'},
            {'type': 'state', 'owner': 'server', 'weaponIndex': 1, 'health': 100},
            {'type': 'state', 'owner': 'client', 'weaponIndex': 1, 'health': 100},
            {'type': 'end', 'stimulus': 'source-input-tape-v1', 'complete': True, 'tapeRecords': 1,
             'tapeCrc': 123, 'tapeStep': 1, 'inputReads': 1}]
        for row in rows:
            row['capture'] = 1
        a, b = self.root / 'a', self.root / 'b'
        def captures(records):
            for folder in (a, b):
                self.write(folder, 'cpu-replay.jsonl', records)
                self.write(folder, 'cpu-perf.jsonl', [{'type': 'recording-health', 'dropped': 0}])
        captures(rows)
        self.assertTrue(compare(a, b)['observedReplayMatch'])
        for bad in ([r for r in rows if r['type'] != 'replay-input-read'],
                    [dict(r, step=2) if r['type'] == 'replay-step' else r for r in rows],
                    [dict(r, tapeStep=0) if r['type'] == 'end' else r for r in rows]):
            captures(bad)
            self.assertFalse(compare(a, b)['observedReplayMatch'])

    def test_percentiles_do_not_make_up_values(self):
        self.assertIsNone(percentile([], .95))
        self.assertEqual(percentile(list(range(1, 101)), .95), 95)


if __name__ == '__main__':
    unittest.main()
