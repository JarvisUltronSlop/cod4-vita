"""Keep the shader isolation experiment's alpha/fog and HUD contracts intact."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def test_benchmark_input_lock_precedes_source_publication():
    bootstrap = (ROOT / "src/vita/vita_bootstrap.cpp").read_text()
    assert bootstrap.index("benchmark.FilterInput(inputSample,") < bootstrap.index("VitaInput_PublishSourceSample(")
    assert 'benchmark.Abort("unexpected-input")' not in bootstrap


def test_simple_material_retains_source_alpha_and_fog():
    backend = (ROOT / "src/gfx_vita/gfx_vita_backend.cpp").read_text()
    diagnostic = (ROOT / "src/gfx_vita/gfx_vita_benchmark_shaders.h").read_text()
    authored = backend.split('kNativeLitFragmentShader = R"VITA_SHADER(', 1)[1].split(')VITA_SHADER"', 1)[0]
    simple = diagnostic.split('kBenchmarkSimpleMaterialFragment = R"VITA_SHADER(', 1)[1].split(')VITA_SHADER"', 1)[0]
    alpha_start = "    if (u_alpha_test_enabled > 0.5) {"
    alpha_end = "    vec3 lit = material.rgb;"
    assert authored.split(alpha_start, 1)[1].split(alpha_end, 1)[0].strip() == simple.split(alpha_start, 1)[1].split(alpha_end, 1)[0].strip()
    fog_start = "    if (u_fog_enabled > 0.5) {"
    assert authored.split(fog_start, 1)[1].replace("#endif\n", "") == simple.split(fog_start, 1)[1]
    assert simple.count("texture2D(") == 1
    assert "texture2D(u_color, v_texcoord)" in simple
    assert "vec4 material = v_color;" in simple
    for sampler in ("u_normal", "u_specular", "u_lightmap_primary", "u_lightmap_secondary", "u_reflection_probe"):
        assert sampler not in simple
    copy = diagnostic.split('kBenchmarkPostCopyFragment = R"VITA_SHADER(', 1)[1]
    assert copy.count("texture2D(") == 1
    assert "texture2D(u_scene, v_texcoord)" in copy


def test_hud_and_measurement_keep_authored_path():
    backend = (ROOT / "src/gfx_vita/gfx_vita_backend.cpp").read_text()
    assert "g_benchmarkShaderMode==2 && !g_nativeHud ? g_nativeSimpleShader" in backend
    hud = backend.split("void GfxVita_BeginNativeHud()", 1)[1].split("bool GfxVita_GetFrameCounters", 1)[0]
    assert hud.index("g_nativeHud=true;") < hud.index("SelectNativeLightShader();")
    assert "glFinish" not in hud
    # Diagnostic GLSL is compiled only for explicitly enabled benchmark builds.
    assert '#if KISAK_VITA_BENCHMARK\n#include "gfx_vita_benchmark_shaders.h"' in backend


def test_material_variants_remove_only_the_requested_shader_block():
    import subprocess
    import pytest
    compiler = ROOT.parent / '.tools/vitasdk/bin/arm-vita-eabi-cpp.exe'
    if not compiler.exists(): pytest.skip('Vita SDK preprocessor unavailable')
    backend = (ROOT / 'src/gfx_vita/gfx_vita_backend.cpp').read_text()
    authored = backend.split('kNativeLitFragmentShader = R"VITA_SHADER(', 1)[1].split(')VITA_SHADER"', 1)[0]
    def preprocess(defines):
        source = authored.replace('#version 100', '')
        return subprocess.run([str(compiler), '-E', '-P', '-x', 'c', *defines, '-'],
                              input=source, text=True, capture_output=True, check=True).stdout
    full = preprocess([])
    lights = preprocess(['-DVITA_SUN_ONLY=1'])
    spec = preprocess(['-DVITA_NO_SPECULAR=1'])
    both = preprocess(['-DVITA_NO_SPECULAR=1', '-DVITA_SUN_ONLY=1'])
    normal = preprocess(['-DVITA_NO_NORMAL_MAP=1'])
    assert 'texture2D(u_normal' not in normal
    for retained in ('texture2D(u_specular', 'texture2D(u_lightmap_primary',
                     'texture2D(u_lightmap_secondary', 'lightIndex < 16',
                     'lightIndex < 4', 'u_fog_enabled'):
        assert retained in normal
    assert normal.count('{') == normal.count('}')
    for shader in (full, lights, spec, both):
        assert shader.count('{') == shader.count('}')
        for retained in ('texture2D(u_normal', 'texture2D(u_lightmap_primary',
                         'texture2D(u_lightmap_secondary', 'u_fog_enabled',
                         'textureCube(u_reflection_probe, normalize(v_worldposition))'):
            assert retained in shader
    for shader in (full, spec):
        assert 'lightIndex < 16' in shader and 'lightIndex < 4' in shader
    for shader in (lights, both):
        assert 'lightIndex <' not in shader
    for shader in (full, lights):
        assert 'texture2D(u_specular' in shader and 'float fresnel =' in shader
    for shader in (spec, both):
        assert 'texture2D(u_specular' not in shader and 'float fresnel =' not in shader
    assert 'g_benchmarkShaderMode==3 && !g_nativeHud ? g_nativeSunOnlyShader' in backend
    assert 'g_benchmarkShaderMode==4 && !g_nativeHud ?' in backend


def test_no_fog_variant_preserves_every_other_shader_operation():
    import subprocess
    import pytest
    compiler = ROOT.parent / '.tools/vitasdk/bin/arm-vita-eabi-cpp.exe'
    if not compiler.exists(): pytest.skip('Vita SDK preprocessor unavailable')
    backend=(ROOT/'src/gfx_vita/gfx_vita_backend.cpp').read_text()
    authored=backend.split('kNativeLitFragmentShader = R"VITA_SHADER(',1)[1].split(')VITA_SHADER"',1)[0].replace('#version 100','')
    def pp(defines):
        return subprocess.run([str(compiler),'-E','-P','-x','c',*defines,'-'],input=authored,text=True,capture_output=True,check=True).stdout
    for lights in ([],['-DVITA_SUN_ONLY=1']):
        full=pp(lights); nofog=pp(lights+['-DVITA_NO_FOG=1'])
        start=full.index('    if (u_fog_enabled > 0.5) {')
        end=full.index('    gl_FragColor =',start)
        assert nofog==full[:start]+full[end:]
    assert 'g_benchmarkShaderMode==7 && !g_nativeHud ?' in backend
