"""Expose the unchanged native brush narrow phase to host differential tests."""
import argparse
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
source = a.source.read_text(encoding='utf-8')
start = source.index('void __cdecl CM_TraceThroughBrush(')
end = source.index('{', start) + 1
depth = 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(source[start:end] + '\n', encoding='utf-8')
