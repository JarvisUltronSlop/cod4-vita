"""Validate run56 compiled-program and actual bound-texture evidence."""
import argparse
import json
from pathlib import Path
from analyze_cpu import read

PROGRAMS = {1: 'opaque-world', 2: 'world-lifetime', 3: 'transparent-world'}
ROLES = ['colour', 'normal', 'specular', 'primary-lightmap', 'secondary-lightmap', 'reflection']
FORMATS = {0: 'U8', 0x0c000000: 'U8U8U8U8', 0x85000000: 'BC1', 0x86000000: 'BC2', 0x87000000: 'BC3'}


def analyze_resources(rows):
    records = [r for r in rows if r.get('name', '').startswith('gpu-audit-')]
    if not records:
        return {'available': False, 'complete': False, 'problems': ['No resource audit records']}
    world_precision = any(r.get('name')=='render-probe-world-precision' for r in rows)
    specular_split = not world_precision and any(r.get('name')=='world-specular-split-ready' or (r.get('name')=='render-probe-kind' and r.get('value0')==29) or r.get('name','').startswith(('gpu-audit-p11-','gpu-audit-p12-')) for r in rows)
    component_names = ('decal-highlight-off','decal-reflection-off') if specular_split else ('decal-secondary-off','decal-specular-reflection-off')
    world_components = not world_precision and (specular_split or any(r.get('name')=='render-probe-world-components' for r in rows))
    world_gloss = not world_precision and (not world_components and any(r.get('name')=='render-probe-world-gloss' for r in rows))
    world_basis = not world_precision and (not world_components and not world_gloss and any(r.get('name') in ('world-basis-ready','render-probe-world-basis') for r in rows))
    world_math = not world_precision and (any(r.get('name') in ('world-math-ready','render-probe-world-math') for r in rows))
    inplace = any(r.get("name") == "lightmap-layout-inplace-schema" for r in rows)
    layout = inplace or any(r.get("name") in ("render-probe-lightmap-layout", "lightmap-layout-upload", "lightmap-layout-batch-ready") for r in rows)
    problems, indexed = [], {}
    for r in records:
        key = (r.get('generation', 0), r['name'][10:])
        if key in indexed:
            problems.append('Duplicate audit record: ' + str(key))
        indexed[key] = r
        if key[1].endswith('-unavailable'):
            problems.append('Unavailable metadata: ' + str(key))

    def pair(generation, name):
        record = indexed.get((generation, name))
        if record is None:
            problems.append('Missing ' + str((generation, name)))
            return None
        return [record['value0'], record['value1']]

    def digest(generation, key, field):
        parts = [pair(generation, key + '-' + field + str(i)) for i in range(2)]
        if any(p is None for p in parts):
            return None
        return b''.join(v.to_bytes(8, 'little') for p in parts for v in p).hex()

    programs = {}
    selected_programs = {1:'opaque-world',3:'transparent-world',4:'opaque-world-math',5:'transparent-world-math'} if world_math else PROGRAMS
    if world_basis:selected_programs={1:'opaque-world',3:'transparent-world',6:'opaque-world-basis',7:'transparent-world-basis'}
    if world_gloss:selected_programs={1:'opaque-world',3:'transparent-world',8:'world-gloss'}
    if world_components:selected_programs={1:'opaque-world',3:'transparent-world',(11 if specular_split else 9):component_names[0],(12 if specular_split else 10):component_names[1]}
    if world_precision:selected_programs={1:'opaque-world',3:'transparent-world',13:'opaque-world-precision',14:'transparent-world-precision'}
    for number, name in selected_programs.items():
        if layout and number == 2:
            continue
        key = f'p{number}'
        generations = {g for g, n in indexed if n == key + '-program'}
        if len(generations) != 1:
            problems.append('Missing or ambiguous program identity: ' + name)
            continue
        generation = next(iter(generations))
        identity = pair(generation, key + '-program')
        stages = {}
        for stage in ('v', 'f'):
            prefix = key + '-' + stage
            fields = {k: pair(generation, prefix + '-' + k) for k in
                      ('size-flags', 'registers', 'instructions', 'buffers', 'texture-flags', 'compiler')}
            hashes = {k: digest(generation, prefix, k) for k in ('binary-hash', 'code-hash')}
            if any(v is None for v in fields.values()) or any(v is None for v in hashes.values()):
                continue
            pa_sa, temp = fields['registers']
            if bool(fields['size-flags'][1] & 1) != (stage == 'f') or fields['compiler'][1] != 1:
                problems.append('Invalid shader stage/schema: ' + prefix)
            stages[stage] = dict(primaryRegisters=pa_sa & 0xffffffff, secondaryRegisters=pa_sa >> 32,
                                 temporaryRegisters=temp & 0xffffffff, secondaryTemporaryRegisters=temp >> 32,
                                 primaryInstructions=fields['instructions'][0], secondaryInstructions=fields['instructions'][1],
                                 scratchBufferCount=fields['buffers'][0], threadBufferCount=fields['buffers'][1],
                                 binaryBytes=fields['size-flags'][0], flags=fields['size-flags'][1],
                                 compilerVersion=fields['compiler'][0], textureFlags=fields['texture-flags'], **hashes)
        programs[name] = {'glProgram': identity[0], 'serializedBytes': identity[1], 'stages': stages}
    captures = []
    generations = sorted({g for g, n in indexed if n.startswith('s')})
    if not generations:
        problems.append('No actual world texture observations')
    for generation in generations:
        for phase in ((1,2,3,4,5) if world_components else (1,2,3)):
            for surface in (261, 507, 588):
                prefix = f's{surface}-p{phase}'
                program = pair(generation, prefix + '-program')
                expected_name = 'transparent-world' if surface == 588 else 'world-lifetime' if phase == 2 and not layout else 'opaque-world'
                if world_math: expected_name = ('transparent-world' if surface==588 else 'opaque-world') + ('-math' if phase==2 else '')
                if world_precision:expected_name=('transparent-world' if surface==588 else 'opaque-world')+('-precision' if phase==2 else '')
                basis=None
                if world_basis:
                    basis=pair(generation,prefix+'-basis')
                    if not basis or basis[0] not in (0,1) or basis[1]!=1:problems.append('Missing or invalid constant-basis certificate: '+prefix)
                    expected_name=('transparent-world' if surface==588 else 'opaque-world')+('-basis' if phase==2 and basis and basis[0] else '')
                gloss=None
                if world_gloss:
                    gloss=pair(generation,prefix+'-gloss')
                    if gloss!=[233 if surface==507 else 0,233]:problems.append('Missing or unexpected gloss certificate: '+prefix)
                    expected_name='world-gloss' if phase==2 and surface==507 else 'transparent-world' if surface==588 else 'opaque-world'
                component=None
                if world_components:
                    component=pair(generation,prefix+'-component')
                    variant=(1 if phase==2 else 2 if phase==4 else 0) if surface==588 else 0
                    if component!=[variant,int(surface==588)]:problems.append('Invalid decal component target or selection: '+prefix)
                    expected_name=(component_names[0] if variant==1 else component_names[1] if variant==2 else 'transparent-world') if surface==588 else 'opaque-world'
                expected = programs.get(expected_name, {}).get('glProgram')
                if not program or not expected or program != [expected, expected]:
                    problems.append('Wrong or missing actual program: ' + prefix)
                textures = []
                for unit, role in enumerate(ROLES):
                    key = prefix + f'-u{unit}'
                    fields = {k: pair(generation, key + '-' + k) for k in
                              ('binding', 'dimensions', 'format-type', 'filter', 'mips', 'address', 'lod-gamma')}
                    if any(v is None for v in fields.values()):
                        continue
                    if not fields['binding'][0] or fields['binding'][0] != fields['binding'][1] or min(fields['dimensions']) <= 0:
                        problems.append('Invalid binding or dimensions: ' + key)
                    if layout and unit in (3, 4):
                        expected_layout = 0 if phase == 2 else 0x60000000
                        if (fields['format-type'][1] != expected_layout or fields['mips'] != [1, 0]
                                or fields['filter'] != [1, 1] or fields['address'] != [2, 2]):
                            problems.append('Lightmap layout or corrected sampler differs: ' + key)
                    fmt = fields['format-type'][0]
                    textures.append(dict(unit=unit, role=role, glTexture=fields['binding'][0],
                                         width=fields['dimensions'][0], height=fields['dimensions'][1],
                                         formatHex=f'0x{fmt:08x}', baseFormat=FORMATS.get(fmt & 0xff000000, 'unmapped'),
                                         layoutHex=f'0x{fields["format-type"][1]:08x}',
                                         minFilter=fields['filter'][0], magFilter=fields['filter'][1],
                                         gxmMipmapCount=fields['mips'][0], mipFilter=fields['mips'][1],
                                         addressModes=fields['address'], lodBiasEncoded=fields['lod-gamma'][0], gammaMode=fields['lod-gamma'][1]))
                captures.append(dict(generation=generation, phase=phase, surface=surface, program=program, textures=textures,**({'component':component} if world_components else {}),**({'constantGloss':gloss[0] if gloss else None} if world_gloss else {}),**({'constantBasis':bool(basis and basis[0])} if world_basis else {})))
    if layout:
        for capture in captures:
            baseline = next((c for c in captures if c['generation'] == capture['generation'] and c['surface'] == capture['surface'] and c['phase'] == 1), None)
            if baseline and len(baseline['textures']) == len(capture['textures']) == 6:
                for old, new in zip(baseline['textures'], capture['textures']):
                    for key in ('width', 'height', 'formatHex', 'minFilter', 'magFilter', 'gxmMipmapCount', 'mipFilter', 'addressModes', 'lodBiasEncoded', 'gammaMode'):
                        if old[key] != new[key]:
                            problems.append('Texture input changed across layout phases: ' + key)
                    changed = new['glTexture'] != old['glTexture']
                    if changed != (not inplace and capture['phase'] == 2 and new['unit'] in (3, 4)):
                        problems.append('Unexpected texture identity across layout phases')
    component_comparison={}
    if world_components:
        for capture in captures:
            base=next((c for c in captures if c['generation']==capture['generation'] and c['surface']==capture['surface'] and c['phase']==1),None)
            if base and capture['textures']!=base['textures']:problems.append('Decal component changed texture bindings or samplers')
        for variant in component_names:
            component_comparison[variant]={}
            for stage in ('v','f'):
                base=programs.get('transparent-world',{}).get('stages',{}).get(stage)
                candidate=programs.get(variant,{}).get('stages',{}).get(stage)
                if base and candidate:
                    component_comparison[variant][stage]=dict(codeBytesIdentical=base['code-hash']==candidate['code-hash'],deltas={k:candidate[k]-base[k] for k in ('primaryInstructions','secondaryInstructions','temporaryRegisters','primaryRegisters','secondaryRegisters','scratchBufferCount')})
                    if stage=='v' and base['code-hash']!=candidate['code-hash']:problems.append('Decal component changed vertex code')
    gloss_comparison={}
    if world_gloss:
        for capture in captures:
            base=next((c for c in captures if c['generation']==capture['generation'] and c['surface']==capture['surface'] and c['phase']==1),None)
            if base and capture['textures']!=base['textures']:problems.append('World gloss changed texture or sampler inputs')
        for stage in ('v','f'):
            base=programs.get('opaque-world',{}).get('stages',{}).get(stage)
            candidate=programs.get('world-gloss',{}).get('stages',{}).get(stage)
            if base and candidate:
                gloss_comparison[stage]=dict(codeBytesIdentical=base['code-hash']==candidate['code-hash'],deltas={k:candidate[k]-base[k] for k in ('primaryInstructions','secondaryInstructions','temporaryRegisters','primaryRegisters','secondaryRegisters','scratchBufferCount')})
                if stage=='v' and base['code-hash']!=candidate['code-hash']:problems.append('World gloss changed vertex code')
    basis_comparison = {}
    if world_basis:
        for capture in captures:
            base=next((c for c in captures if c['generation']==capture['generation'] and c['surface']==capture['surface'] and c['phase']==1),None)
            if base and capture['textures']!=base['textures']:problems.append('World basis changed texture identity or sampler inputs')
            if base and capture['constantBasis']!=base['constantBasis']:problems.append('World basis certificate changed across phases')
        for family in ('opaque-world','transparent-world'):
            stages={}
            for stage in ('v','f'):
                base=programs.get(family,{}).get('stages',{}).get(stage)
                candidate=programs.get(family+'-basis',{}).get('stages',{}).get(stage)
                if base and candidate:
                    stages[stage]=dict(codeBytesIdentical=base['code-hash']==candidate['code-hash'],deltas={k:candidate[k]-base[k] for k in ('primaryInstructions','secondaryInstructions','temporaryRegisters','primaryRegisters','secondaryRegisters','scratchBufferCount')})

            basis_comparison[family]=stages
    precision_comparison = {}
    if world_precision:
        for capture in captures:
            base=next((c for c in captures if c['generation']==capture['generation'] and c['surface']==capture['surface'] and c['phase']==1),None)
            if base and capture['textures']!=base['textures']:problems.append('World precision changed texture identity or sampler inputs')
        for family in ('opaque-world','transparent-world'):
            stages={}
            for stage in ('v','f'):
                base=programs.get(family,{}).get('stages',{}).get(stage)
                candidate=programs.get(family+'-precision',{}).get('stages',{}).get(stage)
                if base and candidate:
                    stages[stage]=dict(codeBytesIdentical=base['code-hash']==candidate['code-hash'],deltas={k:candidate[k]-base[k] for k in ('primaryInstructions','secondaryInstructions','temporaryRegisters','primaryRegisters','secondaryRegisters','scratchBufferCount')})
                    if stage=='v' and base['code-hash']!=candidate['code-hash']:problems.append('World precision changed vertex code')
            precision_comparison[family]=stages
    math_comparison = {}
    if world_math:
        for capture in captures:
            base=next((c for c in captures if c['generation']==capture['generation'] and c['surface']==capture['surface'] and c['phase']==1),None)
            if base and capture['textures']!=base['textures']:problems.append('World math changed texture identity or sampler inputs')
        for family in ('opaque-world','transparent-world'):
            stages={}
            for stage in ('v','f'):
                base=programs.get(family,{}).get('stages',{}).get(stage)
                candidate=programs.get(family+'-math',{}).get('stages',{}).get(stage)
                if base and candidate:
                    stages[stage]=dict(codeBytesIdentical=base['code-hash']==candidate['code-hash'],deltas={k:candidate[k]-base[k] for k in ('primaryInstructions','secondaryInstructions','temporaryRegisters','primaryRegisters','secondaryRegisters','scratchBufferCount')})
                    if stage=='v' and base['code-hash']!=candidate['code-hash']:problems.append('World math changed vertex code')
            math_comparison[family]=stages
    comparison = {}
    for stage in ('v', 'f'):
        base = programs.get('opaque-world', {}).get('stages', {}).get(stage)
        candidate = programs.get('world-lifetime', {}).get('stages', {}).get(stage)
        if base and candidate:
            comparison[stage] = dict(codeBytesIdentical=base['code-hash'] == candidate['code-hash'],
                deltas={k: candidate[k] - base[k] for k in ('primaryInstructions', 'secondaryInstructions', 'temporaryRegisters', 'primaryRegisters', 'secondaryRegisters', 'scratchBufferCount')})
    storage = {}
    if inplace:
        schema=[r for r in rows if r.get('name')=='lightmap-layout-inplace-schema']
        registered=[r for r in rows if r.get('name')=='lightmap-layout-registered']
        verified=[r for r in rows if r.get('name')=='lightmap-layout-content-verified']
        states=[r for r in rows if r.get('name')=='lightmap-layout-content-state']
        transitions=[r for r in rows if r.get('name')=='lightmap-layout-transition-us']
        valid=[r for r in rows if r.get('name')=='lightmap-layout-transition-valid']
        ids={r['value0']:r['value1'] for r in registered}
        storage=dict(inPlace=True, registeredBytes=sum(ids.values()), workspaceBytes=128*1024, extraGpuBytes=0,
                     transitions=[{'elapsedUs':r['value0'],'swizzled':bool(r['value1'])} for r in transitions])
        if len(schema)!=1 or (schema[0]['value0'],schema[0]['value1'])!=(1,128*1024):problems.append('Invalid in-place schema or workspace')
        if len(ids)<2 or len(ids)!=len(registered):problems.append('Missing or duplicate lightmap registration')
        if len(transitions)!=2 or [r['value1'] for r in transitions]!=[1,0] or len(valid)!=2 or any(r['value0']!=1 or r['value1']!=len(ids) for r in valid):
            problems.append('Missing or failed reversible lightmap transitions')
        if len(verified)!=2*len(ids) or len(states)!=len(verified) or any(r['value1']!=ids.get(r['value0']) for r in verified):
            problems.append('Missing or failed canonical lightmap content verification')
        if sorted((r['value0'],r['value1']) for r in states)!=sorted((i,state) for i in ids for state in (0,1)):
            problems.append('Lightmap content states incomplete')
        if any(r.get('name')=='lightmap-layout-failure' for r in rows):problems.append('In-place layout failure recorded')
    elif layout:
        memory = [r for r in rows if r.get('name') == 'lightmap-layout-memory-types']
        uploads = [r for r in rows if r.get('name') == 'lightmap-layout-upload']
        verified = [r for r in rows if r.get('name') == 'lightmap-layout-verified-bytes']
        storage = dict(uploads=len(uploads), successfulUploads=sum(bool(r['value1']) for r in uploads),
                       verifiedBytes=sum(r['value0'] for r in verified), memoryTypes=[[r['value0'],r['value1']] for r in memory],
                       memoryTypesAvailable=bool(memory) and all(r['value0'] and r['value1'] for r in memory),
                       memoryTypesMatch=bool(memory) and all(r['value0'] and r['value0']==r['value1'] for r in memory))
        # Type differences do not invalidate measured rendering, but prevent
        # attributing its entire difference to layout alone.
        if len(uploads)<2 or len(uploads)!=len(verified) or any(not r['value1'] for r in uploads) or any(not r['value0'] for r in verified):
            problems.append('Missing or failed byte-verified layout uploads')
    return dict(available=True, complete=not problems, problems=problems, programs=programs,
                **({'worldComponentComparisons':component_comparison} if world_components else {}),
                **({'worldGlossComparison':gloss_comparison} if world_gloss else {}),
                **({'worldBasisComparison':basis_comparison} if world_basis else {}),
                **({'worldPrecisionComparison':precision_comparison} if world_precision else {}),
                **({'worldMathComparison':math_comparison} if world_math else {}),
                lifetimeComparison=comparison, captures=captures, **({'layoutStorage': storage} if layout else {}),
                limitations=['Static instruction/register counts are not executed cycles, occupancy, utilization or measured texture traffic.',
                             'Scratch/thread buffer counts alone do not prove register spills.',
                             'GXM mip count is descriptor metadata; cube zero denotes packed base faces, not a missing texture.',
                             'Program metadata is captured at startup; texture descriptors are sampled once per target/phase/generation.',
                             'This audit is not a new performance optimization or visual qualification.'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    errors = []
    result = analyze_resources(read(args.run / 'cpu-perf.jsonl', errors))
    result['problems'].extend(errors)
    if errors:
        result['complete'] = False
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(args.output.resolve())
