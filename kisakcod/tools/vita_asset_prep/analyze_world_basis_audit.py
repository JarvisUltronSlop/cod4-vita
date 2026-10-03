"""Read the load-only batch audit and sparse, uncalibrated file-read samples."""
import argparse
import json
from pathlib import Path

TARGETS = {261, 507, 588}
REASONS = ('constant', 'normalVaries', 'tangentVaries', 'bothVary',
           'nonfinite', 'degenerate', 'invalidIndex')


def analyze(script, loading):
    batches, problems, seen = [], [], set()
    for row in script:
        if row.get('stage') != 'world-basis-audit':
            continue
        try:
            fields = dict(item.split('=', 1) for item in row['detail'].split())
            data = {k: int(v) for k, v in fields.items()}
            key = (int(row['generation']), data['batch'])
            if key in seen:
                raise ValueError('duplicate batch')
            seen.add(key)
            n = data['triangles']
            if (data['schema'] != 1 or data['batch'] not in TARGETS or n <= 0
                    or any(v < 0 for v in data.values())
                    or sum(data[k] for k in REASONS) != n
                    or data['inputValid'] != 1 or data['invalidIndex'] != 0
                    or data['certificate'] != int(data['constant'] == n)
                    or not 1 <= data['runs'] <= n
                    or not 0 <= data['constantRuns'] <= data['runs']
                    or not 0 <= data['longestConstantRun'] <= data['constant']
                    or bool(data['constantRuns']) != bool(data['constant'])
                    or bool(data['longestConstantRun']) != bool(data['constant'])):
                raise ValueError('invalid audit accounting or certificate')
            mixed = 0 < data['constant'] < n
            if ((not mixed and data['runs'] != 1)
                    or (mixed and data['runs'] < 2)
                    or not 0 <= data['runs'] - 2 * data['constantRuns'] <= 1
                       and data['runs'] - 2 * data['constantRuns'] != -1):
                raise ValueError('invalid run accounting')
            batches.append(dict(generation=key[0], material=row.get('subject'), **data,
                                extraDrawsIfSplit=data['runs'] - 1 if mixed else 0,
                                splitCanRecoverTriangles=mixed))
        except (KeyError, TypeError, ValueError) as error:
            problems.append(str(error))
    for generation in {x[0] for x in seen}:
        if {x[1] for x in seen if x[0] == generation} != TARGETS:
            problems.append(f'generation {generation}: missing target audit')
    if not seen:
        problems.append('target audit unavailable')
    samples = [x for x in loading if x.get('name') in
               ('package-read-sample-raw', 'package-read-sample-unavailable')]
    reads = []
    for generation in sorted({x['generation'] for x in samples}):
        rows = [x for x in samples if x['generation'] == generation]
        valid = [x for x in rows if x['name'] == 'package-read-sample-raw']
        reads.append(dict(generation=generation, samples=len(rows), available=len(valid),
                          unavailable=len(rows)-len(valid),
                          sampledReadWallUs=sum(x['elapsedUs'] for x in valid),
                          sampledReadBytes=sum(x['value1'] for x in valid),
                          executionCounterDeltaRaw=sum(x['value0'] for x in valid),
                          executionCounterUnit='unvalidated', utilization=None))
    return dict(available=bool(batches), complete=bool(batches) and not problems,
                problems=problems, batches=batches, readSamples=reads,
                limitations=[
                    'Triangle/run counts do not measure pixel coverage or GPU savings.',
                    'No geometry is split by this audit; extra draw counts are hypothetical.',
                    'Read samples cover the first and every 32nd read per opened file.',
                    'Raw execution counter units are unvalidated; do not subtract from wall time.',
                    'Counter sampling has overhead and does not identify a storage-device cause.'])


if __name__ == '__main__':
    from analyze_cpu import read
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    errors = []
    report = analyze(read(args.folder/'script-stages.jsonl', errors),
                     read(args.folder/'loading-stages.jsonl', errors))
    report['problems'].extend(errors)
    report['complete'] &= not errors
    args.output.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
