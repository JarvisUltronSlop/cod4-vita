#pragma once

// The generic color path explicitly disables normal/specular/lightmap inputs.
// Instanced vertices already include the authored six-direction baked light.
// Keep local-light draws on the original shader, even for tiny valid colors.
constexpr bool GfxVitaFastColorEligible(bool instanced, bool worldSpace,
    bool viewmodel, bool baked, bool color, bool alphaTest, bool blend,
    bool localLoops, bool adaptive, bool hud, bool sky, unsigned benchmark) {
    return instanced && worldSpace && !viewmodel && baked && color && alphaTest
        && !blend && !localLoops && adaptive && !hud && !sky && benchmark==0;
}

static constexpr const char* kVitaFastColorFragment = R"VITA_SHADER(
#version 100
precision highp float;
uniform sampler2D u_color;
uniform float u_alpha_test_enabled;
uniform float u_alpha_test_func;
uniform float u_fog_enabled;
uniform float u_fog_density;
uniform float u_fog_start;
uniform vec3 u_fog_color;
varying vec4 v_color;
varying vec2 v_texcoord;
varying float v_viewdistance;
void main() {
    vec4 material = v_color * texture2D(u_color, v_texcoord);
    if (u_alpha_test_enabled > 0.5) {
        if (u_alpha_test_func < 0.5 && material.a <= 0.0)
            discard;
        if (u_alpha_test_func >= 0.5 && u_alpha_test_func < 1.5
            && material.a >= 0.5)
            discard;
        if (u_alpha_test_func >= 1.5 && material.a < 0.5)
            discard;
    }
    vec3 lit = material.rgb;
    if (u_fog_enabled > 0.5) {
        float distance = max(v_viewdistance, 0.0);
        float fog = 1.0 - exp(-u_fog_density
            * max(distance - u_fog_start, 0.0));
        lit = mix(lit, u_fog_color, clamp(fog, 0.0, 1.0));
    }
    gl_FragColor = vec4(lit, material.a);
}
)VITA_SHADER";

static constexpr const char* kVitaFastClearFragment = R"VITA_SHADER(
#version 100
precision highp float;
varying vec4 v_color;
void main() { gl_FragColor = v_color; }
)VITA_SHADER";
