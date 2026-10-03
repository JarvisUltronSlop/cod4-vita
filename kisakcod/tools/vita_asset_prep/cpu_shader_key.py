"""Shader-only compatibility identity; unrelated gameplay edits do not flush caches."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def compatibility(source, provenance):
    shaders = {}
    for name in ('gfx_vita_backend.cpp', 'gfx_vita_viewmodel.cpp'):
        text = (source / 'src/gfx_vita' / name).read_text()
        shaders[name] = [m.group('body') for m in re.finditer(r'R"(?P<tag>\w*)\((?P<body>.*?)\)(?P=tag)"', text, re.S)]
        shaders[name] += re.findall(r'"#define [^"\r\n]*"', text)
        shaders[name] += re.findall(r'glBindAttribLocation\s*\([^;]+;', text)
    prop_builder = source / 'src/gfx_vita/gfx_vita_prop_shader.h'
    if prop_builder.exists():
        shaders['propShaderSpecialization'] = prop_builder.read_text(encoding='utf-8')
    opaque_builder = source / 'src/gfx_vita/gfx_vita_opaque_shader.h'
    if opaque_builder.exists():
        shaders['opaqueShaderSpecialization'] = opaque_builder.read_text(encoding='utf-8')
    material_builder = source / 'src/gfx_vita/gfx_vita_material_probe.h'
    if material_builder.exists():
        shaders['materialProbeSpecialization'] = material_builder.read_text(encoding='utf-8')
    specular_builder = source / 'src/gfx_vita/gfx_vita_specular_shader.h'
    if specular_builder.exists():
        shaders['specularMathSpecialization'] = specular_builder.read_text(encoding='utf-8')
    family_builder = source / 'src/gfx_vita/gfx_vita_material_family.h'
    if family_builder.exists():
        shaders['materialFamilySpecialization'] = family_builder.read_text(encoding='utf-8')
    prefetch_builder = source / 'src/gfx_vita/gfx_vita_texture_prefetch.h'
    if prefetch_builder.exists():
        shaders['texturePrefetch'] = prefetch_builder.read_text(encoding='utf-8')
    color_fast = source / "src/gfx_vita/gfx_vita_color_fast.h"
    if color_fast.exists():
        shaders["fastColorAndClear"] = color_fast.read_text(encoding="utf-8")
    hud_shader = source / "src/gfx_vita/gfx_vita_hud_shader.h"
    if hud_shader.exists():
        shaders["hudPipeline"] = hud_shader.read_text(encoding="utf-8")
    early_alpha = source / "src/gfx_vita/gfx_vita_early_alpha.h"
    if early_alpha.exists():
        shaders["earlyAlpha"] = early_alpha.read_text(encoding="utf-8")
    post_shader = source / "src/gfx_vita/gfx_vita_world_lifetime.h"
    if post_shader.exists():
        shaders["worldLifetimeSpecialization"] = post_shader.read_text(encoding="utf-8")
    world_scaling = source / "src/gfx_vita/gfx_vita_world_scaling.h"
    if world_scaling.exists():
        shaders["worldScalingSpecialization"] = world_scaling.read_text(encoding="utf-8")
    world_precision = source / "src/gfx_vita/gfx_vita_world_precision.h"
    if world_precision.exists():
        shaders["worldPrecisionSpecialization"] = world_precision.read_text(encoding="utf-8")
    world_math = source / "src/gfx_vita/gfx_vita_world_math.h"
    if world_math.exists():
        shaders["worldMathSpecialization"] = world_math.read_text(encoding="utf-8")
    world_basis = source / "src/gfx_vita/gfx_vita_world_basis.h"
    if world_basis.exists():
        shaders["worldBasisSpecialization"] = world_basis.read_text(encoding="utf-8")
    world_gloss = source / "src/gfx_vita/gfx_vita_world_gloss.h"
    if world_gloss.exists():
        shaders["worldGlossSpecialization"] = world_gloss.read_text(encoding="utf-8")
    world_components = source / "src/gfx_vita/gfx_vita_world_components.h"
    if world_components.exists():
        shaders["worldComponentDiagnostic"] = world_components.read_text(encoding="utf-8")
    library = json.loads(provenance.read_text())
    identity = {'cacheFormat': 1, 'shaders': shaders,
                'vitaGL': library['revision'], 'basePatch': library['patchSha256'],
                'cachePatch': library['cpuCache']['patchSha256'],
                'compiler': library['compilerSha256'], 'flags': library['flags'],
                'vitaShaRK': library['vitaShaRK']['revision'],
                'options': 'default-pinned-vitagl-options;original-attribute-bindings;cache-envelope-1'}
    return hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest(), identity


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--provenance', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    digest, identity = compatibility(args.source, args.provenance)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    content = '#pragma once\ninline constexpr const char* kVitaCpuShaderCompatibility="' + digest + '";\n'
    if not args.output.exists() or args.output.read_text() != content:
        args.output.write_text(content)
    args.output.with_suffix('.json').write_text(json.dumps({'sha256': digest, **identity}, indent=2) + '\n')
