"""Exercise the actual recorder writer with injectable host I/O and time."""
import argparse
from pathlib import Path
from cpu_recorder_contract import function

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
s = a.source.read_text(encoding='utf-8')
prefix = s[s.index('namespace kisak::vita {'):s.index('uint64_t VitaCpuNow()')]
# Replace only the platform I/O endpoints. Writer, enqueue and flush ordering
# are compiled verbatim; the test controls storage stalls/errors deterministically.
for old, new in [('std::fwrite', 'TestWrite'), ('std::fflush', 'TestFlush'), ('std::fclose', 'TestClose')]:
    prefix = prefix.replace(old, new)
parts = [function(s, n) for n in ['Enqueue', 'VitaCpuProfile_FlushThread', 'VitaCpuProfile_Flush']]
a.output.write_text(prefix + '\n'.join(parts) + '\n}\n', encoding='utf-8')
