"""Compile and execute the actual pinned failure-path functions, with faults."""
import hashlib
import json
from pathlib import Path
import subprocess
import unittest


class ShaderFailureTests(unittest.TestCase):
    def test_actual_dependency_failure_paths(self):
        root=Path(__file__).resolve().parents[3]
        provenance=json.loads((root/'build/vita/vitaGL-cpu-shader-input/vitaGL-build.json').read_text())
        pin=json.loads((root/'kisakcod/dependencies/vita/vita-cpu-shader-failure.json').read_text())
        self.assertEqual(provenance['cpuShaderFailure'],pin)
        matches=list((root/'build/vita/dependencies').glob('*-failure-'+pin['patchSha256'][:12]+'/source/source/custom_shaders.c'))
        self.assertEqual(len(matches),1)
        data=matches[0].read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(),provenance['sourceFiles']['source/custom_shaders.c'])
        source=data.decode().replace('\r\n','\n')
        serialize=source[source.index('void *serialize_shader('):source.index('\nGLboolean unserialize_shader(')]
        compile_part=source[source.index('// COD4: compiler diagnostics'):source.index('\nvoid reset_custom_shaders')]
        compile_part=compile_part.replace(' __attribute__((weak))','').replace('__attribute__((always_inline)) ','')
        self.assertIn('if (!p->vshader->prog || !p->fshader->prog) {\n            glsl_sema_mode = VGL_MODE_POSTPONED;\n            p->status = PROG_UNLINKED;\n            return;',source)
        target=root/'build/host-shader-failure';target.mkdir(exist_ok=True)
        start=source.index('        if (!p->vshader->prog || !p->fshader->prog) {')
        end=source.index('\n        }',start)+len('\n        }')
        (target/'shader_postponed_gate.inc').write_text(source[start:end])
        (target/'shader_failure_functions.inc').write_text(serialize+compile_part)
        test=root/'kisakcod/tests/vita_portable/shader_failure_tests.cpp'
        (target/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(ShaderFailureTests LANGUAGES CXX)
add_executable(shader-failure "{test.as_posix()}")
target_include_directories(shader-failure PRIVATE "{target.as_posix()}")
target_compile_features(shader-failure PRIVATE cxx_std_20)
''')
        for cmd in [['cmake','-S',str(target),'-B',str(target/'build')],
                    ['cmake','--build',str(target/'build'),'--config','Release'],
                    [str(target/'build/Release/shader-failure.exe')]]:
            result=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
            self.assertEqual(result.returncode,0,result.stdout)


if __name__=='__main__':unittest.main()
