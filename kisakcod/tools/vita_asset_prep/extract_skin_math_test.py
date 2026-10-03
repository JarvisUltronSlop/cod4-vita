"""Extract original source scalar skinning and math for differential host tests."""
import argparse
from pathlib import Path
import re

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()


def function(text, name):
    start = re.search(r'^(?:static )?(?:void|float) __cdecl ' + name + r'\(', text, re.M).start()
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


skin = (a.source / 'gfx_d3d/r_model_skin_cpu.cpp').read_text(encoding='utf-8')
math = (a.source / 'universal/com_math.cpp').read_text(encoding='utf-8')
pack = (a.source / 'qcommon/com_pack.cpp').read_text(encoding='utf-8')
helpers = [function(math, n) for n in ['Vec3Scale', 'Vec3Mad', 'R_TransformSkelMat', 'Vec3Dot']]
helpers += [function(pack, n) for n in ['Vec3UnpackUnitVec', 'Vec2UnpackTexCoords']]
names = ['R_MultiplySkelMat', 'R_SkinXSurfaceSkinned', 'R_SkinXSurfaceWeight',
         'MatrixTransformVertexAndBasis', 'R_SkinXSurfaceWeight0', 'R_SkinXSurfaceWeight1',
         'R_SkinXSurfaceWeight2', 'R_SkinXSurfaceWeight3', 'R_SkinXSurfaceRigid']
functions = [function(skin, n).replace('reinterpret_cast<unsigned>', 'reinterpret_cast<uintptr_t>') for n in names]
declarations = [f[:f.index('{')].strip() + ';' for f in functions]
# Mark reference helpers noinline to model the existing separate translation
# units. No math modification; uintptr_t above only fixes host pointer width.
reference = ['SKIN_NOINLINE ' + f for f in helpers]
bindings = ['using kisak::vita::skinmath::' + n + ';' for n in
            ['Vec3Scale', 'Vec3Mad', 'R_TransformSkelMat', 'Vec3Dot', 'Vec3UnpackUnitVec', 'Vec2UnpackTexCoords']]
backend = (a.source / 'vita/vita_source_model_backend.cpp').read_text(encoding='utf-8')
conversion = backend[backend.index('            dest = {};'):]
conversion = conversion[:conversion.index('            dest.shadowVisibility = 1;') + len('            dest.shadowVisibility = 1;')]
# The retained non-CPU conversion path is the original reference; discard any
# nested CPU helper selection without modifying the conversion expressions.
conversion = re.sub(r'#if KISAK_VITA_CPU_RUNTIME\n.*?#else\n(.*?)#endif', r'\1', conversion, flags=re.S)
reference.append('template<class Project> bool ConvertOriginal(const GfxPackedVertex& input, GfxDrawVertex& dest, Project project) {\n'
                 + conversion.replace('Project(input.xyz', 'project(input.xyz') + '\nreturn true;\n}')
a.output.write_text('namespace reference {\n' + '\n'.join(reference + declarations + functions)
                    + '\n}\nnamespace candidate {\n' + '\n'.join(bindings + declarations + functions) + '\n}\n', encoding='utf-8')
