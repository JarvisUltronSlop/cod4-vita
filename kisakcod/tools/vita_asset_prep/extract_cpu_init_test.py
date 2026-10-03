"""Extract the exact built VitaGL startup code for fault-injection tests."""
import argparse
import hashlib
import json
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--workspace', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
root = a.workspace
base = json.loads((root / 'kisakcod/dependencies/vita/vita-dependencies.json').read_text())['vitaGL']
cache = json.loads((root / 'kisakcod/dependencies/vita/vita-cpu-cache.json').read_text())
init = json.loads((root / 'kisakcod/dependencies/vita/vita-cpu-init.json').read_text())
folder = root / 'build/vita/dependencies' / (base['revision'][:12] + '-' + base['patchSha256'][:12]
    + '-cpu-' + cache['patchSha256'][:12] + '-init-' + init['patchSha256'][:12])
hashes = json.loads((folder / 'source-hashes.json').read_text())
def read(name):
    data = (folder / 'source' / name).read_bytes()
    assert hashlib.sha256(data).hexdigest() == hashes[name], name
    return data.decode().replace('\r\n', '\n')
def function(text, declaration):
    start = text.index(declaration)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]
mem = function(read('source/utils/mem_utils.c'), 'void vgl_mem_init(')
context = function(read('source/gxm.c'), 'void init_gxm_context(')
gpu = read('source/utils/gpu_utils.c')
cpu_gc = function(gpu, 'void *gpu_alloc_mapped_aligned_unsafe_for_cpu(')
gpu_gc = function(gpu, 'void *gpu_alloc_mapped_aligned_unsafe_for_gpu(')
vgl = read('source/vgl.c')
start = vgl.index('\t// Initializing sceGxm\n', vgl.index('GLboolean vglInitWithCustomSizes('))
end = vgl.index('\t// Creating render target for the display', start)
sequence = 'bool Startup() {\n' + vgl[start:end] + '\treturn true;\n}\n'
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text('// Extracted unchanged from pinned dependency with CPU initialization patch.\n'
    + mem + '\n' + context + '\n' + cpu_gc + '\n' + gpu_gc + '\n' + sequence)
