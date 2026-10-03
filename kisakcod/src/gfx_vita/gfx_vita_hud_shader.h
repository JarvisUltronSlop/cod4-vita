#pragma once

// HUD vertices are already in native clip coordinates. Neither world nor
// weapon transforms, lighting, normal/specular maps or fog belong here.
constexpr bool GfxVitaHudShaderEligible(bool hud, bool world, bool viewmodel,
    bool instanced, bool clear, unsigned benchmark) {
    return hud && !world && !viewmodel && !instanced && !clear && !benchmark;
}
constexpr bool GfxVitaUseHudShader(bool eligible, bool ready,
    bool comparison, unsigned phase) {
    return eligible && ready && (!comparison || phase == 2);
}

static constexpr const char* kVitaHudVertex = R"VITA_SHADER(
#version 100
precision highp float;
attribute vec3 a_position;
attribute vec4 a_color;
attribute vec2 a_texcoord;
varying vec4 v_color;
varying vec2 v_texcoord;
void main() {
    gl_Position = vec4(a_position, 1.0);
    v_color = a_color;
    v_texcoord = a_texcoord;
}
)VITA_SHADER";

static constexpr const char* kVitaHudFragment = R"VITA_SHADER(
#version 100
precision highp float;
uniform sampler2D u_color;
uniform float u_has_color;
uniform float u_alpha_test_enabled;
uniform float u_alpha_test_func;
varying vec4 v_color;
varying vec2 v_texcoord;
void main() {
    vec4 material = v_color;
    if (u_has_color > 0.5)
        material *= texture2D(u_color, v_texcoord);
    if (u_alpha_test_enabled > 0.5) {
        if (u_alpha_test_func < 0.5 && material.a <= 0.0)
            discard;
        if (u_alpha_test_func >= 0.5 && u_alpha_test_func < 1.5
            && material.a >= 0.5)
            discard;
        if (u_alpha_test_func >= 1.5 && material.a < 0.5)
            discard;
    }
    gl_FragColor = material;
}
)VITA_SHADER";
