/*
 * test_capture.c -- replay one captured draw outside the game.
 *
 * A capture holds everything a single draw needed: the vertex program, the
 * whole constant bank, the viewport, and the packed vertices. Replaying it
 * here turns "why is this frame black" from a five-minute run of the title
 * into a second, and makes it possible to try three hypotheses in the time
 * one run would take.
 *
 *   ./test_capture cap0.bin [posMode]
 *
 * Prints the program, what it does to the first few vertices, and how much of
 * the surface the batch covers.
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

#define MAX_INSNS NV2A_VSH_MAX_INSNS

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
    uint32_t hdr[8];
    float vps[4], vpo[4];
    static uint32_t prog_tok[MAX_INSNS * 4];
    static float consts[192][4];
    float *verts;
    Nv2aVshProgram vp;
    char *vsrc, dis[32768];
    FILE *f;
    GLuint vs, fs, prog, vao, vbo, fbo, ctex, drb;
    GLint ok = 0;
    unsigned nverts, stride, inputs, prim, w, h;
    int i, pos_mode, nz = 0;
    unsigned char *px;

    if (argc < 2) { printf("usage: %s capture.bin [posMode]\n", argv[0]); return 1; }
    pos_mode = (argc > 2) ? atoi(argv[2]) : 0;

    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    if (fread(hdr, sizeof hdr, 1, f) != 1) { printf("short file\n"); return 1; }
    if (hdr[0] != 0x42544143u) { printf("not a capture\n"); return 1; }
    nverts = hdr[1]; stride = hdr[2]; inputs = hdr[3]; prim = hdr[4];
    w = hdr[5]; h = hdr[6];
    if (fread(vps, sizeof vps, 1, f) != 1) return 1;
    if (fread(vpo, sizeof vpo, 1, f) != 1) return 1;
    if (fread(prog_tok, sizeof prog_tok, 1, f) != 1) return 1;
    if (fread(consts, sizeof consts, 1, f) != 1) return 1;
    verts = (float *)malloc((size_t)nverts * stride * sizeof(float));
    if (fread(verts, sizeof(float) * nverts * stride, 1, f) != 1) return 1;
    fclose(f);

    /* The hardware mirrors the viewport into constants 58 and 59; a capture
     * taken before the backend did the same has them zeroed. Mirror here too
     * so a replay measures the shader, not the omission. */
    if (!getenv("NO_VP_MIRROR")) {
        memcpy(consts[58], vps, sizeof vps);
        memcpy(consts[59], vpo, sizeof vpo);
    }

    printf("%u vertices, prim %u, target %ux%u, inputs %04X\n",
           nverts, prim, w, h, inputs);
    printf("viewport scale (%.2f %.2f %.2f) offset (%.2f %.2f %.2f)\n",
           vps[0], vps[1], vps[2], vpo[0], vpo[1], vpo[2]);

    nv2a_vsh_decode(prog_tok, MAX_INSNS, &vp);
    if (nv2a_vsh_disasm(&vp, dis, sizeof dis))
        printf("--- program (%d slots, inputs %04X) ---\n%s",
               vp.length, vp.inputs_read, dis);

    vsrc = (char *)malloc(96 * 1024);
    if (!nv2a_vsh_emit_glsl(&vp, vsrc, 96 * 1024)) { printf("emit failed\n"); return 1; }
    if (getenv("SHOW_GLSL")) printf("--- glsl ---\n%s\n", vsrc);
    if (getenv("SHOW_MSL")) {
        char *msl = (char *)malloc(96 * 1024);
        if (nv2a_vsh_emit_msl(&vp, msl, 96 * 1024))
            printf("--- msl ---\n%s\n", msl);
        else
            printf("msl emit failed\n");
        free(msl);
    }

    dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    eglInitialize(dpy, &maj, &min);
    eglBindAPI(EGL_OPENGL_API);
    eglChooseConfig(dpy, cfga, &cfg, 1, &n);
    eglMakeCurrent(dpy, eglCreatePbufferSurface(dpy, cfg, pba),
                   eglCreatePbufferSurface(dpy, cfg, pba),
                   eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxa));

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

    glGenFramebuffers(1, &fbo); glGenTextures(1, &ctex);
    glGenRenderbuffers(1, &drb);
    glBindTexture(GL_TEXTURE_2D, ctex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindRenderbuffer(GL_RENDERBUFFER, drb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, (GLsizei)w, (GLsizei)h);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, drb);
    glViewport(0, 0, (GLsizei)w, (GLsizei)h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);

    glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)nverts * stride * sizeof(float), verts, GL_STREAM_DRAW);
    for (i = 0; i < 16; i++) {
        if (vp.inputs_read & (1u << i)) {
            glEnableVertexAttribArray(i);
            glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE,
                                  (GLsizei)(stride * sizeof(float)),
                                  (const void *)(uintptr_t)(i * 4 * sizeof(float)));
        } else glDisableVertexAttribArray(i);
    }

    glUseProgram(prog);
    glUniform4fv(glGetUniformLocation(prog, "c"), 192, &consts[0][0]);
    glUniform4fv(glGetUniformLocation(prog, "vpScale"), 1, vps);
    glUniform4fv(glGetUniformLocation(prog, "vpOff"), 1, vpo);
    glUniform2f(glGetUniformLocation(prog, "vpSurface"), (float)w, (float)h);
    glUniform1i(glGetUniformLocation(prog, "posMode"), pos_mode);

    /* What the shader makes of the first few vertices. Transform feedback is
     * the only way to see a vertex shader's output without inferring it from
     * pixels, and inference is exactly what has been going wrong. */
    {
        GLuint tf, tfbuf;
        GLuint fbprog;
        const char *varying = "gl_Position";
        float out[64];
        int k;

        fbprog = glCreateProgram();
        vs = mkshader(GL_VERTEX_SHADER, vsrc, "vertex(fb)");
        glAttachShader(fbprog, vs);
        glTransformFeedbackVaryings(fbprog, 1, &varying, GL_SEPARATE_ATTRIBS);
        glLinkProgram(fbprog);
        glGetProgramiv(fbprog, GL_LINK_STATUS, &ok);
        if (ok) {
            glGenBuffers(1, &tfbuf);
            glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, tfbuf);
            glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, sizeof out, NULL, GL_STATIC_READ);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, tfbuf);
            glUseProgram(fbprog);
            glUniform4fv(glGetUniformLocation(fbprog, "c"), 192, &consts[0][0]);
            glUniform4fv(glGetUniformLocation(fbprog, "vpScale"), 1, vps);
            glUniform4fv(glGetUniformLocation(fbprog, "vpOff"), 1, vpo);
            glUniform2f(glGetUniformLocation(fbprog, "vpSurface"), (float)w, (float)h);
            glUniform1i(glGetUniformLocation(fbprog, "posMode"), pos_mode);
            glEnable(GL_RASTERIZER_DISCARD);
            glGenTransformFeedbacks(1, &tf);
            glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, tf);
            glBeginTransformFeedback(GL_POINTS);
            glDrawArrays(GL_POINTS, 0, 4);
            glEndTransformFeedback();
            glDisable(GL_RASTERIZER_DISCARD);
            glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof(float) * 16, out);
            printf("gl_Position of the first 4 vertices:\n");
            for (k = 0; k < 4; k++)
                printf("   (%10.3f %10.3f %10.3f %10.3f)   ndc (%8.3f %8.3f %8.3f)\n",
                       out[k*4+0], out[k*4+1], out[k*4+2], out[k*4+3],
                       out[k*4+3] != 0 ? out[k*4+0]/out[k*4+3] : 0.0f,
                       out[k*4+3] != 0 ? out[k*4+1]/out[k*4+3] : 0.0f,
                       out[k*4+3] != 0 ? out[k*4+2]/out[k*4+3] : 0.0f);
            glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, 0);
        } else {
            char log[4096]; GLsizei l = 0;
            glGetProgramInfoLog(fbprog, sizeof log, &l, log);
            printf("transform feedback link failed: %.*s\n", (int)l, log);
        }
        glUseProgram(prog);
    }

    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)nverts);
    printf("glGetError after draw: 0x%04X\n", glGetError());

    px = (unsigned char *)malloc((size_t)w * h * 4);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (i = 0; i < (int)(w * h); i++)
        if (px[i*4] | px[i*4+1] | px[i*4+2]) nz++;
    printf("RESULT: %d / %u pixels covered (posMode %d)\n", nz, w * h, pos_mode);
    return 0;
}
