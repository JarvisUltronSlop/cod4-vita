#pragma once

// Diagnostic fragment programs only. Geometry, texture identity/sampling,
// vertex colours, alpha-test equations, depth and blending stay unchanged.
// Fog is retained so this test isolates material shading rather than fog.
inline constexpr const char* kBenchmarkSimpleMaterialFragment = R"VITA_SHADER(
#version 100
precision highp float;
uniform sampler2D u_color;
uniform float u_has_color;
uniform float u_alpha_test_enabled;
uniform float u_alpha_test_func;
uniform float u_fog_enabled;
uniform float u_fog_start;
uniform float u_fog_density;
uniform vec3 u_fog_color;
varying vec2 v_texcoord;
varying vec4 v_color;
varying float v_viewdistance;
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

inline constexpr const char* kBenchmarkPostCopyFragment = R"VITA_SHADER(
#version 100
precision highp float;
uniform sampler2D u_scene;
varying vec2 v_texcoord;
void main() {
    gl_FragColor = vec4(texture2D(u_scene, v_texcoord).rgb, 1.0);
}
)VITA_SHADER";
