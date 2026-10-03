"""Compile the actual cache envelope from the pinned patch in portable tests."""
import argparse
import hashlib
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--patch', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--padding-patch', type=Path)
args = parser.parse_args()
patch = args.patch.read_text()
section = patch.split('--- a/source/custom_shaders.c\n', 1)[1].split('\n--- a/', 1)[0]
added = '\n'.join(line[1:] for line in section.splitlines() if line.startswith('+') and not line.startswith('+++'))
start = added.index('static GLboolean cod4_cache_enabled')
write = added.index('void vglCod4CacheWrite(', start)
brace = added.index('{', write)
depth = 1
end = brace + 1
while depth:
    depth += (added[end] == '{') - (added[end] == '}')
    end += 1
content = '// Exact cache helper from patch SHA256 ' + hashlib.sha256(args.patch.read_bytes()).hexdigest() + '\n' + added[start:end] + '\n'
if args.padding_patch:
    padding = args.padding_patch.read_text()
    additions = '\n'.join(line[1:] for line in padding.splitlines() if line.startswith(('+', ' ')) and not line.startswith('+++'))
    begin = additions.index('static GLboolean cod4_cache_program_valid(')
    brace = additions.index('{', begin)
    depth, end = 1, brace + 1
    while depth:
        depth += (additions[end] == '{') - (additions[end] == '}')
        end += 1
    helper = additions[begin:end] + '\n'
    anchor = 'typedef struct {uint32_t magic,version,bytes,kind;uint64_t checksum;} cod4_cache_header;'
    assert content.count(anchor) == 1
    content = content.replace(anchor, helper + anchor)
    original = ('    if (valid && kind == 0) valid = header.bytes >= 64\n'
                '        && sceGxmProgramGetSize(data) == header.bytes && sceGxmProgramCheck(data) == 0;')
    corrected = '    if (valid && kind == 0) valid = cod4_cache_program_valid(data,header.bytes);'
    assert content.count(original) == 1 and corrected in additions
    content = content.replace(original, corrected)
    content = '// With padding patch SHA256 ' + hashlib.sha256(args.padding_patch.read_bytes()).hexdigest() + '\n' + content
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(content)
