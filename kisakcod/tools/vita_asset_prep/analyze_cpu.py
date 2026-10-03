"""Summarize CPU/loading evidence without turning missing evidence into a pass."""
import argparse
from collections import Counter, defaultdict
import json
import math
from pathlib import Path
import statistics
import zlib
from cpu_compressed_log import lines as compressed_lines
from analyze_render_passes import analyze_render_passes
from analyze_world_scaling import analyze_world_scaling
from analyze_graphics_profile import analyze_graphics_profile
from analyze_prop_workload import analyze_prop_workload
from analyze_material_cost import analyze_material_cost


def percentile(values, fraction):
    if not values:
        return None
    return sorted(values)[max(0, math.ceil(len(values) * fraction) - 1)]


def distribution(values):
    return {'samples': len(values), 'medianUs': percentile(values, .5),
            'p95Us': percentile(values, .95), 'p99Us': percentile(values, .99),
            'maximumUs': max(values, default=None)}


def read(path, problems):
    path = Path(path)
    packed = path.with_name(path.name+'.z')
    if not path.name.endswith('.z') and packed.exists():
        if path.exists():
            problems.append('ambiguous plain and compressed log: '+path.name)
        path = packed
    if not path.exists():
        problems.append('missing ' + path.name)
        return []
    result = []
    with path.open('rb') as file:
        try:
            source = compressed_lines(file) if path.name.endswith('.z') else file
            for number, line in enumerate(source, 1):
                try:
                    value = json.loads(line.decode('utf-8'))
                    if not isinstance(value, dict):
                        raise ValueError('not an object')
                    result.append(value)
                except (ValueError, UnicodeError):
                    problems.append(f'{path.name}:{number}: incomplete or invalid JSON')
        except (ValueError, zlib.error, OSError) as error:
            problems.append(f'{path.name}: compressed log invalid: {error}')
    return result


def timestamp(record):
    return record.get('startUs', record.get('atUs', record.get('endUs', 0)))


def pose_upload_timings(cpu):
    names=('render-pose-buffer-update','render-pose-end-pipeline','render-pose-stale-errors',
           'render-pose-bind','render-pose-subdata','render-pose-unbind-check')
    result={}
    for name in names:
        rows=[r for r in cpu if r.get('type')=='scope' and r.get('role')==1 and r.get('name')==name]
        calls=sum(r.get('value0',0) for r in rows)
        elapsed=sum(r.get('elapsedUs',0) for r in rows)
        result[name]=dict(available=bool(rows) and calls>0,records=len(rows),calls=calls,
            totalElapsedUs=elapsed if rows else None,meanCallUs=elapsed/calls if calls else None,
            maximumCallUs=max((r.get('value1',0) for r in rows),default=None))
    return dict(stages=result,subdataBreakdownAvailable=result['render-pose-subdata']['available'],
        slowSubdata=[{k:r.get(k) for k in ('generation','sequence','startUs','value0','value1')}
                     for r in cpu if r.get('name')=='render-pose-slow-subdata'],
        interpretation='Elapsed call durations include internal waits. SubData can replace recently used storage in pinned VitaGL; these timings do not separately measure allocation, copying or retirement. Nested scope totals are not additive.')


def ordinary_world_lighting(cpu):
    frames=defaultdict(lambda:defaultdict(list))
    first_source={};warmup=[]
    for r in cpu:
        if r.get('name')=='map-to-first-source-snapshot-us':
            first_source.setdefault(r.get('generation'),[]).append(timestamp(r))
    for r in cpu:
        if r.get('name','').startswith('render-simple-world-'):
            boundary=first_source.get(r.get('generation'),[])
            # Target warmup draws share sequence zero before any source
            # snapshot exists. Keep them separate from ordinary-play frames;
            # duplicate evidence during actual play must still fail.
            if r.get('sequence')==0 and len(boundary)==1 and timestamp(r)<boundary[0]:
                warmup.append(r);continue
            frames[(r.get('generation'),r.get('sequence'))][r['name']].append(r)
    invalid=[];actual=eligible=0;observed=0;unavailable=0
    for (generation,sequence),items in sorted(frames.items()):
        names=('render-simple-world-draws','render-simple-world-state','render-simple-world-invalid')
        reason=None
        if any(len(items[n])!=1 for n in names):reason='missing or duplicate ordinary lighting evidence'
        else:
            draws,state,errors=[items[n][0] for n in names]
            if state['value0'] not in (0,1) or state['value1'] not in range(4):reason='invalid lighting state'
            elif errors['value0'] or draws['value0']!=(draws['value1'] if state['value0'] and state['value1']==3 else 0):reason='wrong actual shader dispatch'
            elif min(draws['value0'],draws['value1'])<0:reason='invalid draw count'
            if state['value0'] and state['value1']!=3:unavailable+=1
            actual+=draws['value0'];eligible+=draws['value1'];observed+=draws['value1']>0
        if reason:invalid.append(dict(generation=generation,sequence=sequence,reason=reason))
    return dict(available=bool(frames),frames=len(frames),framesWithEligibleDraws=observed,
        preSourceWarmupRecords=len(warmup),
        actualSimpleDraws=actual,eligibleDraws=eligible,unavailableShaderFrames=unavailable,
        dispatchEvidencePassed=bool(observed) and not invalid and not unavailable,
        invalidFrames=invalid,visualQualityVerified=False)


def render_pipeline_evidence(cpu):
    frames=defaultdict(lambda:defaultdict(list))
    for r in cpu:
        if r.get('sequence',0)>0 and r.get('name','').startswith('render-pipeline-'):
            frames[(r.get('generation'),r['sequence'])][r['name']].append(r)
    invalid=[];observed=Counter()
    lanes=('family','fast-color','fast-clear','weapon','world-alpha','weapon-prep')
    required=['render-pipeline-'+n for n in lanes]+['render-pipeline-invalid','render-pipeline-weapon-pack','render-pipeline-weapon-bytes']
    for key,items in sorted(frames.items()):
        reason=None
        if any(len(items[n])!=1 for n in required):reason='missing or duplicate pipeline evidence'
        else:
            for lane in lanes:
                row=items['render-pipeline-'+lane][0]
                if min(row['value0'],row['value1'])<0 or row['value0']!=row['value1']:reason='actual optimized dispatch differs'
                if row['value1']>0:observed[lane]+=1
            pack=items['render-pipeline-weapon-pack'][0];byte=items['render-pipeline-weapon-bytes'][0]
            if pack['value0']>0:
                observed['packed-weapon']+=1
                if byte['value1']!=1 or byte['value0']!=pack['value1']*44:reason='packed weapon data unavailable or wrong size'
            if items['render-pipeline-invalid'][0]['value0']:reason='pipeline validation error'
        if reason:invalid.append(dict(generation=key[0],sequence=key[1],reason=reason))
    ring=[r for r in cpu if r.get('name')=='render-pose-ring-use' and r.get('sequence',0)>0]
    fallback=sum(r['value1'] for r in ring)
    return dict(available=bool(frames),frames=len(frames),observedFrames=dict(observed),invalidFrames=invalid,
        ordinaryPipelineObserved=bool(frames) and not invalid and all(observed[n]>0 for n in (*lanes,'packed-weapon')),
        ringRecords=len(ring),ringUploads=sum(r['value0'] for r in ring),ringFallbacks=fallback,
        ringNoFallbackObserved=bool(ring) and any(r['value0']>0 for r in ring) and fallback==0,
        reservationFailures=sum(1 for r in cpu if r.get('name')=='pose-buffer-ring-ready' and not r['value0']),
        extraGpuStorageMaximum=max((r['value0'] for r in cpu if r.get('name')=='pose-buffer-ring-memory'),default=None),
        hardwareQualified=False)


def rate(progress):
    if len(progress) < 2:
        return {'serverHz': None, 'clientHz': None, 'seconds': 0, 'counterRegression': False}
    progress = sorted(progress, key=timestamp)
    first, last = progress[0], progress[-1]
    seconds = (timestamp(last) - timestamp(first)) / 1e6
    backwards = any(b['value0'] < a['value0'] or b['value1'] < a['value1']
                    for a, b in zip(progress, progress[1:]))
    return {'seconds': seconds, 'serverHz': (last['value0'] - first['value0']) / seconds if seconds else None,
            'clientHz': (last['value1'] - first['value1']) / seconds if seconds else None,
            'counterRegression': backwards}


def analyze(folder):
    folder = Path(folder)
    problems = []
    cpu = read(folder / 'cpu-perf.jsonl', problems)
    loading = read(folder / 'loading-stages.jsonl', problems)
    frames = read(folder / 'vita-perf.jsonl', problems)
    stages = read(folder / 'script-stages.jsonl', problems)
    health = [r for r in cpu if r.get('type') in ('recording-health', 'end')]
    if not health:
        problems.append('recording health unavailable')
    dropped = max((r.get('dropped', 0) for r in health), default=0)
    failed = any(r.get('ioFailed') for r in health)
    if dropped or failed:
        problems.append('recording overflow or failure invalidates the run')
    by_name = defaultdict(list)
    for record in cpu + loading:
        if record.get('name'):
            by_name[record['name']].append(record)
    shipment_tests = []
    for request in by_name['shipment-test-request']:
        run_id, requested_at = request.get('value0'), timestamp(request)
        def first(name):
            return min((r for r in by_name[name] if r.get('value0') == run_id and timestamp(r) >= requested_at),
                       key=timestamp, default=None)
        begin = first('shipment-test-begin')
        endings = [r for name in ('shipment-test-complete', 'shipment-test-cancelled', 'shipment-test-failed')
                   if (r := first(name)) is not None]
        ending = min(endings, key=timestamp, default=None)
        start, end = timestamp(begin) if begin else None, timestamp(ending) if ending else None
        elapsed = ending.get('value1') if ending else None
        phase_records = [r for r in by_name['shipment-test-phase'] if begin and ending
                         and r.get('generation') == begin.get('generation') and start <= timestamp(r) <= end]
        phases = {r.get('value0') for r in phase_records}
        skipped = first('shipment-test-skipped-phases')
        complete = bool(begin and ending and ending['name'] == 'shipment-test-complete'
                        and isinstance(elapsed, (int, float)) and elapsed >= 180000000 and end-start >= 180000000)
        shown = [r for r in frames if r.get('type') == 'play-frame' and begin and ending
                 and start <= timestamp(r) <= end and r.get('intervalUs', 0) > 0]
        all_phases = phases == set(range(60)) and skipped is not None and skipped.get('value1') == 0
        shipment_tests.append({'run': run_id, 'started': begin is not None,
                               'outcome': ending['name'].removeprefix('shipment-test-') if ending else 'incomplete',
                               'elapsedSeconds': elapsed / 1e6 if elapsed is not None else None,
                               'threeMinutesComplete': complete, 'phasesObserved': len(phases),
                               'skippedPhases': skipped.get('value1') if skipped else None,
                               'allInputPhasesObserved': all_phases, 'renderedFrameSamples': len(shown),
                               'playableFrameIntervals': distribution([r['intervalUs'] for r in shown]),
                               'captureComplete': bool(complete and all_phases and shown and not problems),
                               'hardwareQualified': False})
    diagnostics = []
    for begin in by_name['cpu-diagnostic-begin']:
        gen, start = begin.get('generation'), timestamp(begin)
        endings = [r for r in by_name['cpu-diagnostic-end'] if r.get('generation') == gen and timestamp(r) > start]
        if not endings:
            diagnostics.append({'generation': gen, 'complete': False, 'cpuMetricsMet': False})
            continue
        ending = min(endings, key=timestamp)
        end = timestamp(ending)
        def during(name):
            if name in ('render-preparation-us', 'cpu-preparation-latency-us') and 'sequence' in begin and 'sequence' in ending:
                # A copied snapshot may finish after the source stops the test.
                # Correlate by originating update, not the late presentation time.
                return [r for r in by_name[name] if r.get('generation') == gen
                        and begin['sequence'] <= r.get('sequence', -1) < ending['sequence']]
            return [r for r in by_name[name] if r.get('generation') == gen and start <= timestamp(r) <= end]
        progress = during('source-progress')
        frequencies = rate(progress)
        prep = distribution([r['value0'] for r in during('cpu-preparation-us')])
        render_prep = distribution([r['value0'] for r in during('render-preparation-us')])
        prep_latency = distribution([r['value0'] for r in during('cpu-preparation-latency-us')])
        presentation = distribution([r['value1'] for r in during('render-preparation-us')])
        late = during('source-deadline-lateness')
        lateness = distribution([r['value0'] for r in late])
        tail_lateness = [r['value0'] for r in late if timestamp(r) >= end - 1e6]
        queue_failures = [r for r in cpu if start <= timestamp(r) <= end and any(
            token in r.get('name', '') for token in ('overflow', 'exhausted', 'backpressure', 'owner-violation', 'snapshot-rejected'))]
        prepared = (prep['p95Us'] is not None and prep['p95Us'] <= 25000
                    and render_prep['p95Us'] is not None and render_prep['p95Us'] <= 25000
                    and prep_latency['p95Us'] is not None and prep_latency['p95Us'] <= 25000)
        clocks = frequencies['seconds'] >= 29 and not frequencies['counterRegression']
        if clocks:
            tolerance = 1.1 / frequencies['seconds']
            clocks = abs(frequencies['serverHz'] - 20) <= tolerance and abs(frequencies['clientHz'] - 30) <= tolerance
        debt_settled = bool(tail_lateness) and max(tail_lateness) < 16667
        diagnostics.append({'generation': gen, 'mode': begin.get('value0'), 'complete': True,
                            'wallSeconds': (end-start)/1e6, 'rates': frequencies, 'preparation': prep,
                            'renderPreparation': render_prep, 'presentationElapsed': presentation,
                            'preparationLatency': prep_latency,
                            'lateness': lateness, 'deadlineMissSamples': sum(r['value0'] > 1000 for r in late),
                            'backlogSettled': debt_settled, 'queueFailures': len(queue_failures),
                            'audioUnderruns': len(during('audio-underrun')),
                            'cpuMetricsMet': bool(begin.get('value0') in (1, 2) and prepared and clocks and debt_settled and not queue_failures and not problems)})
    stall_tests = []
    for stall in by_name['injected-renderer-stall']:
        start = timestamp(stall)
        end = start + stall.get('elapsedUs', 0)
        progress = [r for r in by_name['source-progress'] if r.get('generation') == stall.get('generation') and start <= timestamp(r) <= end]
        server = progress[-1]['value0'] - progress[0]['value0'] if len(progress) > 1 else 0
        client = progress[-1]['value1'] - progress[0]['value1'] if len(progress) > 1 else 0
        stall_tests.append({'generation': stall.get('generation'), 'elapsedUs': end-start,
                            'serverTicksDuringStall': server, 'clientUpdatesDuringStall': client,
                            'continued': server >= max(1, math.floor((end-start)/50000)-1)
                                         and client >= max(1, math.floor((end-start)/33333)-1)})
    viewport = {}
    play_frames = [r for r in frames if r.get('type') == 'play-frame']
    for bit, field in ((1, 'worldViewport'), (2, 'propViewport'), (4, 'viewmodelViewport')):
        observed = [r for r in play_frames if r.get('viewportSeen', 0) & bit]
        mismatches = 0
        for r in observed:
            expected = [0, 0] + (r.get('viewmodel', [960, 544]) if bit == 4 else r.get('world', []))
            mismatches += r.get(field) != expected
        viewport[field] = {'available': bool(observed), 'samples': len(observed), 'mismatches': mismatches,
                           'passed': bool(observed) and mismatches == 0}
    exclusive = defaultdict(int)
    for r in cpu:
        if r.get('type') == 'scope':
            exclusive[r.get('name', '?')] += r.get('exclusiveUs', 0)
    launches = [r['value0'] for r in by_name['launch-to-main-us']]
    main_at = min((timestamp(r) for r in by_name['launch-to-main-us']), default=None)
    early_decode = [r for r in by_name['package-decode-and-construct'] if main_at and timestamp(r) < main_at]
    reloads = [{'generation': r.get('generation'), 'elapsedUs': r.get('elapsedUs')} for r in by_name['map-load-total']]
    load_counts = Counter(r.get('name') for r in loading if r.get('type') == 'loading-stage')
    return {'schema': 1, 'folder': str(folder.resolve()), 'hardwareQualified': False, 'cpuReady': False,
            'qualification': 'Physical provenance, parity, visual checks, suspend/resume and soak acceptance must be reviewed separately.',
            'problems': problems, 'recording': {'dropped': dropped, 'ioFailed': failed, 'available': bool(health)},
            'shipmentTests': shipment_tests, 'diagnostics': diagnostics, 'rendererStalls': stall_tests, 'viewports': viewport,
            'worldScalingTests': analyze_world_scaling(cpu, problems),
            'poseUploadTimings': pose_upload_timings(cpu),
            'ordinaryWorldLighting': ordinary_world_lighting(cpu),
            'renderPipeline': render_pipeline_evidence(cpu),
            'renderPassTests': (render_tests := analyze_render_passes(cpu, problems)),
            'graphicsProfiles': analyze_graphics_profile(cpu, render_tests),
            'propWorkloadTests': analyze_prop_workload(cpu, problems),
            'materialCostTests': analyze_material_cost(cpu, problems),
            'playableFrameIntervals': distribution([r['intervalUs'] for r in play_frames if r.get('intervalUs', 0) > 0]),
            'memory': {'heapSamples': len(by_name['heap-owned-bytes']),
                       'heapMaximumBytes': max((r['value0'] for r in by_name['heap-owned-bytes']), default=None),
                       'arenaMaximumBytes': max((r['value1'] for r in by_name['source-arena-bytes']), default=None),
                       'platformAvailable': bool(by_name['platform-memory-bytes']),
                       'boundedSoakVerified': False},
            'loading': {'launchToMainUs': launches, 'mapLoads': reloads,
                        'firstSourceSnapshotUs': [r['value0'] for r in by_name['map-to-first-source-snapshot-us']],
                        'firstPlayablePresentUs': [r['value0'] for r in by_name['map-to-first-playable-present-us']],
                        'selectedMapDecodeBeforeMain': len(early_decode) if main_at is not None else None,
                        'shaderEvents': {name: count for name, count in load_counts.items() if name and 'shader' in name},
                        'programCreations': [{'name': r['name'], 'generation': r.get('generation'), 'startUs': timestamp(r)}
                                             for r in loading if r.get('name', '').endswith('-program-create')],
                        'mapCompletions': sum(r.get('stage') == 'map_load_complete' for r in stages),
                        'mapFailures': sum(r.get('stage') == 'map_reload_failure' for r in stages)},
            'exclusiveTotalsUs': dict(sorted(exclusive.items(), key=lambda item: -item[1]))}


def compare_loading(reference, candidate):
    def compare(values_a, values_b):
        if not values_a or not values_b:
            return {'available': False, 'improvementBeyondObservedVariation': False}
        return {'available': True, 'referenceSamples': len(values_a), 'candidateSamples': len(values_b),
                'referenceMedianUs': statistics.median(values_a), 'candidateMedianUs': statistics.median(values_b),
                'referenceRangeUs': [min(values_a), max(values_a)], 'candidateRangeUs': [min(values_b), max(values_b)],
                'improvementBeyondObservedVariation': len(values_a) >= 3 and len(values_b) >= 3 and max(values_b) < min(values_a)}
    return {'launches': compare([v for r in reference for v in r['loading']['launchToMainUs']],
                                [v for r in candidate for v in r['loading']['launchToMainUs']]),
            'mapLoads': compare([v['elapsedUs'] for r in reference for v in r['loading']['mapLoads']],
                               [v['elapsedUs'] for r in candidate for v in r['loading']['mapLoads']])}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, action='append', required=True)
    parser.add_argument('--reference', type=Path, action='append', default=[])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = {'runs': [analyze(path) for path in args.run], 'hardwareQualified': False}
    if args.reference:
        result['reference'] = [analyze(path) for path in args.reference]
        result['loadingComparison'] = compare_loading(result['reference'], result['runs'])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(args.output.resolve())
