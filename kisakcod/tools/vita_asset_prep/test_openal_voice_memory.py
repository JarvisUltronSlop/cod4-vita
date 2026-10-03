"""Exercise the actual patched allocator with injected allocation failures."""
import hashlib
import json
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parents[3]
pin=json.loads((root/'kisakcod/dependencies/vita/vita-dependencies.json').read_text())['openalSoft']
name='openal-1.19.1-'+pin['patchSha256'][:12]+'-'+pin['hostToolsPatchSha256'][:12]+'-'+pin['voiceMemoryPatchSha256'][:12]
source=root/'build/vita/dependencies'/name/'source'
manifest=json.loads((source.parent/'source-hashes.json').read_text())
for path in ('Alc/ALc.c','OpenAL32/alSource.c','OpenAL32/Include/alMain.h'):
    assert hashlib.sha256((source/path).read_bytes()).hexdigest()==manifest[path]
alc=(source/'Alc/ALc.c').read_text()
function=alc.split('bool AllocateVoices(',1)[1].split('\n}\n',1)[0]
target=root/'build/host-openal-voice-memory';target.mkdir(exist_ok=True)
(target/'openal_voice_function.inc').write_text('bool AllocateVoices('+function+'\n}\n')
# These callers must propagate failure, rather than merely checking the allocator.
assert 'if(!AllocateVoices(ALContext, 64, device->NumAuxSends))' in alc
assert 'alcSetError(device, ALC_OUT_OF_MEMORY)' in alc
assert 'if(!AllocateVoices(context, context->MaxVoices, old_sends))' in alc
assert 'if(!AllocateVoices(context, newcount, device->NumAuxSends))' in (source/'OpenAL32/alSource.c').read_text()
test=root/'kisakcod/tests/vita_portable/openal_voice_memory_tests.cpp'
(target/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(OpenALVoiceMemoryTests LANGUAGES CXX)
add_executable(openal-voice-memory "{test.as_posix()}")
target_include_directories(openal-voice-memory PRIVATE "{target.as_posix()}")
target_compile_features(openal-voice-memory PRIVATE cxx_std_20)
''')
subprocess.run(['cmake','-S',str(target),'-B',str(target/'build')],check=True)
subprocess.run(['cmake','--build',str(target/'build'),'--config','Release'],check=True)
subprocess.run([str(target/'build/Release/openal-voice-memory.exe')],check=True)
