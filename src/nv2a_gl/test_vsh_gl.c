/*
 * test_vsh_gl.c -- does a translated NV2A vertex program actually draw?
 *
 * Feeds one real program, one real batch of vertices and the constants that
 * were live at the time through the same translator and the same GL setup the
 * backend uses, and reports how many pixels came out. When a frame comes back
 * empty in the game there are half a dozen candidates -- the decoder, the
 * generated GLSL, the attribute binding, the viewport inverse, the FBO. This
 * removes all but the first two in a second, without a five-minute rebuild.
 *
 *   ./test_vsh_gl vsh_XXXXXXXX.bin
 */
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_vsh.h"

static const char *FRAG =
"#version 330 core\n"
"in vec4 oD0; in vec4 oD1; in vec4 oT0; in vec4 oT1; in vec4 oT2; in vec4 oT3;\n"
"in float oFogC;\n"
"out vec4 frag;\n"
"void main() { frag = vec4(1.0, 0.0, 1.0, 1.0); }\n";

static GLuint mkshader(GLenum st, const char *src, const char *what)
{
    GLuint s = glCreateShader(st);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[8192]; GLsizei n = 0;
        glGetShaderInfoLog(s, sizeof log, &n, log);
        printf("%s failed:\n%.*s\n", what, (int)n, log);
        return 0;
    }
    return s;
}

int main(int argc, char **argv)
{
    static const EGLint cfga[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_NONE };
    static const EGLint pba[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    static const EGLint ctxa[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE };
    EGLDisplay dpy; EGLConfig cfg; EGLint n, maj, min;
    Nv2aVshProgram vp;
    uint32_t tokens[NV2A_VSH_MAX_INSNS * 4];
    char *vsrc, dis[16384];
    FILE *f;
    size_t got;
    GLuint vs, fs, prog, vao, vbo, fbo, ctex, drb;
    GLint ok = 0;
    int i, nz = 0;
    float consts[192][4];
    /* One vertex per row, sixteen attributes of four floats each -- the same
     * flat layout the backend builds. */
    static float verts[3][16 * 4];
    unsigned char *px;

    if (argc < 2) { printf("usage: %s program.bin\n", argv[0]); return 1; }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    got = fread(tokens, 1, sizeof tokens, f);
    fclose(f);
    printf("read %zu bytes (%zu instructions)\n", got, got / 16);

    nv2a_vsh_decode(tokens, (int)(got / 16), &vp);
    if (nv2a_vsh_disasm(&vp, dis, sizeof dis))
        printf("--- program ---\n%s", dis);
    printf("inputs read: %04X\n", vp.inputs_read);

    vsrc = malloc(96 * 1024);
    if (!nv2a_vsh_emit_glsl(&vp, vsrc, 96 * 1024)) { printf("emit failed\n"); return 1; }
    if (getenv("SHOW_GLSL")) printf("--- glsl ---\n%s\n", vsrc);

    dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    eglInitialize(dpy, &maj, &min);
    eglBindAPI(EGL_OPENGL_API);
    eglChooseConfig(dpy, cfga, &cfg, 1, &n);
    eglMakeCurrent(dpy, eglCreatePbufferSurface(dpy, cfg, pba),
                   eglCreatePbufferSurface(dpy, cfg, pba),
                   eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxa));
    printf("GL: %s\n", (const char *)glGetString(GL_VERSION));

    vs = mkshader(GL_VERTEX_SHADER, vsrc, "vertex");
    fs = mkshader(GL_FRAGMENT_SHADER, FRAG, "fragment");
    if (!vs || !fs) return 1;
    prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { char log[8192]; GLsizei l = 0;
               glGetProgramInfoLog(prog, sizeof log, &l, log);
               printf("link failed:\n%.*s\n", (int)l, log); return 1; }

    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &ctex);
    glGenRenderbuffers(1, &drb);
    glBindTexture(GL_TEXTURE_2D, ctex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 640, 480, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindRenderbuffer(GL_RENDERBUFFER, drb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 640, 480);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, drb);
    printf("fbo status 0x%04X (complete = 0x%04X)\n",
           glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
    glViewport(0, 0, 640, 480);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    /* The batch the game was drawing when it came back empty: a screen-space
     * triangle covering the surface, positions in attribute 0. */
    memset(verts, 0, sizeof verts);
    for (i = 0; i < 3; i++) verts[i][3] = 1.0f;      /* v0.w = 1 */
    verts[0][0] = -0.531f; verts[0][1] = -0.531f;
    verts[1][0] = 2559.469f; verts[1][1] = -0.531f;
    verts[2][0] = -0.531f; verts[2][1] = 1919.469f;

    memset(consts, 0, sizeof consts);
    consts[0][0] = 1.0f; consts[0][1] = 1.0f;
    consts[0][2] = 16777215.0f; consts[0][3] = 1.0f;
    consts[1][0] = 0.531f; consts[1][1] = 0.531f;

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STREAM_DRAW);
    for (i = 0; i < 16; i++) {
        if (vp.inputs_read & (1u << i)) {
            glEnableVertexAttribArray(i);
            glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, 16 * 4 * sizeof(float),
                                  (const void *)(uintptr_t)(i * 4 * sizeof(float)));
        } else glDisableVertexAttribArray(i);
    }

    glUseProgram(prog);
    glUniform4fv(glGetUniformLocation(prog, "c"), 192, &consts[0][0]);
    { float sc[4] = { 320.0f, -240.0f, 16777215.0f, 0.0f };
      float of[4] = { 320.5f, 240.5f, 0.0f, 0.0f };
      glUniform4fv(glGetUniformLocation(prog, "vpScale"), 1, sc);
      glUniform4fv(glGetUniformLocation(prog, "vpOff"), 1, of);
      glUniform2f(glGetUniformLocation(prog, "vpSurface"), 640.0f, 480.0f); }
    printf("uniform locations: c=%d vpScale=%d vpOff=%d\n",
           glGetUniformLocation(prog, "c"),
           glGetUniformLocation(prog, "vpScale"),
           glGetUniformLocation(prog, "vpOff"));

    glDrawArrays(GL_TRIANGLES, 0, 3);
    printf("glGetError after draw: 0x%04X\n", glGetError());

    px = malloc(640 * 480 * 4);
    glReadPixels(0, 0, 640, 480, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (i = 0; i < 640 * 480; i++)
        if (px[i * 4] | px[i * 4 + 1] | px[i * 4 + 2]) nz++;
    printf("RESULT: %d / %d pixels covered\n", nz, 640 * 480);
    return nz ? 0 : 2;
}
