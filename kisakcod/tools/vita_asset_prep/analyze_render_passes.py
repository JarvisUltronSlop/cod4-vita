"""Attribute diagnostic scene-completion waits without claiming GPU utilization."""
from collections import defaultdict
import math
import struct
from analyze_pass_isolation import DEPTH_NAMES, ISOLATION_NAMES, depth_problem, isolation_problem

PASSES = ('setup', 'world', 'props-fx', 'composition', 'weapon', 'post', 'hud')


def summary(values):
    values = sorted(values)
    return {'samples': len(values), 'meanUs': sum(values)/len(values) if values else None,
            **{name: values[max(0, math.ceil(len(values)*fraction)-1)] if values else None
               for name, fraction in [('medianUs', .5), ('p95Us', .95), ('p99Us', .99), ('maxUs', 1)]}}


def analyze_render_passes(cpu, parse_problems=()):
    result = []
    grouped = defaultdict(list)
    for row in cpu:
        grouped[row.get('generation')].append(row)
    for begin in cpu:
        if begin.get('name') != 'cpu-diagnostic-begin' or begin.get('value0') not in (3, 4, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36):
            continue
        mode = begin['value0']
        comparison = mode in (4, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36)
        opaque = mode in (6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36)
        depth_order = mode == 7
        specular = mode == 9
        baseline = mode == 10
        prefetch = mode == 13
        draw_probe = mode in (14,19,24)
        current_draw_probe = mode == 24
        post_film = mode == 25
        lightmap_layout = mode == 27
        world_components = mode in (31,32)
        pass_isolation = mode == 35
        fused_composition = mode == 34
        graphics_profile = mode == 33
        specular_split = mode == 32
        phase_ids = tuple(range(1,10)) if pass_isolation else (1,2,3,4,5,6) if graphics_profile else (1,2,3,4,5) if world_components else (1,2,3)
        world_gloss = mode == 30
        world_basis = mode == 29
        world_precision = mode == 36
        world_math = mode == 28
        world_lifetime = mode == 26
        fast_draw_probe = mode == 19
        family = mode in (11,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36)
        fast_color = mode in (15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36)
        weapon_opaque = mode == 16
        hud = mode == 17
        early_alpha = mode == 18
        weapon_pipeline = mode in (20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36)
        weapon_vertices = mode in (21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36)
        weapon_specular = mode in (22,23,24,25,26,27,28,29,30,31,32,33,34,35,36)
        world_alpha = mode in (23,24,25,26,27,28,29,30,31,32,33,34,35,36)
        display = mode == 12
        generation = begin['generation']
        lightmap_inplace = lightmap_layout and any(r.get('name')=='lightmap-layout-inplace-schema' for r in grouped[generation])
        rows = [r for r in grouped[generation] if r.get('startUs', 0) >= begin['startUs']]
        depth_detail = pass_isolation or (fused_composition and any(r.get('name')=='render-composition-depth-before' for r in rows))
        anchor = None
        if family or display:
            def unique(name):
                values=[r for r in grouped[generation] if r.get('name')==name and r.get('startUs',0)<begin['startUs']]
                return values[0] if len(values)==1 else None
            xy,z_yaw,angles,ready=[unique(n) for n in ('gpu-anchor-origin-xy','gpu-anchor-origin-z-yaw','gpu-anchor-angles','gpu-anchor-source-ready')]
            def scalar(value):
                if not isinstance(value,int) or not 0<=value<=0xffffffff:return float('nan')
                return struct.unpack('<f',struct.pack('<I',value))[0]
            if all(v is not None for v in (xy,z_yaw,angles,ready)):
                origin=[scalar(xy.get('value0')),scalar(xy.get('value1')),scalar(z_yaw.get('value0'))]
                view=[scalar(angles.get('value0')),scalar(z_yaw.get('value1')),scalar(angles.get('value1'))]
                if (all(math.isfinite(v) for v in origin+view) and all(abs(v)<=131072 for v in origin)
                        and ready.get('value0',0)>0 and ready.get('value1',0)>0
                        and ready['startUs']>=max(xy['startUs'],z_yaw['startUs'],angles['startUs'])+1900000):
                    anchor={'origin':origin,'angles':view,'sourceReadyAtUs':ready['startUs']}

        endings = [r for r in rows if r.get('name') == 'cpu-diagnostic-end' and r.get('value0') == mode]
        markers = [r for r in rows if r.get('name') == 'cpu-diagnostic-capture-complete' and r.get('value0') == mode and r.get('value1') == 0]
        by_sequence = defaultdict(lambda: defaultdict(list))
        for row in rows:
            if row.get('name', '').startswith('render-') or (pass_isolation and row.get('name','').startswith(('graphics-','display-'))):
                by_sequence[row.get('sequence')][row['name']].append(row)
        phases = defaultdict(list)
        invalid = []
        observed_order = []
        for sequence, names in sorted(by_sequence.items(),key=lambda kv:(isinstance(kv[0],int),kv[0] if isinstance(kv[0],int) else -1)):
            frames = names.get('render-probe-frame', [])
            if not frames:
                if any(n.startswith('render-pass-') for n in names):
                    invalid.append({'sequence': sequence, 'reason': 'unfinished renderer frame'})
                continue
            required = ['render-probe-frame', 'render-probe-total-us', 'render-probe-swap-us', 'render-probe-geometry', 'render-probe-viewports']
            required += ['render-pass-'+p for p in PASSES]+['render-pass-'+p+'-geometry' for p in PASSES]
            if comparison:
                required += ['render-probe-kind', 'render-probe-prop-shader']
            if opaque:
                required += ['render-probe-opaque-world', 'render-probe-opaque-props', 'render-probe-opaque-invalid']
            if display:
                required += ['render-probe-camera-xy','render-probe-camera-z']
            if hud:
                required += ['render-probe-hud-draws','render-probe-hud-valid','render-probe-hud-viewport']
            if early_alpha:
                required += ["render-probe-alpha-"+n for n in ("ready","world","props","invalid")]
            if weapon_pipeline:
                required += ['render-probe-weapon-'+n for n in ('family','family-valid','cache-use','cache-hits','cache-resets')]
            if lightmap_layout:
                required += ['render-probe-lightmap-layout'+n for n in ('','-valid','-storage')]
                if lightmap_inplace: required += ['render-probe-lightmap-inplace']
            if depth_detail:required += DEPTH_NAMES
            if pass_isolation:required += ISOLATION_NAMES
            if fused_composition:
                required += ["render-probe-composition-copy", "render-probe-composition-valid"]
            if world_components:
                required += ['render-probe-world-components'+n for n in ('','-valid','-target')]
            if world_gloss:
                required += ['render-probe-world-gloss'+n for n in ('','-valid','-targets')]
            if world_basis:
                required += ['render-probe-world-basis'+n for n in ('','-valid','-targets','-seen','-triangles')]
            if world_precision:
                required += ['render-probe-world-precision'+n for n in ('','-valid','-targets')]
            if world_math:
                required += ['render-probe-world-math'+n for n in ('','-valid','-targets')]
            if world_lifetime:
                required += ['render-probe-world-lifetime'+n for n in ('','-valid','-targets')]
            if post_film:
                required += ['render-probe-post-'+n for n in ('shader','valid','state','gamma-exposure','size')]
            if world_alpha:
                required += ['render-probe-world-alpha'+n for n in ('','-valid','-target')]
            if weapon_specular:
                required += ['render-probe-weapon-'+n for n in ('specular','specular-valid','specular-maps')]
            if weapon_vertices:
                required += ['render-probe-weapon-'+n for n in ('vertices','vertex-count','vertex-table')]
            if weapon_opaque:
                required += ["render-probe-weapon-ready","render-probe-weapon-opaque","render-probe-weapon-invalid","render-probe-weapon-maps"]
            if fast_color:
                required += ["render-probe-fast-ready","render-probe-fast-color","render-probe-fast-clear","render-probe-fast-invalid"]
            if prefetch:
                required += ['render-probe-prefetch-ready','render-probe-prefetch-lightmapped','render-probe-prefetch-baked','render-probe-prefetch-invalid']
            if family:
                required += ['render-probe-family-ready','render-probe-family-lightmapped','render-probe-family-baked','render-probe-family-invalid','render-probe-camera-xy','render-probe-camera-z']
            if specular:
                required += ['render-probe-specular-ready', 'render-probe-specular-world', 'render-probe-specular-props', 'render-probe-specular-inputs']
            if depth_order:
                required += ['render-probe-depth-order', 'render-probe-depth-eligible', 'render-probe-depth-membership', 'render-probe-depth-sort-us']
            phase = frames[0].get('value0')
            if (phase not in phase_ids or frames[0].get('value1') != 127
                    or any(len(names.get(n, [])) != 1 for n in required)
                    or names.get('render-probe-incomplete') or names.get('render-probe-gl-error')):
                invalid.append({'sequence': sequence, 'reason': 'missing, duplicate or failed pass evidence'})
                continue
            single = {n: names[n][0] for n in required}
            if depth_detail:
                problem=isolation_problem(single,phase) if pass_isolation else depth_problem(single,phase==2)
                if problem:
                    invalid.append({'sequence':sequence,'reason':problem});continue
            if (comparison or phase != 2) and any(single['render-pass-'+p]['value1'] != 0 for p in PASSES):
                invalid.append({'sequence': sequence, 'reason': 'unexpected baseline completion wait'})
                continue
            if comparison and (single['render-probe-kind']['value0'] != (33 if world_precision else 32 if pass_isolation else 31 if fused_composition else 30 if graphics_profile else 29 if specular_split else 28 if world_components else 27 if world_gloss else 26 if world_basis else 25 if world_math else 24 if lightmap_layout else 23 if world_lifetime else 22 if post_film else 21 if current_draw_probe else 20 if world_alpha else 19 if weapon_specular else 18 if weapon_vertices else 17 if weapon_pipeline else 16 if fast_draw_probe else 15 if early_alpha else 14 if hud else 13 if weapon_opaque else 12 if fast_color else 11 if draw_probe else 10 if prefetch else 9 if display else 8 if family else (7 if baseline else (6 if specular else (4 if depth_order else (3 if opaque else 1)))))
                    or (phase != 2 and single['render-probe-prop-shader']['value0'] != 0)):
                invalid.append({'sequence': sequence, 'reason': 'comparison program selection or test kind differs'})
                continue
            if sum(single['render-pass-'+p]['value1'] for p in PASSES) != single['render-probe-total-us']['value1']:
                invalid.append({'sequence': sequence, 'reason': 'completion wait accounting differs'})
                continue
            if opaque:
                counters=[single['render-probe-opaque-'+category] for category in ('world','props')]
                if (single['render-probe-opaque-invalid']['value0'] != 0
                        or single['render-probe-prop-shader']['value0'] != 0
                        or single['render-probe-viewports']['value0'] != 7 or single['render-probe-viewports']['value1'] != 0
                        or any(v['value0'] != (v['value1'] if phase==2 or depth_order or specular or baseline or family or display else 0) for v in counters)):
                    invalid.append({'sequence': sequence, 'reason': 'opaque dispatch, state or viewport evidence differs'})
                    continue
            if display:
                xy,z=single['render-probe-camera-xy'],single['render-probe-camera-z']
                camera=[scalar(xy.get('value0')),scalar(xy.get('value1')),scalar(z.get('value0'))]
                if (not anchor or z['value1']!=1 or not all(math.isfinite(v) for v in camera)
                        or any(abs(camera[i]-anchor['origin'][i])>(128 if i==2 else 4) for i in range(3))):
                    invalid.append({'sequence':sequence,'reason':'fixed-view evidence differs'});continue
            if family:
                ready=single['render-probe-family-ready']
                counts=[single['render-probe-family-'+name] for name in ('lightmapped','baked')]
                xy,z=single['render-probe-camera-xy'],single['render-probe-camera-z']
                camera=[scalar(xy.get('value0')),scalar(xy.get('value1')),scalar(z.get('value0'))]
                if (ready['value0']!=3 or ready['value1']!=3 or single['render-probe-family-invalid']['value0']!=0
                        or any(v['value0']!=(v['value1'] if phase==2 or prefetch or draw_probe or fast_color else 0) or v['value1']<0 for v in counts)
                        or sum(v['value1'] for v in counts)>sum(single['render-probe-opaque-'+c]['value1'] for c in ('world','props'))
                        or not anchor or z['value1']!=1 or not all(math.isfinite(v) for v in camera)
                        or any(abs(camera[i]-anchor['origin'][i])>(128 if i==2 else 4) for i in range(3))):
                    invalid.append({'sequence':sequence,'reason':'material family dispatch or fixed-view evidence differs'})
                    continue
            if prefetch:
                ready=single['render-probe-prefetch-ready']
                counts=[single['render-probe-prefetch-'+name] for name in ('lightmapped','baked')]
                if (ready['value0']!=3 or ready['value1']!=3 or single['render-probe-prefetch-invalid']['value0']!=0
                        or any(v['value0']!=(v['value1'] if phase==2 else 0) for v in counts)
                        or any(single['render-probe-prefetch-'+c]['value1']!=single['render-probe-family-'+c]['value1'] for c in ('lightmapped','baked'))):
                    invalid.append({'sequence':sequence,'reason':'early texture dispatch or readiness differs'});continue
            if fused_composition:
                copy=single['render-probe-composition-copy'];valid=single['render-probe-composition-valid']
                geometry=single['render-pass-composition-geometry']
                if (copy['value0']!=1 or copy['value1']!=int(phase==2)
                        or valid['value0']!=0 or valid['value1']!=960*544
                        or geometry['value0']!=(1 if phase==2 else 2)
                        or geometry['value1']!=(2 if phase==2 else 3)):
                    invalid.append({'sequence':sequence,'reason':'composition copy, far depth, restoration or draw coverage differs'});continue
            if fast_color:
                ready=single['render-probe-fast-ready']
                counters=[single['render-probe-fast-'+c] for c in ('color','clear')]
                if (ready['value0']!=1 or ready['value1']!=1 or single['render-probe-fast-invalid']['value0']!=0
                        or any(v['value0']!=(v['value1'] if phase==2 or weapon_opaque or hud or early_alpha or fast_draw_probe or weapon_pipeline else 0) or v['value1']<=0 for v in counters)
                        or single['render-probe-fast-clear']['value1']!=(1 if fused_composition and phase==2 else 2)):
                    invalid.append({'sequence':sequence,'reason':'fast color/clear readiness or actual dispatch differs'});continue
            if hud:
                counts=single['render-probe-hud-draws'];valid=single['render-probe-hud-valid'];viewport=single['render-probe-hud-viewport']
                if (counts['value1']<=0 or valid['value0']!=counts['value1'] or valid['value1']!=0
                        or counts['value0']!=(counts['value1'] if phase==2 else 0)
                        or counts['value1']!=single['render-pass-hud-geometry']['value0']
                        or viewport['value0']!=(960<<32|544) or viewport['value1']!=0):
                    invalid.append({'sequence':sequence,'reason':'HUD program, native viewport or draw coverage differs'});continue
            if early_alpha:
                ready=single['render-probe-alpha-ready']
                counts=[single['render-probe-alpha-'+c] for c in ('world','props')]
                if (ready['value0']!=3 or ready['value1']!=3 or single['render-probe-alpha-invalid']['value0']!=0
                        or any(v['value1']<0 or v['value0']!=(v['value1'] if phase==2 else 0) for v in counts)
                        or sum(v['value1'] for v in counts)<=0
                        or any(v['value1']>single['render-pass-'+p+'-geometry']['value0'] for v,p in zip(counts,('world','props-fx')))):
                    invalid.append({'sequence':sequence,'reason':'early alpha readiness or actual dispatch differs'});continue
            if weapon_pipeline:
                draws=single['render-probe-weapon-family'];valid=single['render-probe-weapon-family-valid']
                use=single['render-probe-weapon-cache-use'];hits=single['render-probe-weapon-cache-hits'];resets=single['render-probe-weapon-cache-resets']
                if (valid['value0']!=0 or valid['value1']!=3 or draws['value1']<=0
                        or draws['value1']>single['render-pass-weapon-geometry']['value0']
                        or draws['value0']!=(draws['value1'] if phase==2 or weapon_vertices else 0)
                        or use['value1']<=0 or use['value0']!=(use['value1'] if phase==2 or weapon_vertices else 0)
                        or hits['value0']<0 or hits['value1']<0 or hits['value0']+hits['value1']<=0
                        or not 0<=resets['value0']<=use['value0'] or not 0<resets['value1']<262144):
                    invalid.append({'sequence':sequence,'reason':'weapon family or exact light cache evidence differs'});continue
            if weapon_vertices:
                use=single['render-probe-weapon-vertices'];count=single['render-probe-weapon-vertex-count'];table=single['render-probe-weapon-vertex-table']
                if (use['value1']!=single['render-probe-weapon-cache-use']['value1']
                        or use['value0']!=(use['value1'] if phase==2 or weapon_specular else 0)
                        or count['value0']<=0 or count['value1']!=count['value0']*(44 if phase==2 or weapon_specular else 84)
                        or table['value0']!=1 or table['value1']!=65537):
                    invalid.append({'sequence':sequence,'reason':'packed weapon vertex conversion evidence differs'});continue
            if weapon_specular:
                draws=single['render-probe-weapon-specular'];valid=single['render-probe-weapon-specular-valid'];maps=single['render-probe-weapon-specular-maps']
                if (valid['value0']!=0 or valid['value1']!=1 or draws['value1']<=0
                        or draws['value0']!=(draws['value1'] if phase==2 or world_alpha else 0)
                        or draws['value1']+single['render-probe-weapon-family']['value1']>single['render-pass-weapon-geometry']['value0']
                        or maps['value0']!=0 or maps['value1']!=draws['value1']):
                    invalid.append({'sequence':sequence,'reason':'authored weapon specular dispatch or texture configuration differs'});continue
            if world_alpha:
                draws=single['render-probe-world-alpha'];valid=single['render-probe-world-alpha-valid'];target=single['render-probe-world-alpha-target']
                if (valid['value0']!=0 or valid['value1']!=1 or draws['value1']<=0
                        or draws['value0']!=(draws['value1'] if phase==2 or current_draw_probe or post_film or world_lifetime or lightmap_layout or world_math or world_basis or world_gloss or world_components or graphics_profile or fused_composition or pass_isolation or world_precision else 0)
                        or draws['value1']+single['render-probe-opaque-world']['value1']>single['render-pass-world-geometry']['value0']
                        or not 0<=target['value1']<=draws['value1']
                        or target['value0']!=(target['value1'] if phase==2 or current_draw_probe or post_film or world_lifetime or lightmap_layout or world_math or world_basis or world_gloss or world_components or graphics_profile or fused_composition or pass_isolation or world_precision else 0)):
                    invalid.append({'sequence':sequence,'reason':'transparent world family readiness or actual dispatch differs'});continue
            if lightmap_layout:
                use=single['render-probe-lightmap-layout'];valid=single['render-probe-lightmap-layout-valid'];storage=single['render-probe-lightmap-layout-storage']
                if (use['value1']<2 or use['value0']!=(use['value1'] if phase==2 else 0)
                        or valid['value0']!=0 or valid['value1']!=0
                        or not 0<storage['value0']<=storage['value1']==8*1024*1024):
                    invalid.append({'sequence':sequence,'reason':'lightmap layout dispatch, sampler, allocation or storage evidence differs'});continue
            if world_components:
                use=single['render-probe-world-components'];valid=single['render-probe-world-components-valid'];target=single['render-probe-world-components-target']
                variant=1 if phase==2 else 2 if phase==4 else 0
                if (valid['value0']!=0 or valid['value1']!=3 or use['value1']<1
                        or use['value1']>single['render-probe-world-alpha']['value1']
                        or use['value0']!=(use['value1'] if variant else 0)
                        or target['value0']!=4 or target['value1']!=variant):
                    invalid.append({'sequence':sequence,'reason':'decal component dispatch or target evidence differs'});continue
            if world_gloss:
                use=single['render-probe-world-gloss'];valid=single['render-probe-world-gloss-valid'];targets=single['render-probe-world-gloss-targets']
                if (valid['value0']!=0 or valid['value1']!=1 or use['value1']<1
                        or use['value1']>single['render-probe-opaque-world']['value1']
                        or use['value0']!=(use['value1'] if phase==2 else 0)
                        or targets['value1']!=2 or targets['value0']!=(2 if phase==2 else 0)):
                    invalid.append({'sequence':sequence,'reason':'world gloss certificate, dispatch or target evidence differs'});continue
            if world_basis:
                use=single['render-probe-world-basis'];valid=single['render-probe-world-basis-valid'];targets=single['render-probe-world-basis-targets']
                seen=single['render-probe-world-basis-seen'];triangles=single['render-probe-world-basis-triangles']
                if (valid['value0']!=0 or valid['value1']!=3 or use['value1']<1
                        or use['value1']>single['render-probe-opaque-world']['value1']+single['render-probe-world-alpha']['value1']
                        or use['value0']!=(use['value1'] if phase==2 else 0)
                        or seen['value0']!=7 or seen['value1']!=1 or not 0<targets['value1']<=7
                        or targets['value0']!=(targets['value1'] if phase==2 else 0)
                        or not 0<triangles['value0']<=triangles['value1']<=single['render-pass-world-geometry']['value1']):
                    invalid.append({'sequence':sequence,'reason':'world constant-basis dispatch, target eligibility or coverage evidence differs'});continue
            if world_precision:
                use=single['render-probe-world-precision'];valid=single['render-probe-world-precision-valid'];targets=single['render-probe-world-precision-targets']
                if (valid['value0']!=0 or valid['value1']!=3 or use['value1']<3
                        or use['value1']>single['render-probe-opaque-world']['value1']+single['render-probe-world-alpha']['value1']
                        or use['value0']!=(use['value1'] if phase==2 else 0)
                        or targets['value1']!=7 or targets['value0']!=(7 if phase==2 else 0)):
                    invalid.append({'sequence':sequence,'reason':'world precision dispatch or target evidence differs'});continue
            if world_math:
                use=single['render-probe-world-math'];valid=single['render-probe-world-math-valid'];targets=single['render-probe-world-math-targets']
                if (valid['value0']!=0 or valid['value1']!=3 or use['value1']<3
                        or use['value1']>single['render-probe-opaque-world']['value1']+single['render-probe-world-alpha']['value1']
                        or use['value0']!=(use['value1'] if phase==2 else 0)
                        or targets['value1']!=7 or targets['value0']!=(7 if phase==2 else 0)):
                    invalid.append({'sequence':sequence,'reason':'world math dispatch or target evidence differs'});continue
            if world_lifetime:
                use=single['render-probe-world-lifetime'];valid=single['render-probe-world-lifetime-valid'];targets=single['render-probe-world-lifetime-targets']
                if (valid['value0']!=0 or valid['value1']!=1 or use['value1']<2
                        or use['value1']>single['render-probe-opaque-world']['value1']
                        or use['value0']!=(use['value1'] if phase==2 else 0)
                        or targets['value1']!=3 or targets['value0']!=(3 if phase==2 else 0)):
                    invalid.append({'sequence':sequence,'reason':'world scalar lifetime dispatch or target evidence differs'});continue
            if post_film:
                use=single['render-probe-post-shader'];valid=single['render-probe-post-valid'];state=single['render-probe-post-state']
                params=single['render-probe-post-gamma-exposure'];size=single['render-probe-post-size']
                if (valid['value0']!=0 or valid['value1']!=1 or state['value0'] not in (0,1,2,3) or not 1<=state['value1']<=12
                        or use['value1']!=int(state['value0']==1) or use['value0']!=(use['value1'] if phase==2 else 0)
                        or single['render-pass-post-geometry']['value0']!=1 or single['render-pass-post-geometry']['value1']!=2
                        or size['value0']!=960 or size['value1']!=544
                        or not math.isfinite(scalar(params['value0'])) or not .1-1e-6<=scalar(params['value0'])<=4
                        or not math.isfinite(scalar(params['value1'])) or scalar(params['value1'])<0):
                    invalid.append({'sequence':sequence,'reason':'post shader dispatch, effect state or native target differs'});continue
            if weapon_opaque:
                ready=single['render-probe-weapon-ready'];counts=single['render-probe-weapon-opaque'];maps=single['render-probe-weapon-maps']
                if (ready['value0']!=1 or ready['value1']!=1 or single['render-probe-weapon-invalid']['value0']!=0
                        or counts['value1']<=0 or counts['value1']>single['render-pass-weapon-geometry']['value0']
                        or counts['value0']!=(counts['value1'] if phase==2 else 0)
                        or not 0<=maps['value0']<=counts['value1'] or not 0<=maps['value1']<=counts['value1']):
                    invalid.append({'sequence':sequence,'reason':'weapon opaque readiness, maps or actual dispatch differs'});continue
            if specular:
                ready=single['render-probe-specular-ready']
                inputs=single['render-probe-specular-inputs']
                counts=[single['render-probe-specular-'+c] for c in ('world','props')]
                if (ready['value0']!=3 or ready['value1']!=3
                        or any(v['value0']!=(v['value1'] if phase==2 else 0) for v in counts)
                        or any(single['render-probe-specular-'+c]['value1']!=single['render-probe-opaque-'+c]['value1'] for c in ('world','props'))
                        or inputs['value0']<0 or inputs['value1']<0 or inputs['value1']>inputs['value0']
                        or inputs['value0']>sum(v['value1'] for v in counts)):
                    invalid.append({'sequence':sequence,'reason':'specular program readiness, dispatch or feature evidence differs'})
                    continue
            if lightmap_inplace:
                workspace=single['render-probe-lightmap-inplace']
                if (workspace['value0'],workspace['value1'])!=(128*1024,0):
                    invalid.append({'sequence':sequence,'reason':'in-place workspace or extra GPU storage differs'});continue
            if depth_order:
                sort=single['render-probe-depth-order'];members=single['render-probe-depth-membership'];eligible=single['render-probe-depth-eligible']
                if (sort['value0']>sort['value1'] or sort['value1']>32768 or sort['value0']<0
                        or (phase!=2 and (sort['value0'] or sort['value1']))
                        or members['value0']!=members['value1'] or eligible['value1']!=0
                        or single['render-probe-depth-sort-us']['value1']!=phase):
                    invalid.append({'sequence':sequence,'reason':'depth order evidence differs'})
                    continue
            observed_order.append(phase)
            phases[phase].append(single)
        reports = []
        for phase in phase_ids:
            all_frames = phases[phase]
            # Queued work from the preceding phase can reach the first frames.
            samples = all_frames[2:]
            reports.append({'phase': phase, 'name': ({i:({2:'world-draws-off',4:'props-fx-draws-off',6:'weapon-draws-off',8:'post-copy'}.get(i,'baseline')) for i in range(1,10)} if pass_isolation else {1:'baseline-before',2:'parameter-buffer',3:'shader-and-texture',4:'geometry-and-pixels',5:'parameter-memory',6:'baseline-after'} if graphics_profile else {1: 'normal-before', 2: 'world-mixed-precision' if world_precision else 'fused-world-copy-and-depth-clear' if fused_composition else 'decal-highlight-off' if specular_split else 'decal-secondary-lightmap-off' if world_components else 'constant-world-gloss' if world_gloss else 'world-vertex-basis' if world_basis else 'world-math' if world_math else 'swizzled-lightmaps' if lightmap_layout else 'world-scalar-lifetime' if world_lifetime else 'film-without-glow-shader' if post_film else 'sampled-current-pipeline' if current_draw_probe else 'transparent-world-shader' if world_alpha else 'authored-weapon-shader' if weapon_specular else 'packed-weapon-vertices' if weapon_vertices else 'weapon-pipeline' if weapon_pipeline else 'sampled-draw-waits-fast-baseline' if fast_draw_probe else 'early-alpha' if early_alpha else 'opaque-weapon' if weapon_opaque else 'fast-color-and-clear' if fast_color else 'sampled-draw-waits' if draw_probe else 'early-textures' if prefetch else 'gpu-finish-before-queue' if display else 'material-family' if family else 'opaque-baseline' if baseline else 'specular-math' if specular else 'world-depth-order' if depth_order else ('opaque-no-discard' if opaque else ('specialized-props' if comparison else 'serialized-passes')), 3: 'normal-between' if world_components else 'normal-after',4:'decal-reflection-off' if specular_split else 'decal-specular-reflection-off',5:'normal-after'})[phase],
                'frames': len(all_frames), 'settlingFramesExcluded': min(2, len(all_frames)),
                'renderElapsed': summary([r['render-probe-total-us']['value0'] for r in samples]),
                'swapElapsed': summary([r['render-probe-swap-us']['value0'] for r in samples]),
                'displayQueueElapsed': summary([r['render-probe-swap-us']['value1'] for r in samples]),
                'viewportsAllObservedAndCorrect': bool(samples) and all(r['render-probe-viewports']['value0'] == 7 and r['render-probe-viewports']['value1'] == 0 for r in samples),
                'passes': {p: {'submissionElapsed': summary([r['render-pass-'+p]['value0'] for r in samples]),
                               'completionWaitElapsed': summary([r['render-pass-'+p]['value1'] for r in samples]),
                               'meanDraws': sum(r['render-pass-'+p+'-geometry']['value0'] for r in samples)/len(samples) if samples else None,
                               'meanTriangles': sum(r['render-pass-'+p+'-geometry']['value1'] for r in samples)/len(samples) if samples else None} for p in PASSES}})
        health = [r for r in cpu if r.get('type') in ('end', 'recording-health')]
        storage_ok = bool(health) and not any(r.get('dropped', 0) or r.get('ioFailed', False) for r in health)
        elapsed = (endings[0]['startUs']-begin['startUs']) if endings else None
        if lightmap_inplace:
            # The phase clock pauses across GPU drain, conversion and hashing;
            # probe/frame clocks start only after each transition completes.
            paused=[r for r in rows if r.get('name')=='lightmap-layout-diagnostic-paused-us']
            transitions=[r for r in rows if r.get('name')=='lightmap-layout-transition-us']
            total_pause=sum(r['value0'] for r in transitions)
            if (len(paused)!=1 or paused[0]['value0']!=total_pause
                    or len(transitions)!=2 or [r['value1'] for r in transitions]!=[1,0]
                    or elapsed is None or elapsed-total_pause<60000000):
                invalid.append({'reason':'lightmap transition time not excluded from diagnostic phases'})
            if any(r.get('name')=='lightmap-layout-failure' for r in rows):
                invalid.append({'reason':'in-place conversion failed'})
        if pass_isolation and any(r.get('name','').startswith('graphics-counter-') for r in rows):
            invalid.append({'reason':'unexpected hardware-counter operation in passive test'})
        complete = bool(elapsed is not None and elapsed >= (180000000 if pass_isolation else 120000000 if graphics_profile else 100000000 if world_components else 180000000 if draw_probe else 60000000) and markers and storage_ok and not parse_problems and not invalid
                        and observed_order == sorted(observed_order) and all(r['renderElapsed']['samples'] >= 3 for r in reports))
        comparison_details = None
        if comparison:
            for phase, report in zip(phase_ids, reports):
                samples = phases[phase][2:]
                report['specializedPropDraws'] = sum(r['render-probe-prop-shader']['value0'] for r in samples)
                report['eligiblePropDraws'] = sum(r['render-probe-prop-shader']['value1'] for r in samples)
            if opaque:
                for phase, report in zip(phase_ids,reports):
                    samples=phases[phase][2:]
                    report['opaqueDraws']={category:sum(v['render-probe-opaque-'+category]['value0'] for v in samples) for category in ('world','props')}
                    report['opaqueEligibleDraws']={category:sum(v['render-probe-opaque-'+category]['value1'] for v in samples) for category in ('world','props')}
            used = reports[1]['specializedPropDraws'] > 0 and reports[1]['specializedPropDraws'] == reports[1]['eligiblePropDraws']
            if opaque:
                used = reports[1]['opaqueDraws']['props']>0 and reports[1]['opaqueDraws']==reports[1]['opaqueEligibleDraws']
            if family:
                for phase,report in zip(phase_ids,reports):
                    samples=phases[phase][2:]
                    report['materialFamilyDraws']={c:sum(v['render-probe-family-'+c]['value0'] for v in samples) for c in ('lightmapped','baked')}
                    report['materialFamilyEligible']={c:sum(v['render-probe-family-'+c]['value1'] for v in samples) for c in ('lightmapped','baked')}
                used=used and all(reports[1]['materialFamilyDraws'][c]>0 for c in ('lightmapped','baked'))
            if prefetch:
                for phase,report in zip(phase_ids,reports):
                    samples=phases[phase][2:]
                    report['texturePrefetchDraws']={c:sum(v['render-probe-prefetch-'+c]['value0'] for v in samples) for c in ('lightmapped','baked')}
                used=used and all(reports[1]['texturePrefetchDraws'][c]>0 for c in ('lightmapped','baked'))
            if fast_color:
                for phase,report in zip(phase_ids,reports):
                    samples=phases[phase][2:]
                    report['fastColorClearDraws']={c:sum(v['render-probe-fast-'+c]['value0'] for v in samples) for c in ('color','clear')}
                    report['fastColorClearEligible']={c:sum(v['render-probe-fast-'+c]['value1'] for v in samples) for c in ('color','clear')}
                used=used and all(reports[1]['fastColorClearDraws'][c]>0 for c in ('color','clear'))
            if fused_composition:
                for phase,report in zip(phase_ids,reports):
                    samples=phases[phase][2:]
                    report['composition']={'copyDraws':len(samples),'fusedDraws':sum(v['render-probe-composition-copy']['value1'] for v in samples),
                        'redundantClearPixelsRemovedPerFrame':960*544 if phase==2 else 0}
            if hud:
                for phase,report in zip(phase_ids,reports):
                    samples=phases[phase][2:]
                    report['hudShaderDraws']=sum(v['render-probe-hud-draws']['value0'] for v in samples)
                    report['hudEligibleDraws']=sum(v['render-probe-hud-draws']['value1'] for v in samples)
                used=used and reports[1]['hudShaderDraws']>0
            if early_alpha:
                for phase,report in zip(phase_ids,reports):
                    samples=phases[phase][2:]
                    report['earlyAlphaDraws']={c:sum(v['render-probe-alpha-'+c]['value0'] for v in samples) for c in ('world','props')}
                    report['earlyAlphaEligible']={c:sum(v['render-probe-alpha-'+c]['value1'] for v in samples) for c in ('world','props')}
                used=used and sum(reports[1]['earlyAlphaDraws'].values())>0
            if weapon_pipeline:
                for phase,report in zip(phase_ids,reports):
                    samples=phases[phase][2:]
                    report['weaponFamilyDraws']=sum(v['render-probe-weapon-family']['value0'] for v in samples)
                    report['weaponFamilyEligible']=sum(v['render-probe-weapon-family']['value1'] for v in samples)
                    report['weaponLightCache']={c:sum(v['render-probe-weapon-cache-'+name][field] for v in samples) for c,name,field in [('hits','hits','value0'),('misses','hits','value1'),('candidateUpdates','use','value0'),('updates','use','value1'),('resets','resets','value0')]}
                    report['weaponLightCache']['bytes']=max((v['render-probe-weapon-cache-resets']['value1'] for v in samples),default=None)
                used=used and reports[1]['weaponFamilyDraws']>0 and reports[1]['weaponLightCache']['candidateUpdates']>0
            if weapon_vertices:
                for phase,report in zip(phase_ids,reports):
                    samples=phases[phase][2:]
                    report['weaponVertices']={c:sum(v['render-probe-weapon-'+name][field] for v in samples) for c,name,field in [('candidateUpdates','vertices','value0'),('updates','vertices','value1'),('vertices','vertex-count','value0'),('intermediateBytes','vertex-count','value1')]}
                used=used and reports[1]['weaponVertices']['candidateUpdates']>0
            if weapon_specular:
                for phase,report in zip(phase_ids,reports):
                    samples=phases[phase][2:]
                    report['weaponSpecular']={c:sum(v['render-probe-weapon-'+name][field] for v in samples) for c,name,field in [('draws','specular','value0'),('eligible','specular','value1'),('normalMaps','specular-maps','value0'),('specularMaps','specular-maps','value1')]}
                used=used and reports[1]['weaponSpecular']['draws']>0
            if world_alpha:
                for phase,report in zip(phase_ids,reports):
                    samples=phases[phase][2:]
                    report['worldAlpha']={c:sum(v['render-probe-world-alpha'+name][field] for v in samples) for c,name,field in [('draws','','value0'),('eligible','','value1'),('targetDraws','-target','value0'),('targetEligible','-target','value1')]}
                used=used and reports[1]['worldAlpha']['draws']>0 and all(p['worldAlpha']['targetEligible']>0 for p in reports)
            if lightmap_layout:
                for phase,report in zip(phase_ids,reports):
                    samples=phases[phase][2:]
                    report['lightmapLayout']=dict(bindings=sum(v['render-probe-lightmap-layout']['value0'] for v in samples),eligible=sum(v['render-probe-lightmap-layout']['value1'] for v in samples),bytes=max(v['render-probe-lightmap-layout-storage']['value0'] for v in samples))
                used=used and reports[1]['lightmapLayout']['bindings']>0
            if world_components:
                for phase,report in zip(phase_ids,reports):
                    samples=phases[phase][2:]
                    report['worldComponents']=dict(draws=sum(v['render-probe-world-components']['value0'] for v in samples),eligible=sum(v['render-probe-world-components']['value1'] for v in samples),variant=1 if phase==2 else 2 if phase==4 else 0)
                used=used and all(reports[i]['worldComponents']['draws']>0 for i in (1,3))
            if world_gloss:
                for phase,report in zip(phase_ids,reports):
                    samples=phases[phase][2:]
                    report['worldGloss']=dict(draws=sum(v['render-probe-world-gloss']['value0'] for v in samples),eligible=sum(v['render-probe-world-gloss']['value1'] for v in samples),
                        targetEligibilityMasks=sorted({v['render-probe-world-gloss-targets']['value1'] for v in samples}))
                used=used and reports[1]['worldGloss']['draws']>0
            if world_basis:
                for phase,report in zip(phase_ids,reports):
                    samples=phases[phase][2:]
                    report['worldBasis']=dict(draws=sum(v['render-probe-world-basis']['value0'] for v in samples),eligible=sum(v['render-probe-world-basis']['value1'] for v in samples),
                        targetEligibilityMasks=sorted({v['render-probe-world-basis-targets']['value1'] for v in samples}),
                        eligibleTriangles=sum(v['render-probe-world-basis-triangles']['value0'] for v in samples),familyTriangles=sum(v['render-probe-world-basis-triangles']['value1'] for v in samples))
                used=used and reports[1]['worldBasis']['draws']>0
            if world_precision:
                for phase,report in zip(phase_ids,reports):
                    samples=phases[phase][2:]
                    report['worldPrecision']=dict(draws=sum(v['render-probe-world-precision']['value0'] for v in samples),eligible=sum(v['render-probe-world-precision']['value1'] for v in samples),
                        targetsObserved=all(v['render-probe-world-precision-targets']['value1']==7 for v in samples))
                used=used and reports[1]['worldPrecision']['draws']>0
            if world_math:
                for phase,report in zip(phase_ids,reports):
                    samples=phases[phase][2:]
                    report['worldMath']=dict(draws=sum(v['render-probe-world-math']['value0'] for v in samples),eligible=sum(v['render-probe-world-math']['value1'] for v in samples),
                        targetsObserved=all(v['render-probe-world-math-targets']['value1']==7 for v in samples))
                used=used and reports[1]['worldMath']['draws']>0
            if world_lifetime:
                for phase,report in zip(phase_ids,reports):
                    samples=phases[phase][2:]
                    report['worldLifetime']=dict(draws=sum(v['render-probe-world-lifetime']['value0'] for v in samples),eligible=sum(v['render-probe-world-lifetime']['value1'] for v in samples),
                        targetsObserved=all(v['render-probe-world-lifetime-targets']['value1']==3 for v in samples))
                used=used and reports[1]['worldLifetime']['draws']>0
            if post_film:
                for phase,report in zip(phase_ids,reports):
                    samples=phases[phase][2:]
                    report['postFilm']=dict(draws=sum(v['render-probe-post-shader']['value0'] for v in samples),eligible=sum(v['render-probe-post-shader']['value1'] for v in samples),
                        effectStates=sorted({v['render-probe-post-state']['value0'] for v in samples}),
                        gammaValues=sorted({scalar(v['render-probe-post-gamma-exposure']['value0']) for v in samples}),
                        exposureValues=sorted({scalar(v['render-probe-post-gamma-exposure']['value1']) for v in samples}))
                used=used and reports[1]['postFilm']['draws']>0 and all(p['postFilm']['eligible']>0 for p in reports)
            if weapon_opaque:
                for phase,report in zip(phase_ids,reports):
                    samples=phases[phase][2:]
                    report['weaponOpaqueDraws']=sum(v['render-probe-weapon-opaque']['value0'] for v in samples)
                    report['weaponOpaqueEligible']=sum(v['render-probe-weapon-opaque']['value1'] for v in samples)
                    report['weaponMaps']={c:sum(v['render-probe-weapon-maps'][field] for v in samples) for c,field in [('normal','value0'),('specular','value1')]}
                used=used and reports[1]['weaponOpaqueDraws']>0 and all(v['weaponMaps'][c]>0 for v in reports for c in ('normal','specular'))
            if specular:
                for phase,report in zip(phase_ids,reports):
                    samples=phases[phase][2:]
                    report['specularMathDraws']={c:sum(v['render-probe-specular-'+c]['value0'] for v in samples) for c in ('world','props')}
                    report['specularInputs']=sum(v['render-probe-specular-inputs']['value0'] for v in samples)
                    report['reflectionInputs']=sum(v['render-probe-specular-inputs']['value1'] for v in samples)
                used=used and all(reports[1]['specularMathDraws'][c]>0 for c in ('world','props'))
                used=used and all(p['specularInputs']>0 and p['reflectionInputs']>0 for p in reports)
            if depth_order:
                for phase,report in zip(phase_ids,reports):
                    samples=phases[phase][2:]
                    report['depthOrderSwaps']=sum(v['render-probe-depth-order']['value0'] for v in samples)
                    report['depthSortCpuElapsed']=summary([v['render-probe-depth-sort-us']['value0'] for v in samples])
                    report['meanDepthEligible']=sum(v['render-probe-depth-eligible']['value0'] for v in samples)/len(samples) if samples else None
                used=used and reports[1]['depthOrderSwaps']>0
            complete = bool(complete and used)
            before, candidate, after = [p['renderElapsed']['meanUs'] for p in reports[:3]]
            comparison_details = {'candidateActuallyUsed': used,
                'fractionFasterThanBefore': 1-candidate/before if before and candidate else None,
                'fractionFasterThanAfter': 1-candidate/after if after and candidate else None,
                'baselineDriftFraction': after/before-1 if before and after else None,
                'acceptance': 'Review both baselines, draw/triangle mix, viewport evidence and visual photos before accepting. No automatic quality or hardware qualification.'}
        component_details = None
        if world_components or pass_isolation:
            component_details=[]
            for index,name in ([(1,'world'),(3,'props-fx'),(5,'weapon'),(7,'post')] if pass_isolation else [(1,'highlight'),(3,'reflection')] if specular_split else [(1,'secondary-lightmap'),(3,'specular-reflection')]):
                trio=reports[index-1:index+2]
                metric={}
                for key in ['renderElapsed','displayQueueElapsed']:
                    values=[p[key]['meanUs'] for p in trio]
                    metric[key]=dict(meanUs=values,savingsUs=[values[i]-values[1] if values[i] is not None and values[1] is not None else None for i in (0,2)],
                        fractionFaster=[1-values[1]/values[i] if values[i] and values[1] else None for i in (0,2)])
                component_details.append(dict(component=name,phases=[p['phase'] for p in trio],timings=metric,diagnosticQualityReduction=True,
                    interpretation='Marginal effect of omitted category submissions (or copy-only post), including GPU dependencies, changed occlusion and reduced driver work. Not isolated GPU duration, additive costs or playable FPS.' if pass_isolation else 'Marginal full-frame effect of disabling this component on batch588,including compiler/dependency changes;not isolated cycles or additive with the other bypass.'))
        source_hud = None
        if hud or early_alpha or fast_draw_probe or weapon_pipeline:
            errors=[v for v in rows if v.get('name')=='source-ui-errors']
            quads=[v for v in rows if v.get('name')=='source-ui-quads']
            packets=[v for v in rows if v.get('name')=='source-ui-packet']
            source_hud=dict(errorSamples=len(errors), quadSamples=len(quads), packetSamples=len(packets),
                maxMissingMaterials=max((v['value0'] for v in errors),default=None),
                maxUnsupportedCommands=max((v['value1'] for v in errors),default=None),
                maxRejectedPacketCommands=max((v['value1'] for v in packets),default=None),
                quads=summary([v['value0'] for v in quads]), batches=summary([v['value1'] for v in quads]))
            source_hud['availableAndClean']=bool(errors and quads and packets
                and source_hud['maxMissingMaterials']==source_hud['maxUnsupportedCommands']==source_hud['maxRejectedPacketCommands']==0
                and any(v['value0']>0 and v['value1']>0 for v in quads) and any(v['value0']>0 for v in packets))
            if hud:
                complete=bool(complete and source_hud['availableAndClean'])
            if early_alpha or fast_draw_probe or weapon_pipeline:
                source_hud['qualificationLimit']='Known missing source HUD menus remain unresolved. Shared packet rejects are not HUD-specific; graphics capture validity does not qualify HUD or source completeness.'
        draw_details = None
        if draw_probe:
            from analyze_gpu_draws import analyze_gpu_draws
            draw_details=analyze_gpu_draws(cpu,generation,begin['startUs'])
            complete=complete and draw_details['captureComplete']
            if fast_draw_probe:
                draw_details['baselineContract']='Fast color/clear and opaque material families active throughout; dedicated HUD unchanged. Early-alpha and weapon-opaque candidates disabled.'
                draw_details['normalFrameImprovementClaimed']=False
            if current_draw_probe:
                draw_details['baselineContract']='Run52 transparent-world shader, both authored weapon families, exact lighting cache, fused vertices and fast grass/clear/opaque world/prop paths active throughout.'
                draw_details['normalFrameImprovementClaimed']=False
                draw_details['surfaceIdentity']='Updated for each cached native world draw; fullscreen source labels cleared.'
                post_rows=[v for v in rows if v.get('name')=='gpu-post-state']
                post_parameters=[v for v in rows if v.get('name')=='gpu-post-gamma-exposure']
                post_sizes=[v for v in rows if v.get('name')=='gpu-post-size']
                expected_sequences={v.get('sequence') for v in rows if v.get('name')=='render-probe-frame'}
                def once_each(records):
                    return len(records)==len(expected_sequences) and {v.get('sequence') for v in records}==expected_sequences
                post_valid=bool(once_each(post_rows) and once_each(post_parameters) and once_each(post_sizes)
                    and all(v['value0'] in (0,1,2,3) and 1<=v['value1']<=12 for v in post_rows)
                    and all(math.isfinite(scalar(v['value0'])) and .1-1e-6<=scalar(v['value0'])<=4
                        and math.isfinite(scalar(v['value1'])) and scalar(v['value1'])>=0 for v in post_parameters)
                    and all(v['value0']==960 and v['value1']==544 for v in post_sizes))
                draw_details['postState']=dict(availableAndValid=post_valid,filmGlowAndRadius=sorted({(v['value0'],v['value1']) for v in post_rows}),
                    gamma=summary([scalar(v['value0']) for v in post_parameters]),exposure=summary([scalar(v['value1']) for v in post_parameters]))
                # Unseen or thin targets are unavailable attribution, never a
                # successful check. Capture health and coverage stay distinct.
                targets={
                    'opaqueWorld':[v for v in draw_details['draws'] if v['pipeline']=='world' and v['sourceSurface'] is not None and not v['pathFlags']&192],
                    'post':[v for v in draw_details['draws'] if v['pipeline']=='post' and v['pathFlags']&256],
                    'composition':[v for v in draw_details['draws'] if v['pipeline']=='composition' and v['pathFlags']&256]}
                draw_details['targetCoverage']={name:dict(observedSignatures=len(values),sampledSignatures=sum(v['completionWait']['samples']>0 for v in values),
                    sufficient=bool(values) and all(v['completionWait']['samples']>=3 for v in values if v['baselineObservations']>=3)
                        and any(v['baselineObservations']>=3 for v in values)) for name,values in targets.items()}
                complete=bool(complete and post_valid)
            for report in reports:
                report['timingMeaning']='Middle-phase pass elapsed includes injected draw/prefix/control completion waits; baseline phases have no probe fences.'
                if report['phase']==2:
                    for timing in report['passes'].values():
                        timing['instrumentedElapsed']=timing.pop('submissionElapsed')
        queue_details = None
        if display:
            from analyze_display_queue import analyze_display_queue
            queue_details=analyze_display_queue(cpu,generation,begin['startUs'],reports)
            complete=complete and queue_details['captureComplete']
        result.append({'generation': generation, 'mode': mode, 'elapsedUs': elapsed, 'captureComplete': complete,
                       'hardwareQualified': False, 'phases': reports, 'invalidFrames': invalid,
                       'propShaderComparison': comparison_details if not opaque else None,
                       'opaqueShaderComparison': comparison_details if opaque and not depth_order and not specular and not baseline and not family and not display else None,
                       'opaqueBaselineRecording': {'allPhasesSameShader': True, 'opaqueProgramsObserved': used} if baseline else None,
                       'depthOrderComparison': comparison_details if depth_order else None,
                       'specularMathComparison': comparison_details if specular else None,
                       'materialFamilyComparison': comparison_details if family and not prefetch and not draw_probe and not fast_color else None,
                       'weaponOpaqueComparison': comparison_details if weapon_opaque else None,
                       'hudPipelineComparison': comparison_details if hud else None,
                       'earlyAlphaComparison': comparison_details if early_alpha else None,
                       'weaponPipelineComparison': comparison_details if weapon_pipeline and not weapon_vertices else None,
                       'weaponVertexComparison': comparison_details if weapon_vertices and not weapon_specular else None,
                       'weaponSpecularComparison': comparison_details if weapon_specular and not world_alpha else None,
                       **({'compositionComparison':comparison_details} if fused_composition else {}),
                       'worldAlphaComparison': comparison_details if world_alpha and not current_draw_probe and not post_film and not world_lifetime and not lightmap_layout and not world_math and not world_basis and not world_gloss and not world_components and not graphics_profile and not fused_composition and not pass_isolation and not world_precision else None,
                       'postFilmComparison': comparison_details if post_film else None,
                       **({'passIsolationComparisons':component_details} if pass_isolation else {}),
                       **({'worldComponentComparisons':component_details} if world_components else {}),
                       **({'worldGlossComparison': comparison_details} if world_gloss else {}),
                       **({'worldBasisComparison': comparison_details} if world_basis else {}),
                       **({'worldPrecisionComparison': comparison_details} if world_precision else {}),
                       **({'worldMathComparison': comparison_details} if world_math else {}),
                       'worldLifetimeComparison': comparison_details if world_lifetime else None,
                       'lightmapLayoutComparison': comparison_details if lightmap_layout else None,
                       'sourceHudEvidence': source_hud,
                       'fastColorClearComparison': comparison_details if fast_color and not weapon_opaque and not hud and not early_alpha and not fast_draw_probe and not weapon_pipeline else None,
                       'drawWaitBreakdown': draw_details,
                       'texturePrefetchComparison': comparison_details if prefetch else None,
                       'diagnosticAnchor': anchor if family or display else None,
                       'displayQueueProbe': queue_details,
                       'interpretation': 'Nine unfenced windows alternate normal rendering with world draws off, props/FX off, weapon off, and copy-only post. Clears, target transitions, CPU preparation and gameplay continue; passive native scene counts verify three scenes. Diagnostic marginal effects include occlusion and dependency changes; no GPU utilization or additive cost claims.' if pass_isolation else 'Unfenced baseline/fused-copy/baseline. The candidate world copy also clears native weapon depth, removing one fullscreen clear. Same source shaders, resolution, lighting and post treatment. Hardware visual/depth validation remains required.' if fused_composition else 'Six identical-rendering windows: baseline, four optional Razor metric groups, baseline. Measure observer overhead and counter availability separately. GPU jobs may overlap; CPU scene submission is not GPU scene duration.' if graphics_profile else 'Five unfenced windows: baseline,highlight contribution off,baseline,environment reflection contribution off,baseline. Shared work stays available to the remaining branch; compiler elimination is measured on hardware. Diagnostic reductions only, not additive GPU cycles.' if specular_split else 'Five unfenced windows: baseline,secondary-lightmap bypass,baseline,specular/reflection bypass,baseline. Only the certified gravel decal changes. Diagnostic visual reductions estimate marginal workload sensitivity,not playable improvements or additive GPU cycles.' if world_components else 'Same opaque baseline in all phases. Phase 2 waits on the existing GXM context before AddEntry to distinguish deferred graphics completion from display callback/queue delay; no speed gain claimed.' if display else 'Three equal-length windows use the same opaque baseline to validate recording; no candidate optimization is present.' if baseline else 'Unfenced baseline/candidate/baseline with actual program-use counters. Compare source workload and geometry mix; all source gameplay continues.' if comparison else 'Fences include scene submission, completion and tile store/reload overhead. These are serialized elapsed waits, not isolated shader times or utilization. Phase 2 FPS is diagnostic only; compare both normal phases for drift.'})
    return result
