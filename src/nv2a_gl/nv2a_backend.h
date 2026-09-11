/*
 * The line between "what the NV2A was asked to do" and "how this machine does
 * it".
 *
 * Everything above this line -- decoding the pushbuffer, tracking surfaces and
 * texture formats, deciding when a frame has ended, translating vertex
 * microcode and register combiners -- is the same on every platform and is
 * roughly two thirds of the backend. Everything below it is one graphics API.
 * The OpenGL implementation exists and runs this title at sixty frames a
 * second; the Metal one is what makes iOS possible at all, since iOS has no
 * OpenGL.
 *
 * The interface is deliberately small and deliberately not a graphics API. It
 * does not expose buffers, or binding points, or a command stream: those are
 * exactly the things the two APIs disagree about. It exposes the handful of
 * operations a pushbuffer translator actually performs, in the terms the
 * console uses.
 *
 * ---------------------------------------------------------------------------
 * What an implementation has to get right, and what it costs to get wrong.
 * Each of these was learned the expensive way in the OpenGL backend.
 * ---------------------------------------------------------------------------
 *
 * A FRAME IS A FLIP, NOT A PASS. This title clears an off-screen scratch
 * surface twice a frame before drawing into one of two alternating display
 * buffers. Treating every finished pass as a frame shows the window
 * half-finished intermediate passes at three hundred hertz and makes a frame
 * rate cap throttle the game three times too hard. The frame boundary is
 * NV097_FLIP_STALL for a title that uses it, and the full-surface clear only
 * for one that does not.
 *
 * THE FIXED-FUNCTION MATRICES DO NOT BELONG IN THE CONSTANT FILE. The hardware
 * mirrors them into its low end, and copying that faithfully destroys the
 * constants a title's own vertex programs read -- this one writes them from
 * index 0 upward. They get their own storage here (Nv2aUniforms.ff_mat,
 * ff_texmat) and never touch c[].
 *
 * CLIP SPACE IS NOT THE SAME SHAPE. OpenGL's clip volume runs z from -w to w;
 * Metal's and D3D's from 0 to w, and the console is D3D. Getting this backwards
 * does not produce a black screen: it produces a picture where everything is
 * in the right place with half the depth range collapsed, which reads as
 * z-fighting and bad sorting rather than as a coordinate bug. The shared
 * epilogue emits the remap under NV2A_CLIP_Z01, which Metal defines and GL
 * does not.
 *
 * THE INTERNAL RESOLUTION IS NOT THE SURFACE SIZE. The title asks for 640x480
 * and everything it can observe stays 640x480 -- the surface, the clip
 * rectangles, its screen-space passes. The raster does not have to: the
 * transform ends in normalised coordinates. Measured on an M2 Pro, sixteen
 * times the pixels costs nothing, because the frame is bounded entirely by the
 * CPU side of the emulation. So `scale` is a parameter of the surface, not of
 * the geometry, and only the scissor and the viewport multiply by it.
 *
 * A PROGRAM CACHE SIZED FOR A MENU IS NOT SIZED FOR A LEVEL. 128 entries had
 * the driver's shader compiler taking a fifth of every frame, forever, because
 * the table filled in seconds and every miss evicted something wanted again
 * moments later. This title uses a few hundred distinct (vertex program,
 * combiner) pairs. Cache generously and index by hash rather than scanning.
 *
 * ONLY FETCH THE ATTRIBUTES THE PROGRAM READS. Expanding every vertex to all
 * sixteen attributes was 39 MB of vertex data and 32 million decode calls a
 * second. An attribute a program reads but the title supplies no array for is
 * one value for the whole draw -- a constant vertex attribute -- not four
 * floats repeated in every vertex.
 */

#ifndef NV2A_BACKEND_H
#define NV2A_BACKEND_H

#include <stdint.h>
#include "nv2a_vsh.h"
#include "nv2a_psh.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where each attribute sits in the packed vertex this draw hands over. The
 * NV2A attribute number is the shader's input index in both dialects, so one
 * of these serves a GL vertex array and a Metal vertex descriptor alike. */
typedef struct {
    uint8_t attr;            /* 0..15, the NV2A register number */
    uint8_t float_offset;    /* where it starts within the vertex */
} Nv2aAttrSlot;

/* Everything both shader stages read that is not a texture. Laid out to match
 * the generated `Nv2aVshUniforms` and `Nv2aFfUniforms` structs: float4 is
 * 16-byte aligned and the trailing scalars pack into one more slot. */
typedef struct {
    float   c[NV2A_VSH_NUM_CONSTS][4];  /* the title's own constant file */
    float   ff_mat[4][4];               /* NV097_SET_COMPOSITE_MATRIX */
    float   ff_texmat[16][4];           /* NV097_SET_TEXTURE_MATRIX, 4 stages */
    float   vp_scale[4], vp_off[4];     /* NV097_SET_VIEWPORT_SCALE / _OFFSET */
    float   z_clip[2];                  /* NV097_SET_CLIP_MIN / _MAX */
    float   vp_surface[2];              /* logical surface size, not the raster */
    int32_t pos_mode;                   /* how to read what the program wrote */
    float   tex_scale[4][2];
    float   fog_color[4];
    int32_t alpha_func;
    float   alpha_ref;
} Nv2aUniforms;

/* The per-draw state that is not a shader and not a uniform. Named for the
 * console's registers rather than either API's enums, so that neither API's
 * spelling leaks across the line. */
/* Blend state has to be in here AND in the program hash, because Metal
 * compiles it into the pipeline object while OpenGL sets it between draws.
 * The front half folds it into the hash for both; the GL backend then ignores
 * it in program() and applies it in state(), and the Metal backend does the
 * reverse. Neither is wrong, and the split is the one real structural
 * difference between the two APIs for this job. */
typedef struct Nv2aRenderState {
    int   depth_test, depth_write, depth_func;
    int   blend_enable, blend_src, blend_dst;
    int   alpha_test;
    int   cull_enable, cull_face, front_face;
    /* The window clip, as a scissor. In the title's coordinates, top-down,
     * inclusive of both corners -- the shape the registers hold -- so that
     * each backend flips and scales it in whichever direction its own
     * framebuffer runs, rather than receiving one that has already been
     * flipped for the other one. */
    int      scissor_enable;
    uint32_t scissor[4];
    /* NV097_SET_COLOR_MASK as the console holds it: bits 0, 8, 16 and 24 for
     * blue, green, red and alpha. 0 means the register was never written,
     * which the console treats as all channels enabled. */
    uint32_t color_mask;
} Nv2aRenderState;

typedef struct {
    int wrap_s, wrap_t;      /* NV097_SET_TEXTURE_ADDRESS */
    int min_filter, mag_filter;
} Nv2aSampler;

typedef struct {
    const char *name;

    /* Bring the device up. Returns 0 if this backend cannot run here, which
     * is not an error: it is how a build that has both picks one. */
    int  (*init)(void);

    /* Make `offset` the render target, `w` by `h` in the title's terms,
     * rasterised at `scale` times that. Creates it if this is the first time
     * that address has been drawn into. */
    void (*surface)(uint32_t offset, uint32_t w, uint32_t h, uint32_t scale);

    /* NV097_CLEAR_SURFACE. `rect` is in the title's coordinates, top-down,
     * or NULL for the whole surface. */
    void (*clear)(unsigned mask, const float rgba[4], float depth,
                  uint32_t stencil, const uint32_t rect[4]);

    /* A compiled program, identified by a hash of the microcode and the
     * combiner state together -- a title reuses one vertex program with
     * several combiner setups and vice versa, and caching on either alone
     * hands back a shader that is half right. Returns a handle, or -1.
     *
     * The backend emits its own shading language from the descriptor. That is
     * the whole reason this takes a descriptor rather than source text. */
    int  (*program)(const Nv2aVshProgram *vp,   /* NULL for fixed-function */
                    const Nv2aVshFixed *ff,     /* NULL for a real program */
                    const Nv2aPshState *ps,
                    const struct Nv2aRenderState *rs,
                    uint32_t hash);

    /* Decoded texels, already converted to RGBA8 and unswizzled. `key`
     * identifies the contents so the backend can keep its own cache. */
    void (*texture)(int stage, const void *rgba, uint32_t w, uint32_t h,
                    uint32_t key, const Nv2aSampler *s);

    void (*state)(const Nv2aRenderState *rs);

    /* One batch. `verts` is packed to `stride_floats` with `slots` describing
     * it; `const_attr_mask` marks attributes the program reads that have no
     * array, whose value is in `const_attr` and applies to the whole draw. */
    void (*draw)(int prog, int prim,
                 const float *verts, uint32_t nverts, uint32_t stride_floats,
                 const Nv2aAttrSlot *slots, int nslots,
                 const Nv2aUniforms *u,
                 uint32_t const_attr_mask, const float const_attr[16][4]);

    /* The finished frame, as BGRA8 at the raster's size -- which is the
     * surface size times the scale, not the surface size. The pointer stays
     * valid until the next call. */
    void (*present)(const uint8_t **pixels, uint32_t *w, uint32_t *h);

    void (*report)(void);
} Nv2aBackend;

/* The implementations. Each returns NULL where it cannot be built at all. */
const Nv2aBackend *nv2a_backend_gl(void);
const Nv2aBackend *nv2a_backend_metal(void);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_BACKEND_H */
