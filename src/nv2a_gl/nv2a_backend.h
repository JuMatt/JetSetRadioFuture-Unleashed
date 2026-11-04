/*
 * The line between "what the NV2A was asked to do" and "how this machine does
 * it".
 *
 * Everything above this line -- decoding the pushbuffer, tracking surfaces and
 * texture formats, deciding when a frame has ended, translating vertex
 * microcode and register combiners, caching programs and textures -- lives in
 * nv2a_gl.c and is the same on every platform. Everything below it is one
 * graphics API: OpenGL (nv2a_gfx_gl.c, every platform) or Metal
 * (nv2a_metal/nv2a_metal.m, Apple's).
 *
 * The device interface (Nv2aGfx, at the foot of this file) is small and close
 * to the metal on purpose. An earlier version of it was pitched higher -- "here
 * is a program descriptor, here are decoded texels, cache them yourself" --
 * and the OpenGL half never moved onto it, because everything worth having in
 * the renderer (the program cache, the texture cache, the surface cache, the
 * render-to-texture rules, two hundred bring-up switches) would have had to be
 * written twice. So the front half keeps all of that, and a backend is asked
 * for render targets, textures, programs, uniforms by name, state, vertices
 * and draws. Both APIs can do every one of those things; what differs is how,
 * and that is all a backend contains.
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
 * rate cap throttle the game three times too hard. The front half calls
 * present() once per flip and never otherwise.
 *
 * THE FIXED-FUNCTION MATRICES DO NOT BELONG IN THE CONSTANT FILE. The hardware
 * mirrors them into its low end, and copying that faithfully destroys the
 * constants a title's own vertex programs read -- this one writes them from
 * index 0 upward. They are uniforms of their own (ffMat, ffTexMat) and never
 * touch c[].
 *
 * CLIP SPACE IS NOT THE SAME SHAPE. OpenGL's clip volume runs z from -w to w;
 * Metal's and D3D's from 0 to w, and the console is D3D. Getting this backwards
 * does not produce a black screen: it produces a picture where everything is
 * in the right place with half the depth range collapsed, which reads as
 * z-fighting and bad sorting rather than as a coordinate bug. The shared
 * epilogue emits the remap under NV2A_CLIP_Z01, which the Metal prelude
 * defines to 1 and the GLSL one to 0.
 *
 * THE PICTURE IS NOT THE SAME WAY UP IN MEMORY. OpenGL keeps a render target
 * bottom row first and Metal top row first, while the shaders put the top of
 * the title's screen at clip y = +1 in both. So a surface sampled as a texture
 * needs its t turned over on OpenGL and not on Metal (rt_bottom_up says
 * which), a scissor or clear rectangle -- which the front half hands over
 * top-down, in raster pixels -- is flipped by the OpenGL backend and not by
 * Metal's, and target_read() returns rows bottom-up from both, because every
 * consumer of a read-back frame (the BMP writer, the window, the copy into
 * guest memory) was written against glReadPixels.
 *
 * THE INTERNAL RESOLUTION IS NOT THE SURFACE SIZE. The title asks for 640x480
 * and everything it can observe stays 640x480 -- the surface, the clip
 * rectangles, its screen-space passes. The raster does not have to: the
 * transform ends in normalised coordinates. So targets are created at the
 * raster's size and only rectangles (scissor, clear) are multiplied by the
 * scale, in the front half, before they cross this line.
 *
 * A PROGRAM CACHE SIZED FOR A MENU IS NOT SIZED FOR A LEVEL. 128 entries had
 * the driver's shader compiler taking a fifth of every frame, forever, because
 * the table filled in seconds and every miss evicted something wanted again
 * moments later. The front half caches a level's worth (2048) and indexes by
 * hash; a backend is only asked to build a program when that cache misses.
 *
 * ONLY FETCH THE ATTRIBUTES THE PROGRAM READS. Expanding every vertex to all
 * sixteen attributes was 39 MB of vertex data and 32 million decode calls a
 * second. An attribute a program reads but the title supplies no array for is
 * one value for the whole draw -- a constant vertex attribute -- not four
 * floats repeated in every vertex. And one the title supplies neither for
 * keeps whatever value it last had, as a GL generic attribute does: a backend
 * without generic attributes (Metal) keeps that value itself.
 *
 * BLEND STATE IS PART OF THE PIPELINE IN METAL. OpenGL sets it between draws;
 * Metal compiles it into the render pipeline object along with the colour
 * write mask. So state() hands both over every draw and the Metal backend
 * keeps pipelines keyed on (program, blend, mask), while the front half's
 * program cache stays keyed on the shaders alone.
 */

#ifndef NV2A_BACKEND_H
#define NV2A_BACKEND_H

#include <stdint.h>
#include "nv2a_vsh.h"
#include "nv2a_psh.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The front half's own record of the transform state: what the title has
 * written to the registers that feed the vertex stage. Not part of the device
 * interface -- uniforms cross the line one at a time, by name, below -- but
 * kept here beside it because it is what those uniforms are made from. */
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
typedef struct Nv2aRenderState {
    int   depth_test, depth_write, depth_func;
    int   blend_enable, blend_src, blend_dst;
    int   alpha_test;
    int   cull_enable, cull_face, front_face;
    /* The window clip, as a scissor: x0, y0, x1, y1 in RASTER pixels (the
     * scale already applied), top-down, x1/y1 exclusive. Each backend turns it
     * whichever way its own framebuffer runs. */
    int      scissor_enable;
    uint32_t scissor[4];
    /* NV097_SET_COLOR_MASK as the console holds it: bits 0, 8, 16 and 24 for
     * blue, green, red and alpha. 0 means the register was never written,
     * which the console treats as all channels enabled. */
    uint32_t color_mask;
    /* NV097_SET_ZMIN_MAX_CONTROL: bits 4-7 non-zero means a fragment past the
     * near or far plane is clamped to it rather than culled. */
    uint32_t zclamp;
    int      w_buffer;       /* CONTROL0 Z_PERSPECTIVE_ENABLE: depth is w */
    /* NV097_SET_STENCIL_*: the test, its function, reference and read mask,
     * the write mask, and the three operations (fail, z-fail, z-pass). The
     * console's values are OpenGL's enums. */
    int      stencil_test;
    uint32_t stencil_func, stencil_ref, stencil_rmask, stencil_wmask;
    uint32_t stencil_op[3];
    /* Polygon offset for filled polygons, as glPolygonOffset takes it: the
     * slope's factor (per raster pixel) and units of the depth buffer, already
     * turned round for reversed depth. 0 in poly_offset: none. */
    int      poly_offset;
    float    poly_offset_factor, poly_offset_units;
} Nv2aRenderState;

/* How a texture is sampled. Set when it is created and kept with it, which is
 * what OpenGL's texture-object parameters did and what the front half's
 * texture cache was written against. */
typedef struct {
    uint8_t min_linear, mag_linear;
    uint8_t mip;             /* 0 none (sample level 0), 1 nearest, 2 linear */
    uint8_t wrap_s, wrap_t;  /* 0 clamp to edge, 1 repeat, 2 mirrored repeat */
    uint8_t aniso;           /* 0 or 1: off */
    /* With `mip`: how many levels tex_new's texels hold, level 0 first and
     * each after it half the size of the one before (at least 1x1), packed
     * one after another -- the title's own chain. 0 or 1: the base level
     * only, and the device makes the rest of the chain itself. */
    uint8_t levels;
} Nv2aSampler;

/* Primitive types, after the front half has expanded quads into triangles. */
enum {
    NV2A_PRIM_POINTS = 1, NV2A_PRIM_LINES, NV2A_PRIM_LINE_LOOP,
    NV2A_PRIM_LINE_STRIP, NV2A_PRIM_TRIANGLES, NV2A_PRIM_TRIANGLE_STRIP,
    NV2A_PRIM_TRIANGLE_FAN
};

/* The shading language a backend compiles. */
enum { NV2A_LANG_GLSL = 0, NV2A_LANG_MSL = 1 };

/* The first lines of every GLSL program: OpenGL 3.3 core, or OpenGL ES 3.2
 * (NV2A_GLES: Android), which wants its version line and then a precision for
 * floats and ints -- a fragment shader has no default for floats -- and for
 * the sampler types that have none. Spliced defines go after the first line
 * (nv2a_gl.c's splice_define), which is still the #version. */
#if defined(NV2A_GLES)
#  define NV2A_GLSL_VERSION "#version 320 es\n"                               \
    "precision highp float;\nprecision highp int;\n"                        \
    "precision highp sampler2D;\nprecision highp sampler3D;\n"              \
    "precision highp samplerCube;\nprecision highp sampler2DShadow;\n"
#else
#  define NV2A_GLSL_VERSION "#version 330 core\n"
#endif

/*
 * Every uniform the generated shaders read, by name.
 *
 * The GLSL emitters declare them as plain uniforms and the OpenGL backend
 * looks each one up per program, exactly as the renderer always did. The
 * Metal emitters declare one struct per stage (Nv2aVU, Nv2aFU), generated
 * from the table in nv2a_uniforms.c, and the Metal backend writes each value
 * at the offset the same table gives -- one definition, so the two sides of
 * the buffer cannot drift apart. c[] is the exception: 3 KB that changes far
 * less often than the rest, so on Metal it is a buffer of its own.
 */
typedef enum {
    /* vertex stage */
    NV2A_U_C = 0,      NV2A_U_VPSCALE,    NV2A_U_VPOFF,      NV2A_U_ZCLIP,
    NV2A_U_VPSURFACE,  NV2A_U_POSMODE,    NV2A_U_FOGENABLE,  NV2A_U_FFMAT,
    NV2A_U_FFVPOFF,    NV2A_U_LITAMBIENT, NV2A_U_FFTEXMAT,   NV2A_U_FFMV,
    NV2A_U_FFIMV,      NV2A_U_LTSCENEAMB, NV2A_U_LTMATEM,    NV2A_U_LTMATALPHA,
    NV2A_U_LTAMB,      NV2A_U_LTDIF,      NV2A_U_LTSPC,      NV2A_U_LTDIR,
    NV2A_U_LTHALF,     NV2A_U_LTPOS,      NV2A_U_LTATT,      NV2A_U_LTRANGE,
    NV2A_U_LTSPOTDIR,  NV2A_U_LTEYE,      NV2A_U_LTSPECPOW,  NV2A_U_WIDESHIFT,
    NV2A_U_FOGMODE,    NV2A_U_FOGPARAM,   NV2A_U_FOGPLANE,  NV2A_U_VPRECT,
    NV2A_U_ZREV,
    /* fragment stage */
    NV2A_U_DRAWTINT,   NV2A_U_FOGCOLOR,   NV2A_U_ALPHAFUNC,  NV2A_U_ALPHAREF,
    NV2A_U_TEX0,                              /* ..3: sampler units, GL only */
    NV2A_U_TEXSCALE0 = NV2A_U_TEX0 + 4,       /* ..3 */
    NV2A_U_TEXOFF0   = NV2A_U_TEXSCALE0 + 4,  /* ..3 */
    NV2A_U_BUMPMAT0  = NV2A_U_TEXOFF0 + 4,    /* ..3 */
    NV2A_U_BUMPSCALE0 = NV2A_U_BUMPMAT0 + 4,  /* ..3 */
    NV2A_U_BUMPOFF0  = NV2A_U_BUMPSCALE0 + 4, /* ..3 */
    NV2A_U_PSHC      = NV2A_U_BUMPOFF0 + 4,   /* the combiner constants (nv2a_psh_consts.h) */
    NV2A_U_COUNT
} Nv2aUniformId;

enum { NV2A_UT_FLOAT, NV2A_UT_VEC2, NV2A_UT_VEC3, NV2A_UT_VEC4,
       NV2A_UT_INT, NV2A_UT_MAT2, NV2A_UT_SAMPLER };

typedef struct {
    const char *name;       /* as the GLSL declares it */
    uint8_t     type;       /* NV2A_UT_* */
    uint8_t     frag;       /* 0 vertex stage, 1 fragment stage */
    uint16_t    count;      /* array length, 1 for a plain value */
} Nv2aUniformDesc;

extern const Nv2aUniformDesc nv2a_uniform_desc[NV2A_U_COUNT];

/* Where a uniform sits in its stage's Metal struct, by the Metal Shading
 * Language's layout rules (float3 takes sixteen bytes, float2x2 sixteen with
 * eight-byte alignment). Returns 0 for one that is not in a struct (c[], the
 * samplers). `size` is the struct's whole size, for the stage. */
int nv2a_uniform_msl_offset(int id, uint32_t *offset, uint32_t *stride);
uint32_t nv2a_uniform_msl_struct_size(int frag);

/* The two structs' declarations, for the emitters: "struct Nv2aVU {...};" or
 * "struct Nv2aFU {...};" and the #defines that let a shader body read the
 * members by their bare GLSL names through `prefix` (U or F). Return the
 * number of characters written, 0 if `cap` was too small. */
int nv2a_uniform_msl_struct(char *buf, int cap, int frag);
int nv2a_uniform_msl_defines(char *buf, int cap, int frag, const char *prefix);

/* The vertex-fetch table a Metal vertex function reads its inputs through
 * (vertex pulling: one buffer of packed floats, an offset per attribute, and
 * the value of every attribute that has no array). Host twin of the MSL
 * struct nv2a_vsh.c emits. */
typedef struct {
    uint32_t stride;          /* floats per vertex */
    uint32_t mask;            /* bit N: attribute N comes from the array */
    uint32_t pad[2];
    uint32_t off[16];         /* float offset of each, within a vertex */
    float    value[16][4];    /* the others: one value for the whole draw */
} Nv2aMslAttrTable;

/* Metal buffer indices the emitted functions and the backend agree on. */
#define NV2A_MSL_BUF_VERTS    0
#define NV2A_MSL_BUF_CONSTS   1
#define NV2A_MSL_BUF_VU       2
#define NV2A_MSL_BUF_ATTRS    3
#define NV2A_MSL_BUF_FU       0

/*
 * The device.
 *
 * Handles are small positive integers the backend hands out; 0 means none.
 * Everything is called on the one thread that walks the pushbuffer.
 */
typedef struct Nv2aGfx {
    const char *name;
    int  lang;               /* NV2A_LANG_* */
    int  rt_bottom_up;       /* render targets hold the bottom row first */

    /* Bring the device up, once. 0 means it cannot run here, which is not an
     * error: it is how a build that has both picks one. (OpenGL's context is
     * already current when this is called.) */
    int      (*init)(void);

    /* Render targets: an RGBA8 colour buffer and a depth/stencil buffer,
     * pw x ph raster pixels. bind makes one current and sets the viewport to
     * the whole of it. */
    uint32_t (*target_new)(uint32_t pw, uint32_t ph);
    void     (*target_free)(uint32_t rt);
    void     (*target_bind)(uint32_t rt);
    /* Its colour as a texture handle, to sample a finished surface. */
    uint32_t (*target_texture)(uint32_t rt);
    /* A copy of its colour as it stands, to sample the surface being drawn
     * into. Valid until the next call. */
    uint32_t (*target_snapshot)(uint32_t rt);
    /* Read back colour, bottom row first, 4 bytes a pixel, BGRA or RGBA.
     * (x, y) counts from the bottom-left, as glReadPixels does. 1 on
     * success. */
    int      (*target_read)(uint32_t rt, uint32_t x, uint32_t y, uint32_t w,
                            uint32_t h, int bgra, void *out);
    /* The depth buffer as floats, bottom row first; 0 if unsupported. */
    int      (*target_read_depth)(uint32_t rt, uint32_t pw, uint32_t ph,
                                  float *out);

    /* Textures: w x h RGBA8 texels, row 0 first, sampled as `s` says. */
    uint32_t (*tex_new)(uint32_t w, uint32_t h, const void *rgba,
                        const Nv2aSampler *s);
    void     (*tex_free)(uint32_t tex);
    /* Its texels back, BGRA, for dumps; 0 if unsupported. */
    int      (*tex_read)(uint32_t tex, uint32_t w, uint32_t h, void *bgra);

    /* Programs, from source in this backend's language. 0 on failure, having
     * said why. */
    uint32_t (*prog_new)(const char *vsrc, const char *fsrc, const char *what);
    void     (*prog_free)(uint32_t prog);
    /* Whether the program reads this uniform at all; the front half skips
     * work for one it does not. */
    int      (*prog_uses)(uint32_t prog, int id);
    void     (*prog_use)(uint32_t prog);

    /* Uniforms of the program in use, by Nv2aUniformId: `n` array elements
     * of the uniform's own type (vec3 as three packed floats, mat2 as four,
     * column-major). */
    void     (*uniform_f)(int id, const float *v, int n);
    void     (*uniform_i)(int id, int v);

    void     (*texture)(int unit, uint32_t tex);
    void     (*state)(const Nv2aRenderState *rs);
    /* Forget what state was last set: the next state() applies all of it. */
    void     (*state_invalidate)(void);

    /* The vertices for the draws that follow: packed floats, `attr_at[i]` the
     * float offset of attribute i within a vertex or -1, and the values of
     * the attributes in `const_mask`, which have no array. */
    void     (*vertices)(const float *v, uint32_t nverts, uint32_t stride_floats,
                         const int attr_at[16], uint32_t const_mask,
                         const float const_attr[16][4]);
    void     (*draw)(int prim, uint32_t count);

    /* NV097_CLEAR_SURFACE on the bound target: mask bit 0 colour, 1 depth,
     * 2 stencil. `rect` is x0, y0, x1, y1 in raster pixels, top-down,
     * exclusive, or NULL for all of it. Ignores the colour mask, as the
     * console's clear does. */
    void     (*clear)(unsigned mask, const float rgba[4], float depth,
                      uint32_t stencil, const uint32_t rect[4]);

    /* A flip: show target `rt` (pw x ph raster pixels) in the window if there
     * is one, and finish the frame's work. */
    void     (*present)(uint32_t rt, uint32_t pw, uint32_t ph);

    void     (*report)(void);

    /*
     * Everything below is optional: a backend that leaves it NULL gets every
     * draw through vertices() and draw() and every constant upload whole, as
     * before. (Metal does.)
     *
     * Geometry kept between draws. The front half decides what is kept, for
     * how long, and whether it is still what the title's memory holds (the
     * vertex cache in nv2a_gl.c); a backend only holds the buffers. A kept
     * buffer is vertices laid out as vertices() takes them and, for an
     * indexed draw, a list of 16- or 32-bit indices into them.
     */
    /* `stream`: refilled for nearly every draw (the one-off geometry). */
    uint32_t (*vbuf_new)(int stream);
    void     (*vbuf_free)(uint32_t vb);
    /* Replace what it holds: the vertices if `v`, the indices if `idx`. */
    void     (*vbuf_fill)(uint32_t vb, const float *v, uint32_t vbytes,
                          const void *idx, uint32_t ibytes);
    /* vertices(), for the draws that follow, from a kept buffer. */
    void     (*vbuf_bind)(uint32_t vb, uint32_t stride_floats, const int attr_at[16],
                          uint32_t const_mask, const float const_attr[16][4]);
    /* `count` of the bound kept buffer's indices, 32-bit ones if `idx32`. */
    void     (*draw_indexed)(int prim, uint32_t count, int idx32);

    /* uniform_f() for elements first .. first+n-1 of an array uniform of
     * the program in use; elements past the end of the program's array are
     * skipped. 0 if the backend cannot address part of this one, and the
     * front half uploads the whole array instead. */
    int      (*uniform_f_part)(int id, int first, const float *v, int n);

    /* Milliseconds spent so far making pipelines at draw time -- the part
     * of building a program that a backend does lazily (Metal's pipeline
     * states) and prog_new does not cover. NULL where there is none. */
    double   (*pipeline_ms)(void);

    /* The shader cache (nv2a_shader_cache.h): open it in `dir` for `tag`
     * and get ready the programs earlier runs built -- in the background,
     * from saved binaries, or queued for cache_idle. */
    void     (*cache_open)(const char *dir, const char *tag);
    /* A frame with little to draw (a loading screen, a menu): spend about
     * `budget_ms` building queued programs. Returns how many are still
     * queued, 0 when there is nothing left to do. NULL where the backend
     * builds them on a thread of its own. */
    int      (*cache_idle)(double budget_ms);

    /* SMAA (nv2a_smaa.h): target `rt`'s picture anti-aliased, by three
     * passes over it into another target of the same size, which the backend
     * keeps -- the one to put on screen instead. `rt` itself is left as it
     * was, and is what comes back while the passes cannot run (not built
     * yet, or they would not build). NULL where the backend has none. */
    uint32_t (*target_smaa)(uint32_t rt);

    /* Multisampling: the most samples a pixel a target can have here (1:
     * none), and every target drawn with `n` from now on, what each holds
     * carried across -- or, where the backend can only take a count before
     * its first target (OpenGL ES, Metal), the count then and the one in use
     * after. Returns the count in use, fewer than asked where the device
     * refuses one. NULL where the backend has none. */
    int      (*samples_max)(void);
    int      (*samples)(int n);
    /* Reversed depth (nv2a_vsh.c, depth_defines): a 32-bit floating-point
     * depth buffer, and a clip volume with z from 0 to w. Asked once, before
     * the first target and the first program; 1 if from now on that is how
     * it is. NULL, or 0, where the device cannot -- an OpenGL without
     * glClipControl. */
    int      (*zrev)(int want);
    /* For a slow frame's line in the log: milliseconds so far that the GPU
     * spent on the work it finished, and that the render thread waited for
     * it (every frame's buffers in flight, none to record into). NULL where
     * the backend cannot say. */
    void     (*gpu_ms)(double *busy, double *waited);

    /* A DXT1, DXT3 or DXT5 texture (`dxt` 1, 3 or 5) as the title stored
     * it: w x h texels in 4x4 blocks, and with `s->levels` the rest of the
     * chain, each level's blocks after the last's. Sampled as tex_new's.
     * 0 where the device does not take them as they are, and the front half
     * decodes them for tex_new instead (Metal: where the GPU has BC
     * formats). NULL where the backend has none. */
    uint32_t (*tex_new_dxt)(uint32_t w, uint32_t h, int dxt, const void *blocks,
                            const Nv2aSampler *s);
} Nv2aGfx;

/* The implementations. Each returns NULL where it cannot run (no Metal
 * device, no Metal in this build). */
const Nv2aGfx *nv2a_gfx_gl(void);
const Nv2aGfx *nv2a_gfx_metal(void);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_BACKEND_H */
