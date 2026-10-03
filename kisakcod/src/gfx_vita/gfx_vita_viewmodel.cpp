#include <vita/vita_cpu_render.h>
#include <vita/vita_cpu_profile.h>
#include "gfx_vita_viewmodel.h"
#if KISAK_VITA_CLEAN_VIEWMODEL
#include <vitaGL.h>
#include <psp2/kernel/clib.h>

namespace {
GLuint program = 0, vao = 0;
GLint colorSampler = -1;
constexpr const char* vertexSource = R"GLSL(
#version 100
attribute vec3 position;
attribute vec2 texcoord;
attribute vec4 color;
varying vec2 uv;
varying vec4 modelLight;
void main() {
    gl_Position = vec4(position.xy, position.z - 0.2, position.z);
    uv = texcoord;
    modelLight = color;
}
)GLSL";
constexpr const char* fragmentSource = R"GLSL(
#version 100
precision highp float;
uniform sampler2D diffuse;
varying vec2 uv;
varying vec4 modelLight;
void main() {
    gl_FragColor = texture2D(diffuse, uv) * modelLight;
}
)GLSL";

GLuint Compile(GLenum type, const char* text) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char message[512]{};
        glGetShaderInfoLog(shader, sizeof(message), nullptr, message);
        sceClibPrintf("[kisak-vita] clean viewmodel shader failed: %s\n", message);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}
}

bool GfxVitaViewmodel_Init() {
    if (program) return true;
    VITA_CPU_LOADING("viewmodel-program-create");
    const GLuint vertex = Compile(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragment = Compile(GL_FRAGMENT_SHADER, fragmentSource);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return false;
    }
    program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "position");
    glBindAttribLocation(program, 1, "texcoord");
    glBindAttribLocation(program, 2, "color");
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) { GfxVitaViewmodel_Shutdown(); return false; }
    colorSampler = glGetUniformLocation(program, "diffuse");
    glGenVertexArrays(1, &vao);
    if (!vao || colorSampler == -1) { GfxVitaViewmodel_Shutdown(); return false; }
    sceClibPrintf("[kisak-vita] clean viewmodel ready: isolated program/VAO, source skin/FOV/diffuse/model-light; diagnostic fidelity baseline\n");
    return true;
}

void GfxVitaViewmodel_Shutdown() {
    if (vao) glDeleteVertexArrays(1, &vao);
    if (program) glDeleteProgram(program);
    vao = program = 0;
    colorSampler = -1;
}

bool GfxVitaViewmodel_Draw(uint32_t vertices, uint32_t indices,
    uint32_t firstVertex, uint32_t firstIndex, uint32_t indexCount,
    uint32_t texture) {
    if (!program || !vao || !vertices || !indices || !texture || indexCount < 3) return false;
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vertices);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices);
    glUseProgram(program);
    glUniform1i(colorSampler, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    // Original first-person depth allocation. Source material cull/blend
    // state is applied by the caller. No scene reset or second draw.
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glDepthRangef(0.0f, 0.015625f);
    const uintptr_t base = firstVertex * sizeof(GfxVitaViewmodelVertex);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GfxVitaViewmodelVertex),
        reinterpret_cast<const void*>(base + offsetof(GfxVitaViewmodelVertex, position)));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GfxVitaViewmodelVertex),
        reinterpret_cast<const void*>(base + offsetof(GfxVitaViewmodelVertex, texCoord)));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GfxVitaViewmodelVertex),
        reinterpret_cast<const void*>(base + offsetof(GfxVitaViewmodelVertex, color)));
    static uint32_t observed = 0;
    if (observed++ < 5) {
        GLint target = 0, viewport[4]{};
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &target);
        glGetIntegerv(GL_VIEWPORT, viewport);
        sceClibPrintf("[kisak-vita] clean draw target=%u viewport=%d,%d,%d,%d program=%u attributes=%d,%d,%d vbo=%u ibo=%u texture=%u first=%u count=%u\n",
            static_cast<unsigned>(target), viewport[0], viewport[1], viewport[2], viewport[3], program,
            glGetAttribLocation(program, "position"), glGetAttribLocation(program, "texcoord"),
            glGetAttribLocation(program, "color"), vertices, indices, texture, firstIndex, indexCount);
    }
#if KISAK_VITA_CPU_RUNTIME
    if(GfxVita_SceneSubmissionEnabled() && GfxVita_ProfileDrawSubmission())
#endif
    glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_SHORT,
        reinterpret_cast<const void*>(static_cast<uintptr_t>(firstIndex) * sizeof(uint16_t)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glDepthRangef(0.015625f, 1.0f);
    return glGetError() == GL_NO_ERROR;
}
#endif
