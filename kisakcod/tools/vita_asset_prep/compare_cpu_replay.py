"""Compare source observations only when actual timestamped commands match.

The CPU stimulus alone is not deterministic replay proof. This tool rejects
different commands, incomplete tapes, missing source observations and damaged
recording. The explicit source-tape harness is separate from the ordinary CPU
stimulus. It never certifies all gameplay from a subset of observed state.
"""
import argparse
import json
from pathlib import Path
from analyze_cpu import read


def capture(folder, number, problems):
    folder = Path(folder)
    records = read(folder / 'cpu-replay.jsonl', problems)
    health = [r for r in read(folder / 'cpu-perf.jsonl', problems)
              if r.get('type') in ('recording-health', 'end')]
    if not health or any(r.get('dropped', 0) or r.get('ioFailed') for r in health):
        problems.append('recording health missing or invalid')
    records = [r for r in records if r.get('capture') == number]
    if not records or records[0].get('type') != 'begin' or records[-1].get('type') != 'end' or not records[-1].get('complete'):
        problems.append('capture is missing, cancelled or incomplete')
    for kind, owner in (('command', None), ('state', 'server'), ('state', 'client'), ('world', None), ('event', None)):
        if not any(r.get('type') == kind and (not owner or r.get('owner') == owner) for r in records):
            problems.append('missing observation: ' + kind + ('/' + owner if owner else ''))
    if records and records[0].get('stimulus') == 'source-input-tape-v1':
        begin, end = records[0], records[-1]
        steps = [r for r in records if r.get('type') == 'replay-step']
        count = begin.get('tapeRecords', 0)
        if (count <= 0 or count > 4096 or len(steps) != count or end.get('tapeStep') != count
                or end.get('tapeCrc') != begin.get('tapeCrc')
                or end.get('tapeRecords') != count or begin.get('tapeStep') != 0):
            problems.append('input tape was not consumed exactly once in full')
        previous = None
        for index, row in enumerate(steps, 1):
            elapsed = row.get('elapsedMs', 0)
            if (row.get('step') != index or not 0 < elapsed <= 1000
                    or (previous is not None and row.get('time', 0) != previous + elapsed)):
                problems.append('input tape timestamps or step order are invalid')
                break
            previous = row.get('time', 0)
        reads = [r for r in records if r.get('type') == 'replay-input-read']
        if (not reads or end.get('inputReads') != len(reads)
                or [r.get('read') for r in reads] != list(range(1, len(reads) + 1))
                or any(not 1 <= r.get('step', 0) <= count for r in reads)):
            problems.append('source input consumption evidence is incomplete')
        for owner in ('server', 'client'):
            if not any(r.get('type') == 'state' and r.get('owner') == owner
                       and r.get('weaponIndex', 0) > 0 and r.get('health', 0) > 0 for r in records):
                problems.append('no living armed source player: ' + owner)
    return records


def compare(reference, candidate, reference_capture=1, candidate_capture=1):
    problems = []
    a = capture(reference, reference_capture, problems)
    b = capture(candidate, candidate_capture, problems)
    def canonical(record):
        return {k: v for k, v in record.items() if k not in ('atUs', 'generation', 'sequence', 'capture')}
    commands_a = [canonical(r) for r in a if r.get('type') == 'command']
    commands_b = [canonical(r) for r in b if r.get('type') == 'command']
    identical = bool(commands_a) and commands_a == commands_b
    if not identical:
        problems.append('actual timestamped command streams differ; this is not an identical-input comparison')
    rows_a, rows_b = list(map(canonical, a)), list(map(canonical, b))
    mismatch = next(({'index': i, 'reference': left, 'candidate': right}
                     for i, (left, right) in enumerate(zip(rows_a, rows_b)) if left != right), None)
    if mismatch is None and len(a) != len(b):
        mismatch = {'index': min(len(a), len(b)), 'reason': 'record counts differ'}
    if mismatch:
        problems.append('source state, RNG, event ordering or capture context differs')
    controlled = bool(a and b and a[0].get('stimulus') == 'source-input-tape-v1'
                      and b[0].get('stimulus') == 'source-input-tape-v1')
    return {'schema': 1, 'reference': str(Path(reference).resolve()), 'candidate': str(Path(candidate).resolve()),
            'referenceRecords': len(a), 'candidateRecords': len(b), 'identicalTimestampedCommands': identical,
            'observedFieldsMatch': bool(identical and not mismatch and not problems),
            'controlledTapeReplay': controlled,
            'observedReplayMatch': bool(controlled and identical and not mismatch and not problems),
            'firstMismatch': mismatch, 'problems': problems, 'gameplayParityQualified': False,
            'scope': 'Observed player movement/weapon/ammunition/event state, selected entity collision state, RNG and event ordering. The tape uses identical 30 Hz source calls and virtual source time; ordinary scheduling, physical timing, audio quality and unobserved source state remain separate gates.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--reference-capture', type=int, default=1)
    parser.add_argument('--candidate-capture', type=int, default=1)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = compare(args.reference, args.candidate, args.reference_capture, args.candidate_capture)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(args.output.resolve())
    raise SystemExit(0 if result['observedFieldsMatch'] else 1)
