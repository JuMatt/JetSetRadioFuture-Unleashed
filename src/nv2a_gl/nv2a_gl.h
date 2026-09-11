/*
 * nv2a_gl.h -- OpenGL backend for the NV2A pushbuffer.
 *
 * The software rasteriser in nv2a_pb_exec.c answers "is the title drawing
 * anything sensible". This answers "what does it look like": it keeps the
 * full NV2A pipeline state, translates the title's vertex programs to GLSL,
 * uploads its textures, and draws with a depth buffer -- so the output is the
 * frame the console would have produced rather than a screen-space
 * approximation of it.
 *
 * It observes the same method stream and keeps its own state deliberately.
 * Sharing the rasteriser's state would tie the two together at exactly the
 * point where they need to disagree (it skips batches that need a vertex
 * program; this one runs them), and keeping them independent means the
 * software path stays available as a reference when this one draws something
 * wrong.
 *
 * Enabled by RECOMP_GL=1. Without it every entry point returns immediately
 * and nothing is initialised, so a build with this linked in behaves exactly
 * as it did before.
 */
#ifndef NV2A_GL_H
#define NV2A_GL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Is the GL backend switched on for this run? Cheap after the first call. */
int  nv2a_gl_enabled(void);

/* Every decoded pushbuffer method, in order. Safe to call before init. */
void nv2a_gl_method(uint32_t method, uint32_t param);

/* End-of-run summary: what was drawn, what was refused, and why. */
void nv2a_gl_report(void);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_GL_H */
