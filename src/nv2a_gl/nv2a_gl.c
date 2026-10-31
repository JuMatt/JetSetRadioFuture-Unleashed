/*
 * nv2a_gl.c -- NV2A pushbuffer -> OpenGL.
 *
 * Structure: a state machine fed by nv2a_gl_method(), and a draw path that
 * runs when the title closes a primitive. The state machine is the boring
 * half and the one that has to be right; the draw path is where the NV2A's
 * conventions get converted into OpenGL's.
 *
 * Three of those conventions are worth naming here, because each one produces
 * a plausible-looking but wrong picture if it is missed:
 *
 *   - The viewport is applied *inside* the vertex program. What a program
 *     writes to oPos is already in screen coordinates, so the translated
 *     shader has to undo the title's viewport scale and offset to get back to
 *     clip space. See nv2a_vsh.c's epilogue.
 *
 *   - Texture coordinates are normalised for swizzled textures and in texels
 *     for linear ones. The same UV stream means different things depending on
 *     a bit in the format register.
 *
 *   - Primitive types are numbered from 1 (POINTS), so TRIANGLES is 5, not 4.
 *     Off by one here turns strips into lists and produces exactly the kind of
 *     smeared geometry that looks like a transform bug.
 *
 * Rendering goes to an FBO and is read back into the title's own surface in
 * guest memory, so everything downstream -- the framebuffer dumps, the
 * presentation window -- keeps working unchanged and the two rasterisers can
 * be compared frame for frame.
 */
#include "nv2a_gl.h"
#include "nv2a_vsh.h"
#include "nv2a_psh.h"
#include "nv2a_backend.h"
#include "nv2a_shader_cache.h"
#include "nv2a_hash.h"
#include "nv2a_psh_consts.h"

/*
 * Two ways to get a 3.3 core context.
 *
 * EGL with a surfaceless display is what a headless Linux box wants: no
 * window server, no window, just a context to render into an FBO. It does not
 * exist on macOS at all.
 *
 * SDL2 gives a window and a context on both, so it is what the Apple build
 * uses -- and being able to select it on Linux too means this path can be
 * tested here rather than first compiled on the machine it is meant for.
 */
#define GL_GLEXT_PROTOTYPES 1
/* NV2A_GL_USE_NONE: no OpenGL at all -- iOS, which has none (only the
 * long-deprecated OpenGL ES). The device is Metal's, and the few
 * diagnostics built from GL objects are left out. */
#if !defined(NV2A_GL_USE_NONE)
#  define NV2A_GL_HAVE_GL 1
#endif
#if defined(NV2A_GL_USE_NONE)
   /* nothing to include */
#elif defined(NV2A_GL_USE_CGL)
#  include <OpenGL/OpenGL.h>
#  include <OpenGL/gl3.h>
#  include <OpenGL/gl3ext.h>
#elif defined(NV2A_GL_USE_SDL)
#  include <SDL2/SDL.h>
#  include <sys/stat.h>
#  if defined(NV2A_GLES)
     /* OpenGL ES 3.2: Android, and Linux (Mesa), where this path is tested.
      * The core entry points are the library's own exports; extensions are
      * not linked against, so no prototypes for them. */
#    undef GL_GLEXT_PROTOTYPES
#    include <GLES3/gl32.h>
#    include <GLES2/gl2ext.h>
#  elif defined(__APPLE__)
#    include <OpenGL/gl3.h>
#    include <OpenGL/gl3ext.h>
#  elif defined(_WIN32)
     /* opengl32.dll stops at OpenGL 1.1: everything after it is fetched from
      * the driver at run time, which libepoxy does behind the usual names. */
#    include <epoxy/gl.h>
#    include <direct.h>
#  else
#    include <GL/gl.h>
#    include <GL/glext.h>
#  endif
#else
#  include <EGL/egl.h>
#  include <EGL/eglext.h>
#  include <GL/gl.h>
#  include <GL/glext.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stddef.h>

#include "d3d8_swizzle.h"          /* swizzle + DXT helpers, header-only */

#if defined(_WIN32)
/* Windows' mkdir takes no mode. */
#  define mkdir(path, mode) _mkdir(path)
#endif

static double mono_now(void);

/* The guest-thread lock, if this is linked into the runtime.
 *
 * Weak, because the GL backend also builds standalone for its own capture
 * test harness, where there is no guest and no lock. */
extern int  xbox_guest_lock_drop(void)   __attribute__((weak));
extern unsigned long g_vblank_count      __attribute__((weak));
extern void xbox_guest_lock_retake(int)  __attribute__((weak));
extern void xbox_nv2a_vblank_service(void) __attribute__((weak));

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern uint32_t  xbox_ContiguousAllocatedBytes(void);
extern uint32_t  xbox_ContiguousBlockSerial(uint32_t va);

#ifndef XBOX_CONTIG_BASE
#define XBOX_CONTIG_BASE 0x80000000u
#endif
#define XBOX_CONTIG_SIZE 0x04000000u

/* ---- NV097 methods ------------------------------------------------------ */

#define M_SET_SURFACE_CLIP_H        0x0200
#define M_SET_SURFACE_CLIP_V        0x0204
#define M_SET_SURFACE_FORMAT        0x0208
#define M_SET_SURFACE_PITCH         0x020C
#define M_SET_SURFACE_COLOR_OFFSET  0x0210
#define M_SET_SURFACE_ZETA_OFFSET   0x0214

#define M_SET_VIEWPORT_OFFSET       0x0A20   /* +0..0xC, four floats */
#define M_SET_VIEWPORT_SCALE        0x0AF0   /* +0..0xC, four floats */

/*
 * The viewport is not only pipeline state: the hardware mirrors it into two
 * fixed slots of the vertex constant file, and the D3D8 shader epilogue reads
 * it from there rather than being told about it.
 *
 * That is why nothing ever loads constants 58 and 59 through
 * NV097_SET_TRANSFORM_CONSTANT_LOAD -- the title never writes them, it writes
 * the viewport, and the silicon puts a copy where the program will look. A
 * backend that treats the constant file as write-only-by-the-title leaves both
 * at zero, and every program that ends with the standard
 *
 *      MUL R12.xyz, R12, c[58]
 *      MAD oPos.xyz, R12, 1/w, c[59]
 *
 * multiplies its finished position by nothing. Geometry is transformed
 * correctly, submitted correctly, and collapses to a single point.
 */
#define NV2A_XFCTX_VPSCL            58
#define NV2A_XFCTX_T0MAT 68   /* NV_IGRAPH_XF_XFCTX_T0MAT */
#define NV2A_XFCTX_VPOFF            59

#define M_SET_TRANSFORM_PROGRAM     0x0B00   /* +0..0x7C, 32 dwords    */
#define M_SET_TRANSFORM_CONSTANT    0x0B80   /* +0..0x7C, 32 dwords    */
#define M_SET_TRANSFORM_EXEC_MODE   0x1E94
#define NV2A_XF_MODE_FIXED          0u      /* hardware T&L; 2 = program */
#define M_SET_WINDOW_CLIP_TYPE      0x02B4   /* 0 inclusive, 1 exclusive */
#define M_SET_WINDOW_CLIP_HORZ      0x02C0   /* +i*4, 8 rectangles */
#define M_SET_WINDOW_CLIP_VERT      0x02E0   /* +i*4, 8 rectangles */
#define M_SET_CLIP_MIN              0x0394   /* screen-space z range */
#define M_SET_CLIP_MAX              0x0398
#define M_SET_LIGHTING_ENABLE       0x0314
#define M_SET_SKIN_MODE             0x0328   /* 0 off, else N-matrix blend */
#define M_SET_MODEL_VIEW_MATRIX     0x0480   /* 4 matrices of 16 floats */
#define M_SET_TEXTURE_MATRIX_ENABLE 0x0420   /* +i*4, 4 stages */
#define M_SET_TEXGEN_S              0x03C0   /* +0x10 a stage, S T R Q at +0 +4 +8 +C */
#define M_SET_TEXGEN_PLANE_S        0x0840   /* +0x40 a stage, S T R Q at +0 +0x10 ..., 4 floats */
#define M_SET_POINT_PARAMS_ENABLE   0x0318
#define M_SET_POINT_SMOOTH_ENABLE   0x031C
#define M_SET_POINT_SIZE            0x043C
#define M_SET_POINT_PARAMS          0x0A30   /* 8 floats */
#define M_SET_COMPOSITE_MATRIX      0x0680   /* 16 floats, model*view*proj */
#define M_SET_TEXTURE_MATRIX        0x06C0   /* +i*64, 4 matrices of 16 */
#define M_SET_TRANSFORM_PROGRAM_LD  0x1E9C
#define M_SET_TRANSFORM_PROGRAM_ST  0x1EA0
#define M_SET_TRANSFORM_CONSTANT_LD 0x1EA4

#define M_SET_VERTEX_DATA4UB        0x1940   /* +i*4,  constant attribute */
#define M_SET_VERTEX_DATA4F         0x1A00   /* +i*16, constant attribute */
#define M_SET_VERTEX_ARRAY_OFFSET   0x1720   /* +i*4, 16 attributes */
#define M_SET_VERTEX_ARRAY_FORMAT   0x1760   /* +i*4 */
#define M_SET_BEGIN_END             0x17FC
#define M_ARRAY_ELEMENT16           0x1800
#define M_ARRAY_ELEMENT32           0x1808
#define M_DRAW_ARRAYS               0x1810
#define M_INLINE_ARRAY              0x1818

#define M_SET_TEXTURE_OFFSET        0x1B00   /* +stage*0x40 */
#define M_SET_TEXTURE_FORMAT        0x1B04
#define M_SET_TEXTURE_ADDRESS       0x1B08
#define M_SET_TEXTURE_CONTROL0      0x1B0C
#define M_SET_TEXTURE_CONTROL1      0x1B10
#define M_SET_TEXTURE_FILTER        0x1B14
#define M_SET_TEXTURE_IMAGE_RECT    0x1B1C

#define M_SET_COMBINER_ALPHA_ICW    0x0260   /* +i*4, 8 stages */
#define M_SET_COMBINER_SPECFOG_CW0  0x0288
#define M_SET_COMBINER_SPECFOG_CW1  0x028C
#define M_SET_COMBINER_FACTOR0      0x0A60   /* +i*4 */
#define M_SET_COMBINER_FACTOR1      0x0A80   /* +i*4 */
#define M_SET_COMBINER_ALPHA_OCW    0x0AA0   /* +i*4 */
#define M_SET_COMBINER_COLOR_ICW    0x0AC0   /* +i*4 */
#define M_SET_COMBINER_COLOR_OCW    0x1E40   /* +i*4 */
#define M_SET_COMBINER_CONTROL      0x1E60
/* Fog. FOG_COLOR was at 0x02FC, which is not an NV097 method at all -- the
 * register is 0x02A8 -- so the fog colour was never read and sat at zero.
 * The header also says its byte order is A-B-G-R, red in the LOW byte; the
 * old decode took the high byte as red. Both fixed. The other fog registers
 * were never decoded. */
#define M_SET_FOG_MODE              0x029C
#define M_SET_FOG_GEN_MODE          0x02A0
#define M_SET_FOG_ENABLE            0x02A4
#define M_SET_FOG_COLOR             0x02A8
#define M_SET_FOG_PARAMS            0x09C0   /* +0..+8, three floats */
#define M_SET_FOG_PLANE             0x09D0   /* +0..+C, four floats */
#define M_SET_SHADER_CLIP_PLANE_MODE 0x1E6C  /* per stage, per component: >= or < */
#define M_SET_SHADER_STAGE_PROGRAM  0x1E70   /* texture shader mode, 5 bits per stage */
#define M_SET_SHADER_OTHER_STAGE_INPUT 0x1E78
/* Depth behaviour registers the renderer never decoded. CONTROL0 carries the
 * w-buffering switch (Z_PERSPECTIVE_ENABLE) and the depth format; xemu reads
 * both into its shader state. ZMIN_MAX_CONTROL says whether a fragment past
 * the near or far plane is CULLED or CLAMPED to the plane -- xemu maps its
 * ZCLAMP_EN bit straight onto GL_DEPTH_CLAMP. A title that clamps, drawn by a
 * renderer that clips, loses everything beyond its far plane: the near half
 * of the world, and nothing else, which is the picture on screen. */
#define M_SET_CONTROL0              0x0290
#define M_SET_ZMIN_MAX_CONTROL      0x1D78

#define M_SET_COLOR_CLEAR_VALUE     0x1D90
#define M_CLEAR_SURFACE             0x1D94
/* The clear registers are consecutive: zstencil value, colour value, the
 * clear itself, then the rectangle. Reading the depth clear from the wrong
 * address leaves it at zero, and with the usual "less or equal" test that
 * rejects very nearly every pixel a title draws -- a black frame with a
 * healthy draw count and no error anywhere. */
#define M_SET_ZSTENCIL_CLEAR_VALUE  0x1D8C
#define M_SET_CLEAR_RECT_H          0x1D98
#define M_SET_CLEAR_RECT_V          0x1D9C

#define M_SET_DEPTH_TEST_ENABLE     0x030C
/* 0x035C, not 0x036C. The wrong one is NV097_SET_STENCIL_FUNC_MASK, which
 * this title never writes -- so every depth-write toggle it ever sent went
 * unseen, the mask sat at its "unknown" value for the whole run, and depth
 * was written for every draw regardless of what was asked. Found by a
 * census that printed write=-1 for 1,176,606 draws out of 1,176,606, which
 * is not a state a title leaves a register in; it is a register nobody is
 * reading. src/nv2a/nv2a_regs.h had the right number all along. */
#define M_SET_DEPTH_MASK            0x035C
#define M_SET_COLOR_MASK            0x0358   /* per-channel write enables */
#define M_SET_DEPTH_FUNC            0x0354
#define M_SET_ALPHA_TEST_ENABLE     0x0300
#define M_SET_ALPHA_FUNC            0x033C
#define M_SET_ALPHA_REF             0x0340
#define M_SET_BLEND_ENABLE          0x0304
#define M_SET_BLEND_FUNC_SFACTOR    0x0344
#define M_SET_BLEND_FUNC_DFACTOR    0x0348
#define M_SET_CULL_FACE_ENABLE      0x0308
/* Both of these were one method too high, so the backend's "cull face" was
 * reading the front-face convention and its "front face" was reading the
 * register after it. Culling has never had correct inputs here. */
#define M_SET_CULL_FACE             0x039C   /* 0x404 front, 0x405 back */
#define M_SET_FRONT_FACE            0x03A0   /* 0x900 CW, 0x901 CCW */

#define M_FLIP_INCREMENT_WRITE      0x012C
#define M_FLIP_STALL                0x0130

/* Primitive types, numbered from one. */
#define PRIM_POINTS         1
#define PRIM_LINES          2
#define PRIM_LINE_LOOP      3
#define PRIM_LINE_STRIP     4
#define PRIM_TRIANGLES      5
#define PRIM_TRIANGLE_STRIP 6
#define PRIM_TRIANGLE_FAN   7
#define PRIM_QUADS          8
#define PRIM_QUAD_STRIP     9
#define PRIM_POLYGON        10

/* A batch's indices, and its inline vertex data in words.
 *
 * Both were 16384, and what came after was dropped without a word: the
 * hardware has no such limit, so a batch of more than 16384 indices -- 5461
 * triangles -- lost its tail, and an inline batch more than 16384 words --
 * 2048 vertices of eight words -- lost the vertices after that. Room for a
 * million of either now; batch_size_note says how often a batch went past
 * the old limits (and, should one ever reach these, that it was cut). */
#define MAX_INDICES   (1u << 20)
#define MAX_INLINE    (1u << 22)
#define NUM_ATTRS     16
#define NUM_STAGES    4

/* ---- state -------------------------------------------------------------- */

typedef struct {
    uint32_t offset, type, size, stride;
    uint32_t raw;       /* the format word exactly as the title wrote it */
    uint8_t  seen;      /* whether it ever wrote one at all */
} Attr;

typedef struct {
    uint32_t offset, format, addr, control0, control1, filter, image_rect;
    uint32_t width, height, pitch, color, levels;
    int      enabled;
    /* NV097_SET_TEXTURE_SET_BUMP_ENV_MAT/_SCALE/_OFFSET, in this stage's own
     * block: the 2x2 matrix a BUMPENVMAP stage applies to the (du, dv) it
     * reads from its source stage, and the luminance scale and offset. */
    float    bump_mat[4], bump_scale, bump_off;
} TexStage;

static struct {
    /* surface */
    uint32_t clip_x, clip_y, clip_w, clip_h;
    uint32_t surf_pitch, color_offset, zeta_offset, surf_format;
    uint32_t clear_color, clear_zstencil;
    uint32_t clear_x0, clear_x1, clear_y0, clear_y1;

    /* viewport, as the title programmed it */
    float vp_off[4], vp_scale[4];

    /* vertex program */
    uint32_t prog[NV2A_VSH_MAX_INSNS * 4];
    uint32_t prog_load, prog_start, prog_sub;
    uint32_t xf_mode;        /* SET_TRANSFORM_EXECUTION_MODE, low 2 bits */
    uint32_t ff_texmat_enable;   /* bit per stage */
    /* NV097_SET_TEXGEN_S..Q (0x03C0, 0x10 a stage, 4 a component): the
     * OpenGL enum the title wrote, 0 for none; and the planes the two linear
     * modes would dot with (NV097_SET_TEXGEN_PLANE_S..Q, 0x0840, 0x40 a
     * stage, 0x10 a component), kept for the log. */
    uint32_t texgen[4][4];
    float    texgen_plane[16][4];
    /* Points (NV097_SET_POINT_PARAMS_ENABLE, _SMOOTH_ENABLE, _SIZE,
     * _PARAMS), kept for the log: point lists are not drawn. */
    uint32_t point_params_enable, point_smooth_enable, point_size;
    float    point_params[8];
    uint32_t ff_skin_mode;       /* NV097_SET_SKIN_MODE */
    uint32_t draws_skinned;      /* draws made with it non-zero */
    int      ff_lighting;
    int      prog_dirty;
    float    consts[NV2A_VSH_NUM_CONSTS][4];
    uint32_t const_load, const_sub;
    int      consts_dirty;
    int      consts_since_geom;   /* constants written since the last
                                   * vertex went into this batch */
    int      arrays_since_geom;   /* ... and likewise the vertex arrays */
    uint32_t midbatch_splits;
    uint32_t midbatch_array_splits;

    /* geometry */
    Attr     attr[NUM_ATTRS];
    /* The value an attribute takes when the batch supplies no array for it.
     * The NV2A keeps a latched value per attribute and every vertex reads it;
     * a backend that substitutes zero instead turns every untextured surface
     * black, because the diffuse colour a program passes through is exactly
     * the attribute a batch is most likely to omit. */
    float    const_attr[NUM_ATTRS][4];
    /* What the title last set explicitly (SET_VERTEX_DATA*), for
     * RECOMP_GL_STICKYLOG to compare the inherited values against. */
    float    const_set[NUM_ATTRS][4];
    int      const_attr_init;
    uint32_t prim;
    uint32_t idx[MAX_INDICES];
    uint32_t idx_count;
    uint32_t inline_buf[MAX_INLINE];
    uint32_t inline_count;
    uint32_t draw_first, draw_count;

    /* the pixel pipeline, as raw register words */
    Nv2aPshState psh;
    uint32_t fog_color;
    uint32_t fog_mode, fog_gen_mode, fog_enable;
    uint32_t shader_stage_prog, shader_clip_mode, shader_other_input;
    float    fog_params[3];
    float    fog_plane[4];       /* NV097_SET_FOG_PLANE: the fixed-function
                                  * transform's planar fog distance */
    uint32_t control0;           /* NV097_SET_CONTROL0 */
    uint32_t zminmax;            /* NV097_SET_ZMIN_MAX_CONTROL */

    /* textures and fixed state */
    TexStage tex[NUM_STAGES];
    int      depth_test, depth_mask, depth_func;
    uint32_t color_mask;         /* NV097_SET_COLOR_MASK, 0x01010101 = all */
    /* Stencil (0x032C, 0x0360..0x0378). Recorded; see stencil_apply. */
    uint32_t stencil_en, stencil_wmask, stencil_func, stencil_ref,
             stencil_fmask, stencil_op[3];
    /* Polygon offset (0x0338, 0x0384, 0x0388): filled polygons' depth moved
     * by factor x their slope + units of the depth buffer. */
    uint32_t poly_off_fill;
    float    poly_off_factor, poly_off_units;
    int      blend_enable, blend_src, blend_dst;
    int      cull_enable, cull_face, front_face;
    int      alpha_test, alpha_func;
    float    alpha_ref;

    /* stats */
    uint32_t draws, tris, clears, flips, prog_switches, since_present;
    /* Displayed frames, as distinct from finished passes: see M_FLIP_STALL. */
    uint32_t frames;
    uint32_t tris_ff, tris_pm;   /* triangles by transform pipeline */
    uint32_t tris_ff_lit;        /* of the fixed-function ones, drawn with lighting on */
    /* The T&L unit's lighting state, as the title programs it (xemu vsh-ff.c
     * reads the same from its lighting context). Decoded so the fixed-
     * function shader can light the way the hardware does. */
    uint32_t light_enable_mask;      /* NV097_SET_LIGHT_ENABLE_MASK: 2 bits per light, 0 off 1 infinite 2 local 3 spot */
    uint32_t color_material;         /* NV097_SET_COLOR_MATERIAL */
    uint32_t light_control;          /* NV097_SET_LIGHT_CONTROL */
    uint32_t normalization;          /* NV097_SET_NORMALIZATION_ENABLE */
    uint32_t specular_enable;        /* NV097_SET_SPECULAR_ENABLE */
    float    scene_ambient[3];       /* NV097_SET_SCENE_AMBIENT_COLOR */
    float    material_emission[3];   /* NV097_SET_MATERIAL_EMISSION */
    float    material_alpha;         /* NV097_SET_MATERIAL_ALPHA */
    float    light_amb[8][3], light_dif[8][3], light_spc[8][3];
    float    light_range[8], light_half[8][3], light_dir[8][3];
    float    light_spot_falloff[8][3], light_spot_dir[8][4];
    float    light_pos[8][3], light_att[8][3];
    float    eye_position[4];        /* NV097_SET_EYE_POSITION */
    float    specular_params[6];     /* NV097_SET_SPECULAR_PARAMS */
    float    ff_mv[4][4], ff_imv[4][4]; /* model-view matrix 0 and its inverse, as sent */
    int      seen_flip_stall;
    /*
     * The fixed-function matrices, kept apart from the constant file.
     *
     * The hardware mirrors them into the low end of the vertex constant file,
     * and this used to do the same. That is only safe if a title's own vertex
     * programs never read from there -- and this one writes its constants from
     * c[0] upward, so every fixed-function draw was destroying the constants
     * the next programmable draw was about to read. The road's matrix ended up
     * transforming the characters, which is why they and the traffic hung in
     * mid-air while the city itself was correct.
     *
     * Faithful mirroring would need to know which slots the title's programs
     * read, which is not knowable in general. Separate storage is not what the
     * hardware does, but it is what the hardware's *effect* is for any title
     * that respects the D3D8 split -- and it cannot corrupt one that does not.
     */
    /*
     * Everything both shader stages read that is not a texture, kept in the
     * shape the backend interface passes rather than as scattered fields.
     *
     * Maintained as the methods arrive rather than gathered per draw: this
     * title issues eighteen thousand draws a second and the block is three
     * kilobytes, so building it at draw time would be fifty megabytes a second
     * of copying to no purpose.
     */
    Nv2aUniforms u;
    /*
     * NV097_SET_WINDOW_CLIP: up to eight rectangles the raster is confined to.
     *
     * The title sets these thirty-six thousand times a run and this backend
     * ignored them, which means any pass the console clips to a rectangle was
     * being sprayed across the whole frame here. Only the first rectangle is
     * honoured -- that is what a scissor can express -- and only when the
     * title is using the inclusive mode, which is the one that means "draw
     * inside this".
     */
    uint32_t wclip_h[8], wclip_v[8];
    uint32_t wclip_type;
    int      wclip_valid;
    uint32_t wclip_applied;
    int      ff_mats_dirty;
    uint32_t draws_3d;      /* draws made with a centred, full-screen viewport */
    uint32_t frame_tris;    /* triangles in the frame just finished */
    uint32_t skipped_no_prog, skipped_no_pos, shader_fails;
    float    vp_off_reg[4];       /* NV097_SET_VIEWPORT_OFFSET exactly as written:
                                   * the fixed-function shader adds it after its
                                   * divide, the way the hardware does */
    uint32_t draws_inline, draws_array, draws_depth_on, surface_switches;
    uint32_t draws_xf[4];   /* draws by transform execution mode */
    int      surf_dirty;    /* offset or clip changed since the last bind */
} S;

/* do_draw is drawing a batch sent one vertex at a time (see imm_draw). */
static int g_imm_drawing;

/* ── where a frame's CPU time goes ────────────────────────────────────────
 *
 * RECOMP_GL_PROFILE=1 prints a breakdown once a second. Every guess about why
 * a backend is slow is worth less than one measurement, and the candidates
 * here -- per-draw constant uploads, per-draw vertex buffer orphaning, the
 * texture cache's content hash, the conversion of vertices from the console's
 * formats -- all look identical from the frame rate alone.
 *
 * CLOCK_MONOTONIC around GL calls measures what the driver does on this
 * thread, which on this platform is most of the cost: an OpenGL draw call on
 * macOS is a substantial piece of CPU work before it is anything else.
 */
typedef struct {
    double tex, verts, vbo, unif, draw, state;
    double whole_draw;      /* all of do_draw, the above included */
    double present, pace;   /* at the flip: showing the frame, waiting for its time */
    unsigned n_draws, n_const_upload, n_prog_switch, n_tex_upload, n_tex_lookup;
    unsigned long vbo_bytes, unif_vec4;
    unsigned tex_held;                  /* textures the cache holds, now */
    unsigned long long tex_held_bytes;  /* ... and their size, as handed over */
    unsigned n_tex_rewritten;           /* dropped by tcache_verify: rewritten in place */
    unsigned long long tex_verified;    /* source bytes tcache_verify compared */
} GlProfile;
static void vc_report(const char *when);
static GlProfile g_prof;
static int g_prof_on = -1;

static double prof_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
#define PROF_ON() (g_prof_on < 0 \
        ? (g_prof_on = getenv("RECOMP_GL_PROFILE") ? 1 : 0) : g_prof_on)
#define PROF_T0() double _t0 = PROF_ON() ? prof_now() : 0.0
#define PROF_ADD(field) do { if (PROF_ON()) g_prof.field += prof_now() - _t0; } while (0)

/* Time spent walking the pushbuffer, measured by the walker and reported here
 * so that it lands beside the GL numbers. See nv2a_pb_scan. */
static double g_pb_time;
static unsigned long g_pb_words;
static double g_pb_t0;
void nv2a_pb_time_begin(void);
void nv2a_pb_time_end(unsigned words);
void nv2a_pb_time_begin(void) { if (PROF_ON()) g_pb_t0 = prof_now(); }
void nv2a_pb_time_end(unsigned words)
{
    if (!PROF_ON()) return;
    g_pb_time += prof_now() - g_pb_t0;
    g_pb_words += words;
}

static void prof_report(void)
{
    static double last;
    static GlProfile prev;
    static uint32_t prev_tris_p;
    double now;
    if (!PROF_ON()) return;
    now = prof_now();
    if (last == 0.0) { last = now; prev = g_prof; return; }
    if (now - last < 1.0) return;
    {
        double dt = now - last;
        GlProfile d;
        d.tex   = g_prof.tex   - prev.tex;
        d.verts = g_prof.verts - prev.verts;
        d.vbo   = g_prof.vbo   - prev.vbo;
        d.unif  = g_prof.unif  - prev.unif;
        d.draw  = g_prof.draw  - prev.draw;
        d.state = g_prof.state - prev.state;
        d.whole_draw = g_prof.whole_draw - prev.whole_draw;
        d.present = g_prof.present - prev.present;
        d.pace    = g_prof.pace    - prev.pace;
        fprintf(stderr,
            "[GLPROF] %.0f draws/s | tex %4.1f%%  verts %4.1f%%  vbo %4.1f%%  "
            "uniforms %4.1f%%  glDraw %4.1f%%  state %4.1f%%  (rest %4.1f%%)\n",
            (g_prof.n_draws - prev.n_draws) / dt,
            100 * d.tex / dt, 100 * d.verts / dt, 100 * d.vbo / dt,
            100 * d.unif / dt, 100 * d.draw / dt, 100 * d.state / dt,
            100 * (dt - d.tex - d.verts - d.vbo - d.unif - d.draw - d.state) / dt);
        /* Where the rest goes: the draws' own work outside the calls above
         * (finding the program, the uniforms by name, the bookkeeping), and
         * the flip -- showing the frame, and waiting for its moment. */
        fprintf(stderr,
            "[GLPROF]   draws in all %4.1f%% (%.1f us each, %.1f us outside the above) | "
            "present %4.1f%%  pacing %4.1f%%\n",
            100 * d.whole_draw / dt,
            (g_prof.n_draws - prev.n_draws)
                ? 1e6 * d.whole_draw / (g_prof.n_draws - prev.n_draws) : 0.0,
            (g_prof.n_draws - prev.n_draws)
                ? 1e6 * (d.whole_draw - d.tex - d.verts - d.vbo - d.unif - d.draw - d.state)
                  / (g_prof.n_draws - prev.n_draws) : 0.0,
            100 * d.present / dt, 100 * d.pace / dt);
        { static double last_vc;
          if (now - last_vc >= 10.0) { last_vc = now; vc_report(" (so far)"); } }
        /* Per second, not per frame -- which is what it used to say, and the
         * numbers were read as per-frame for long enough to be worth the
         * correction: six thousand program switches in a frame is an
         * emergency, and six thousand a second is forty-five per frame.
         *
         * The frame rate goes on the same line because it is the number this
         * is all for, and because without it "13,756 draws a second" cannot
         * be turned into "two hundred and thirty draws a frame", which is the
         * form that says whether the title or the port is the expensive one.
         * Frames are flips; passes are every render target the title finishes,
         * and this title finishes three of those per frame. */
        { static uint32_t prev_frames, prev_flips;
          double fps = (S.frames - prev_frames) / dt;
          fprintf(stderr,
            "[GLPROF]   %.1f fps, %.0f passes/s, %.0f draws and %.0f "
            "triangles per frame\n",
            fps, (S.flips - prev_flips) / dt,
            fps > 0 ? (g_prof.n_draws - prev.n_draws) / dt / fps : 0.0,
            fps > 0 ? (double)(S.tris - prev_tris_p) / dt / fps : 0.0);
          prev_frames = S.frames; prev_flips = S.flips; prev_tris_p = S.tris; }
        fprintf(stderr,
            "[GLPROF]   per second: %lu KB of vertices, %lu constant vec4s, "
            "%u constant uploads, %u program switches, %u texture uploads, "
            "%u texture lookups; %u textures held (%llu MB); %llu KB compared, "
            "%u rewritten in place\n",
            (g_prof.vbo_bytes - prev.vbo_bytes) / 1024,
            g_prof.unif_vec4 - prev.unif_vec4,
            g_prof.n_const_upload - prev.n_const_upload,
            g_prof.n_prog_switch - prev.n_prog_switch,
            g_prof.n_tex_upload  - prev.n_tex_upload,
            g_prof.n_tex_lookup  - prev.n_tex_lookup,
            g_prof.tex_held, g_prof.tex_held_bytes >> 20,
            (g_prof.tex_verified - prev.tex_verified) >> 10,
            g_prof.n_tex_rewritten - prev.n_tex_rewritten);
        {
            static double prev_pb; static unsigned long prev_words;
            fprintf(stderr,
                "[GLPROF]   pushbuffer walk (decode + backend state machine): "
                "%4.1f%% of wall time, %lu K words/s -- everything not "
                "accounted for is the recompiled guest itself\n",
                100 * (g_pb_time - prev_pb) / dt,
                (g_pb_words - prev_words) / 1000);
            prev_pb = g_pb_time; prev_words = g_pb_words;
        }
        fflush(stderr);
        last = now; prev = g_prof;
    }
}


/* ---- GL objects --------------------------------------------------------- */

static int      g_on = -1;
static int      g_ready;
static int      g_failed;

/* The device everything below draws through: OpenGL (nv2a_gfx_gl.c) or Metal
 * (nv2a_metal.m). Chosen once, in gfx_init(). */
static const Nv2aGfx *G;
static int g_is_gl;        /* G is OpenGL: the GL-only diagnostics may run */
#ifdef NV2A_GL_HAVE_GL
/* GL errors reported so far (eight at most), and whether they are asked for
 * after every draw (RECOMP_GL_ERRORS_PER_DRAW=1) or once a frame. */
static int g_gl_err_reports;
static int gl_err_per_draw(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("RECOMP_GL_ERRORS_PER_DRAW");
                 v = (e && *e && *e != '0'); }
    return v;
}
#endif
unsigned nv2a_gfx_gl_fbo(uint32_t rt);   /* nv2a_gfx_gl.c */
#if defined(NV2A_GL_USE_CGL)
static CGLContextObj g_cgl_ctx;
#elif defined(NV2A_GL_USE_SDL)
static SDL_Window   *g_window;
static SDL_GLContext g_sdl_ctx;
int  nv2a_widescreen(void);   /* below; the window opens in its shape */
#elif defined(NV2A_GL_USE_NONE)
#else
static EGLDisplay g_dpy;
static EGLContext g_ctx;
static EGLSurface g_egl_surf;
#endif

/* One framebuffer per surface the title renders into.
 *
 * A single shared framebuffer is enough right up to the moment a title
 * double-buffers, and then it is silently wrong: the game draws a frame into
 * one surface, switches to the other, clears it, and the shared framebuffer
 * loses the frame that was about to be shown. Everything still runs, the draw
 * counters still climb, and the picture is black. Keying the framebuffer on
 * the surface's address costs a few megabytes and makes the two independent,
 * which is also what render-to-texture will need. */
/*
 * Sixteen, evicted least-recently-used. It was eight, evicting slot 0 --
 * and slot 0 is the first surface a title draws into, which for JSRF is the
 * off-screen frame everything is built in. The boost dash's blur uses three
 * more targets, the eager binding below added four phantom ones, the ninth
 * evicted slot 0 and the next bind of the real frame evicted it straight
 * back: every frame of the effect started from an empty picture.
 */
#define SURF_CACHE 16
static struct {
    uint32_t offset, w, h;
    uint32_t rt;          /* the device's render target */
    int      used;
    uint32_t last_use;    /* bind order, for eviction */
    uint32_t last_draw;   /* S.draws when last drawn or cleared into */
    uint32_t fmt;         /* SET_SURFACE_FORMAT when it was last bound */
    uint32_t serial;      /* allocation holding its memory when last bound */
} g_surf[SURF_CACHE];
static uint32_t g_surf_clock;
static int      g_dump_burst_left, g_dump_burst_every = 1;
static int      g_present_slot = -1;  /* set at FLIP_STALL, see present() */
static uint32_t g_last_flip_draws;    /* S.draws at the last FLIP_STALL */
static int      g_cur_surf = -1;
/* nv2a_gl_trace_frames(): per-pass log for the next few frames. */
static uint32_t g_pass_trace_until;
static uint32_t g_cur_rt;            /* the bound surface's render target */
/* The surface the title is drawing into could not be made (a target the
 * device refused): its draws and clears go nowhere until a surface binds,
 * rather than landing on whichever surface was bound before. */
static int      g_surf_lost;
static uint32_t g_fbo_w, g_fbo_h;
#define SLOTCENSUS_MAX 4096
static uint32_t g_slot_draws[SLOTCENSUS_MAX];
/* The finished frame's counts.
 *
 * The dump and the per-frame reset both happen inside present(), and the
 * reset ran first -- so the census written beside each screenshot counted
 * one draw, not the thousands the picture was made of. Keep the completed
 * frame's totals and write those: they are the frame the screenshot shows. */
static uint32_t g_slot_prev[SLOTCENSUS_MAX];
/* Which frame the per-draw hunt should examine. Set either from the env at
 * the hunt itself, or by the end-of-frame brightness watcher below. */
int g_darkhunt_at = -1;
/* The raster's real size. Equal to the logical size unless RECOMP_GL_SCALE
 * asks for more -- see gl_scale(). Everything the title can observe uses the
 * logical size; only the pixels use this one. */
static uint32_t g_fbo_pw, g_fbo_ph;
static uint8_t *g_readback;
static uint32_t g_readback_bytes;

/* RECOMP_GL_DUMP_SLOT=<n>: the program a particular cache slot holds, printed
 * once, together with the value of every constant it reads.
 *
 * Dumping every program buries the one that matters in two hundred others,
 * and the draw log already names the slot: the title's whole scene comes out
 * of one program, and what is wanted is that program and the numbers it is
 * working from, side by side. */
static int dump_slot(void)
{
    static long want = -2;
    if (want == -2) { const char *v = getenv("RECOMP_GL_DUMP_SLOT");
                      want = v ? atol(v) : -1; }
    return (int)want;
}

/* RECOMP_GL_DUMP_BIG=<verts>: the same dump, chosen by the size of the batch
 * rather than by a cache slot number. Which slot a program lands in depends
 * on the order programs happen to be compiled, so a slot number read off one
 * run is not reliably the same program in the next; "the first batch with
 * five thousand vertices in it" is the scene either way. */
static long dump_big(void)
{
    static long want = -2;
    if (want == -2) { const char *v = getenv("RECOMP_GL_DUMP_BIG");
                      want = v ? atol(v) : -1; }
    return want;
}

/*
 * Room for a level's shaders, and a lookup that does not walk the table.
 *
 * 128 slots was a menu's worth. This title's title-screen level alone uses
 * many hundreds of distinct (vertex program, combiner) pairs, so the table was
 * full within seconds and every miss evicted a slot chosen by hash -- often
 * one needed again a few draws later. The result is a backend that compiles
 * GLSL forever: a sampling profile taken in the level found 962 of 4,478
 * samples inside the driver's shader compiler, in the middle of the frame.
 *
 * With room for the level the compiles happen once. The lookup is open
 * addressing from the hash rather than a scan of every slot, because a scan of
 * 2048 entries per draw would cost more than the thrashing did.
 */
#define PROG_CACHE 2048
static struct {
    uint32_t hash;      /* vertex program and combiner state together */
    int      used;
    uint32_t last_draw; /* for eviction, if a title ever does fill this */
    uint32_t prog;      /* the device's program; it knows where each uniform is */
    uint16_t inputs;
    uint8_t  uses_a0;   /* indexes the constant file through the address register */
    uint8_t  a0_input;  /* the v register the index comes from, 0xFF if unknown */
    uint32_t csync;     /* constants stamped before this it already has; 0 none */
    char    *dis;       /* disassembly, kept only when a dump is armed */
} g_pcache[PROG_CACHE];

/*
 * Hitches.
 *
 * A player who reports "micro-stuttering" is describing frames that took two
 * or three times the period, every now and then -- which the averages in the
 * pacing report (and RECOMP_GL_PROFILE's per-second shares) cannot show. So
 * the renderer notes, per frame, the work that comes in lumps: programs built
 * (a GLSL compile and link, or a Metal library, is milliseconds each, and a
 * new effect or object brings several at once), and textures decoded and
 * uploaded. A frame half again the period or longer is written to the log
 * with those, and with what is left over -- the game itself, its loading and
 * its sound, which the renderer does not see. The first two hundred are
 * written, then one in fifty.
 */
static struct {
    unsigned compiles, tex_new, tex_dxt;        /* tex_dxt: DXT as they are (tex_dxt_direct) */
    double   compile_s, compile_worst, tex_s;
    double   idle_s;                            /* building ahead for the shader cache */
    double   pso_ms0;                           /* the backend's pipeline time at the frame's start */
    double   gpu_busy0, gpu_wait0;              /* and its GPU times (gpu_ms) */
    uint32_t draws0;
} g_hf;                                         /* this frame */
static struct {
    unsigned long long hitches, logged, compiles;
    double   compile_s, compile_worst;
} g_ht;                                         /* the whole run */
/* The clock (mono_now) as of the last frame shown: what the draw's own
 * notes, written every few seconds, go by. They each read the clock at every
 * draw, five reads a draw for lines that come once in five seconds. */
static double g_frame_clock;
/* Reversed depth (zrev_start, below gfx_init's declaration). */
static int g_zrev;
static uint32_t zrev_func(uint32_t f);
static void zrev_row(float out[4]);

/* The most the resolution menus offer: 6x (3840x2880, for a 4K screen) on a
 * computer; 4x on Android, as on the iPhone, where 4x already fills the
 * screen and a 6x frame would be most of the memory a phone has to give.
 * RECOMP_GL_SCALE itself still takes up to 8. */
#if defined(__ANDROID__)
#define NV2A_SCALE_MAX 4
#else
#define NV2A_SCALE_MAX 6
#endif

/*
 * Internal resolution.
 *
 * The title asks for a 640x480 surface and everything it can observe stays
 * 640x480 -- the surface it thinks it is drawing into, the clip rectangles it
 * sets, the coordinates its screen-space passes use. What does not have to be
 * 640x480 is the raster: the transform ends in normalised coordinates, so the
 * number of samples taken along the way is free. RECOMP_GL_SCALE=2 renders
 * four times the pixels into the same logical surface and hands the window a
 * frame at that size.
 *
 * This is where an idle GPU goes. At 640x480 an M2 Pro is bounded entirely by
 * the CPU side of the emulation and the GPU has nothing to do.
 */
static uint32_t gl_scale(void)
{
    static uint32_t sc;
    if (!sc) {
        const char *v = getenv("RECOMP_GL_SCALE");
        long n = v ? strtol(v, NULL, 0) : 1;
        sc = (uint32_t)(n < 1 ? 1 : n > 8 ? 8 : n);
        if (sc > 1)
            fprintf(stderr, "  [GL] internal resolution x%u\n", sc);
    }
    return sc;
}

/*
 * Anti-aliasing, two kinds, either or both:
 *
 *   SMAA (nv2a_smaa.h): the frame is shown through three passes over it that
 *   find its edges and blend across them (the backend's target_smaa), into a
 *   copy. Switched on and off at any time, taken at the next flip.
 *
 *   Multisampling: every target drawn with 2, 4 or 8 samples a pixel (the
 *   backend's samples), which smooths what is drawn rather than the picture
 *   afterwards. OpenGL takes a new count at the next flip, carrying every
 *   target across; OpenGL ES and Metal only at start, so there a change is
 *   kept for the next one.
 *
 * Nothing the title can observe changes with either -- its surfaces, their
 * sizes and the raster stay what they were. RECOMP_GL_SMAA=0|1 and
 * RECOMP_GL_MSAA=n win over the settings (the window's smaa= and msaa=, the
 * Mac's and the iPhone's preferences); both are off by default.
 */
static volatile int g_smaa_want;       /* asked for */
static int          g_smaa_now;        /* and in use */
static volatile int g_msaa_want = 1;   /* samples a pixel asked for, 1 none */
static int          g_msaa_now = 1;    /* and in use */
static int          g_msaa_max;        /* the most the device offers; 0 not up yet */
static uint32_t     g_shown_rt;        /* what was put on screen at the last flip */

/* For the windows' menus and settings (nv2a_window_mac.m, the iPhone app's):
 * what is in use and what can be asked for. Storing a choice is theirs to
 * do; this only takes it. */
void nv2a_set_smaa(int on);
void nv2a_set_smaa(int on) { g_smaa_want = on ? 1 : 0; }
int  nv2a_smaa(void);
int  nv2a_smaa(void) { return g_smaa_want; }
int  nv2a_msaa(void);
int  nv2a_msaa(void) { return g_msaa_now; }
int  nv2a_msaa_max(void);
int  nv2a_msaa_max(void) { return g_msaa_max; }
static int g_msaa_live;                /* the device takes a new count while running */
int  nv2a_msaa_live(void);
int  nv2a_msaa_live(void) { return g_msaa_live; }
void nv2a_set_msaa(int n);
void nv2a_set_msaa(int n) { g_msaa_want = n > 1 ? n : 1; }

static void smaa_take(void)
{
    if (g_smaa_want == g_smaa_now || !G->target_smaa) return;
    g_smaa_now = g_smaa_want;
    fprintf(stderr, "  [GL] SMAA: %s from present %u\n", g_smaa_now ? "on" : "off", S.flips);
}

static void msaa_take(void)
{
    int want = g_msaa_want, was = g_msaa_now;
    if (want == was || !G->samples || !g_msaa_live) return;
    g_msaa_now = G->samples(want);
    fprintf(stderr, "  [GL] MSAA: %dx from present %u (was %dx%s)\n",
            g_msaa_now, S.flips, was, g_msaa_now != want ? "; fewer than asked" : "");
    g_msaa_want = g_msaa_now;
}

static uint32_t g_cur_prog;
static int      g_cur_slot = -1;

/* Bumped by every write to the vertex program pool, so that a draw whose
 * program is the one the last draw looked up skips decoding and hashing it
 * (program_for_current). */
static uint32_t g_vsh_gen = 1;

/*
 * When each constant last changed, by a clock that ticks once per upload
 * that follows a change (see the upload in do_draw). A program is sent the
 * constants stamped at or after the tick it was last brought up to date at,
 * rather than all 192 whenever any one changed or the program did: this
 * title sends its matrices around nearly every draw and switches programs
 * twenty times a frame, and re-sending the whole file each time was three
 * hundred 3 KB uploads a frame -- most of it values the program already had.
 */
static uint32_t g_cstamp[NV2A_VSH_NUM_CONSTS];
static uint32_t g_cclock = 1;    /* the stamp a change gets now */
static uint32_t g_clast;         /* the newest stamp given out */
static void const_changed(uint32_t i)
{
    g_cstamp[i] = g_cclock;
    g_clast = g_cclock;
}

/* Uniforms, by name (nv2a_backend.h), for the program in use. `uses` is
 * whether the program at a cache slot reads one at all: work for one it does
 * not is skipped, as it always was when a GL location came back -1. */
static int  uses(int slot, int id) { return G->prog_uses(g_pcache[slot].prog, id); }
static void uni1f(int id, float a) { G->uniform_f(id, &a, 1); }
static void uni2f(int id, float a, float b)
{ float v[2]; v[0] = a; v[1] = b; G->uniform_f(id, v, 1); }
static void uni4f(int id, float a, float b, float c, float d)
{ float v[4]; v[0] = a; v[1] = b; v[2] = c; v[3] = d; G->uniform_f(id, v, 1); }

/* Find a compiled program by hash, or -1. Open addressing: a run of used slots
 * with different hashes is a collision chain, and an empty slot ends it. */
static int pcache_find(uint32_t hash)
{
    uint32_t i;
    for (i = 0; i < PROG_CACHE; i++) {
        uint32_t k = (hash + i) & (PROG_CACHE - 1);
        if (!g_pcache[k].used) return -1;
        if (g_pcache[k].hash == hash) {
            g_pcache[k].last_draw = S.draws;
            return (int)k;
        }
    }
    return -1;
}

/* A slot to compile into. The first free one on the probe chain, or -- only if
 * the table is genuinely full -- the least recently drawn. */
static int pcache_alloc(uint32_t hash)
{
    uint32_t i, oldest = 0xFFFFFFFFu;
    int victim = (int)(hash & (PROG_CACHE - 1));
    for (i = 0; i < PROG_CACHE; i++) {
        uint32_t k = (hash + i) & (PROG_CACHE - 1);
        if (!g_pcache[k].used) return (int)k;
        if (g_pcache[k].last_draw < oldest) {
            oldest = g_pcache[k].last_draw; victim = (int)k;
        }
    }
    return victim;
}

/* Last state actually pushed to GL.
 *
 * A title issues thousands of small batches a frame and changes almost
 * nothing between them, so the honest cost of a draw here is not the
 * triangles, it is the hundred redundant GL calls around them: sixteen
 * attribute pointers, three kilobytes of constants and half a dozen state
 * toggles, per draw, all identical to the last one. Remembering what was set
 * turns that into a handful of calls. */
static struct {
    int      prog_slot;
    uint32_t tex0;
    int      tex_enable;
    float    tex_sx, tex_sy;
    int      alpha_func;
    float    alpha_ref;
    float    vp_scale[4], vp_off[4];
    float    wide_shift;
    uint32_t fbo_w, fbo_h;      /* the surface size vpSurface and vpRect hold */
    uint32_t psh_c[NV2A_PSH_NCONSTS];   /* the combiner constants, as words */
    int      psh_ok;                    /* ... and whether the program has them */
} g_last;
static uint32_t g_frame_chars;       /* big skinned batches this frame */
static uint32_t g_frame_chars_prev;  /* ... and in the frame just finished */

/*
 * Room for a level, not just a menu.
 *
 * A title screen uses a few dozen textures and any size works. A level streams
 * hundreds, and once the table is full every miss evicts by hash -- which can
 * be an entry another stage of the very same draw is still bound to, since the
 * GL object is deleted on the spot. The symptom is a surface going black or
 * picking up someone else's texture, intermittently, in a way that moves as
 * the camera does.
 */
#define TEX_CACHE 4096
static struct {
    uint32_t offset, format, w, h, pitch, hash;
    uint32_t levels;        /* of the title's mipmap chain uploaded (tex_title_levels) */
    uint32_t content;       /* every byte of it, hashed (see tcache_verify) */
    uint32_t drawn;         /* the frame it was last looked up in */
    uint32_t last_draw;     /* when this entry was last looked up */
    uint32_t tex;           /* the device's texture */
    uint32_t va, serial;    /* read from: where, and the allocation (contig_serial) */
    uint32_t sampling;      /* how it is sampled (sampler_key): part of what it is */
    int      bias;          /* uploaded as a bump source (g_upload_bias_bg) */
    int      used;
    size_t   held;          /* bytes handed to the device: RGBA8, or DXT as stored */
} g_tcache[TEX_CACHE];

/* ---- helpers ------------------------------------------------------------ */

int nv2a_gl_enabled(void)
{
    if (g_on < 0) g_on = getenv("RECOMP_GL") ? 1 : 0;
    return g_on;
}

static int verbose(void)
{
    static int v = -1;
    if (v < 0) v = getenv("RECOMP_GL_VERBOSE") ? 1 : 0;
    return v;
}

/* RECOMP_GL_VP_TRACE: log viewport register writes that change the value in
 * the slot, with the method address each arrived on. Printing every write
 * drowns in the thousands of identical ones a frame; printing only changes
 * gives the whole run's history in a few dozen lines, which is what
 * distinguishes "the title never set this" from "we mis-tracked the run". */
static int vp_trace(void)
{
    static int v = -1;
    if (v < 0) v = getenv("RECOMP_GL_VP_TRACE") ? 1 : 0;
    return v;
}

/* A DMA offset from the title, turned into something addressable.
 *
 * Same rule the software rasteriser uses: an offset inside the contiguous
 * heap this runtime handed out names physical memory, which lives at
 * 0x80000000; anything else is already a guest virtual address. Getting this
 * wrong does not draw badly, it reads the wrong memory entirely. */
/*
 * A GPU address is a physical offset. Which of this runtime's two memories
 * holds that physical page is the whole question.
 *
 * Contiguous memory is not an alias of RAM here -- it is a separate mapping
 * of its own at XBOX_CONTIG_BASE, because the XBE image occupies low RAM and
 * the two would otherwise collide. That is a workable arrangement only for
 * as long as each physical offset is read back through the same one of the
 * two routes the title wrote it through. Read it through the other and the
 * bytes are simply unrelated.
 *
 * The second line below sent EVERY offset under sixty-four megabytes into
 * the contiguous window, which is nearly all of them, and it made the first
 * line -- the one that asks whether the offset is actually inside a
 * contiguous allocation -- dead code. So a vertex buffer the title allocated
 * as ordinary memory and filled in RAM was being read out of the window, and
 * what came back was whatever else had been put there, or nothing.
 *
 * SKINDBG found it by reading one failing vertex both ways: the two routes
 * disagreed in every case it caught, and the bone index that came out of the
 * window was 943 where the ceiling is twenty. A model whose vertices all
 * pick the same wrong bone matrix does not fall apart -- it moves somewhere
 * plausible, a few metres off. Static world geometry, loaded once into
 * contiguous memory, is unaffected, which is exactly the split the
 * player described: the buildings and the roads are right and everything else
 * floats.
 *
 * So the window is used only for offsets the contiguous allocator has
 * actually handed out. RECOMP_GL_CONTIG_ALL=1 restores the old behaviour.
 */
/* Where the runtime's general heap begins (xbox_memory_layout.h: stacks end
 * at 0x00F80000 and the heap runs from there to the top of the map). A DMA
 * offset at or above this is a heap VA the title handed straight to the GPU
 * -- MmGetPhysicalAddress is the identity here -- and it can only be heap.
 * The contiguous arena's high-water mark is a byte count from physical 0, so
 * once the arena has grown past 15.5 MB every heap VA below the mark is
 * ambiguous with a contiguous offset, and the resolver, checking the arena
 * first, sends it to the window: fresh zeroed pages where the title's
 * vertices are not. RECOMP_GL_HEAP_FIRST=1 sends heap VAs to the heap. */
#define GL_HEAP_BASE 0x00F80000u

int g_world_dump_pending;   /* set by present() when a frame dump is taken */

static uint32_t resolve(uint32_t off)
{
    extern uint32_t nv2a_dma_resolve(uint32_t) __attribute__((weak));
    static int all = -1, heap_first = -1;
    if (all < 0) { const char *v = getenv("RECOMP_GL_CONTIG_ALL");
                   all = v ? atoi(v) : 0; }
    if (heap_first < 0) { const char *v = getenv("RECOMP_GL_HEAP_FIRST");
                          heap_first = v ? atoi(v) : 0; }
    if (all) {   /* the old behaviour, for comparing against */
        if (off < XBOX_CONTIG_SIZE) return XBOX_CONTIG_BASE + off;
        return off;
    }
    if (heap_first && off >= GL_HEAP_BASE && off < XBOX_CONTIG_SIZE)
        return off;
    if (nv2a_dma_resolve) return nv2a_dma_resolve(off);
    if (off < xbox_ContiguousAllocatedBytes()) return XBOX_CONTIG_BASE + off;
    return off;
}

static const uint8_t *guest(uint32_t va)
{
    return (const uint8_t *)xbox_GetMemoryOffset() + va;
}

static uint32_t fnv(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 2166136261u;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

/* ---- context ------------------------------------------------------------ */

#if defined(NV2A_GL_USE_NONE)

/* No OpenGL in this build: Metal or nothing. */
static int gl_init(uint32_t w, uint32_t h)
{
    (void)w; (void)h;
    fprintf(stderr, "  [GL] this build has no OpenGL renderer\n");
    return 0;
}

#else

/* Everything that is the same whichever way the context was obtained. */
static int gl_init_common(uint32_t w, uint32_t h)
{
    (void)w; (void)h;
    G = nv2a_gfx_gl();
    g_is_gl = 1;
    return G->init();
}

#endif

#if defined(NV2A_GL_USE_NONE)
#elif defined(NV2A_GL_USE_CGL)

/*
 * A context with no window and no third-party dependency.
 *
 * macOS has no EGL, and the obvious substitute -- SDL or GLFW -- has to come
 * from a package manager, which on a Mac carrying Intel Homebrew means an
 * x86_64 library that will not link into an arm64 binary. CGL is in the
 * system OpenGL framework, needs nothing installed, and gives exactly what
 * the backend wants: a Core Profile context to render into an FBO.
 *
 * No window, so nothing appears on screen -- frames come out through the same
 * BMP dump the headless Linux build uses. That is the right trade while the
 * renderer is being debugged: what the GPU buys here is speed, and a window
 * can be added once there is something worth watching in real time.
 */
/* The AppKit window, in nv2a_window_mac.m. Plain C types across the boundary
 * -- see the note at the top of that file for why the two cannot share a
 * translation unit. */
int   nv_window_is_open(void);
void  nv_window_submit_frame(const void *pixels, int width, int height);
void  nv_window_present(void);
void  nv_window_pump(void);
int   nv_window_should_close(void);
void  nv_window_drawable_size(int *w, int *h);

static int g_windowed;

static int gl_init(uint32_t w, uint32_t h)
{
    CGLPixelFormatAttribute attribs[] = {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
        kCGLPFAColorSize,   (CGLPixelFormatAttribute)24,
        kCGLPFAAlphaSize,   (CGLPixelFormatAttribute)8,
        kCGLPFADepthSize,   (CGLPixelFormatAttribute)24,
        kCGLPFAAccelerated,
        (CGLPixelFormatAttribute)0
    };
    CGLPixelFormatObj pix = NULL;
    GLint npix = 0;
    CGLError err;

    /* RECOMP_WINDOW: also show the frame.
     *
     * The window has its own context and draws on the main thread; this one
     * stays exactly as it is. They are deliberately not shared -- the window
     * is handed finished pixels rather than GL objects, which is what makes
     * the display path independent of AppKit's layer behaviour. */
    err = CGLChoosePixelFormat(attribs, &pix, &npix);
    if (err != kCGLNoError || !pix) {
        fprintf(stderr, "  [GL] CGLChoosePixelFormat failed (%d)\n", (int)err);
        return 0;
    }
    err = CGLCreateContext(pix, NULL, &g_cgl_ctx);
    CGLDestroyPixelFormat(pix);
    if (err != kCGLNoError || !g_cgl_ctx) {
        fprintf(stderr, "  [GL] CGLCreateContext failed (%d)\n", (int)err);
        return 0;
    }
    CGLSetCurrentContext(g_cgl_ctx);

    fprintf(stderr, "  [GL] %s | %s | GLSL %s\n",
            (const char *)glGetString(GL_VERSION),
            (const char *)glGetString(GL_RENDERER),
            (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));

    return gl_init_common(w, h);
}

#elif defined(NV2A_GL_USE_SDL)

#if !defined(NV2A_GLES)
static void pres_start(void);        /* the presenter, below */
#endif
#if defined(_WIN32)
static void sdl_menu_init(void);     /* the menu bar, below */
static int  sdl_cfg_get(const char *key, int def);   /* the settings file, below */
#endif
static int   sdl_pref_picture(void);     /* the picture (4:3, 16:9, 21:9), below */
static int   sdl_picture_now(void);
static float sdl_screen_aspect(void);
float nv2a_wide_aspect(void);
void  nv2a_set_picture(int pic, float screen, int apply);

static int gl_init(uint32_t w, uint32_t h)
{
    /* A real window, sized to the guest surface. The frame is still built in
     * an FBO exactly as the headless path builds it -- what the window adds
     * is somewhere to show it and, on a machine with a GPU, a driver worth
     * rendering with. */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "  [GL] SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }
#if defined(NV2A_GLES)
    /* OpenGL ES 3.0 is all the renderer uses; a driver gives the newest 3.x
     * it has, and the shaders follow what that is (nv2a_gfx_gl.c). */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    /* macOS gives 3.3 core only when asked for forward-compatible; without
     * this the request quietly returns a 2.1 context and every shader in the
     * game fails to compile. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,
                        SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    /* The window is not the game's resolution: the game draws 640x480, the
     * renderer works at a multiple of that (RECOMP_GL_SCALE), and the window
     * is whatever size the player makes it -- each frame is fitted into it
     * (sdl_present). It opens at 1280x960, or at the picture's shape at that
     * height in widescreen (16:9, or 21:9), shrunk to fit the screen. */
    {
        int ww = 1280, wh = 960;
        Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
        SDL_Rect usable;
        const char *e;
        (void)w; (void)h;
        /* The stored picture's shape, unless the environment says (tests). */
        if (!getenv("RECOMP_WIDESCREEN") && !getenv("RECOMP_WIDE_ASPECT"))
            nv2a_set_picture(sdl_pref_picture(), sdl_screen_aspect(), 0);
        if (nv2a_widescreen()) ww = (int)((float)wh * nv2a_wide_aspect() + 0.5f);
        if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.h > 0 && usable.w > 0) {
            int maxh = usable.h * 9 / 10, maxw = usable.w * 9 / 10;
            if (wh > maxh) { ww = ww * maxh / wh; wh = maxh; }
            if (ww > maxw) { wh = wh * maxw / ww; ww = maxw; }
        }
        if ((e = getenv("RECOMP_WINDOW_W")) && atoi(e) >= 320) ww = atoi(e);
        if ((e = getenv("RECOMP_WINDOW_H")) && atoi(e) >= 240) wh = atoi(e);
        /* Full screen: RECOMP_FULLSCREEN when it is set -- the launcher's
         * --fullscreen (1) and --windowed (0), and a restart at a new
         * resolution, which says where the game was. Otherwise, on Windows,
         * the way the game was left: F11, Alt+Enter and Video > Full screen
         * keep fullscreen= in the settings (sdl_fullscreen_by_player). */
        if ((e = getenv("RECOMP_FULLSCREEN")) && *e) {
            if (atoi(e)) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        }
#if defined(_WIN32)
        else if (sdl_cfg_get("fullscreen", 0)) {
            flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
            fprintf(stderr, "[WIN] full screen, as the game was left (fullscreen=1 in the "
                    "settings; --windowed starts in a window)\n");
        }
#endif
        g_window = SDL_CreateWindow("JSRF Unleashed",
                                    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    ww, wh, flags);
    }
    if (!g_window) {
        fprintf(stderr, "  [GL] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 0;
    }
    SDL_SetWindowMinimumSize(g_window, 320, 240);
    g_sdl_ctx = SDL_GL_CreateContext(g_window);
    if (!g_sdl_ctx) {
        fprintf(stderr, "  [GL] no 3.3 core context: %s\n", SDL_GetError());
        return 0;
    }
    SDL_GL_MakeCurrent(g_window, g_sdl_ctx);
    /* No waiting for the display's refresh by default: the frame is paced
     * by the emulated vblank (RECOMP_FPS), and the swap happens on the
     * thread that runs the GPU. RECOMP_VSYNC=1 asks for it anyway. */
    { const char *v = getenv("RECOMP_VSYNC");
      SDL_GL_SetSwapInterval(v && atoi(v) ? 1 : 0); }
#if !defined(NV2A_GLES)
    /* Or, on Windows, a presenter thread swaps in step with the display. */
    pres_start();
#endif
    if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP)
        SDL_ShowCursor(SDL_DISABLE);
#if defined(_WIN32)
    sdl_menu_init();
#endif
    fprintf(stderr, "  [WIN] SDL window (%s video), %s (%.3f:1; the screen %.3f:1)\n", SDL_GetCurrentVideoDriver(),
            nv2a_widescreen() ? (sdl_picture_now() == 2 ? "21:9" : "16:9") : "4:3",
            nv2a_widescreen() ? (double)nv2a_wide_aspect() : 4.0 / 3.0, (double)sdl_screen_aspect());

    fprintf(stderr, "  [GL] %s | %s | GLSL %s\n",
            (const char *)glGetString(GL_VERSION),
            (const char *)glGetString(GL_RENDERER),
            (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));

    return gl_init_common(w, h);
}

/* ---- the SDL window: settings, keyboard, presenting, events --------------
 *
 * What the AppKit window (nv2a_window_mac.m) does on the Mac, for the SDL
 * build (Linux): the frame fitted into a resizable window, the keyboard as a
 * pad, full screen, and Widescreen with its choice remembered. All of it on
 * the thread that created the window, which is the one that presents. */

int  nv2a_widescreen(void);
void nv2a_set_widescreen(int on);
int  nv2a_wide_scene(void);
float nv2a_wide_aspect(void);
void nv2a_set_picture(int pic, float screen, int apply);

/* $XDG_CONFIG_HOME/jsrf-unleashed/settings (or JSRF_SETTINGS_FILE): lines of
 * key=value, shared with the launcher script, which keeps game_dir and
 * scale there. Only the keys this file owns are rewritten. */
static void sdl_cfg_path(char *buf, size_t n)
{
    const char *f = getenv("JSRF_SETTINGS_FILE"), *x = getenv("XDG_CONFIG_HOME"),
               *h = getenv("HOME");
#if defined(_WIN32)
    /* %LOCALAPPDATA%\JSRF Unleashed\settings: the launcher's, which also
     * passes it as JSRF_SETTINGS_FILE. */
    const char *ad = getenv("LOCALAPPDATA");
    if (!(f && *f) && ad && *ad) { snprintf(buf, n, "%s/JSRF Unleashed/settings", ad); return; }
#endif
    if (f && *f)      snprintf(buf, n, "%s", f);
    else if (x && *x) snprintf(buf, n, "%s/jsrf-unleashed/settings", x);
    else if (h && *h) snprintf(buf, n, "%s/.config/jsrf-unleashed/settings", h);
    else buf[0] = 0;
}

static int sdl_cfg_get(const char *key, int def)
{
    char path[1024], line[1200];
    size_t kl = strlen(key);
    FILE *f;
    int v = def;
    sdl_cfg_path(path, sizeof path);
    if (!path[0] || !(f = fopen(path, "r"))) return def;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, kl) && line[kl] == '=') v = atoi(line + kl + 1);
    fclose(f);
    return v;
}

static void sdl_cfg_set(const char *key, int val)
{
    char path[1024], tmp[1100], line[1200];
    size_t kl = strlen(key);
    FILE *in, *out;
    int done = 0;
    sdl_cfg_path(path, sizeof path);
    if (!path[0]) return;
    {   /* mkdir -p the directory */
        char dir[1024]; char *p;
        snprintf(dir, sizeof dir, "%s", path);
        if ((p = strrchr(dir, '/')) != NULL) {
            *p = 0;
            for (p = dir + 1; *p; p++)
                if (*p == '/') { *p = 0; mkdir(dir, 0755); *p = '/'; }
            mkdir(dir, 0755);
        }
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (!(out = fopen(tmp, "w"))) return;
    if ((in = fopen(path, "r")) != NULL) {
        while (fgets(line, sizeof line, in)) {
            if (!strncmp(line, key, kl) && line[kl] == '=') {
                if (!done) fprintf(out, "%s=%d\n", key, val);
                done = 1;
            } else fputs(line, out);
        }
        fclose(in);
    }
    if (!done) fprintf(out, "%s=%d\n", key, val);
    fclose(out);
    /* On Windows rename() will not replace a file that exists -- and the
     * launcher has always written this one -- so a choice made here was
     * never kept and settings.tmp stayed behind. */
    if (rename(tmp, path) != 0) {
        remove(path);
        rename(tmp, path);
    }
}

/* nv2a_widescreen() asks this when RECOMP_WIDESCREEN is not set. */
int nv_window_pref_widescreen(void) { return sdl_cfg_get("widescreen", 0) != 0; }

/* The camera on the right stick (port/recomp_manual.c): on unless turned off --
 * Controls in the window's menu bar on Windows, F7 anywhere (and the Android
 * app's settings), kept as rstick_camera= (0 for off). Asked every frame, so
 * the file is read once. */
static volatile int g_rstick_cam = -1;
int nv_window_pref_rstick_camera(void);
int nv_window_pref_rstick_camera(void)
{
    if (g_rstick_cam < 0) g_rstick_cam = sdl_cfg_get("rstick_camera", 1) != 0;
    return g_rstick_cam;
}
static void nv_window_set_rstick_camera(int on)
{
    g_rstick_cam = on ? 1 : 0;
    sdl_cfg_set("rstick_camera", g_rstick_cam);
    fprintf(stderr, "[WIN] the camera on the right stick: %s\n", on ? "on" : "off");
}
/* Asked for from another thread (the Android app's settings): done at the
 * next sdl_pump, by the thread that owns the window and the settings file. */
static volatile int g_sdl_want_rcam = -1;
void nv_window_request_rstick_camera(int on);
void nv_window_request_rstick_camera(int on) { g_sdl_want_rcam = on ? 1 : 0; }

/* The keyboard as a gamepad -- the same layout as the Mac's, by key POSITION
 * (scancodes), so W A S D on QWERTY is Z Q S D on AZERTY:
 *   W A S D    left stick + d-pad       arrows   right stick
 *   Space, J   A        K  B            H  X     L  Y
 *   E          R trigger                Q  L trigger
 *   R          Black                    F  White
 *   Return     Start                    Esc  Back (and Android's Back)
 *   C / V      left / right stick click
 * F11 or Alt+Return: full screen. F10: widescreen. F9: resolution. F8:
 * anti-aliasing. F7: the camera on the right stick. */
static volatile unsigned short g_sdl_kb_buttons;
static volatile unsigned char  g_sdl_kb_analog[8];

static void sdl_key(SDL_Scancode sc, int down)
{
    unsigned short bit = 0;
    int analog = -1;
    switch (sc) {
    case SDL_SCANCODE_W:      bit = 1u << 0; break;
    case SDL_SCANCODE_S:      bit = 1u << 1; break;
    case SDL_SCANCODE_A:      bit = 1u << 2; break;
    case SDL_SCANCODE_D:      bit = 1u << 3; break;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: bit = 1u << 4; break;
    case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_AC_BACK: bit = 1u << 5; break;   /* AC_BACK: Android's Back */
    case SDL_SCANCODE_UP:     bit = 1u << 6; break;
    case SDL_SCANCODE_DOWN:   bit = 1u << 7; break;
    case SDL_SCANCODE_LEFT:   bit = 1u << 8; break;
    case SDL_SCANCODE_RIGHT:  bit = 1u << 9; break;
    case SDL_SCANCODE_C:      bit = 1u << 10; break;
    case SDL_SCANCODE_V:      bit = 1u << 11; break;
    case SDL_SCANCODE_SPACE: case SDL_SCANCODE_J: analog = 0; break;   /* A */
    case SDL_SCANCODE_K:      analog = 1; break;                       /* B */
    case SDL_SCANCODE_H:      analog = 2; break;                       /* X */
    case SDL_SCANCODE_L:      analog = 3; break;                       /* Y */
    case SDL_SCANCODE_R:      analog = 4; break;                       /* Black */
    case SDL_SCANCODE_F:      analog = 5; break;                       /* White */
    case SDL_SCANCODE_Q:      analog = 6; break;                       /* L trigger */
    case SDL_SCANCODE_E:      analog = 7; break;                       /* R trigger */
    default: return;
    }
    if (bit) {
        if (down) g_sdl_kb_buttons |= bit;
        else      g_sdl_kb_buttons &= (unsigned short)~bit;
    }
    if (analog >= 0) g_sdl_kb_analog[analog] = down ? 255 : 0;
}

#if defined(__ANDROID__)
/* The Android app's on-screen controls (port/android/jsrf_android.c): what
 * they hold, in the same bits, and 1 while they are showing -- the pad then
 * counts as connected with nothing held, as on the iPhone. */
int nv_touch_keys(unsigned short *buttons, unsigned char *analog);
#endif

/* Read by the input layer (xinput_device.c), with the Mac's signature. */
int nv_window_keys(unsigned short *buttons, unsigned char *analog)
{
    int i, any;
    unsigned short b = g_sdl_kb_buttons;
    unsigned char ta[8] = { 0 };
#if defined(__ANDROID__)
    unsigned short tb = 0;
    int touch = nv_touch_keys(&tb, ta);
    b |= tb;
#else
    int touch = 0;
#endif
    if (buttons) *buttons = b;
    any = b != 0 || touch;
    for (i = 0; i < 8; i++) {
        unsigned char v = g_sdl_kb_analog[i] > ta[i] ? g_sdl_kb_analog[i] : ta[i];
        if (analog) analog[i] = v;
        if (v) any = 1;
    }
    return any;
}

#if defined(_WIN32)
static void sdl_menu_show(int on);
static void sdl_menu_update(void);
#else
static void sdl_menu_show(int on) { (void)on; }
static void sdl_menu_update(void) { }
#endif

static void sdl_toggle_fullscreen(void)
{
    int fs = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    if (!fs) sdl_menu_show(0);          /* the menu bar is the window's */
    SDL_SetWindowFullscreen(g_window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (fs) sdl_menu_show(1);
    SDL_ShowCursor(fs ? SDL_ENABLE : SDL_DISABLE);
    fprintf(stderr, "[WIN] full screen %s\n", fs ? "off" : "on");
}

/* F11, Alt+Enter and Video > Full screen: the player's choice, which the next
 * start keeps on Windows (gl_init) -- the window as it is after the switch,
 * so a switch that did not happen is not kept. Not the switches around the
 * F9 and F8 boxes, which leave full screen while a box is up and go back. */
static void sdl_fullscreen_by_player(void)
{
    sdl_toggle_fullscreen();
#if defined(_WIN32)
    sdl_cfg_set("fullscreen", (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 1 : 0);
#endif
}

/* Widescreen asked for from another thread (the Android app's settings): done
 * at the next sdl_pump, by the thread that owns the window, as F10 is. */
static volatile int g_sdl_want_wide = -1;
void nv_window_request_widescreen(int on);
void nv_window_request_widescreen(int on) { g_sdl_want_wide = on ? 1 : 0; }

/* The picture -- 0 4:3, 1 16:9, 2 21:9 (nv2a_set_picture) -- kept as
 * widescreen= and ultrawide= in the settings, the first as before, so the
 * Android app's switch and an older version's choice still read right. */
static int sdl_pref_picture(void)
{
    if (!sdl_cfg_get("widescreen", 0)) return 0;
    return sdl_cfg_get("ultrawide", 0) ? 2 : 1;
}
/* The one shown now (RECOMP_WIDESCREEN and RECOMP_WIDE_ASPECT included). */
static int sdl_picture_now(void)
{
    if (!nv2a_widescreen()) return 0;
    return nv2a_wide_aspect() > 16.0f / 9.0f + 0.01f ? 2 : 1;
}
/* The shape of the screen the window is on (before there is a window, the
 * first screen's); 0 if SDL cannot say. */
static float sdl_screen_aspect(void)
{
    SDL_DisplayMode dm;
    int d = g_window ? SDL_GetWindowDisplayIndex(g_window) : 0;
    if (d < 0) d = 0;
    if (SDL_GetDesktopDisplayMode(d, &dm) != 0 || dm.w <= 0 || dm.h <= 0) return 0.0f;
    return (float)dm.w / (float)dm.h;
}

/* F10 (round 4:3, 16:9, 21:9), Video > Picture on Windows, and the Android
 * app's widescreen switch (16:9 or 4:3). */
static void sdl_set_picture(int pic)
{
    nv2a_set_picture(pic, sdl_screen_aspect(), 1);
    sdl_cfg_set("widescreen", pic != 0);
    sdl_cfg_set("ultrawide", pic == 2);
    /* A window (not full screen, not maximised) takes the new shape at the
     * same height, as the Mac's does -- narrower if that would not fit. */
    if (!(SDL_GetWindowFlags(g_window) & (SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_MAXIMIZED))) {
        int ww, wh, nw;
        SDL_Rect usable;
        int d = SDL_GetWindowDisplayIndex(g_window);
        SDL_GetWindowSize(g_window, &ww, &wh);
        nw = (int)((float)wh * (pic ? nv2a_wide_aspect() : 4.0f / 3.0f) + 0.5f);
        if (SDL_GetDisplayUsableBounds(d < 0 ? 0 : d, &usable) == 0 && usable.w > 320
            && nw > usable.w * 19 / 20) {
            wh = wh * (usable.w * 19 / 20) / nw;
            nw = usable.w * 19 / 20;
        }
        SDL_SetWindowSize(g_window, nw, wh);
    }
    sdl_menu_update();
    if (pic) fprintf(stderr, "[WIN] picture %s (%.3f:1)\n", pic == 2 ? "21:9" : "16:9",
                     (double)nv2a_wide_aspect());
    else     fprintf(stderr, "[WIN] picture 4:3\n");
}

/* F9: the internal resolution, 1x to 6x the Xbox's 640x480 (4x on Android,
 * as the iPhone's settings) -- what the Mac's Video menu offers. 6x is
 * 3840x2880: a 4K screen's width, one pixel for one, with the height drawn
 * finer than the screen shows it -- in widescreen, where the 640x480 surface
 * holds a 16:9 view squeezed, that is 3840x2160 exactly across. The
 * renderer sizes its targets
 * once, at start-up, so a new one means starting again: after asking, the
 * choice is stored under the launchers' own "scale" key (they pass it on as
 * RECOMP_GL_SCALE), put in the environment and the engine relaunches itself
 * (port/main.c), as the Mac's does -- in full screen again if that is where
 * it was. */
extern void recomp_restart_self(void) __attribute__((weak));
static void sdl_restart_at(int pick, int fs)
{
    char val[4];
    if (!recomp_restart_self) return;
    sdl_cfg_set("scale", pick);
    snprintf(val, sizeof val, "%d", pick);
#if defined(_WIN32)
    _putenv_s("RECOMP_GL_SCALE", val);
    _putenv_s("RECOMP_FULLSCREEN", fs ? "1" : "0");
#else
    setenv("RECOMP_GL_SCALE", val, 1);
    setenv("RECOMP_FULLSCREEN", fs ? "1" : "0", 1);
#endif
    fprintf(stderr, "[WIN] resolution %dx chosen; restarting\n", pick);
    recomp_restart_self();      /* does not return when it works */
}

/* Keys held when a box took the keyboard never see their release. */
static void sdl_keys_release(void)
{
    g_sdl_kb_buttons = 0;
    memset((void *)g_sdl_kb_analog, 0, sizeof g_sdl_kb_analog);
}

static void sdl_choose_scale(void)
{
    static const char *const names[NV2A_SCALE_MAX] = {
        "1x  640x480", "2x  1280x960", "3x  1920x1440", "4x  2560x1920",
#if NV2A_SCALE_MAX > 4
        "5x  3200x2400", "6x  3840x2880 (4K)"
#endif
    };
    SDL_MessageBoxButtonData b[NV2A_SCALE_MAX + 1];
    SDL_MessageBoxData box;
    char msg[512];
    int cur = (int)gl_scale(), pick = -1, i, was_fs;
    if (!recomp_restart_self) return;
    /* The box is modal: the game waits for an answer. Over a full-screen
     * window it was hidden behind the game on Windows, so the game simply
     * looked hung until Alt+F4 closed the box it could not see. So full
     * screen is left while it is up, and put back if nothing is chosen. */
    was_fs = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
#if defined(__ANDROID__)
    was_fs = 0;     /* the app is always full screen, and the box shows over it */
#endif
    if (was_fs) sdl_toggle_fullscreen();
    for (i = 0; i < NV2A_SCALE_MAX; i++) {
        b[i].flags    = i + 1 == cur ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0;
        b[i].buttonid = i + 1;
        b[i].text     = names[i];
    }
    b[NV2A_SCALE_MAX].flags    = SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
    b[NV2A_SCALE_MAX].buttonid = 0;
    b[NV2A_SCALE_MAX].text     = "Cancel";
    /* Short lines: SDL's own X11 message box does not wrap, and cut the
     * first line off at its width. */
    snprintf(msg, sizeof msg,
             "The game is drawn at a multiple of the Xbox's 640x480,\n"
             "then fitted to the window. Higher is sharper and asks\n"
             "more of the graphics card.%s\nNow: %dx (%dx%d).\n\n"
             "Changing it restarts the game: anything since\n"
             "your last save is lost.",
             NV2A_SCALE_MAX > 4 ? " 6x suits a 4K screen." : "",
             cur, 640 * cur, 480 * cur);
    memset(&box, 0, sizeof box);
    box.flags      = SDL_MESSAGEBOX_INFORMATION | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT;
    box.window     = g_window;
    box.title      = "Resolution";
    box.message    = msg;
    box.numbuttons = NV2A_SCALE_MAX + 1;
    box.buttons    = b;
    if (SDL_ShowMessageBox(&box, &pick) != 0 || pick < 1 || pick > NV2A_SCALE_MAX || pick == cur) {
        if (was_fs) sdl_toggle_fullscreen();
        sdl_keys_release();
        return;
    }
    sdl_restart_at(pick, was_fs);
}

/* Anti-aliasing, as the menu (Windows) and F8 choose it: taken at the next
 * flip and kept as smaa= and msaa= (0 for none), as F10 keeps widescreen=. A
 * device that only takes a count of samples at start (OpenGL ES) keeps the
 * choice for the next one. */
static void sdl_pick_smaa(int on)
{
    if (!G || !G->target_smaa) return;
    g_smaa_want = on ? 1 : 0;
    sdl_cfg_set("smaa", g_smaa_want);
    sdl_menu_update();
    fprintf(stderr, "[WIN] SMAA %s\n", on ? "on" : "off");
}

static void sdl_pick_msaa(int n)
{
    if (n < 1) n = 1;
    if (g_msaa_max && n > g_msaa_max) n = g_msaa_max;
    sdl_cfg_set("msaa", n > 1 ? n : 0);
    if (g_msaa_live) g_msaa_want = n;
    sdl_menu_update();
    fprintf(stderr, "[WIN] MSAA %dx%s\n", n, g_msaa_live ? "" : " from the next start");
}

/* F8: the same choices in a box -- SMAA on or off, or a number of samples --
 * for a window without a menu bar (Linux) and in full screen. */
static void sdl_choose_aa(void)
{
    static const char *const ms[4] = { "MSAA off", "MSAA 2x", "MSAA 4x", "MSAA 8x" };
    SDL_MessageBoxButtonData b[6];
    SDL_MessageBoxData box;
    char msg[640];
    int pick = -1, nb = 0, i, was_fs;
    was_fs = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
#if defined(__ANDROID__)
    was_fs = 0;
#endif
    if (was_fs) sdl_toggle_fullscreen();     /* see sdl_choose_scale */
    if (G && G->target_smaa) {
        b[nb].flags = 0; b[nb].buttonid = 100; b[nb].text = g_smaa_want ? "SMAA off" : "SMAA on";
        nb++;
    }
    for (i = 0; i < 4; i++) {
        if (g_msaa_max && (1 << i) > g_msaa_max) break;
        b[nb].flags = (1 << i) == g_msaa_want ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0;
        b[nb].buttonid = 1 << i;
        b[nb].text = ms[i];
        nb++;
    }
    b[nb].flags = SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT; b[nb].buttonid = 0; b[nb].text = "Cancel";
    nb++;
    /* Short lines, as in sdl_choose_scale. */
    snprintf(msg, sizeof msg,
             "SMAA smooths the jagged edges in each finished\n"
             "frame, for a small cost, the HUD's with the rest.\n"
             "MSAA draws every edge with 2, 4 or 8 samples a\n"
             "pixel: cleaner, and more work for the graphics\n"
             "card. The two can be on together.\n\n"
             "Now: SMAA %s, MSAA %s.%s",
             g_smaa_want ? "on" : "off",
             g_msaa_want > 1 ? (g_msaa_want == 2 ? "2x" : g_msaa_want == 4 ? "4x" : "8x") : "off",
             g_msaa_live ? "" : "\nA new MSAA is used the next time the game starts.");
    memset(&box, 0, sizeof box);
    box.flags      = SDL_MESSAGEBOX_INFORMATION | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT;
    box.window     = g_window;
    box.title      = "Anti-aliasing";
    box.message    = msg;
    box.numbuttons = nb;
    box.buttons    = b;
    if (SDL_ShowMessageBox(&box, &pick) == 0) {
        if (pick == 100) sdl_pick_smaa(!g_smaa_want);
        else if (pick >= 1) sdl_pick_msaa(pick);
    }
    if (was_fs) sdl_toggle_fullscreen();
    sdl_keys_release();
}

/* F7 and Controls > Camera on the Right Stick: on or off, kept. */
static void sdl_toggle_rstick_camera(void)
{
    nv_window_set_rstick_camera(!nv_window_pref_rstick_camera());
    sdl_menu_update();
}

#if defined(_WIN32)
/*
 * The window's menu bar (Windows): Video -- the resolution, anti-aliasing,
 * the picture (4:3, 16:9, 21:9) and full screen, which is what the Mac's
 * Video menu has and what F9, F8, F10 and F11 do. Not in full screen, where
 * it would sit across the top of the picture; the keys work there, and
 * leaving full screen brings it back. SDL hands the menu's WM_COMMAND over
 * as a system event (sdl_pump).
 *
 * Holding a menu open holds the thread that owns the window, which is the
 * one that runs the GPU, so the game waits while it is open -- as a pause
 * menu would. Choosing a resolution asks before it restarts.
 */
#include <SDL2/SDL_syswm.h>
enum { MENU_RES1 = 101, MENU_FULL = 111, MENU_PIC1 = 112, MENU_SMAA = 120, MENU_MSAA1 = 121,
       MENU_RCAM = 130 };
static HWND  g_menu_hwnd;
static HMENU g_menu_bar, g_menu_video, g_menu_res, g_menu_aa, g_menu_pic, g_menu_controls;

static void sdl_menu_update(void)
{
    int i, cur = (int)gl_scale();
    if (!g_menu_bar) return;
    for (i = 1; i <= NV2A_SCALE_MAX; i++)
        CheckMenuItem(g_menu_res, MENU_RES1 + i - 1,
                      MF_BYCOMMAND | (i == cur ? MF_CHECKED : MF_UNCHECKED));
    if (g_menu_pic)
        CheckMenuRadioItem(g_menu_pic, MENU_PIC1, MENU_PIC1 + 2, MENU_PIC1 + sdl_picture_now(), MF_BYCOMMAND);
    /* Anti-aliasing: what is asked for, and only what the device can do. */
    CheckMenuItem(g_menu_aa, MENU_SMAA, MF_BYCOMMAND | (g_smaa_want ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(g_menu_aa, MENU_SMAA,
                   MF_BYCOMMAND | (!G || G->target_smaa ? MF_ENABLED : MF_GRAYED));
    for (i = 0; i < 4; i++) {
        int n = 1 << i;
        CheckMenuItem(g_menu_aa, MENU_MSAA1 + i,
                      MF_BYCOMMAND | (n == g_msaa_want ? MF_CHECKED : MF_UNCHECKED));
        EnableMenuItem(g_menu_aa, MENU_MSAA1 + i,
                       MF_BYCOMMAND | (!g_msaa_max || n <= g_msaa_max ? MF_ENABLED : MF_GRAYED));
    }
    if (g_menu_controls)
        CheckMenuItem(g_menu_controls, MENU_RCAM,
                      MF_BYCOMMAND | (nv_window_pref_rstick_camera() ? MF_CHECKED : MF_UNCHECKED));
}

/* The menu takes its height from the window, not from the picture: the
 * window is grown by it, so the game keeps the size it had. */
static void sdl_menu_show(int on)
{
    int w = 0, h = 0;
    if (!g_menu_bar || !g_menu_hwnd) return;
    if ((GetMenu(g_menu_hwnd) != NULL) == (on != 0)) return;
    SDL_GetWindowSize(g_window, &w, &h);
    SetMenu(g_menu_hwnd, on ? g_menu_bar : NULL);
    if (on && w > 0 && h > 0) SDL_SetWindowSize(g_window, w, h);
}

static void sdl_menu_init(void)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(g_window, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return;
    g_menu_hwnd  = info.info.win.window;
    g_menu_bar   = CreateMenu();
    g_menu_video = CreatePopupMenu();
    g_menu_res   = CreatePopupMenu();
    g_menu_aa    = CreatePopupMenu();
    g_menu_pic   = CreatePopupMenu();
    if (!g_menu_bar || !g_menu_video || !g_menu_res || !g_menu_aa || !g_menu_pic) { g_menu_bar = NULL; return; }
    AppendMenuW(g_menu_res, MF_STRING, MENU_RES1 + 0, L"1x  (640 x 480)");
    AppendMenuW(g_menu_res, MF_STRING, MENU_RES1 + 1, L"2x  (1280 x 960)");
    AppendMenuW(g_menu_res, MF_STRING, MENU_RES1 + 2, L"3x  (1920 x 1440)");
    AppendMenuW(g_menu_res, MF_STRING, MENU_RES1 + 3, L"4x  (2560 x 1920)");
    AppendMenuW(g_menu_res, MF_STRING, MENU_RES1 + 4, L"5x  (3200 x 2400)");
    AppendMenuW(g_menu_res, MF_STRING, MENU_RES1 + 5, L"6x  (3840 x 2880) -- 4K");
    AppendMenuW(g_menu_aa, MF_STRING, MENU_SMAA, L"&SMAA");
    AppendMenuW(g_menu_aa, MF_SEPARATOR, 0, NULL);
    AppendMenuW(g_menu_aa, MF_STRING, MENU_MSAA1 + 0, L"MSAA &off");
    AppendMenuW(g_menu_aa, MF_STRING, MENU_MSAA1 + 1, L"MSAA &2x");
    AppendMenuW(g_menu_aa, MF_STRING, MENU_MSAA1 + 2, L"MSAA &4x");
    AppendMenuW(g_menu_aa, MF_STRING, MENU_MSAA1 + 3, L"MSAA &8x");
    AppendMenuW(g_menu_video, MF_POPUP, (UINT_PTR)g_menu_res, L"&Resolution\tF9");
    AppendMenuW(g_menu_pic, MF_STRING, MENU_PIC1 + 0, L"&4:3");
    AppendMenuW(g_menu_pic, MF_STRING, MENU_PIC1 + 1, L"16:9  (&widescreen)");
    AppendMenuW(g_menu_pic, MF_STRING, MENU_PIC1 + 2, L"21:9  (&ultrawide)");
    AppendMenuW(g_menu_video, MF_POPUP, (UINT_PTR)g_menu_aa, L"&Anti-aliasing\tF8");
    AppendMenuW(g_menu_video, MF_POPUP, (UINT_PTR)g_menu_pic, L"&Picture\tF10");
    AppendMenuW(g_menu_video, MF_STRING, MENU_FULL, L"&Full screen\tF11");
    AppendMenuW(g_menu_bar, MF_POPUP, (UINT_PTR)g_menu_video, L"&Video");
    if ((g_menu_controls = CreatePopupMenu()) != NULL) {
        AppendMenuW(g_menu_controls, MF_STRING, MENU_RCAM, L"Camera on the &Right Stick\tF7");
        AppendMenuW(g_menu_bar, MF_POPUP, (UINT_PTR)g_menu_controls, L"&Controls");
    }
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
    sdl_menu_update();
    sdl_menu_show(!(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP));
}

static void sdl_menu_command(unsigned id)
{
    if (id >= MENU_RES1 && id < MENU_RES1 + NV2A_SCALE_MAX) {
        int pick = (int)(id - MENU_RES1) + 1, ans = 0;
        char msg[256];
        SDL_MessageBoxButtonData b[2];
        SDL_MessageBoxData box;
        if (pick == (int)gl_scale() || !recomp_restart_self) return;
        b[0].flags = SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT; b[0].buttonid = 1; b[0].text = "Restart";
        b[1].flags = SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT; b[1].buttonid = 0; b[1].text = "Cancel";
        snprintf(msg, sizeof msg,
                 "Restart the game at %dx (%d x %d)?\n\n"
                 "Anything since your last save is lost.",
                 pick, 640 * pick, 480 * pick);
        memset(&box, 0, sizeof box);
        box.flags = SDL_MESSAGEBOX_INFORMATION;
        box.window = g_window;
        box.title = "Resolution";
        box.message = msg;
        box.numbuttons = 2;
        box.buttons = b;
        if (SDL_ShowMessageBox(&box, &ans) == 0 && ans == 1) sdl_restart_at(pick, 0);
        sdl_keys_release();
    } else if (id >= MENU_PIC1 && id < MENU_PIC1 + 3) {
        sdl_set_picture((int)(id - MENU_PIC1));
    } else if (id == MENU_FULL) {
        sdl_fullscreen_by_player();
    } else if (id == MENU_SMAA) {
        sdl_pick_smaa(!g_smaa_want);
    } else if (id >= MENU_MSAA1 && id < MENU_MSAA1 + 4) {
        sdl_pick_msaa(1 << (id - MENU_MSAA1));
    } else if (id == MENU_RCAM) {
        sdl_toggle_rstick_camera();
    }
}
#endif

/* The frame, fitted into the window between black bars: a widened scene at
 * 16:9 (the 640x480 surface holds a 16:9 view squeezed), anything else at its
 * own shape. One blit; every piece of GL state the renderer caches is put
 * back the way it was. */
static void sdl_present(GLuint src_fbo, int sw, int sh)
{
    GLint vp[4], sbox[4], rfb = 0, dfb = 0;
    GLboolean sc = glIsEnabled(GL_SCISSOR_TEST), cm[4];
    int ww = 0, wh = 0, fw = sw, fh = sh, dw, dh;
    if (sw <= 0 || sh <= 0) return;
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_SCISSOR_BOX, sbox);
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
    SDL_GL_GetDrawableSize(g_window, &ww, &wh);
    if (ww <= 0 || wh <= 0) return;
    if (nv2a_wide_scene()) { fw = (int)(nv2a_wide_aspect() * 900.0f + 0.5f); fh = 900; }   /* 16:9 = 1600:900 */
    dw = ww; dh = wh;
    if ((long)ww * fh > (long)wh * fw) dw = (int)((long)wh * fw / fh);
    else                               dh = (int)((long)ww * fh / fw);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, ww, wh);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
    glBlitFramebuffer(0, 0, sw, sh,
                      (ww - dw) / 2, (wh - dh) / 2, (ww - dw) / 2 + dw, (wh - dh) / 2 + dh,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    SDL_GL_SwapWindow(g_window);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)dfb);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glScissor(sbox[0], sbox[1], sbox[2], sbox[3]);
    if (sc) glEnable(GL_SCISSOR_TEST);
    glColorMask(cm[0], cm[1], cm[2], cm[3]);
    { static int said;
      if (said < 3) {
          said++;
          fprintf(stderr, "[WIN] presented #%d: %dx%d frame into %dx%d (%dx%d)\n",
                  said, sw, sh, ww, wh, dw, dh);
      } }
}

/* ---- the presenter: frames reach the screen from a thread of their own ---
 *
 * Presenting straight from the GPU thread at the flip cannot wait for the
 * display. That thread also answers the title's vblank interrupt while it
 * paces the frame (frame_pace), and the title holds the guest lock until it
 * does: blocked in a synchronised swap, every guest thread would wait with
 * it. So the swap was never synchronised (interval 0) and a frame reached the
 * screen whenever it was finished. On Windows, where the compositor shows the
 * newest frame at each refresh, the game's own 60 Hz clock and the display's
 * drift against each other, and every so often a frame is shown twice and the
 * next not at all -- a stutter; in full screen, tearing as well.
 *
 * Now it goes as the Metal renderer does it on the Mac. The flip only hands
 * the frame over: a copy on the GPU into one of three textures, and a fence.
 * A presenter thread, with a context of its own that shares those textures,
 * puts the newest one on screen with the swap synchronised to the display,
 * for as long as that takes; nothing else waits on it. It also measures the
 * display's refresh, and frame_pace then paces the game at that rate (or at a
 * whole fraction of it, when that is within 3% of 60 Hz) and lands each frame
 * half a refresh before the next one, so that each is shown exactly once.
 *
 * On by default on Windows. RECOMP_GL_PRESENTER=1 turns it on elsewhere
 * (Linux, with GLX), 0 off -- the swap at the flip, as before. Not on
 * Android: EGL lets a window surface be current on one thread only.
 */
#if !defined(NV2A_GLES)
#define NV2A_SDL_PRESENTER 1

typedef struct {
    GLuint   tex, fbo;       /* the GL thread's: the copy, and a framebuffer on it */
    int      w, h, wide;     /* the frame's size; a 16:9 scene (nv2a_wide_scene) */
    unsigned gen;            /* changes when the texture is given new storage */
    GLsync   fence;          /* signalled when the copy is done */
} GlShown;
static GlShown       g_gs[3];
static SDL_mutex    *g_gs_mu;
static SDL_cond     *g_gs_cv;
static int           g_gs_new = -1;     /* the newest slot handed over */
static int           g_gs_busy = -1;    /* the slot being put on screen */
static uint64_t      g_gs_serial;       /* frames handed over so far */
static SDL_GLContext g_pres_ctx;
static volatile int  g_presenter;       /* 0 not used, 1 running, -1 failed */

/* The display as the presenter measured it, for frame_pace: seconds per
 * refresh (0: unknown, the swap does not wait for it), and when the last
 * swap ended (mono_now). */
static volatile double g_vs_period, g_vs_last;
static volatile int    g_vs_hz;         /* what SDL says the display runs at */

/* How it went: frames shown, frames replaced before they could be, and how
 * long each stayed on screen, in refreshes (1, 2, 3, 4 or more; or, with no
 * refresh to count in, under 14 ms / about 1/60 s / up to 30 ms / up to
 * 45 ms / longer). */
static struct { uint64_t shown, dropped, on[5]; double last; } g_ps;

#if defined(_WIN32)
/* Windows' compositor knows exactly when the display refreshed last and how
 * often it does (DwmGetCompositionTimingInfo): a better reference than when a
 * swap returned, which with one frame in flight need not wait for anything.
 * Fetched from dwmapi.dll when there; Wine fills in the period but not the
 * time, and then the swaps are what there is. */
#include <dwmapi.h>
typedef HRESULT (WINAPI *DwmTimingFn)(HWND, DWM_TIMING_INFO *);
static DwmTimingFn g_dwm_timing;
static double      g_qpc_s;          /* seconds per QueryPerformanceCounter tick */
static double      g_qpc_off;        /* mono_now() - QPC time, should be 0 */

static void dwm_open(void)
{
    LARGE_INTEGER f, c;
    HMODULE m = LoadLibraryW(L"dwmapi.dll");
    if (m) g_dwm_timing = (DwmTimingFn)(void (*)(void))GetProcAddress(m, "DwmGetCompositionTimingInfo");
    if (QueryPerformanceFrequency(&f) && f.QuadPart > 0 && QueryPerformanceCounter(&c)) {
        g_qpc_s = 1.0 / (double)f.QuadPart;
        g_qpc_off = mono_now() - (double)c.QuadPart * g_qpc_s;
    } else {
        g_dwm_timing = NULL;
    }
}

/* The compositor's last refresh (in mono_now() time) and its period. */
static int dwm_vblank(double *last, double *period)
{
    DWM_TIMING_INFO ti;
    double p;
    if (!g_dwm_timing) return 0;
    memset(&ti, 0, sizeof ti);
    ti.cbSize = sizeof ti;
    if (g_dwm_timing(NULL, &ti) != S_OK || !ti.qpcRefreshPeriod || !ti.qpcVBlank) return 0;
    p = (double)ti.qpcRefreshPeriod * g_qpc_s;
    if (p < 0.004 || p > 0.05) return 0;
    *period = p;
    *last = (double)ti.qpcVBlank * g_qpc_s + g_qpc_off;
    return 1;
}
#endif

static int ps_bucket(double dt)
{
    double p = g_vs_period;
    if (p > 0.0) {
        int n = (int)(dt / p + 0.5);
        return n <= 1 ? 0 : n >= 5 ? 4 : n - 1;
    }
    return dt < 0.014 ? 0 : dt < 0.0195 ? 1 : dt < 0.030 ? 2 : dt < 0.045 ? 3 : 4;
}

static void ps_line(const char *what, const uint64_t *on, uint64_t shown, uint64_t dropped)
{
    if (g_vs_period > 0.0)
        fprintf(stderr, "[PRESENT] %s: %llu frames shown, %llu replaced before they could be; "
                "on screen for 1 refresh %llu, 2 %llu, 3 %llu, 4 %llu, more %llu "
                "(display %.2f Hz)\n", what,
                (unsigned long long)shown, (unsigned long long)dropped,
                (unsigned long long)on[0], (unsigned long long)on[1], (unsigned long long)on[2],
                (unsigned long long)on[3], (unsigned long long)on[4], 1.0 / g_vs_period);
    else
        fprintf(stderr, "[PRESENT] %s: %llu frames shown, %llu replaced before they could be; "
                "on screen under 14 ms %llu, a 60th of a second %llu, 19-30 ms %llu, "
                "30-45 ms %llu, longer %llu (the swap does not wait for the display)\n", what,
                (unsigned long long)shown, (unsigned long long)dropped,
                (unsigned long long)on[0], (unsigned long long)on[1], (unsigned long long)on[2],
                (unsigned long long)on[3], (unsigned long long)on[4]);
}

static void pres_clear(void)
{
    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(g_window, &ww, &wh);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, ww > 0 ? ww : 1, wh > 0 ? wh : 1);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

/* How long a synchronised swap takes, with nothing else going on: the
 * display's refresh -- or next to nothing, when the driver does not wait
 * (vsync turned off in its control panel, a remote desktop, a VM). */
static void pres_calibrate(void)
{
    enum { N = 40, SKIP = 8 };
    double t[N], iv[N], sum = 0.0, med;
    int i, j, n = 0, kept = 0;
    for (i = 0; i < N; i++) {
        pres_clear();
        SDL_GL_SwapWindow(g_window);
        glFinish();
        t[i] = mono_now();
    }
    for (i = SKIP + 1; i < N; i++) iv[n++] = t[i] - t[i - 1];
    for (i = 1; i < n; i++)                         /* sort, for the median */
        for (j = i; j > 0 && iv[j - 1] > iv[j]; j--) { double x = iv[j]; iv[j] = iv[j - 1]; iv[j - 1] = x; }
    med = iv[n / 2];
    for (i = 0; i < n; i++)
        if (iv[i] > med * 0.9 && iv[i] < med * 1.1) { sum += iv[i]; kept++; }
    if (med >= 0.004 && med <= 0.05 && kept >= n * 3 / 4) {
        g_vs_period = sum / kept;
        fprintf(stderr, "[WIN] display: %.3f Hz, measured over %d swaps (%d Hz reported)\n",
                1.0 / g_vs_period, kept, g_vs_hz);
    } else {
        g_vs_period = 0.0;
        fprintf(stderr, "[WIN] the swap does not wait for the display (a swap takes %.2f ms; "
                "%d Hz reported): frames are paced by the game's clock\n", med * 1000.0, g_vs_hz);
    }
    g_vs_last = mono_now();
}

static int SDLCALL pres_main(void *arg)
{
    GLuint fbo[3] = { 0, 0, 0 };
    uint64_t seen = 0, shown0 = 0, dropped0 = 0, on0[5] = { 0, 0, 0, 0, 0 };
    double report = 0.0;
    int told = 0;
    (void)arg;
    if (SDL_GL_MakeCurrent(g_window, g_pres_ctx) != 0) {
        fprintf(stderr, "[WIN] the presenter has no context (%s): frames are shown at the flip\n",
                SDL_GetError());
        g_presenter = -1;
        return 0;
    }
    /* No epoxy_handle_external_wglMakeCurrent here: it switches libepoxy to
     * per-thread function tables, which then exist only on the threads that
     * called it -- the GPU thread died on its next shader. Both contexts have
     * the window's pixel format and the same driver, so the entry points
     * libepoxy has already fetched are good for both. */
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
    if (SDL_GL_SetSwapInterval(1) != 0)
        fprintf(stderr, "[WIN] no synchronised swap: %s\n", SDL_GetError());
    glGenFramebuffers(3, fbo);
    pres_calibrate();
#if defined(_WIN32)
    { double l = 0.0, p = 0.0;
      if (dwm_vblank(&l, &p))
          fprintf(stderr, "[WIN] the compositor refreshes at %.3f Hz: frames are lined up with it\n",
                  1.0 / p); }
#endif
    /* What was handed over while that ran is not counted as replaced. */
    SDL_LockMutex(g_gs_mu);
    seen = g_gs_serial ? g_gs_serial - 1 : 0;
    SDL_UnlockMutex(g_gs_mu);
    report = mono_now() + 10.0;
    for (;;) {
        int slot, w, h, wide, ww = 0, wh = 0, fw, fh, dw, dh;
        GLuint tex;
        GLsync fence;
        uint64_t serial;
        double t;
        SDL_LockMutex(g_gs_mu);
        while (g_gs_serial == seen) SDL_CondWait(g_gs_cv, g_gs_mu);
        slot = g_gs_new;
        serial = g_gs_serial;
        g_gs_busy = slot;
        tex = g_gs[slot].tex; w = g_gs[slot].w; h = g_gs[slot].h; wide = g_gs[slot].wide;
        fence = g_gs[slot].fence;
        SDL_UnlockMutex(g_gs_mu);
        if (serial - seen > 1) g_ps.dropped += serial - seen - 1;
        seen = serial;

        if (fence) glWaitSync(fence, 0, GL_TIMEOUT_IGNORED);
        /* Attached afresh each time: what one context did to a shared texture
         * is only promised to the other once it binds it again. */
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[slot]);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        SDL_GL_GetDrawableSize(g_window, &ww, &wh);
        pres_clear();
        if (ww > 0 && wh > 0 && w > 0 && h > 0) {
            fw = w; fh = h;
            if (wide) { fw = (int)(nv2a_wide_aspect() * 900.0f + 0.5f); fh = 900; }
            dw = ww; dh = wh;
            if ((long)ww * fh > (long)wh * fw) dw = (int)((long)wh * fw / fh);
            else                               dh = (int)((long)ww * fh / fw);
            glBlitFramebuffer(0, 0, w, h,
                              (ww - dw) / 2, (wh - dh) / 2, (ww - dw) / 2 + dw, (wh - dh) / 2 + dh,
                              GL_COLOR_BUFFER_BIT, GL_LINEAR);
            if (told < 3) {
                told++;
                fprintf(stderr, "[WIN] presented #%d: %dx%d frame into %dx%d (%dx%d), from the "
                        "presenter\n", told, w, h, ww, wh, dw, dh);
            }
        }
        SDL_GL_SwapWindow(g_window);
        glFinish();                     /* the swap done: when the display took it */
        t = mono_now();
        SDL_LockMutex(g_gs_mu);
        g_gs_busy = -1;
        SDL_UnlockMutex(g_gs_mu);

        if (g_ps.last > 0.0) g_ps.on[ps_bucket(t - g_ps.last)]++;
        g_ps.last = t;
        g_ps.shown++;
        g_vs_last = t;
        if (t >= report) {
            uint64_t on[5];
            int i;
            for (i = 0; i < 5; i++) { on[i] = g_ps.on[i] - on0[i]; on0[i] = g_ps.on[i]; }
            ps_line("the last 10 s", on, g_ps.shown - shown0, g_ps.dropped - dropped0);
            shown0 = g_ps.shown; dropped0 = g_ps.dropped;
            report = t + 10.0;
        }
    }
    return 0;
}

/* On the GL thread, with its context current, once the window is open. */
static void pres_start(void)
{
    const char *e = getenv("RECOMP_GL_PRESENTER");
    SDL_DisplayMode m;
#if defined(_WIN32)
    int want = !(e && e[0] == '0');
#else
    int want = e && e[0] == '1';
#endif
    if (!want) return;
    if (SDL_GetWindowDisplayMode(g_window, &m) == 0) g_vs_hz = m.refresh_rate;
#if defined(_WIN32)
    dwm_open();
#endif
    g_gs_mu = SDL_CreateMutex();
    g_gs_cv = SDL_CreateCond();
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    g_pres_ctx = SDL_GL_CreateContext(g_window);         /* current on this thread for now */
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    SDL_GL_MakeCurrent(g_window, g_sdl_ctx);
    if (!g_gs_mu || !g_gs_cv || !g_pres_ctx) {
        fprintf(stderr, "[WIN] no presenter (%s): frames are shown at the flip\n", SDL_GetError());
        g_presenter = -1;
        return;
    }
    g_presenter = 1;
    if (!SDL_CreateThread(pres_main, "nv2a-present", NULL)) {
        fprintf(stderr, "[WIN] no presenter thread (%s): frames are shown at the flip\n",
                SDL_GetError());
        g_presenter = -1;
        return;
    }
    fprintf(stderr, "[WIN] frames are put on screen by a presenter thread, in step with the display\n");
}

/* The flip's half: copy the finished frame into a free slot and say so. */
static void sdl_hand_over(GLuint src_fbo, int sw, int sh)
{
    GLint rfb = 0, dfb = 0;
    GLboolean sc = glIsEnabled(GL_SCISSOR_TEST), cm[4];
    GlShown *s;
    int slot = 0, i;
    if (sw <= 0 || sh <= 0) return;
    SDL_LockMutex(g_gs_mu);
    for (i = 0; i < 3; i++)
        if (i != g_gs_new && i != g_gs_busy) { slot = i; break; }
    SDL_UnlockMutex(g_gs_mu);
    s = &g_gs[slot];
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
    if (!s->tex || s->w != sw || s->h != sh) {
        GLint act = 0, prev = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &act);
        glActiveTexture(GL_TEXTURE7);           /* a unit no shader samples */
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
        if (!s->tex) glGenTextures(1, &s->tex);
        glBindTexture(GL_TEXTURE_2D, s->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
        glActiveTexture((GLenum)act);
        if (!s->fbo) glGenFramebuffers(1, &s->fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s->fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
        s->w = sw;
        s->h = sh;
        s->gen++;
    }
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s->fbo);
    glBlitFramebuffer(0, 0, sw, sh, 0, 0, sw, sh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (s->fence) glDeleteSync(s->fence);
    s->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();                                  /* the copy and the fence on their way */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)dfb);
    if (sc) glEnable(GL_SCISSOR_TEST);
    glColorMask(cm[0], cm[1], cm[2], cm[3]);
    SDL_LockMutex(g_gs_mu);
    s->wide = nv2a_wide_scene();
    g_gs_new = slot;
    g_gs_serial++;
    SDL_CondSignal(g_gs_cv);
    SDL_UnlockMutex(g_gs_mu);
}

/* For frame_pace: the period to pace the game at when the display can set
 * it -- a whole number of its refreshes within 3% of the game's own period --
 * and a refresh to line the frames up against. 0 when it cannot. */
static double pres_lock(double base, double *ref, double *refresh)
{
    double p = g_vs_period, last = g_vs_last, n;
    if (g_presenter != 1 || base <= 0.0) return 0.0;
#if defined(_WIN32)
    { double dl, dp;
      if (dwm_vblank(&dl, &dp)) { p = dp; last = dl; } }
#endif
    if (p <= 0.0) return 0.0;
    n = floor(base / p + 0.5);
    if (n < 1.0 || fabs(n * p - base) > base * 0.03) return 0.0;
    *ref = last;
    *refresh = p;
    return n * p;
}
#endif /* !NV2A_GLES */

static void sdl_pump(void)
{
    SDL_Event ev;
    if (g_sdl_want_wide >= 0) {
        int want = g_sdl_want_wide;
        g_sdl_want_wide = -1;
        if (want != nv2a_widescreen()) sdl_set_picture(want ? 1 : 0);
    }
    if (g_sdl_want_rcam >= 0) {
        int want = g_sdl_want_rcam;
        g_sdl_want_rcam = -1;
        if (want != nv_window_pref_rstick_camera()) sdl_toggle_rstick_camera();
    }
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            fprintf(stderr, "  [GL] window closed; stopping\n");
            nv2a_gl_report();
            fflush(stderr);
            exit(0);
        case SDL_KEYDOWN:
            if (!ev.key.repeat) {
                SDL_Scancode k = ev.key.keysym.scancode;
                if (k == SDL_SCANCODE_F11
                 || (k == SDL_SCANCODE_RETURN && (ev.key.keysym.mod & KMOD_ALT))) {
                    sdl_fullscreen_by_player();
                    break;
                }
                if (k == SDL_SCANCODE_F10) { sdl_set_picture((sdl_picture_now() + 1) % 3); break; }
                if (k == SDL_SCANCODE_F9)  { sdl_choose_scale(); break; }
                if (k == SDL_SCANCODE_F8)  { sdl_choose_aa(); break; }
                if (k == SDL_SCANCODE_F7)  { sdl_toggle_rstick_camera(); break; }
            }
            sdl_key(ev.key.keysym.scancode, 1);
            break;
        case SDL_KEYUP:
            sdl_key(ev.key.keysym.scancode, 0);
            break;
        case SDL_WINDOWEVENT:
            /* Losing focus with a key held never delivers its key-up. */
            if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                int i;
                g_sdl_kb_buttons = 0;
                for (i = 0; i < 8; i++) g_sdl_kb_analog[i] = 0;
            }
#if SDL_VERSION_ATLEAST(2, 0, 18)
            /* 21:9 is the screen's own shape: moved to another screen, the
             * window takes that one's. */
            if (ev.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED && sdl_picture_now() == 2
                && !getenv("RECOMP_WIDE_ASPECT")) {
                nv2a_set_picture(2, sdl_screen_aspect(), 0);
                fprintf(stderr, "[WIN] another screen: picture 21:9 at %.3f:1\n", (double)nv2a_wide_aspect());
            }
#endif
            break;
#if defined(_WIN32)
        case SDL_SYSWMEVENT:
            /* The menu bar: a command from a menu has 0 in the high word. */
            if (ev.syswm.msg && ev.syswm.msg->subsystem == SDL_SYSWM_WINDOWS
                && ev.syswm.msg->msg.win.msg == WM_COMMAND
                && HIWORD(ev.syswm.msg->msg.win.wParam) == 0)
                sdl_menu_command(LOWORD(ev.syswm.msg->msg.win.wParam));
            break;
#endif
        default:
            break;
        }
    }
}

#else

static int gl_init(uint32_t w, uint32_t h)
{
    static const EGLint cfga[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_NONE
    };
    static const EGLint pba[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    static const EGLint ctxa[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint n, maj, min;

    g_dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,
                                  EGL_DEFAULT_DISPLAY, NULL);
    if (g_dpy == EGL_NO_DISPLAY || !eglInitialize(g_dpy, &maj, &min)) {
        fprintf(stderr, "  [GL] no EGL display; GL backend disabled\n");
        return 0;
    }
    eglBindAPI(EGL_OPENGL_API);
    if (!eglChooseConfig(g_dpy, cfga, &cfg, 1, &n) || n < 1) {
        fprintf(stderr, "  [GL] no usable EGL config\n");
        return 0;
    }
    g_egl_surf = eglCreatePbufferSurface(g_dpy, cfg, pba);
    g_ctx  = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctxa);
    if (g_ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "  [GL] cannot create a 3.3 core context (0x%x)\n",
                eglGetError());
        return 0;
    }
    eglMakeCurrent(g_dpy, g_egl_surf, g_egl_surf, g_ctx);

    fprintf(stderr, "  [GL] %s | %s | GLSL %s\n",
            (const char *)glGetString(GL_VERSION),
            (const char *)glGetString(GL_RENDERER),
            (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));

    return gl_init_common(w, h);
}

#endif /* NV2A_GL_USE_SDL */

/* Which contiguous allocation holds guest address va now (0: none, or not
 * in the window). */
static uint32_t contig_serial(uint32_t va)
{
    return va - XBOX_CONTIG_BASE < XBOX_CONTIG_SIZE ? xbox_ContiguousBlockSerial(va) : 0;
}

/* The same for a surface offset. Surfaces are physical offsets; the window
 * maps physical P at XBOX_CONTIG_BASE + P. */
static uint32_t surf_serial(uint32_t offset)
{
    return contig_serial(XBOX_CONTIG_BASE + (offset & 0x0FFFFFFFu));
}

/* Make the framebuffer for `offset` current, creating it if this is the first
 * time the title has drawn there. */
static void surface_bind(uint32_t offset, uint32_t w, uint32_t h)
{
    int i, slot = -1;

    if (!w || !h || w > 4096 || h > 4096) return;

    for (i = 0; i < SURF_CACHE; i++) {
        if (g_surf[i].used && g_surf[i].offset == offset
         && g_surf[i].w == w && g_surf[i].h == h) { slot = i; break; }
        if (!g_surf[i].used && slot < 0) slot = i;
    }
    if (slot < 0) {
        /* Full: evict the least recently bound, never the current one. */
        uint32_t oldest = 0xFFFFFFFFu;
        for (i = 0; i < SURF_CACHE; i++)
            if (i != g_cur_surf && g_surf[i].last_use < oldest) {
                oldest = g_surf[i].last_use; slot = i;
            }
        if (slot < 0) slot = 0;
        fprintf(stderr, "  [GL] surface cache full: %08X %ux%u evicted for %08X %ux%u\n",
                g_surf[slot].offset, g_surf[slot].w, g_surf[slot].h, offset, w, h);
    }
    g_surf[slot].last_use = ++g_surf_clock;
    g_surf[slot].serial = surf_serial(offset);

    if (!g_surf[slot].used || g_surf[slot].offset != offset
     || g_surf[slot].w != w || g_surf[slot].h != h) {
        /* A colour buffer and a depth/stencil one -- with a stencil: the
         * title keeps its shadow counts there -- at the raster's size. */
        uint32_t rt;
        if (g_surf[slot].rt) {
            G->target_free(g_surf[slot].rt);
            g_surf[slot].rt = 0;
            g_surf[slot].used = 0;
            if (g_cur_surf == slot) { g_cur_surf = -1; g_cur_rt = 0; }
        }
        rt = G->target_new(w * gl_scale(), h * gl_scale());
        if (!rt) {
            fprintf(stderr, "  [GL] no render target at %ux%u\n", w, h);
            g_surf_lost = 1;
            return;
        }
        g_surf[slot].rt = rt;
        g_surf[slot].used = 1;
        g_surf[slot].offset = offset;
        g_surf[slot].w = w; g_surf[slot].h = h;
        fprintf(stderr, "  [GL] surface %08X: new %ux%u render target\n",
                offset, w, h);
    }

    if (g_cur_surf != slot) {
        G->target_bind(g_surf[slot].rt);
        g_cur_surf = slot;
    }
    g_cur_rt = g_surf[slot].rt;
    g_surf_lost = 0;
    g_fbo_w = w; g_fbo_h = h;
    g_fbo_pw = w * gl_scale(); g_fbo_ph = h * gl_scale();

    if (g_readback_bytes < w * h * 4) {
        free(g_readback);
        g_readback_bytes = w * h * 4;
        g_readback = (uint8_t *)malloc(g_readback_bytes);
    }
}

/* Is this address one of the surfaces we render into? A title that samples a
 * surface it has just drawn is doing render-to-texture, and the answer has to
 * be the live framebuffer rather than whatever stale bytes sit at that
 * address in guest memory. */
/* Which cached surface a texture at `offset` is, or -1. With several at one
 * address, the one whose size matches the texture's, else the one drawn into
 * most recently -- that is the one whose pixels the memory holds. The current
 * surface counts only when `allow_cur` is set: sampling it while drawing into
 * it needs a copy (see surface_texture_for). */
static int surface_find(uint32_t offset, uint32_t w, uint32_t h, int allow_cur)
{
    int i, best = -1;
    for (i = 0; i < SURF_CACHE; i++) {
        if (!g_surf[i].used || g_surf[i].offset != offset) continue;
        if (i == g_cur_surf && !allow_cur) continue;
        /* The title freed this target and the memory holds something else
         * now (contiguous memory is reused since the allocator frees): what
         * is there is a texture, not the picture drawn into it last stage. */
        if (g_surf[i].serial != surf_serial(offset)) continue;
        if (w && h && g_surf[i].w == w && g_surf[i].h == h) return i;
        if (best < 0 || g_surf[i].last_draw > g_surf[best].last_draw) best = i;
    }
    return best;
}

static uint32_t surface_texture(uint32_t offset, uint32_t w, uint32_t h)
{
    int i = surface_find(offset, w, h, 0);
    return i >= 0 ? G->target_texture(g_surf[i].rt) : 0;
}

/* ---- shaders ------------------------------------------------------------ */

/* RECOMP_GL_SOLID: paint every fragment opaque magenta and ignore depth.
 * When a frame comes back empty this says, in one run, whether the geometry
 * is reaching the surface at all -- which is a different bug from it being
 * shaded to black or hidden behind the depth test. The shader is
 * nv2a_psh_emit_solid(); this is its buffer. */
static char *frag_solid(void)
{
    static char *src;
    if (!src) {
        src = (char *)malloc(16 * 1024);
        if (src && !nv2a_psh_emit_solid(src, 16 * 1024, G->lang == NV2A_LANG_MSL)) {
            free(src); src = NULL;
        }
    }
    return src;
}

/* A #define spliced into generated source: after the #version line, which
 * GLSL requires to come first, or at the very top of Metal source. Returns a
 * new string, or NULL to keep the original. */
static char *splice_define(const char *src, const char *def)
{
    size_t head = 0, len, dl = strlen(def);
    char *out;
    if (!src) return NULL;
    if (strncmp(src, "#version", 8) == 0) {
        const char *nl = strchr(src, '\n');
        if (!nl) return NULL;
        head = (size_t)(nl - src) + 1;
    }
    len = strlen(src);
    out = (char *)malloc(len + dl + 1);
    if (!out) return NULL;
    memcpy(out, src, head);
    memcpy(out + head, def, dl);
    memcpy(out + head + dl, src + head, len - head + 1);
    return out;
}

/* Build a program from generated source, with the bring-up switches that
 * work by editing it.
 *
 * RECOMP_GL_ZVIEW turns every fragment shader into a depth view (the
 * emitter carries the epilogue behind an #ifdef; this is only the switch).
 * RECOMP_GL_Z01 takes the program's depth as the 0..1 it already is, rather
 * than dividing it by a clip range it never used. */
static uint32_t build_program(const char *vsrc, const char *fsrc, const char *what)
{
    static int zview = -1, z01 = -1;
    char *v2 = NULL, *f2 = NULL;
    uint32_t prog;
    if (zview < 0) zview = getenv("RECOMP_GL_ZVIEW") ? 1 : 0;
    if (z01 < 0)   z01   = getenv("RECOMP_GL_Z01") ? 1 : 0;
    if (zview) f2 = splice_define(fsrc, "#define NV2A_ZVIEW_ON 1\n");
    if (z01)   v2 = splice_define(vsrc, "#define NV2A_Z01 1\n");
    { double t0 = mono_now(), dt;
      prog = G->prog_new(v2 ? v2 : vsrc, f2 ? f2 : fsrc, what);
      dt = mono_now() - t0;
      g_hf.compiles++; g_hf.compile_s += dt;
      if (dt > g_hf.compile_worst) g_hf.compile_worst = dt;
      g_ht.compiles++; g_ht.compile_s += dt;
      if (dt > g_ht.compile_worst) g_ht.compile_worst = dt; }
    free(v2); free(f2);
    return prog;
}

/* Translate the current vertex program and link it against the fragment
 * shader, caching on the microcode's hash: a title re-uploads the same
 * program constantly, and compiling per draw would dominate the frame. */
/* Which texture stages this draw actually has bound, and whether each one's
 * alpha is all it carries. The combiner shader is generated against that, so
 * a stage coming and going produces a different shader rather than a sampler
 * reading from nothing. */
/* Why a stage was judged not to have a texture. A surface that renders flat
 * and untextured looks the same whatever the reason, and the reasons are
 * different bugs: the title disabling the stage, an offset that never
 * arrived, or a size we failed to derive from the format register. */
static uint32_t g_tex_dead[4][3];   /* [stage][0=disabled 1=no offset 2=no size] */

static void psh_sync_textures(void)
{
    int i;
    for (i = 0; i < 4; i++) {
        const TexStage *t = &S.tex[i];
        int live = (t->enabled || (i == 0 && !t->control0))
                && t->offset && t->width && t->height;
        if (!live) {
            if (!(t->enabled || (i == 0 && !t->control0))) g_tex_dead[i][0]++;
            else if (!t->offset)                          g_tex_dead[i][1]++;
            else                                          g_tex_dead[i][2]++;
        }
        /* The stage's texture shader mode decides whether it is sampled at
         * all: NONE means the sampler is never touched, whatever is bound. */
        { uint32_t mode = (S.shader_stage_prog >> (5 * i)) & 31u;
          static int nobump = -1;
          if (nobump < 0) nobump = getenv("RECOMP_GL_NOBUMP") ? 1 : 0;
          /* RECOMP_GL_NOBUMP=1: sample a BUMPENVMAP stage as plain 2D,
           * the way every stage was sampled before it was implemented. */
          if (nobump && (mode == 6 || mode == 7)) mode = 1;
          S.psh.tex_mode[i] = (uint8_t)mode;
          /* Which earlier stage a BUMPENVMAP stage takes its (du, dv) from:
           * stage 1 always from 0; 2 and 3 from SET_SHADER_OTHER_STAGE_INPUT
           * (xemu psh.c, input_tex). */
          S.psh.bump_src[i] = (uint8_t)(i == 2 ? ((S.shader_other_input >> 16) & 1u)
                                      : i == 3 ? ((S.shader_other_input >> 20) & 3u) : 0u);
          if (S.psh.bump_src[i] >= i) S.psh.bump_src[i] = 0;
          S.psh.tex_signed[i] = 0;   /* set below for bump sources */
          S.psh.clip_cmp[i] = (uint8_t)((S.shader_clip_mode >> (4 * i)) & 15u);
          if (mode == 0 || mode == 4 || mode == 5) live = 0; }
        S.psh.tex_bound[i] = (uint8_t)(live ? 1 : 0);
        /* A8 and the swizzled A8: colour comes back as zero from GL and as
         * one from the console. */
        S.psh.tex_alpha_only[i] = (uint8_t)((t->color == 0x19 || t->color == 0x1F)
                                            ? 1 : 0);
    }
    /* Keep the bump fields out of the program key unless a stage uses them:
     * a signed bit on a texture nobody bumps from changes nothing. */
    /* A bump stage's (du, dv) are signed bytes whatever the title's sign
     * bits say (xemu reads them as two's complement unless the texture is
     * already signed). A source decoded from guest memory is uploaded with
     * those two bytes offset by 0x80 (g_upload_bias_bg) so that filtering
     * interpolates them as numbers rather than as bit patterns -- two's
     * complement read unsigned jumps from 255 to 0 at every zero crossing,
     * and a linear filter turns each crossing into a spike. A surface used
     * as a source cannot be re-encoded and is read raw.
     * tex_signed: 0x10 = offset-binary source, 0x20 = raw two's complement. */
    { uint8_t need[4] = { 0, 0, 0, 0 }; int k;
      for (k = 1; k < 4; k++)
          if (S.psh.tex_mode[k] == 6 || S.psh.tex_mode[k] == 7) need[S.psh.bump_src[k]] = 1;
          else S.psh.bump_src[k] = 0;
      S.psh.bump_src[0] = 0;
      for (k = 0; k < 4; k++)
          S.psh.tex_signed[k] = !need[k] ? 0
              : surface_find(S.tex[k].offset, S.tex[k].width, S.tex[k].height, 1) >= 0
                ? 0x20 : 0x10; }
    S.psh.alpha_test = (uint8_t)(S.alpha_test ? 1 : 0);
    S.psh.alpha_func = (uint32_t)S.alpha_func;
    S.psh.alpha_ref  = S.alpha_ref;
    /* RECOMP_GL_WBUFFER=0 ignores the register (the old behaviour); =1
     * forces it on; unset honours what the title wrote to CONTROL0. */
    { static int wb = -2;
      if (wb == -2) { const char *v = getenv("RECOMP_GL_WBUFFER");
                      wb = v ? atoi(v) : -1; }
      S.psh.w_buffer = (uint8_t)(wb >= 0 ? wb
                                         : ((S.control0 >> 16) & 1u)); }
}

/*
 * Sticky vertex attributes (RECOMP_GL_STICKY_ATTRS=0 turns this off).
 *
 * The hardware keeps one current value per vertex attribute, and fetching a
 * vertex from an array writes into it: after a draw, every attribute that
 * came from an array is left holding the last vertex's value, and the next
 * draw that leaves that attribute's array disabled reads it. xemu models it
 * the same way (pgraph_gl_bind_vertex_attributes: the "provoking" element of
 * every enabled array is copied into the attribute's inline value).
 *
 * This used to keep only what the title set explicitly with the
 * SET_VERTEX_DATA methods, which this title never does for the normal -- and
 * the pause map's counter box is five lit draws with no normal array of their
 * own. On the console they are lit with the normal the map's own geometry
 * left behind, one that faces the viewer, and come out in the colours the
 * title gives the light for each of them; here they got a normal of nothing
 * and came out black.
 */
static int sticky_attrs(void)
{
    static int on = -1;
    if (on < 0) { const char *v = getenv("RECOMP_GL_STICKY_ATTRS");
                  on = (v && v[0] == '0') ? 0 : 1; }
    return on;
}

/* The combiner state's half of the program key, hashed again only when it
 * changed: comparing it costs a few nanoseconds, hashing it a byte at a time
 * a few hundred. It leaves out what the shader reads as uniforms
 * (nv2a_psh_key), so a fade through a constant keeps its program. */
static uint32_t psh_hash(void)
{
    static Nv2aPshState last;
    static uint32_t h;
    static int valid;
    Nv2aPshState key;
    nv2a_psh_key(&key, &S.psh);
    if (!valid || memcmp(&last, &key, sizeof last)) {
        memcpy(&last, &key, sizeof last);
        h = fnv(&key, sizeof key);
        valid = 1;
    }
    return h;
}

/* What a program was made from, for the shader cache's states: the combiner
 * state as the key has it (nv2a_psh_key), without the constants the shader
 * reads as uniforms -- one state for every shade of a fade. */
static void sc_state_note(uint32_t hash, int kind, const void *vdata, uint32_t vlen)
{
    Nv2aPshState key;
    nv2a_psh_key(&key, &S.psh);
    nv2a_sc_state_note(hash, kind, vdata, vlen, &key, (uint32_t)sizeof key);
}

/* The combiner constants (nv2a_psh_consts.h), sent to the program when they
 * changed since it last had them: a fade changes one a frame, and most draws
 * none. */
static void psh_consts_send(int slot)
{
    uint32_t w[NV2A_PSH_NCONSTS];
    float v[NV2A_PSH_NCONSTS * 4];
    if (!uses(slot, NV2A_U_PSHC)) return;
    nv2a_psh_const_words(w, &S.psh);
    if (g_last.psh_ok && !memcmp(w, g_last.psh_c, sizeof w)) return;
    nv2a_psh_const_values(v, w);
    G->uniform_f(NV2A_U_PSHC, v, NV2A_PSH_NCONSTS);
    memcpy(g_last.psh_c, w, sizeof w);
    g_last.psh_ok = 1;
}

/* Which inputs a fixed-function batch actually has.
 *
 * The emitted shader declares only the arrays the batch supplies; an
 * attribute the title left as a set constant still reads correctly through
 * fetch(), which copies the constant into the packed vertex, so "has an
 * array" and "has a value" are the same question here. Position is always
 * present -- do_draw() rejects a batch without it before this is reached. */
/* RECOMP_GL_TEXGEN=0: no generated coordinates, as before; =ab: every other
 * displayed frame without them, for a before-and-after from one run. */
static int texgen_off(void)
{
    static int off = -1;
    if (off < 0) { const char *v = getenv("RECOMP_GL_TEXGEN");
                   off = !v ? 0 : *v == '0' ? 1 : (*v == 'a' || *v == 'A') ? 2 : 0; }
    return off == 2 ? (int)(S.frames & 1u) : off;
}

/*
 * Texture coordinate generation, for the fixed-function transform
 * (NV097_SET_TEXGEN_S..Q): which components of which stage the T&L unit makes
 * itself, and what that needs read.
 *
 * The normal-based modes are made (see emit_ff). The two linear modes would
 * need the planes as uniforms of their own and this title has not been seen
 * to ask for them: their components stay the vertex's own, and the first
 * batch that asks says so. RECOMP_GL_TEXGEN=0 makes none, as before.
 */
static void texgen_describe(Nv2aVshFixed *f)
{
    int s, c, nrm = 0;
    if (texgen_off()) return;
    for (s = 0; s < 4; s++) {
        int any = 0;
        for (c = 0; c < 4; c++) {
            uint8_t code = NV2A_TEXGEN_OFF;
            switch (S.texgen[s][c]) {
            case 0x2402: if (c < 2) code = NV2A_TEXGEN_SPHERE_MAP; break;
            case 0x8511: if (c < 3) code = NV2A_TEXGEN_NORMAL_MAP; break;
            case 0x8512: if (c < 3) code = NV2A_TEXGEN_REFLECTION_MAP; break;
            case 0: break;
            default: {
                static int said;
                if (said++ < 4)
                    fprintf(stderr, "[GL] texture coordinates generated by mode %04X (stage %d, %c) "
                            "are not made: the vertex's own are used\n", S.texgen[s][c], s, "STRQ"[c]);
                break; }
            }
            f->texgen[s][c] = code;
            if (code) { any = 1; nrm = 1; }
        }
        /* The components left to the vertex read its coordinate -- the
         * attribute's latched value when the batch has no array of them. */
        if (any) f->inputs_read |= 1u << (9 + s);
    }
    if (nrm && !f->lit) {
        if (S.attr[2].size || sticky_attrs()) { f->inputs_read |= 1u << 2; f->has_normal = 1; }
        f->tg_normalize = S.normalization ? 1 : 0;
    }
}

static void ff_describe(Nv2aVshFixed *f)
{
    int i;
    memset(f, 0, sizeof *f);
    f->inputs_read = 1u;                       /* v0, position */
    if (S.attr[3].size) { f->inputs_read |= 1u << 3;  f->has_diffuse = 1; }
    if (S.attr[4].size) { f->inputs_read |= 1u << 4;  f->has_specular = 1; }
    for (i = 0; i < 4; i++)
        if (S.attr[9 + i].size) f->inputs_read |= 1u << (9 + i);
    f->tex_matrix = (uint8_t)(S.ff_texmat_enable & 0xF);
    /* How the fog distance is made. Part of the shape whether or not fog is
     * on: the title switches fog on and off between neighbouring batches,
     * and with the enable left to a uniform those share one program. */
    f->fog_gen = (uint8_t)(S.fog_gen_mode <= 6 ? S.fog_gen_mode : 0xFF);
    if (f->fog_gen == 6 && S.attr[5].size) f->inputs_read |= 1u << 5;
    f->lit = (uint8_t)(S.ff_lighting ? 1 : 0);
    /* RECOMP_GL_FF_NOLIGHT=1: the old way -- the stream colour through, or
     * white -- for A/B. */
    { static int off = -1;
      if (off < 0) off = getenv("RECOMP_GL_FF_NOLIGHT") ? 1 : 0;
      if (off) f->lit = 0; }
    if (f->lit) {
        /* Lit, the normal is always read: from its array when the batch has
         * one, otherwise the attribute's current value (see sticky_attrs). */
        if (S.attr[2].size || sticky_attrs()) {
            f->inputs_read |= 1u << 2; f->has_normal = 1;
        }
        f->colmat = (uint8_t)(S.color_material & 0xFF);
        f->lflags = (uint8_t)(((S.light_control & 1u) ? 1 : 0)
                            | ((S.light_control & (1u << 16)) ? 2 : 0)
                            | (S.specular_enable ? 4 : 0)
                            | (S.normalization ? 8 : 0));
        f->light_mask = (uint16_t)(S.light_enable_mask & 0xFFFFu);
    }
    texgen_describe(f);
}


/*
 * RECOMP_GL_DUMP_MSL=<dir>: the Metal translation of every shader this title
 * uses, written out beside the GLSL one that is actually being run.
 *
 * The Metal backend has to be written blind -- Metal cannot be compiled on the
 * machine this is developed on -- so the translator's output cannot be checked
 * here at all. It can be checked on the target: emit the Metal for every
 * distinct program the title compiles, carry the files across, and hand them to
 * the real Metal compiler. That finds every translation error before a single
 * line of backend exists, and names the program that has it.
 */
static void dump_msl_pair(uint32_t hash, const char *vsrc, const char *fsrc)
{
    static const char *dir = (const char *)1;
    char nm[512];
    FILE *f;
    if (dir == (const char *)1) dir = getenv("RECOMP_GL_DUMP_MSL");
    if (!dir || !vsrc || !fsrc) return;
    snprintf(nm, sizeof nm, "%s/msl_%08X.vert.metal", dir, hash);
    f = fopen(nm, "wb");
    if (f) { fputs(vsrc, f); fclose(f); }
    snprintf(nm, sizeof nm, "%s/msl_%08X.frag.metal", dir, hash);
    f = fopen(nm, "wb");
    if (f) { fputs(fsrc, f); fclose(f); }
}

/* The program being built right now, by its key in g_pcache -- for a backend
 * to file the blend recipes it draws it with under (the shader seed, at
 * shader_cache_start). 0 outside a build. */
static uint32_t g_seed_building;
uint32_t nv2a_seed_building_id(void);
uint32_t nv2a_seed_building_id(void) { return g_seed_building; }

static int ff_program_for_current(void)
{
    Nv2aVshFixed f;
    uint32_t hash, prog;
    char *vsrc, *fsrc;
    int slot, msl = (G->lang == NV2A_LANG_MSL);

    ff_describe(&f);
    psh_sync_textures();
    /* A namespace of its own: hashing the descriptor alone could collide with
     * a translated program's microcode hash, and the two are not
     * interchangeable. */
    hash = 0xF1EDF1EDu;   /* a namespace of its own */
    hash = hash * 16777619u + fnv(&f, sizeof f);
    hash = hash * 16777619u + psh_hash();
    { int hit = pcache_find(hash);
      if (hit >= 0) return hit; }
    slot = pcache_alloc(hash);

    vsrc = (char *)malloc(96 * 1024);
    if (!vsrc) return -1;
    if (!(msl ? nv2a_vsh_emit_ff_msl(&f, vsrc, 96 * 1024)
              : nv2a_vsh_emit_ff_glsl(&f, vsrc, 96 * 1024))) {
        free(vsrc); S.shader_fails++; return -1;
    }
    if (getenv("RECOMP_GL_DUMP_FF")) {
        static int shown;
        if (shown++ < 2)
            fprintf(stderr, "----- fixed-function GLSL (inputs %04X, "
                    "texmat %X) -----\n%s\n",
                    f.inputs_read, f.tex_matrix, vsrc);
    }
    fsrc = (char *)malloc(96 * 1024);
    if (!fsrc) { free(vsrc); return -1; }
    if (!(msl ? nv2a_psh_emit_msl(&S.psh, fsrc, 96 * 1024)
              : nv2a_psh_emit_glsl(&S.psh, fsrc, 96 * 1024))) {
        free(fsrc); free(vsrc); S.shader_fails++; return -1;
    }
    if (msl && getenv("RECOMP_GL_DUMP_MSL")) dump_msl_pair(hash, vsrc, fsrc);
    g_seed_building = hash;
    prog = build_program(vsrc, fsrc, "fixed-function transform + combiners");
    g_seed_building = 0;
    free(vsrc);
    free(fsrc);
    if (!prog) { S.shader_fails++; return -1; }
    /* What it was made from, for the next start, here or anywhere. */
    sc_state_note(hash, 2, &f, (uint32_t)sizeof f);
    if (g_pcache[slot].used && g_pcache[slot].prog) G->prog_free(g_pcache[slot].prog);
    free(g_pcache[slot].dis);
    memset(&g_pcache[slot], 0, sizeof g_pcache[slot]);
    g_pcache[slot].used = 1;
    g_pcache[slot].hash = hash;
    g_pcache[slot].last_draw = S.draws;
    g_pcache[slot].prog = prog;
    g_pcache[slot].inputs = f.inputs_read;
    g_pcache[slot].dis = (dump_slot() >= 0 || dump_big() > 0)
                       ? strdup("  (fixed-function transform, no microcode)\n")
                       : NULL;
    S.prog_switches++;
    return slot;
}

static int program_for_current(void)
{
    uint32_t hash, start;
    Nv2aVshProgram vp;
    char *vsrc, *fsrc;
    uint32_t prog;
    int slot, len, msl = (G->lang == NV2A_LANG_MSL);

    /*
     * Programs run from where NV097_SET_TRANSFORM_PROGRAM_START points, not
     * from slot zero.
     *
     * The 136 instruction slots are one shared pool. A title uploads several
     * programs into it and switches between them by moving the start address
     * -- which costs one method, where re-uploading would cost a hundred and
     * thirty. JSRF keeps a dozen in there at once.
     *
     * Decoding from slot zero regardless therefore runs ONE program for the
     * whole game: whichever happens to sit at the bottom of the pool. Draws
     * whose intended program is that one come out right, which is why the
     * logos and the menu text look correct, and every other draw is shaded by
     * a program written for different geometry. The scenery's program reads
     * its texture coordinates from v9 and v10; the program at slot zero reads
     * v2, which those vertices do not have, so every surface sampled one
     * texel and the city rendered as flat panels.
     *
     * The hash has to start there too, or the cache cannot tell two programs
     * apart either.
     */
    /*
     * A fixed-function batch runs no program at all.
     *
     * NV097_SET_TRANSFORM_EXECUTION_MODE selects between the hardware T&L
     * unit and the programmable one, and D3D8 puts every FVF draw through the
     * former. Decoding the pool regardless hands such a batch whatever
     * program was loaded last, which is how the scenery ended up being drawn
     * by the driver's pre-transformed-vertex passthrough.
     */
    if (S.xf_mode == NV2A_XF_MODE_FIXED) {
        static int off = -1;
        if (off < 0) off = getenv("RECOMP_GL_NO_FIXEDFUNC") ? 1 : 0;
        if (!off) return ff_program_for_current();
    }

    start = S.prog_start < NV2A_VSH_MAX_INSNS ? S.prog_start : 0;
    /* Decoding the program and hashing it was most of a draw's own cost here
     * (a hundred instructions, and two kilobytes hashed a byte at a time),
     * for a program that is nearly always the one the last draw used. The
     * pool's write counter and the start address say whether it is; the
     * decode is redone only if this turns out to be a program the cache has
     * never seen. */
    { static uint32_t m_gen, m_start, m_hash; static int m_len, m_ok;
      int decoded = 0;
      if (m_gen != g_vsh_gen || m_start != start) {
          len = nv2a_vsh_decode(S.prog + start * 4,
                                (int)(NV2A_VSH_MAX_INSNS - start), &vp);
          m_gen = g_vsh_gen; m_start = start; m_len = len;
          /* No write to oPos means every vertex lands on the origin. That is
           * not a program the title expects to draw with -- it is the state
           * before one has been uploaded -- and compiling it wastes a slot
           * and a shader. */
          m_ok = len > 0 && (vp.outputs_written & 1u);
          m_hash = m_ok ? fnv(S.prog + start * 4, (size_t)len * 16) : 0;
          decoded = 1;
      }
      if (!m_ok) { S.skipped_no_prog++; return -1; }
      len = m_len;

      psh_sync_textures();
      /* One key for both halves: a title reuses a vertex program with
       * several combiner setups and vice versa, and caching on either alone
       * hands back a shader that is half right. */
      hash = m_hash * 16777619u + psh_hash();
      { int hit = pcache_find(hash);
        if (hit >= 0) return hit; }
      if (!decoded)
          len = nv2a_vsh_decode(S.prog + start * 4,
                                (int)(NV2A_VSH_MAX_INSNS - start), &vp); }
    slot = pcache_alloc(hash);

    vsrc = (char *)malloc(96 * 1024);
    if (!vsrc) return -1;
    if (!(msl ? nv2a_vsh_emit_msl(&vp, vsrc, 96 * 1024)
              : nv2a_vsh_emit_glsl(&vp, vsrc, 96 * 1024))) {
        free(vsrc); S.shader_fails++; return -1;
    }
    if (getenv("RECOMP_GL_DUMP_RAW")) {
        /* The microcode itself, so a decoder can be checked against it
         * offline instead of through a rebuild each time. */
        static int dumped;
        if (dumped < 8) {
            char nm[64];
            FILE *f;
            snprintf(nm, sizeof nm, "vsh_%08X.bin", hash);
            f = fopen(nm, "wb");
            if (f) { fwrite(S.prog + start * 4, 1, (size_t)len * 16, f); fclose(f); }
            fprintf(stderr, "  [GL] wrote %s (%d slots, start=%u load=%u)\n",
                    nm, len, S.prog_start, S.prog_load);
            dumped++;
        }
    }
    if (dump_slot() >= 0 || dump_big() > 0) {
        char dis[16384];
        free(g_pcache[slot].dis);
        g_pcache[slot].dis = nv2a_vsh_disasm(&vp, dis, sizeof dis)
                           ? strdup(dis) : NULL;
    }
    if (getenv("RECOMP_GL_DUMP_VSH") || (dump_slot() >= 0 && slot == dump_slot())) {
        char dis[16384];
        if (nv2a_vsh_disasm(&vp, dis, sizeof dis))
            fprintf(stderr, "----- NV2A program (%d slots, hash %08X) -----\n%s",
                    len, hash, dis);
        fprintf(stderr, "----- %s -----\n%s\n", msl ? "MSL" : "GLSL", vsrc);
    }

    fsrc = (char *)malloc(96 * 1024);
    if (!fsrc) { free(vsrc); return -1; }
    if (!(msl ? nv2a_psh_emit_msl(&S.psh, fsrc, 96 * 1024)
              : nv2a_psh_emit_glsl(&S.psh, fsrc, 96 * 1024))) {
        free(fsrc); free(vsrc); S.shader_fails++; return -1;
    }
    /* RECOMP_GL_DUMP_MSL=<dir>: every program's Metal translation, from
     * either backend -- the OpenGL one translates it only to write it out. */
    if (getenv("RECOMP_GL_DUMP_MSL")) {
        if (msl) dump_msl_pair(hash, vsrc, fsrc);
        else {
            char *mv = (char *)malloc(96 * 1024), *mf = (char *)malloc(96 * 1024);
            if (mv && mf && nv2a_vsh_emit_msl(&vp, mv, 96 * 1024)
                         && nv2a_psh_emit_msl(&S.psh, mf, 96 * 1024))
                dump_msl_pair(hash, mv, mf);
            free(mv); free(mf);
        }
    }
    { static int solid = -1;
      if (solid < 0) solid = (getenv("RECOMP_GL_SOLID")
                           || getenv("RECOMP_GL_DRAWID")
                           || getenv("RECOMP_GL_TINT_XF")
                           || getenv("RECOMP_GL_TINT_GEQUAL")
                           || getenv("RECOMP_GL_TINTCLIP")) ? 1 : 0;
      if (solid) { free(fsrc); fsrc = NULL; } }
    /* RECOMP_GL_DUMP_PSH_3D=<n>: the generated combiner shader for the first
     * few programs compiled once the scene is up, with the raw register words
     * beside it. The logo phase compiles dozens of one-stage shaders that all
     * look the same and say nothing about why the streets are dark. */
    { static int psh_shots; static long at3d = -2;
      if (at3d == -2) { const char *v = getenv("RECOMP_GL_DUMP_PSH_3D");
                        at3d = v ? atol(v) : -1; }
      if (getenv("RECOMP_GL_DUMP_PSH")
       || (dump_slot() >= 0 && slot == dump_slot())
       || (at3d >= 0 && (long)S.draws_3d >= at3d && psh_shots < 4)) {
        int k;
        psh_shots++;
        fprintf(stderr, "----- combiners: %u stage(s), control %08X, "
                        "final %08X/%08X c0 %08X c1 %08X -----\n",
                (unsigned)(S.psh.control & 0xF), S.psh.control,
                S.psh.final_abcd, S.psh.final_efg, S.psh.final_c0, S.psh.final_c1);
        for (k = 0; k < (int)(S.psh.control & 0xF) && k < NV2A_PSH_STAGES; k++)
            fprintf(stderr, "  stage %d: rgb in %08X out %08X | "
                            "a in %08X out %08X | c0 %08X c1 %08X\n",
                    k, S.psh.rgb_icw[k], S.psh.rgb_ocw[k],
                    S.psh.alpha_icw[k], S.psh.alpha_ocw[k],
                    S.psh.c0[k], S.psh.c1[k]);
        fprintf(stderr, "  tex bound %d%d%d%d, alpha-only %d%d%d%d, "
                        "alpha test %d func %04X ref %.3f\n%s\n",
                S.psh.tex_bound[0], S.psh.tex_bound[1], S.psh.tex_bound[2],
                S.psh.tex_bound[3], S.psh.tex_alpha_only[0],
                S.psh.tex_alpha_only[1], S.psh.tex_alpha_only[2],
                S.psh.tex_alpha_only[3], S.psh.alpha_test, S.psh.alpha_func,
                S.psh.alpha_ref, fsrc ? fsrc : "(solid)");
      } }
    g_seed_building = fsrc ? hash : 0;
    prog = build_program(vsrc, fsrc ? fsrc : frag_solid(),
                         "translated vertex program + combiners");
    g_seed_building = 0;
    if (prog && fsrc)   /* what it was made from (a diagnostic's solid colour: not) */
        sc_state_note(hash, 1, S.prog + start * 4, (uint32_t)len * 16u);
    free(vsrc);
    free(fsrc);
    if (!prog) { S.shader_fails++; return -1; }

    if (g_pcache[slot].used && g_pcache[slot].prog)
        G->prog_free(g_pcache[slot].prog);
    g_pcache[slot].used   = 1;
    g_pcache[slot].hash   = hash;
    g_pcache[slot].last_draw = S.draws;
    g_pcache[slot].prog   = prog;
    g_pcache[slot].csync  = 0;     /* a new program has no constants yet */
    g_pcache[slot].inputs = vp.inputs_read;
    /* Which input feeds the address register.
     *
     * The bone index is not always in the same place. One family of this
     * title's programs holds it in v2 with the weight in v1; another holds it
     * in v1 with a normal in v2, and a diagnostic that assumed the first read
     * a unit normal as a bone index and reported a hundred vertices reaching
     * past the end of the constant file when nothing was wrong. So ask the
     * program: find the ARL, see which register it reads, and if that is a
     * temporary, find the instruction that last wrote it and take the input
     * IT read. One level is enough for every program here -- they all spell
     * it "multiply the input by a scale, then ARL". */
    { int q; g_pcache[slot].uses_a0 = 0; g_pcache[slot].a0_input = 0xFF;
      for (q = 0; q < vp.length; q++)
          if (vp.insns[q].rel_addr) { g_pcache[slot].uses_a0 = 1; break; }
      for (q = 0; q < vp.length; q++) {
          const Nv2aVshInsn *in = &vp.insns[q];
          if (in->mac != NV2A_MAC_ARL) continue;
          /* An input register's number is a single field in the
           * instruction, shared by every source that selects the input bank
           * -- src[].index is the register number only for temporaries. The
           * first version of this read src[0].index for an input and got 0
           * every time, which named the position attribute as the bone index
           * and made the position of every vertex look like a wild index. */
          if (in->src[0].type == NV2A_PARAM_V) {
              g_pcache[slot].a0_input = in->input_index;
          } else if (in->src[0].type == NV2A_PARAM_R) {
              int r = in->src[0].index, k, j;
              for (k = (int)q - 1; k >= 0; k--) {
                  if (!vp.insns[k].mac_mask || vp.insns[k].mac_temp != r)
                      continue;
                  for (j = 0; j < 3; j++)
                      if (vp.insns[k].src[j].type == NV2A_PARAM_V) {
                          g_pcache[slot].a0_input = vp.insns[k].input_index;
                          break;
                      }
                  break;
              }
          }
          break;
      } }
    S.prog_switches++;
    if (verbose())
        fprintf(stderr, "  [GL] compiled vertex program %08X (%d slots, "
                "inputs %04X)\n", hash, len, vp.inputs_read);
    return slot;
}

/* ---- textures ----------------------------------------------------------- */

static int fmt_is_dxt(uint32_t f) { return d3d8_format_dxt_block_bytes(f) != 0; }
static int fmt_is_swz(uint32_t f) { return d3d8_format_is_swizzled(f) != 0; }

/* Bytes per texel for the linear formats we decode. 0 = not understood. */
static uint32_t linear_bpp(uint32_t f)
{
    switch (f) {
    case 0x12: case 0x1E: case 0x3F: case 0x40: case 0x41: return 4;
    case 0x10: case 0x1C: case 0x11: case 0x1D: case 0x20: return 2;
    case 0x13: case 0x1F: return 1;
    default: return 0;
    }
}

static uint32_t swz_to_linear(uint32_t f)
{
    switch (f) {
    case 0x00: return 0x13; case 0x02: return 0x10; case 0x03: return 0x1C;
    case 0x04: return 0x1D; case 0x05: return 0x11; case 0x06: return 0x12;
    case 0x07: return 0x1E; case 0x19: return 0x1F; case 0x1A: return 0x20;
    default: return f;
    }
}

/* Inlined wherever it is called, so that a loop over one format (decode_rows)
 * gets that format's code alone. */
#if defined(_MSC_VER)
#define TEXEL_INLINE static __forceinline
#else
#define TEXEL_INLINE static inline __attribute__((always_inline))
#endif
TEXEL_INLINE uint32_t decode_texel(uint32_t f, const uint8_t *p)
{
    uint32_t v;
    switch (f) {
    case 0x12: return *(const uint32_t *)p;
    case 0x1E: return *(const uint32_t *)p | 0xFF000000u;
    case 0x3F: v = *(const uint32_t *)p;      /* A8B8G8R8 */
        return (v & 0xFF00FF00u) | ((v & 0xFF) << 16) | ((v >> 16) & 0xFF);
    case 0x40: v = *(const uint32_t *)p;      /* B8G8R8A8 */
        return (v >> 8) | ((v & 0xFF) << 24);
    case 0x41: v = *(const uint32_t *)p;      /* R8G8B8A8 */
        return ((v >> 8) & 0x00FFFFFFu) | ((v & 0xFF) << 24);
    case 0x10: case 0x1C: v = *(const uint16_t *)p;
        return ((f == 0x10 && (v & 0x8000)) || f == 0x1C ? 0xFF000000u : 0u)
             | (d3d8_expand_channel((v >> 10) & 0x1F, 5) << 16)
             | (d3d8_expand_channel((v >> 5) & 0x1F, 5) << 8)
             | d3d8_expand_channel(v & 0x1F, 5);
    case 0x11: v = *(const uint16_t *)p;
        return 0xFF000000u
             | (d3d8_expand_channel((v >> 11) & 0x1F, 5) << 16)
             | (d3d8_expand_channel((v >> 5) & 0x3F, 6) << 8)
             | d3d8_expand_channel(v & 0x1F, 5);
    case 0x1D: v = *(const uint16_t *)p;
        return (d3d8_expand_channel((v >> 12) & 0xF, 4) << 24)
             | (d3d8_expand_channel((v >> 8) & 0xF, 4) << 16)
             | (d3d8_expand_channel((v >> 4) & 0xF, 4) << 8)
             | d3d8_expand_channel(v & 0xF, 4);
    case 0x13: v = p[0]; return 0xFF000000u | (v << 16) | (v << 8) | v;
    case 0x1F: return ((uint32_t)p[0] << 24) | 0x00FFFFFFu;
    case 0x20: return ((uint32_t)p[1] << 24) | (p[0] << 16) | (p[0] << 8) | p[0];
    default: return 0;
    }
}

/*
 * How much of each uploaded texture is transparent, by source format.
 *
 * Turning alpha testing off brings back a whole glass panel that was
 * invisible with it on, so surfaces in this game really are being discarded
 * per fragment -- and a road deck whose texture decodes to zero alpha would
 * vanish completely while every bollard and bus standing on it stayed
 * exactly where it is. That is the picture.
 *
 * The classic way for this to happen is a format with no alpha channel
 * decoded as though it had one: X8R8G8B8 read as A8R8G8B8 hands back
 * whatever is in the unused byte, which is usually zero, and the whole
 * texture disappears. DXT1 has its own version -- a block whose first colour
 * is not greater than its second means the fourth index is transparent, and
 * a decoder that applies that rule where the title did not intend it punches
 * holes through solid ground.
 *
 * Reading the decoder cannot settle which, if either, is happening here.
 * Counting can: one pass over each texture as it is uploaded, grouped by the
 * format it came from. A format whose textures are reliably a few percent
 * transparent is doing its job. One that is reliably half transparent is not
 * a texture with holes in it, it is a decode that has lost the alpha
 * channel, and the format number says exactly which case to go and read.
 */
static void tex_alpha_census(const uint8_t *rgba, uint32_t w, uint32_t h,
                             uint32_t fmt, int is_dxt)
{
    enum { NF = 24 };
    static struct { uint32_t fmt; int dxt; uint64_t texels, clear, n; } f[NF];
    static int nf;
    static uint32_t uploads, last_report;
    size_t i, n = (size_t)w * h;
    uint64_t clear = 0;
    int k;

    { static int on = -1;
      if (on < 0) on = getenv("RECOMP_GL_TEXALPHA") ? 1 : 0;
      if (!on) return; }

    for (i = 0; i < n; i++) if (rgba[i * 4 + 3] < 16) clear++;

    for (k = 0; k < nf; k++) if (f[k].fmt == fmt) break;
    if (k == nf) { if (nf == NF) return; f[nf].fmt = fmt; f[nf].dxt = is_dxt; nf++; }
    f[k].texels += n; f[k].clear += clear; f[k].n++;

    if (++uploads - last_report < 48u) return;
    last_report = uploads;
    fprintf(stderr, "  [TEXA] transparent texels by source format, over %u "
                    "uploads:\n", uploads);
    for (k = 0; k < nf; k++)
        fprintf(stderr, "  [TEXA]   format 0x%02X%s  %llu texture(s), "
                        "%.1f%% of texels clear\n",
                f[k].fmt, f[k].dxt ? " (DXT)" : "        ",
                (unsigned long long)f[k].n,
                f[k].texels ? 100.0 * (double)f[k].clear / (double)f[k].texels
                            : 0.0);
    fflush(stderr);
}

/* One level of a texture -- w x h texels at `src`, `pitch` bytes a row for a
 * linear one -- decoded to RGBA8, row 0 first, into `rgba`. 0 for a format
 * this cannot decode. */
static int g_upload_bias_bg;   /* see psh_sync_textures: bump sources */
/* A texel, ARGB, as the bytes R, G, B, A, in one write: every host this runs
 * on is little-endian. */
TEXEL_INLINE void put_rgba(uint8_t *o, uint32_t argb)
{
    uint32_t v = (argb & 0xFF00FF00u) | ((argb >> 16) & 0xFFu) | ((argb & 0xFFu) << 16);
    memcpy(o, &v, 4);
}
/* A swizzled or linear level's rows in one format, `lin` a constant: each
 * format a loop of its own, where every texel used to ask decode_texel which
 * format it was -- two to seven times as fast, texel for texel the same
 * (spriteice's; tests/tex_decode). */
TEXEL_INLINE void decode_rows(uint32_t lin, uint32_t bpp, int swz, const uint8_t *src,
                              uint32_t w, uint32_t h, uint32_t pitch, const uint32_t *offx,
                              const uint32_t *offy, uint32_t bias, uint8_t *rgba)
{
    uint32_t x, y;
    for (y = 0; y < h; y++) {
        const uint8_t *row = swz ? src + (size_t)offy[y] * bpp : src + (size_t)y * pitch;
        uint8_t *o = rgba + (size_t)y * w * 4;
        if (swz)
            for (x = 0; x < w; x++)
                put_rgba(o + (size_t)x * 4, decode_texel(lin, row + (size_t)offx[x] * bpp) ^ bias);
        else
            for (x = 0; x < w; x++)
                put_rgba(o + (size_t)x * 4, decode_texel(lin, row + (size_t)x * bpp) ^ bias);
    }
}
/* decode_level's for every format but DXT: apart, so that the DXT loop keeps
 * its block decode inlined. */
static int decode_texels(uint32_t color, const uint8_t *src, uint32_t w, uint32_t h,
                         uint32_t pitch, uint8_t *rgba)
{
    /* Where each column and each row lands in a swizzled image, worked
     * out once per level: swizzle_offset(x, y) is offx[x] | offy[y], and
     * working it out per texel was most of the time a swizzled upload
     * took. */
    static uint32_t offx[4096], offy[4096];
    uint32_t lin = fmt_is_swz(color) ? swz_to_linear(color) : color;
    uint32_t bpp = linear_bpp(lin), bias = g_upload_bias_bg ? 0x00008080u : 0u;
    uint32_t x, y;
    int swz = fmt_is_swz(color);
    if (!bpp) return 0;
    if (swz) {
        uint32_t mx, my;
        xbox_swizzle_masks(w, h, &mx, &my);
        for (x = 0; x < w; x++) offx[x] = swizzle_deposit(x, mx);
        for (y = 0; y < h; y++) offy[y] = swizzle_deposit(y, my);
    }
    if (!pitch) pitch = w * bpp;
#define ROWS(f, b) case f: decode_rows(f, b, swz, src, w, h, pitch, offx, offy, bias, rgba); break
    switch (lin) {
    ROWS(0x12, 4); ROWS(0x1E, 4); ROWS(0x3F, 4); ROWS(0x40, 4); ROWS(0x41, 4);
    ROWS(0x10, 2); ROWS(0x1C, 2); ROWS(0x11, 2); ROWS(0x1D, 2); ROWS(0x20, 2);
    ROWS(0x13, 1); ROWS(0x1F, 1);
    default: return 0;
    }
#undef ROWS
    return 1;
}
static int decode_level(uint32_t color, const uint8_t *src, uint32_t w, uint32_t h,
                        uint32_t pitch, uint8_t *rgba)
{
    if (fmt_is_dxt(color)) {
        /* Decoded here only where the device doesn't take the blocks as
         * they are, or the texels are read (tex_dxt_direct says when).
         *
         * Handing every texture over compressed would make it depend on
         * the host advertising S3TC -- and when it does not,
         * glCompressedTexImage2D fails, the texture stays undefined, and
         * every surface samples black. So the blocks go straight through
         * only where the device says it takes them.
         *
         * A block at a time: decoded per texel, every block was decoded
         * sixteen times -- 4 ms a texture, and the 49 a new area brings at
         * once were a 200 ms stall in the middle of play. */
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bb = d3d8_format_dxt_block_bytes(color);
        uint32_t bx, by, k;
        for (by = 0; by < bh; by++)
            for (bx = 0; bx < bw; bx++) {
                uint32_t px[16];
                d3d8_dxt_decode_block(src + ((size_t)by * bw + bx) * bb, color, px);
                for (k = 0; k < 16; k++) {
                    uint32_t x = bx * 4 + (k & 3u), y = by * 4 + (k >> 2), argb = px[k];
                    uint8_t *o;
                    if (x >= w || y >= h) continue;
                    if (g_upload_bias_bg) argb ^= 0x00008080u;
                    o = rgba + ((size_t)y * w + x) * 4;
                    o[0] = (uint8_t)(argb >> 16);
                    o[1] = (uint8_t)(argb >> 8);
                    o[2] = (uint8_t)argb;
                    o[3] = (uint8_t)(argb >> 24);
                }
            }
        return 1;
    }
    return decode_texels(color, src, w, h, pitch, rgba);
}

/*
 * The title's own mipmap chain: how many of its levels to upload.
 *
 * The console sampled a mipmapped texture from the chain stored in it, level
 * after level -- each a swizzled or compressed image half the size of the one
 * before, packed one after another from the texture's address, as many as
 * the format register's MIPMAP_LEVELS says. For a long time only level 0 was
 * decoded here, and a chain made by box-filtering it (RECOMP_GL_MIPMAP=1)
 * blurred this title's texture atlases into each other; so a distant surface
 * sampled its full-size image at several texels a pixel and shimmered. The
 * title's chain is what it was drawn with.
 *
 * All of it when the stage samples with a mipmapped filter (3 to 6), as many
 * as the format register says -- no more than the size allows, and no more
 * than TEXTURE_CONTROL0's MAX_LOD_CLAMP lets the sampler reach, as xemu reads
 * it -- and 1 otherwise: a linear texture has no chain. RECOMP_GL_TITLE_MIPS=0
 * uploads the base level alone, as before.
 */
static uint32_t tex_title_levels(const TexStage *t, uint32_t w, uint32_t h)
{
    static int on = -1;
    uint32_t minf = (t->filter >> 16) & 0xF, n, m, maxl, clamp;
    if (on < 0) { const char *v = getenv("RECOMP_GL_TITLE_MIPS"); on = v ? atoi(v) != 0 : 1; }
    if (!on || minf < 3 || minf > 6 || t->levels < 2) return 1;
    if (!fmt_is_swz(t->color) && !fmt_is_dxt(t->color)) return 1;
    m = w > h ? w : h;
    for (maxl = 1; (1u << (maxl - 1)) < m; maxl++) { }
    n = t->levels < maxl ? t->levels : maxl;
    clamp = ((t->control0 >> 6) & 0xFFFu) >> 8;      /* MAX_LOD_CLAMP, 4.8 fixed point */
    if (clamp + 1 < n) n = clamp + 1;
    return n ? n : 1;
}

/* Anisotropic filtering for a mipmapped texture: 16x, or RECOMP_GL_ANISO
 * (1 turns it off), and never less than the title asked for itself
 * (TEXTURE_CONTROL0's LOG_MAX_ANISO). With the chain, the level is chosen for
 * a surface's narrow side and up to that many samples are taken along its
 * long one: a road or a wall seen at a glancing angle stays sharp instead of
 * going to the blur of the level its long side needs. */
static uint8_t tex_aniso(const TexStage *t)
{
    static int forced = -1;
    int title = 1 << ((t->control0 >> 4) & 3);
    if (forced < 0) {
        const char *v = getenv("RECOMP_GL_ANISO");
        forced = v ? atoi(v) : 16;
        if (forced < 1) forced = 1;
        if (forced > 16) forced = 16;
    }
    return (uint8_t)(forced > title ? forced : title);
}

static uint32_t upload_texture_inner(const TexStage *t);
/*
 * The texture for a stage whose address is a surface this backend renders
 * into, or 0 when it is not one.
 *
 * A linear texture is addressed in texels, and texel (x, y) of a texture
 * laid over a surface is pixel (x, y) of that surface -- so the scale that
 * normalises its coordinates is the surface's size, not the size the texture
 * was declared with. The two differ when a title samples part of a target.
 *
 * Sampling the surface that is being drawn into is a feedback loop GL leaves
 * undefined; the console reads whatever is there. The surface is copied
 * first and the copy is sampled.
 */
static int g_fb_copies;
static uint32_t surface_texture_for(const TexStage *t, float *sx, float *sy, int *slot_out)
{
    int i = surface_find(t->offset, t->width, t->height, 1);
    int linear;
    if (slot_out) *slot_out = i;
    if (i < 0) return 0;
    linear = !fmt_is_swz(t->color) && !fmt_is_dxt(t->color);
    *sx = linear ? 1.0f / (float)g_surf[i].w : 1.0f;
    *sy = linear ? 1.0f / (float)g_surf[i].h : 1.0f;
    if (i != g_cur_surf) return G->target_texture(g_surf[i].rt);
    if (g_fb_copies++ < 4)
        fprintf(stderr, "  [GL] draw %u samples surface %08X while drawing into it:"
                        " sampling a copy\n", S.draws, g_surf[i].offset);
    return G->target_snapshot(g_surf[i].rt);
}

/* When upload_texture_inner began to make a texture the cache did not have
 * (0: it found one). Only those are timed for [HITCH]: a texture found is
 * most of a frame's lookups, and timing each was two clock reads for every
 * texture of every draw. */
static double g_tex_make_t0;
static uint32_t upload_texture(const TexStage *t)
{
    uint32_t r; PROF_T0();
    g_prof.n_tex_lookup++;
    g_tex_make_t0 = 0.0;
    r = upload_texture_inner(t);
    if (g_tex_make_t0 > 0.0) g_hf.tex_s += mono_now() - g_tex_make_t0;
    PROF_ADD(tex);
    return r;
}
static uint64_t vc_hash(const void *data, size_t n, uint64_t seed);   /* the vertex cache's, below */
/* The bytes a texture's decode reads from its base level. */
static uint32_t tex_bytes(uint32_t color, uint32_t w, uint32_t h)
{
    return fmt_is_dxt(color)
         ? ((w + 3) / 4) * ((h + 3) / 4) * d3d8_format_dxt_block_bytes(color)
         : w * h * (fmt_is_swz(color) ? linear_bpp(swz_to_linear(color)) : linear_bpp(color));
}
/* The sparse sample a lookup compares: 64 of the bytes, cheap enough for
 * every lookup, and it catches a title that reuses one address for many
 * textures -- the case that matters most. */
static uint32_t tex_sample(uint32_t color, const uint8_t *src, uint32_t bytes, int bias)
{
    uint32_t step = bytes > 4096 ? bytes / 64 : 64, hash, i;
    /* RECOMP_GL_TEXHASH_FULL=1: hash every byte (slow; for testing
     * whether a texture the CPU rewrote in place is being served stale). */
    { static int full = -1;
      if (full < 0) full = getenv("RECOMP_GL_TEXHASH_FULL") ? 1 : 0;
      if (full) step = 1; }
    hash = color * 2654435761u;
    for (i = 0; i < bytes && step; i += step)
        hash = hash * 31u + src[i];
    hash = hash * 31u + bytes;
    if (bias) hash ^= 0x9E3779B9u;   /* a different upload */
    return hash;
}
/* Every byte a texture's decode reads -- row by row at the pitch for a linear
 * one -- hashed as the vertex cache hashes its sources. */
static uint32_t tex_content(uint32_t color, uint32_t height, uint32_t pitch,
                            const uint8_t *src, uint32_t bytes)
{
    int lin = !fmt_is_swz(color) && !fmt_is_dxt(color);
    uint32_t rows = lin ? height : 1, row = bytes / rows, i;
    uint64_t h = color;
    if (!(lin && pitch)) pitch = row;
    for (i = 0; i < rows; i++)
        h = vc_hash(src + (size_t)i * pitch, row, h);
    return (uint32_t)(h ^ (h >> 32));
}
/* Empty a slot but keep its place in the probe chains: a tombstone that
 * matches no lookup, and that the next upload probing past it fills. */
static void tcache_drop(uint32_t k)
{
    if (g_tcache[k].used && g_tcache[k].tex) {
        G->tex_free(g_tcache[k].tex);
        g_prof.tex_held--;
        g_prof.tex_held_bytes -= (unsigned long long)g_tcache[k].held;
    }
    g_tcache[k].tex = 0;
    g_tcache[k].offset = 0xFFFFFFFFu;
    g_tcache[k].hash = 0xFFFFFFFFu;
}
/* Once a frame: drop every texture whose memory the title has freed since
 * it was uploaded. JSRF's D3D frees a stage's textures when it leaves it
 * (MmFreeContiguousMemory), and nothing looks those entries up again: they
 * stayed on the GPU until the table was full, stage after stage. A texture
 * read from outside the contiguous window has no allocation to go by. */
static void tcache_sweep(void)
{
    uint32_t k;
    for (k = 0; k < TEX_CACHE; k++)
        if (g_tcache[k].tex && g_tcache[k].serial
         && contig_serial(g_tcache[k].va) != g_tcache[k].serial)
            tcache_drop(k);
}
/* Once a frame, after the sweep: the whole content of the textures being
 * drawn, compared a few at a time.
 *
 * The sample misses a texture the title rewrites in place where those 64
 * bytes do not change: the graffiti picker loads each chosen design into its
 * one 1024x1024 source image (01808000), and the preview and the wall posters
 * kept the default. Comparing every texture in full at its first lookup of
 * each frame fixes that, and cost a Mac in 99th Street 0.6 ms a frame --
 * textures 3.6% of the wall time where they took 0.7% -- and two and a half
 * times the frames that missed their vblank. So the comparison runs here, at
 * most RECOMP_GL_TEXCHECK_KB of source a frame (1024; 0 turns it off) and
 * always one texture however large, in turn through the textures drawn this
 * frame or the last. One that no longer matches is dropped, and the next
 * lookup uploads what is there now: a rewrite shows a few frames later rather
 * than on the next.
 *
 * Only where the sample still matches: a different picture that the sample
 * does see is the lookup's to handle, and a title that puts two pictures
 * through one place in a frame keeps an entry for each. */
static void tcache_verify(void)
{
    static uint32_t cursor;
    static long budget = -1;
    uint64_t spent = 0;
    uint32_t n;
    if (budget < 0) { const char *e = getenv("RECOMP_GL_TEXCHECK_KB");
                      budget = (e ? atol(e) : 1024) * 1024L; if (budget < 0) budget = 0; }
    if (!budget) return;
    for (n = 0; n < TEX_CACHE && spent < (uint64_t)budget; n++) {
        uint32_t k = cursor, bytes;
        const uint8_t *src;
        cursor = (cursor + 1) % TEX_CACHE;
        if (!g_tcache[k].tex || S.flips - g_tcache[k].drawn > 1) continue;
        bytes = tex_bytes(g_tcache[k].format, g_tcache[k].w, g_tcache[k].h);
        src = guest(g_tcache[k].va);
        spent += bytes;
        if (tex_sample(g_tcache[k].format, src, bytes, g_tcache[k].bias) != g_tcache[k].hash)
            continue;
        g_prof.tex_verified += bytes;
        if (tex_content(g_tcache[k].format, g_tcache[k].h, g_tcache[k].pitch, src, bytes)
            != g_tcache[k].content) {
            tcache_drop(k);
            g_prof.n_tex_rewritten++;
        }
    }
}

/* How a texture is sampled, from its stage's registers: filtering and
 * wrapping, for `nlev` levels of the title's chain (tex_title_levels). */
static void tex_sampler(const TexStage *t, uint32_t nlev, Nv2aSampler *out)
{
    Nv2aSampler smp;
    /*
     * Filtering, as the title asked for it.
     *
     * NV097_SET_TEXTURE_FILTER was being read into t->filter and then
     * ignored: every texture in the game got LINEAR/LINEAR and no mipmaps at
     * all. That is wrong in both directions. A title that asks for point
     * sampling -- interface art, palettised sprites, anything whose texels
     * are meant to be square -- got it blurred; and a title that asks for a
     * mipmapped minification filter, which this one does for most of the
     * city, got none, so every surface at a distance sampled its full-size
     * texture at one texel per several pixels and aliased.
     *
     * The aliasing gets worse the better the rest of the renderer gets. At
     * 640x480 it is a shimmer; at the 2560x1920 the launcher now uses, the
     * same surfaces are being minified four times as hard and it is the most
     * obvious thing left in a still frame.
     *
     * The chain is generated rather than decoded from the title's own levels.
     * The console supplies its mipmaps in the texture and this reads only
     * level zero, so glGenerateMipmap is not what the hardware sampled -- it
     * is a box filter over the same image. For a title's own artist-authored
     * chain the difference is visible in principle and small in practice, and
     * it costs one call at upload against decoding a swizzled, DXT-compressed
     * chain per level. Worth revisiting; not worth blocking the fix on.
     */
    {
        uint32_t minf = (t->filter >> 16) & 0xF;
        uint32_t magf = (t->filter >> 24) & 0xF;
        static int want_mips = -1;
        if (want_mips < 0) {
            const char *v = getenv("RECOMP_GL_MIPMAP");
            want_mips = v ? atoi(v) : 0;
        }

        /* A generated chain is not the title's chain, and on this title's art
         * that difference is not small.
         *
         * The reasoning above was that a box filter over level zero is close
         * enough to an artist's chain to be worth having. On a photograph it
         * would be. This title's textures are mostly ATLASES -- many small
         * pieces packed into one image, addressed by uv rectangle -- and a box
         * filter does not know where the seams are. Every level mixes
         * neighbouring pieces into each other, and by the third level a sign
         * is carrying its neighbour's colour. Trilinear then blends toward
         * those levels for anything not filling the screen, so the whole
         * world goes soft and slightly wrong at once. The first player to
         * see it called it straight away: worse than before.
         *
         * The console had the real chain sitting in the texture. Until this
         * decodes it -- swizzled and DXT-compressed, per level -- the honest
         * thing is to sample level zero, which is what the port did for
         * months and what nobody complained about. The half of the fix that
         * was a real bug stays: the title asks for NEAREST on art that must
         * not be blurred, and for mirrored wrap, and was getting neither.
         *
         * So a mipmapped mode is honoured as its base filter -- 3 and 5 are
         * the NEAREST pair, 4 and 6 the LINEAR pair -- and no chain is
         * invented. RECOMP_GL_MIPMAP=1 puts the generated chain back for
         * anyone who wants to compare.
         *
         * The title's own chain is decoded now (tex_title_levels), and a
         * texture that has one is sampled as the title asked, mipmaps,
         * trilinear and all, with anisotropic filtering (tex_aniso). One
         * without a chain is still sampled at its base level.
         */
        memset(&smp, 0, sizeof smp);
        if (minf >= 3 && minf <= 6) {
            if (nlev > 1 || want_mips) {
                /* 3 and 5 sample nearest within a level, 4 and 6 linear;
                 * 3 and 4 take the nearest level, 5 and 6 blend two. */
                smp.min_linear = (minf == 4 || minf == 6);
                smp.mip = (minf == 3 || minf == 4) ? 1 : 2;
                /* The title's levels, in rgba after the base; none (0)
                 * asks the device to make a chain itself. */
                smp.levels = (uint8_t)(nlev > 1 ? nlev : 0);
                /* Anisotropy only makes sense alongside a chain, and it was
                 * the other half of what made distant surfaces look smeared
                 * rather than sharp. It goes with the chain. */
                smp.aniso = tex_aniso(t);
            } else {
                smp.min_linear = !(minf == 3 || minf == 5);
            }
        } else {
            smp.min_linear = (minf != 1);
        }
        smp.mag_linear = (magf != 1);
    }
    /* Wrap. Mode 1 is repeat, 2 mirrors, 4 uses the border colour and 3 and 5
     * clamp; everything that was not 1 used to clamp, so a mirrored texture
     * came out as its left half stretched across the whole surface. Fixing
     * that was the point and it stays fixed.
     *
     * Mode 4 goes back to clamping, though, and for the same reason the
     * invented mipmaps had to go: it needs a border colour this does not
     * read yet. NV097_SET_TEXTURE_BORDER_COLOR is not decoded anywhere here,
     * so asking GL for CLAMP_TO_BORDER asks for GL's default border, which
     * is transparent black -- and a texture atlas sampled a hair outside its
     * rectangle then returns black instead of the neighbouring texel. That
     * is a thin dark line along every seam in the world, which is exactly
     * what the player saw appear. Clamping is wrong in the same small way it was
     * wrong before, and wrong invisibly rather than wrong in black. */
    /* 0 clamp, 1 repeat, 2 mirrored repeat -- in the device's terms. */
    { static const uint8_t wrap_of[8] = { 0, 1, 2, 0, 0, 0, 0, 0 };
      smp.wrap_s = wrap_of[t->addr & 7u];
      smp.wrap_t = wrap_of[(t->addr >> 8) & 7u]; }
    *out = smp;
}

/*
 * The sampler as one number, for the texture cache to tell two apart.
 *
 * A device texture is made with its sampling (Nv2aSampler) and keeps it, so
 * the cache has to treat one picture sampled two ways as two textures. It did
 * not: whichever draw uploaded a texture first fixed its wrapping and filtering
 * for every draw after it. The characters are shaded through one shared
 * texture of tone ramps -- a coordinate from the light on the surface picks
 * the tone -- which some of their materials address clamped and some wrapped.
 * A ramp read past its end then came back from the far side of the picture
 * where the title asked for its edge, or the other way round, depending on
 * which material happened to be drawn first in the session: a face or an arm
 * in another material's tones.
 */
static uint32_t sampler_key(const Nv2aSampler *s)
{
    return (uint32_t)s->min_linear | (uint32_t)s->mag_linear << 1 | (uint32_t)(s->mip & 3) << 2
         | (uint32_t)(s->wrap_s & 3) << 4 | (uint32_t)(s->wrap_t & 3) << 6 | (uint32_t)s->aniso << 8
         | (uint32_t)s->levels << 16;
}

/*
 * DXT textures handed to the device as the title stored them.
 *
 * They were all decoded to RGBA8 here (decode_level), so that no texture
 * depended on the driver taking S3TC. Entering a level makes 50 to 110
 * textures in one frame, and decoding them was most of the time those frames
 * took: 61 to 75% of it in frames of 50 to 217 ms, as spriteice measured on a
 * ROG Xbox Ally X. Where the device takes them (tex_new_dxt), DXT1, 3 and 5
 * now go to it as they are: nothing to decode, and a quarter to an eighth of
 * the bytes to hand over. Where it doesn't (Metal, an OpenGL without S3TC --
 * most phones -- one drawing on the processor, and Apple's for now: see
 * glx_s3tc), they are decoded as before, and so they are where the texels
 * are read -- a bump source's bias (g_upload_bias_bg), the transparency
 * census, the texture dump -- or for a chain the device makes itself
 * (RECOMP_GL_MIPMAP). RECOMP_GL_DXT=0 decodes them all, to compare. The log
 * says once which it is.
 */
static int g_dxt_direct = -1;   /* not asked yet; 0 decoded; 1 as they are */
static unsigned g_dxt_made[2];  /* DXT textures made: decoded, as they are */
static int tex_dxt_direct(const Nv2aSampler *smp)
{
    if (g_dxt_direct < 0) {
        const char *v = getenv("RECOMP_GL_DXT"), *why = NULL;
        if (v && atoi(v) == 0) why = "RECOMP_GL_DXT=0";
        else if (!G->tex_new_dxt) why = "this device has no way to take them";
        else if (getenv("RECOMP_GL_TEXALPHA") || getenv("RECOMP_GL_DUMP_TEX"))
            why = "the census or the dump reads their texels";
        g_dxt_direct = !why;
        if (why) fprintf(stderr, "  [GL] DXT textures decoded (%s)\n", why);
    }
    return g_dxt_direct && !g_upload_bias_bg && !(smp->mip && smp->levels < 2);
}

static uint32_t upload_texture_inner(const TexStage *t)
{
    uint32_t va = resolve(t->offset);
    const uint8_t *src = guest(va);
    uint32_t w = t->width, h = t->height, i, hash, key, bytes, nlev, sampling;
    int other_sampling = 0;
    uint32_t tex;
    uint8_t *rgba = NULL;
    size_t rgba_bytes = 0;      /* handed over: RGBA8, or DXT as it is */
    Nv2aSampler smp;
    int slot = -1, stale = -1, tomb = -1, dxt = 0;

    if (!w || !h || w > 4096 || h > 4096) return 0;
    nlev = tex_title_levels(t, w, h);
    tex_sampler(t, nlev, &smp);
    sampling = sampler_key(&smp);
    /* RECOMP_GL_TEX_ONESAMPLER=1: one texture however it is sampled -- the
     * first draw's sampling for all -- as before, to compare (=ab: every
     * other displayed frame, from textures of their own). */
    { static int one = -1;
      if (one < 0) { const char *v = getenv("RECOMP_GL_TEX_ONESAMPLER");
                     one = !v ? 0 : (*v == 'a' || *v == 'A') ? 2 : 1; }
      if (one == 1 || (one == 2 && (S.frames & 1u))) sampling = 0x80000000u; }

    /* Same routing census as the vertex arrays, for textures: a texture at
     * a heap VA below the arena mark is decoded from the window instead. */
    { static int dc = -1; static uint64_t n[5]; static uint32_t said;
      if (dc < 0) dc = getenv("RECOMP_GL_DMACENSUS") ? 1 : 0;
      if (dc) {
          uint32_t off = t->offset, mark = xbox_ContiguousAllocatedBytes(); int cls;
          if (off & 0x80000000u)      cls = 0;
          else if (off < GL_HEAP_BASE) cls = off < mark ? 1 : 4;
          else                         cls = off < mark ? 2 : 3;
          n[cls]++;
          if (++said % 2000 == 0)
              fprintf(stderr, "  [DMA] texture uploads by route: bit31 %llu, contiguous %llu, "
                              "HEAP-BELOW-MARK %llu, heap-above-mark %llu, low-above-mark %llu\n",
                      (unsigned long long)n[0], (unsigned long long)n[1],
                      (unsigned long long)n[2], (unsigned long long)n[3],
                      (unsigned long long)n[4]);
      } }

    /* Cache on where it came from and what it looks like: the sample, at every
     * lookup, and the whole content once in a while (tcache_verify).
     *
     * A changed texture is uploaded over its old entry; a new entry would
     * leave the old picture behind, never looked up again, until the table
     * filled. The one exception is an old picture this frame has drawn: a
     * title that puts two pictures through one place within a frame keeps an
     * entry for each. */
    bytes = tex_bytes(t->color, w, h);
    hash = tex_sample(t->color, src, bytes, g_upload_bias_bg);
    key = (t->offset ^ (w << 16) ^ (h << 4) ^ t->color) * 2246822519u;
    for (i = 0; i < TEX_CACHE; i++) {
        uint32_t k = (key + i) % TEX_CACHE;
        if (g_tcache[k].used && g_tcache[k].offset == t->offset
         && g_tcache[k].format == t->color && g_tcache[k].w == w
         && g_tcache[k].h == h && g_tcache[k].bias == g_upload_bias_bg
         && g_tcache[k].levels == nlev && g_tcache[k].sampling == sampling) {
            if (g_tcache[k].hash != hash) {
                /* The sample changed. Not this frame's: upload over it,
                 * unless a later entry already holds the new picture. */
                if (stale < 0 && g_tcache[k].drawn != S.flips) stale = (int)k;
                continue;
            }
            g_tcache[k].drawn = S.flips;
            g_tcache[k].last_draw = S.draws;
            return g_tcache[k].tex;
        }
        if (g_tcache[k].used && g_tcache[k].offset == t->offset && g_tcache[k].format == t->color
         && g_tcache[k].w == w && g_tcache[k].h == h && g_tcache[k].sampling != sampling)
            other_sampling = 1;
        if (!g_tcache[k].used) { slot = (int)k; break; }
        if (!g_tcache[k].tex && tomb < 0) tomb = (int)k;   /* see tcache_drop */
    }
    /* Not in the cache: from here on, a texture is being made (timed). */
    g_tex_make_t0 = mono_now();
    if (stale >= 0) slot = stale;
    else if (tomb >= 0) slot = tomb;
    if (slot < 0) {
        /* Full: evict, but never something this draw is still using. Four
         * texture stages can be bound at once, so anything touched within the
         * last few lookups is live. */
        uint32_t best = key % TEX_CACHE, oldest = 0xFFFFFFFFu;
        for (i = 0; i < 16; i++) {
            uint32_t k = (key + i * 37u) % TEX_CACHE;
            if (S.draws - g_tcache[k].last_draw < 8) continue;
            if (g_tcache[k].last_draw < oldest) { oldest = g_tcache[k].last_draw;
                                                  best = k; }
        }
        slot = (int)best;
    }

    /* RECOMP_GL_TEXLOG_SEC=<s>: every texture uploaded (a cache miss) from
     * s seconds on -- where, what size and format -- to find the texture a
     * newly appearing object is drawn with. */
    { static int on = -1; static double from, t0; static int n;
      if (on < 0) { const char *v = getenv("RECOMP_GL_TEXLOG_SEC");
                    on = v ? 1 : 0; from = v ? atof(v) : 0; t0 = mono_now(); }
      if (on && n < 400 && mono_now() - t0 >= from) {
          n++;
          fprintf(stderr, "[TEXUP] draw %u flip %u: %08X %ux%u fmt %02X pitch %u (draw uses prog %s)\n",
                  S.draws, S.flips, t->offset, w, h, t->color, t->pitch,
                  S.xf_mode == NV2A_XF_MODE_FIXED ? "ff" : "vp");
      } }
    /* Until the new texture exists the slot holds nothing. */
    tcache_drop((uint32_t)slot);

    /* DXT as it is, where the device takes it (tex_dxt_direct). */
    if (fmt_is_dxt(t->color) && tex_dxt_direct(&smp)) {
        tex = G->tex_new_dxt(w, h, t->color == 0x0C ? 1 : t->color == 0x0E ? 3 : 5, src, &smp);
        if (tex) {
            uint32_t l, wl = w, hl = h;
            for (l = 0; l < (smp.mip && nlev > 1 ? nlev : 1); l++) {
                rgba_bytes += tex_bytes(t->color, wl, hl);
                wl = wl > 1 ? wl / 2 : 1;
                hl = hl > 1 ? hl / 2 : 1;
            }
            if (!g_dxt_made[1])
                fprintf(stderr, "  [GL] DXT textures handed to the device as they are\n");
            dxt = 1;
            goto made;
        }
        g_dxt_direct = 0;
        fprintf(stderr, "  [GL] DXT textures decoded (the device doesn't take them as they are)\n");
    }

    /* Level 0, and the rest of the title's chain after it when it has one
     * (tex_title_levels): each level decoded as the base is, from where the
     * last one ended, into one buffer level after level. */
    {
        uint32_t l, wl = w, hl = h;
        size_t so = 0, dof = 0;
        for (l = 0; l < nlev; l++) {
            rgba_bytes += (size_t)wl * hl * 4;
            wl = wl > 1 ? wl / 2 : 1;
            hl = hl > 1 ? hl / 2 : 1;
        }
        rgba = (uint8_t *)malloc(rgba_bytes);
        if (!rgba) return 0;
        for (l = 0, wl = w, hl = h; l < nlev; l++) {
            if (!decode_level(t->color, src + so, wl, hl, t->pitch, rgba + dof)) {
                /* Say so: a texture in a format this cannot decode draws as
                 * nothing (or black), and it is otherwise invisible in every
                 * log. Once per distinct format and size, then a count. */
                static uint32_t seen[32]; static int nseen; static unsigned long total;
                uint32_t k = (t->color << 16) ^ (w << 4) ^ h; int q, found = 0;
                total++;
                for (q = 0; q < nseen; q++) if (seen[q] == k) { found = 1; break; }
                if (!found && nseen < 32) {
                    seen[nseen++] = k;
                    fprintf(stderr, "  [GL] TEXTURE FORMAT %02X NOT DECODED: %ux%u at %08X "
                            "(pitch %u, draw %u; %lu such uploads so far)\n",
                            t->color, w, h, t->offset, t->pitch, S.draws, total);
                }
                free(rgba);
                return 0;
            }
            so  += tex_bytes(t->color, wl, hl);
            dof += (size_t)wl * hl * 4;
            wl = wl > 1 ? wl / 2 : 1;
            hl = hl > 1 ? hl / 2 : 1;
        }
        tex_alpha_census(rgba, w, h, t->color, fmt_is_dxt(t->color));
    }
    /* RECOMP_GL_DUMP_TEX=<prefix>: every texture as uploaded. A frame that
     * shades to black is either sampling the wrong place or sampling a black
     * texture, and nothing short of looking at the texture separates them. */
    {
        static int shots;
        static const char *pfx; static int init; static uint32_t after3d;
        if (!init) { init = 1; pfx = getenv("RECOMP_GL_DUMP_TEX");
                     { const char *a = getenv("RECOMP_GL_DUMP_TEX_3D");
                       after3d = a ? (uint32_t)strtoul(a, 0, 0) : 0; } }
        /* RECOMP_GL_DUMP_TEX_OFF=<hex>: only the texture at this address. A
         * scene uploads hundreds, and the one worth looking at is usually a
         * particular address that showed up in a draw dump. */
        { static uint32_t only[8]; static int oinit, nonly;
          int hit = 0, q;
          if (!oinit) { const char *o = getenv("RECOMP_GL_DUMP_TEX_OFF");
                        oinit = 1;
                        /* A comma-separated list now: "0250F000,02512000". */
                        while (o && *o && nonly < 8) {
                            char *e; only[nonly++] = (uint32_t)strtoul(o, &e, 16);
                            o = (*e == ',') ? e + 1 : NULL;
                        } }
          for (q = 0; q < nonly; q++) if (t->offset == only[q]) hit = 1;
        if (pfx && shots < 24 && S.draws_3d >= after3d
            && (!nonly || hit)) {
            uint8_t *back = (uint8_t *)malloc((size_t)w * h * 4);
            if (back) {
                char nm[128]; FILE *f; uint32_t x, y, row, sz, nz = 0;
                uint8_t hdr[54];
                /* What is being uploaded, as BGRA: the same bytes the
                 * device gets, without asking it for them back. */
                for (x = 0; x < w * h; x++) {
                    back[x*4+0] = rgba[x*4+2]; back[x*4+1] = rgba[x*4+1];
                    back[x*4+2] = rgba[x*4+0]; back[x*4+3] = rgba[x*4+3];
                }
                for (x = 0; x < w * h; x++)
                    if (back[x*4] | back[x*4+1] | back[x*4+2]) nz++;
                snprintf(nm, sizeof nm, "%s%02d_%08X_f%02X_%ux%u.bmp",
                         pfx, shots++, t->offset, t->color, w, h);
                f = fopen(nm, "wb");
                if (f) {
                    row = (w * 3 + 3) & ~3u; sz = 54 + row * h;
                    memset(hdr, 0, sizeof hdr);
                    hdr[0]='B'; hdr[1]='M'; memcpy(hdr+2,&sz,4);
                    { uint32_t o=54; memcpy(hdr+10,&o,4); }
                    { uint32_t n=40; memcpy(hdr+14,&n,4); }
                    memcpy(hdr+18,&w,4); memcpy(hdr+22,&h,4);
                    hdr[26]=1; hdr[28]=24;
                    fwrite(hdr,1,sizeof hdr,f);
                    for (y = 0; y < h; y++) {
                        const uint8_t *r = back + (size_t)(h-1-y) * w * 4;
                        uint8_t pad[3] = {0,0,0};
                        for (x = 0; x < w; x++) fwrite(r + x*4, 1, 3, f);
                        fwrite(pad, 1, row - w*3, f);
                    }
                    fclose(f);
                    fprintf(stderr, "  [GL] texture %s (%u non-black)\n", nm, nz);
                }
                /* And its alpha, as grey: a texture that decodes with alpha
                 * one everywhere draws opaque where the title meant soft. */
                { char na[144]; snprintf(na, sizeof na, "%s_a.bmp", nm);
                  f = fopen(na, "wb");
                  if (f) {
                      fwrite(hdr, 1, sizeof hdr, f);
                      for (y = 0; y < h; y++) {
                          const uint8_t *r = back + (size_t)(h-1-y) * w * 4;
                          uint8_t pad[3] = {0,0,0}, px[3];
                          for (x = 0; x < w; x++) { px[0] = px[1] = px[2] = r[x*4+3]; fwrite(px, 1, 3, f); }
                          fwrite(pad, 1, row - w*3, f);
                      }
                      fclose(f);
                  } }
                free(back);
            }
        } }
    }
    tex = G->tex_new(w, h, rgba, &smp);
    free(rgba);
    if (!tex) return 0;

made:
    /* How much of the city has a chain, once a while: uploads with one,
     * and the levels they carried. */
    { static unsigned long n, with, lv, twice;
      n++;
      if (nlev > 1) { with++; lv += nlev; }
      if (other_sampling) twice++;
      if (n == 200 || n % 2000 == 0)
          fprintf(stderr, "  [GL] mipmaps: %lu of %lu textures uploaded with the title's own "
                          "chain (%.1f levels each); %lu a second time, sampled another way\n",
                  with, n, with ? (double)lv / with : 0.0, twice); }
    if (fmt_is_dxt(t->color)) g_dxt_made[dxt]++;
    if (dxt) g_hf.tex_dxt++;
    g_prof.n_tex_upload++;
    g_hf.tex_new++;
    g_prof.tex_held++;
    g_prof.tex_held_bytes += (unsigned long long)rgba_bytes;
    g_tcache[slot].held = rgba_bytes;
    g_tcache[slot].used = 1;
    g_tcache[slot].offset = t->offset; g_tcache[slot].format = t->color;
    g_tcache[slot].w = w; g_tcache[slot].h = h; g_tcache[slot].pitch = t->pitch;
    g_tcache[slot].levels = nlev;
    g_tcache[slot].sampling = sampling;
    g_tcache[slot].hash = hash; g_tcache[slot].tex = tex;
    g_tcache[slot].bias = g_upload_bias_bg;
    g_tcache[slot].va = va; g_tcache[slot].serial = contig_serial(va);
    g_tcache[slot].content = tex_content(t->color, h, t->pitch, src, bytes);
    g_tcache[slot].drawn = S.flips;
    g_tcache[slot].last_draw = S.draws;
    return tex;
}

/* ---- vertex fetch ------------------------------------------------------- */

static void fetch(const Attr *a, uint32_t index, float out[4], int inl)
{
    const uint8_t *p;
    uint32_t i;

    out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f;
    if (!a->size || !a->stride) {
        memcpy(out, S.const_attr[a - S.attr], sizeof(float) * 4);
        return;
    }

    if (inl)
        p = (const uint8_t *)S.inline_buf + a->offset + index * a->stride;
    else {
        if (!a->offset) return;
        p = guest(resolve(a->offset)) + (size_t)index * a->stride;
    }

    switch (a->type) {
    case 0:  /* D3DCOLOR: one dword, B,G,R,A in memory order */
        out[0] = p[2] / 255.0f; out[1] = p[1] / 255.0f;
        out[2] = p[0] / 255.0f; out[3] = p[3] / 255.0f;
        break;
    case 1:  /* signed short, normalised */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = ((const int16_t *)p)[i] / 32767.0f;
        break;
    case 2:  /* float */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = ((const float *)p)[i];
        break;
    case 4:  /* unsigned byte, normalised, RGBA order */
        for (i = 0; i < a->size && i < 4; i++) out[i] = p[i] / 255.0f;
        break;
    case 5:  /* signed short, not normalised */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i];
        break;
    case 6: { /* packed 11/11/10, signed and normalised */
        uint32_t v = *(const uint32_t *)p;
        int32_t x = (int32_t)(v << 21) >> 21;
        int32_t y = (int32_t)(v << 10) >> 21;
        int32_t z = (int32_t)(v) >> 22;
        out[0] = x / 1023.0f; out[1] = y / 1023.0f; out[2] = z / 511.0f;
        break;
    }
    default:
        break;
    }
}

/* ---- draw --------------------------------------------------------------- */

static int nv2a_prim(uint32_t p, uint32_t *fan_expand)
{
    *fan_expand = 0;
    switch (p) {
    case PRIM_POINTS:         return NV2A_PRIM_POINTS;
    case PRIM_LINES:          return NV2A_PRIM_LINES;
    case PRIM_LINE_LOOP:      return NV2A_PRIM_LINE_LOOP;
    case PRIM_LINE_STRIP:     return NV2A_PRIM_LINE_STRIP;
    case PRIM_TRIANGLES:      return NV2A_PRIM_TRIANGLES;
    case PRIM_TRIANGLE_STRIP: return NV2A_PRIM_TRIANGLE_STRIP;
    case PRIM_TRIANGLE_FAN:   return NV2A_PRIM_TRIANGLE_FAN;
    case PRIM_POLYGON:        return NV2A_PRIM_TRIANGLE_FAN;
    /* Quads have no core-profile equivalent (nor a Metal one), so they are
     * expanded into two triangles per quad on the way to the buffer rather
     * than emulated with a geometry shader. */
    case PRIM_QUADS:          *fan_expand = 1; return NV2A_PRIM_TRIANGLES;
    case PRIM_QUAD_STRIP:     *fan_expand = 2; return NV2A_PRIM_TRIANGLES;
    default:                  return 0;
    }
}

#ifdef NV2A_GL_HAVE_GL
/* The same in GL's terms, for the GL-only probes below. */
static GLenum gl_mode_of(int prim)
{
    switch (prim) {
    case NV2A_PRIM_LINES:          return GL_LINES;
    case NV2A_PRIM_LINE_LOOP:      return GL_LINE_LOOP;
    case NV2A_PRIM_LINE_STRIP:     return GL_LINE_STRIP;
    case NV2A_PRIM_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
    case NV2A_PRIM_TRIANGLE_FAN:   return GL_TRIANGLE_FAN;
    case NV2A_PRIM_POINTS:         return GL_POINTS;
    default:                       return GL_TRIANGLES;
    }
}

static GLenum gl_blend(uint32_t f)
{
    switch (f) {
    case 0x0000: return GL_ZERO;
    case 0x0001: return GL_ONE;
    case 0x0300: return GL_SRC_COLOR;
    case 0x0301: return GL_ONE_MINUS_SRC_COLOR;
    case 0x0302: return GL_SRC_ALPHA;
    case 0x0303: return GL_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return GL_DST_ALPHA;
    case 0x0305: return GL_ONE_MINUS_DST_ALPHA;
    case 0x0306: return GL_DST_COLOR;
    case 0x0307: return GL_ONE_MINUS_DST_COLOR;
    case 0x0308: return GL_SRC_ALPHA_SATURATE;
    default:     return GL_ONE;
    }
}

static GLenum gl_depth_func(uint32_t f)
{
    switch (f) {
    case 0x0200: return GL_NEVER;   case 0x0201: return GL_LESS;
    case 0x0202: return GL_EQUAL;   case 0x0203: return GL_LEQUAL;
    case 0x0204: return GL_GREATER; case 0x0205: return GL_NOTEQUAL;
    case 0x0206: return GL_GEQUAL;  case 0x0207: return GL_ALWAYS;
    default:     return GL_LEQUAL;
    }
}
#endif /* NV2A_GL_HAVE_GL */


/*
 * A ring of the last few draws of a frame.
 *
 * "Something is painted over the scene" is a claim about draw ORDER, and every
 * other diagnostic here reports on a draw in isolation. The draws that matter
 * are the last ones before the flip -- a backdrop that should have been behind
 * everything, or a fade that should have been transparent, looks exactly like
 * a correct draw until you notice when it happened.
 */
#define DRAW_RING 12
static struct {
    uint32_t draw, verts, prim;
    uint32_t tex0_off, tex0_w, tex0_h;
    int      blend, src, dst, depth_test, depth_mask, alpha_test;
    uint32_t stages;
    float    minx, maxx, miny, maxy;   /* extent of attribute 0, as submitted */
} g_ring[DRAW_RING];
static uint32_t g_ring_n;

static void ring_dump(void)
{
    uint32_t i, n = g_ring_n < DRAW_RING ? g_ring_n : DRAW_RING;
    fprintf(stderr, "  [GL] last %u draws of the frame (oldest first):\n", n);
    for (i = 0; i < n; i++) {
        uint32_t k = (g_ring_n - n + i) % DRAW_RING;
        fprintf(stderr, "    #%-8u %4u verts prim %u  tex %08X %ux%u  "
                "%u stage(s)  blend %d %04X/%04X  depth %d/%d  atest %d  "
                "v0 x %.0f..%.0f y %.0f..%.0f\n",
                g_ring[k].draw, g_ring[k].verts, g_ring[k].prim,
                g_ring[k].tex0_off, g_ring[k].tex0_w, g_ring[k].tex0_h,
                g_ring[k].stages, g_ring[k].blend, g_ring[k].src, g_ring[k].dst,
                g_ring[k].depth_test, g_ring[k].depth_mask, g_ring[k].alpha_test,
                g_ring[k].minx, g_ring[k].maxx, g_ring[k].miny, g_ring[k].maxy);
    }
}

/* Which frame the per-draw log is following, latched at present time once a
 * frame turns out to be busy enough to be the phase under investigation. */
static long g_draw_log_flip = -1;

static void draw_log_latch(void)
{
    /* RECOMP_GL_DRAW_LOG_SEC=<s>: and not before s seconds into the run. */
    static long want = -2; static double sec, t0;
    if (want == -2) { const char *v = getenv("RECOMP_GL_DRAW_LOG");
                      const char *w = getenv("RECOMP_GL_DRAW_LOG_SEC");
                      want = v ? atol(v) : -1; sec = w ? atof(w) : 0.0; t0 = mono_now(); }
    if (want >= 0 && g_draw_log_flip < 0 && (long)S.since_present >= want
        && (sec <= 0.0 || mono_now() - t0 >= sec))
        g_draw_log_flip = (long)S.flips + 1;
}

/* One BMP from a BGRA buffer.
 *
 * Pulled out of dump_fbo so that a single frame can be written twice -- once
 * as colour and once as depth. Those two were alternatives before, chosen by
 * an environment variable, which meant comparing them required two runs and
 * two runs of an attract-mode fly-through never reach the same moment. For
 * the question at hand -- is there any geometry under the floating bus, or
 * is the deck simply never drawn -- the colour and the depth have to be the
 * same frame or they say nothing. */
static void write_bmp(const char *nm, const uint8_t *buf,
                      uint32_t pw, uint32_t ph)
{
    FILE *f = fopen(nm, "wb");
    uint8_t hdr[54];
    uint32_t row, sz, x, y;
    if (!f) return;
    row = (pw * 3 + 3) & ~3u;
    sz = 54 + row * ph;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &sz, 4);
    { uint32_t o = 54; memcpy(hdr + 10, &o, 4); }
    { uint32_t n = 40; memcpy(hdr + 14, &n, 4); }
    memcpy(hdr + 18, &pw, 4);
    memcpy(hdr + 22, &ph, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);
    /* A BMP with a positive height is stored bottom row first, which is also
     * the order glReadPixels returns, and the framebuffer holds the picture
     * the GL way up (the vertex stage sends screen y = 0 to NDC +1) -- so
     * the rows go out in the order they came. This used to write them
     * reversed, which showed every surface the title renders into upside
     * down and only a surface that had been through one render-to-texture
     * copy (which turned it over) the right way up. */
    for (y = 0; y < ph; y++) {
        const uint8_t *r = buf + (size_t)y * pw * 4;
        uint8_t pad[3] = { 0, 0, 0 };
        for (x = 0; x < pw; x++) fwrite(r + x * 4, 1, 3, f);
        fwrite(pad, 1, row - pw * 3, f);
    }
    fclose(f);
}

/* How many times the frame dumps (RECOMP_GL_DUMP_FB) fire: 24, or
 * RECOMP_GL_DUMP_MAX (up to the 72 pictures a surface is allowed). */
static int dump_max(void)
{
    static int n = -1;
    if (n < 0) { const char *v = getenv("RECOMP_GL_DUMP_MAX");
                 n = v ? atoi(v) : 24; if (n < 1) n = 24; if (n > 72) n = 72; }
    return n;
}

/* Write the current framebuffer out as a BMP, for looking at.
 *
 * Reads back rather than sharing the present path's buffer: the two are asked
 * at different moments -- one at the end of a frame, one straight after a
 * draw -- and the whole value of the second is that it is not the first. */
static void dump_fbo(uint32_t rt, const char *prefix, int *counter, int limit)
{
    static uint8_t *buf;
    static uint32_t cap;
    uint32_t need, nz = 0, i;
    char nm[128];
    uint32_t pw, ph;
    if (!g_ready || !g_fbo_w || !g_fbo_h || *counter >= limit) return;
    pw = g_fbo_pw ? g_fbo_pw : g_fbo_w;
    ph = g_fbo_ph ? g_fbo_ph : g_fbo_h;
    need = pw * ph * 4;
    if (cap < need) { free(buf); buf = (uint8_t *)malloc(need); cap = need; }
    if (!buf) return;

    /* Colour first, always. */
    if (!G->target_read(rt, 0, 0, pw, ph, 1, buf)) return;
    for (i = 0; i < pw * ph; i++)
        if (buf[i*4] | buf[i*4+1] | buf[i*4+2]) nz++;
    snprintf(nm, sizeof nm, "%s%03d.bmp", prefix, (*counter)++);
    write_bmp(nm, buf, pw, ph);
    fprintf(stderr, "  [GL] wrote %s (%u non-black)\n", nm, nz);

    /* RECOMP_GL_DUMP_DEPTH=1 writes the depth buffer as well, from the same
     * frame, beside the colour.
     *
     * "That object is in the wrong place" and "that object is in the right
     * place and the thing it stands on was never drawn" produce the same
     * picture -- a bus hanging in mid-air with nothing under it -- and only
     * the depth buffer separates them. If there is geometry under the bus at
     * a sensible distance, the deck is being drawn and then made invisible
     * by shading, and the search is about fragments. If the depth there is
     * the far plane, nothing was ever submitted, and the search moves to the
     * game's own visibility logic on the CPU side. Those are opposite ends
     * of the port, so the answer decides where the next day goes.
     *
     * Rescaled around the range actually present: a perspective depth buffer
     * is almost all 1.0 and a straight copy is a white rectangle. */
    { static int depth = -1;
      if (depth < 0) depth = getenv("RECOMP_GL_DUMP_DEPTH") ? 1 : 0;
      if (depth) {
          static float *zb; static uint32_t zcap;
          uint32_t np = pw * ph;
          if (zcap < np) { free(zb); zb = (float *)malloc(np * sizeof(float));
                           zcap = np; }
          if (zb) {
              float lo = 1e30f, hi = -1e30f;
              unsigned h[10] = {0}, at1 = 0, j;
              if (!G->target_read_depth(rt, pw, ph, zb)) {
                  fprintf(stderr, "  [GL] depth readback: not on the %s backend\n", G->name);
                  return;
              }
              if (g_zrev) for (i = 0; i < np; i++) zb[i] = 1.0f - zb[i];
              for (i = 0; i < np; i++) {
                  if (zb[i] < lo) lo = zb[i];
                  if (zb[i] < 1.0f && zb[i] > hi) hi = zb[i];
              }
              if (hi <= lo) hi = lo + 1e-6f;
              for (i = 0; i < np; i++) {
                  float t = (zb[i] - lo) / (hi - lo);
                  uint8_t v = (uint8_t)(255.0f * (t < 0 ? 0 : t > 1 ? 1 : t));
                  buf[i*4] = buf[i*4+1] = buf[i*4+2] = (uint8_t)(255 - v);
                  buf[i*4+3] = 255;
              }
              /* A histogram, not a range: "everything is at 1.0 except a few
               * pixels" and "everything is crammed near zero" have the same
               * minimum and maximum and mean opposite things. */
              for (j = 0; j < np; j++) {
                  float z = zb[j];
                  if (z >= 1.0f) { at1++; continue; }
                  h[(int)(z * 10.0f) % 10]++;
              }
              /* And what a w-buffered frame's depths are in the title's own
               * units (w = depth x 2^24): the spread of a sample of them. */
              { static float smp[4096]; uint32_t ns = 0, step = np / 4096 + 1, a, b;
                for (j = 0; j < np && ns < 4096; j += step) if (zb[j] < 1.0f) smp[ns++] = zb[j];
                for (a = 1; a < ns; a++) { float v = smp[a]; for (b = a; b > 0 && smp[b-1] > v; b--) smp[b] = smp[b-1]; smp[b] = v; }
                if (ns) fprintf(stderr, "  [GL] depth as w (x 2^24): 1%% %.2f, 10%% %.2f, median %.2f, "
                                "90%% %.2f, 99%% %.2f (%u samples)\n",
                                smp[ns / 100] * 16777216.0, smp[ns / 10] * 16777216.0,
                                smp[ns / 2] * 16777216.0, smp[ns * 9 / 10] * 16777216.0,
                                smp[ns * 99 / 100] * 16777216.0, ns); }
              snprintf(nm, sizeof nm, "%s%03d_z.bmp", prefix, *counter - 1);
              write_bmp(nm, buf, pw, ph);
              fprintf(stderr, "  [GL] wrote %s -- %u px at the far plane "
                      "(nothing drawn there); rest by tenth: "
                      "%u %u %u %u %u %u %u %u %u %u  (nearest %.8f)\n",
                      nm, at1, h[0],h[1],h[2],h[3],h[4],
                      h[5],h[6],h[7],h[8],h[9], (double)lo);
          }
      } }
}

/* Build one packed vertex array from the batch's indices and draw it.
 *
 * De-indexing rather than uploading an index buffer: batches are small, the
 * NV2A's index list is already expanded in the pushbuffer, and a flat array
 * sidesteps having to reproduce the hardware's per-attribute strides on the
 * GL side. It costs a little bandwidth and removes a whole class of bug. */
/*
 * A surface with no alpha channel keeps none.
 *
 * Most of this title's surfaces are R5G6B5 -- the frame, and the copies of it
 * the boost dash's heat haze is made from -- and the console stores no alpha
 * for them at all: a blend that writes alpha writes nothing, and a texture
 * read of the surface afterwards gives alpha 1. The targets here are RGBA,
 * so the alpha a blend wrote stayed in them: where a shadow had been laid
 * over the scene (black at half alpha, blended), on glass, under a spray of
 * particles, the copy came back partly transparent, and the haze -- the copy
 * blended over a distorted second copy by its alpha -- showed through there
 * and nowhere else. So for those formats the alpha channel is never written
 * (clears put 1 there), as it is never stored on the console.
 * RECOMP_GL_SURF_ALPHA=1: write it as before.
 */
static int surf_has_alpha(uint32_t fmt)
{
    switch (fmt & 0xF) {
    case 0:                    /* not set yet: leave it alone */
    case 6: case 7:            /* X1A7R8G8B8 */
    case 8:                    /* A8R8G8B8 */
        return 1;
    default:                   /* X1R5G5B5, R5G6B5, X8R8G8B8, B8, G8B8 */
        return 0;
    }
}

static uint32_t surf_color_mask(void)
{
    static int keep = -1;
    uint32_t cm = S.color_mask ? S.color_mask : 0x01010101u;   /* 0: never set, all */
    if (keep < 0) keep = getenv("RECOMP_GL_SURF_ALPHA") ? 1 : 0;
    if (keep || surf_has_alpha(S.surf_format)) return S.color_mask;
    cm &= ~0x01000000u;
    /* The backends read 0 as "never set", so a mask that is left with no
     * channel at all says so with a bit no channel uses. */
    return cm ? cm : 0x80000000u;
}

/*
 * Bind the surface the title has set up, at the moment it is used.
 *
 * It used to be bound as each surface method arrived. D3D's SetRenderTarget
 * sends the colour offset first and the clip after it, so every switch passed
 * through a surface that does not exist -- the new offset at the old size --
 * and each of those got a framebuffer of its own. JSRF's boost dash switches
 * between a 640x480 frame, a 512x512 copy and a 128x128 one, which made four
 * phantoms (003E8000 640x480, 00084000 512x512, 00568000 640x480, 00084000
 * 128x128): they filled the cache, evicted the real frame, and a texture
 * lookup by address could land on one of them. A draw or a clear is the
 * only point where offset and size are known to belong together.
 */
static void surface_sync(void)
{
    if (!g_ready) return;
    if (S.surf_dirty) {
        S.surf_dirty = 0;
        surface_bind(S.color_offset, S.clip_w, S.clip_h);
        if (g_pass_trace_until && S.frames < g_pass_trace_until)
            fprintf(stderr, "[PASS] f%u d%u bind surface %08X %ux%u pitch %u fmt %02X zeta %08X\n",
                    S.frames, S.draws, S.color_offset, S.clip_w, S.clip_h,
                    S.surf_pitch, S.surf_format & 0xFF, S.zeta_offset);
    }
    if (g_cur_surf >= 0) {
        g_surf[g_cur_surf].last_draw = S.draws + 1;
        g_surf[g_cur_surf].fmt = S.surf_format & 0xFFFFu;
    }
}

/* ---- widescreen ---------------------------------------------------------
 *
 * The title only ever draws 4:3. In widescreen mode the port widens its
 * camera (jsrf-recomp recomp_manual.c: m00 = 0.75 for every perspective
 * projection, culling widened to match), so the 640x480 frame holds a 16:9
 * view squeezed horizontally and the window shows it stretched back to
 * 16:9. Everything 3D comes out right that way; what does not is 2D drawn
 * in screen pixels -- the HUD, text, menus -- which would be stretched. Those
 * draws come through D3D's XYZRHW pass-through program (inputs 0x1F9B), and
 * this squeezes them by the same 3/4 about the centre, so they keep their
 * shape and sit in the middle 4:3. A pass-through draw that spans (nearly)
 * the whole width -- a backdrop, a fade, the copy of the scene to the
 * display -- is left to fill the screen. RECOMP_WIDESCREEN=1/0 overrides the
 * window's Video > Widescreen choice. */
static int g_wide = -1;
void nv2a_set_wide_aspect(float a);
int nv2a_widescreen(void)
{
    if (g_wide < 0) {
        const char *v = getenv("RECOMP_WIDESCREEN");
        extern int nv_window_pref_widescreen(void) __attribute__((weak));
        if (v && *v) g_wide = atoi(v) != 0;
        else g_wide = (getenv("RECOMP_WINDOW") && nv_window_pref_widescreen)
                    ? (nv_window_pref_widescreen() != 0) : 0;
        /* RECOMP_WIDE_ASPECT=<width/height>, e.g. 2.17: widen to that shape
         * instead of 16:9 (the iPhone sets its screen's itself). */
        if ((v = getenv("RECOMP_WIDE_ASPECT")) != NULL && atof(v) > 0.0)
            nv2a_set_wide_aspect((float)atof(v));
    }
    return g_wide;
}
void nv2a_set_widescreen(int on) { g_wide = on ? 1 : 0; }
/* Whether the scene on screen is one the game side widened. A title can
 * keep a screen 4:3 -- JSRF keeps its title and the boot logos that way,
 * since their overlays put things just past the 4:3 edges that a 16:9 view
 * would show -- and such a screen is shown pillarboxed in the 16:9 window,
 * with nothing squeezed. Titles that never say anything are widened
 * throughout. */
static volatile int g_wide_scene = 1;
void nv2a_set_wide_scene(int on) { g_wide_scene = on ? 1 : 0; }
int nv2a_wide_scene(void) { return nv2a_widescreen() && g_wide_scene; }
/* The shape a widened scene is shown at, width over height: 16:9 unless a
 * platform asks for its screen's own (the iPhone's "Fill the screen", about
 * 19.5:9). The camera and its culling (recomp_manual.c), the 2D squeeze and
 * the framed-3D move below and the presenter all follow it, through s, how
 * much the 640x480 frame squeezes the wider view: (4/3) / aspect, 0.75 at
 * 16:9 -- exactly, so that the 16:9 picture is what it always was. Kept
 * between 16:9 and 2.4:1. */
static volatile float g_wide_aspect = 16.0f / 9.0f, g_wide_s = 0.75f;
void  nv2a_set_wide_aspect(float a);
float nv2a_wide_aspect(void);
float nv2a_wide_squeeze(void);
void nv2a_set_wide_aspect(float a)
{
    if (!(a >= 16.0f / 9.0f)) a = 16.0f / 9.0f;     /* NaN too */
    if (a > 2.4f) a = 2.4f;
    if (a == g_wide_aspect) return;
    g_wide_s = (a == 16.0f / 9.0f) ? 0.75f : 4.0f / (3.0f * a);
    g_wide_aspect = a;
    fprintf(stderr, "[WIN] widescreen shape %.3f:1 (the 4:3 frame squeezes it by %.3f)\n",
            (double)a, (double)g_wide_s);
}
float nv2a_wide_aspect(void) { return g_wide_aspect; }
float nv2a_wide_squeeze(void) { return g_wide_s; }
/* The windows' Video menus (Mac, Windows, Linux) offer three pictures: 4:3,
 * 16:9 and 21:9. "21:9" is an ultrawide screen's own shape when the window
 * is on one -- 2560x1080 is 2.370:1, 3440x1440 2.389:1, 3840x1600 2.4:1 --
 * so the picture fills it exactly; on any other screen it is 64:27, the
 * 2.370:1 that "21:9" is sold as, between bars above and below. */
float nv2a_ultrawide_aspect(float screen);
float nv2a_ultrawide_aspect(float screen)
{
    if (screen >= 2.2f) return screen < 2.4f ? screen : 2.4f;
    return 64.0f / 27.0f;
}
/* A window's picture: 0 4:3, 1 16:9, 2 21:9, on a screen of shape `screen`
 * (width over height). `apply` switches widescreen too -- a choice made in
 * the menu; at start-up, whether it is on is nv2a_widescreen's to read, from
 * RECOMP_WIDESCREEN or the stored choice. RECOMP_WIDE_ASPECT, a test's, keeps
 * its shape. */
void nv2a_set_picture(int pic, float screen, int apply);
void nv2a_set_picture(int pic, float screen, int apply)
{
    const char *v = getenv("RECOMP_WIDE_ASPECT");
    if (apply) nv2a_set_widescreen(pic != 0);
    if (!(v && atof(v) > 0.0))
        nv2a_set_wide_aspect(pic == 2 ? nv2a_ultrawide_aspect(screen) : 16.0f / 9.0f);
}
static int g_wide_squeeze;   /* this draw's 2D squeeze, set in do_draw */
static float g_wide_shift;   /* this draw's framed-3D move, title pixels, ditto */
static float g_wide_anchor;  /* this draw's move to a screen edge, NDC (see wide_hud_sheet) */
static Nv2aRenderState g_draw_rs;   /* the state this draw was given */
static float g_wide_ext[4];  /* its x and y extent, for the draw log */

/* ---- the HUD at the screen's edges, in widescreen ---------------------------
 *
 * The squeeze above keeps every overlay its shape by drawing it into the
 * middle 4:3 of the wide screen. For menus and dialogue that is right -- they
 * are compositions centred on the screen -- but the in-game HUD is pinned to
 * the 4:3 screen's corners: the health bar, the spray cans and a race's rival
 * at the top right, the gauge at the bottom right (the timer is at the top
 * centre). Kept in the middle 4:3 they ended 82% of the way across a 16:9
 * screen and 74% across 21:9.
 *
 * The HUD draws its pieces from one texture, a 256x256 DXT3 sheet of them --
 * the bars, the digits, the can, the icons -- while the dialogue box, the
 * menus and their text have sheets of their own. So a flat piece of that
 * sheet drawn wholly in the right part of the 4:3 screen (from 0.1 of the
 * half-width right of centre) goes with the right edge, one wholly in the
 * left part with the left edge, and the rest -- the timer -- stays centred.
 * Moved by the width the squeeze took away, 1 - s of the half-width (a
 * quarter at 16:9), a piece is as far from its edge as on the console, in
 * the screen's height. The HUD's few pieces drawn in 3D (the can, the icon of
 * the rival in a race) come right after the flat ones and go with them.
 *
 * The sheet is told by its content, not by where it is in memory.
 * RECOMP_WIDE_HUD=0: the HUD in the middle 4:3, as before. */
static int wide_hud_on(void)
{
    static int on = -1;
    if (on < 0) { const char *v = getenv("RECOMP_WIDE_HUD"); on = !(v && v[0] == '0'); }
    return on;
}
/* The content hash (tex_content) of the texture a stage is bound to, from
 * the texture cache: 0 if it has not been made yet. */
static uint32_t tcache_content_of(const TexStage *t)
{
    uint32_t key = (t->offset ^ (t->width << 16) ^ (t->height << 4) ^ t->color) * 2246822519u, i;
    for (i = 0; i < TEX_CACHE; i++) {
        uint32_t k = (key + i) % TEX_CACHE;
        if (!g_tcache[k].used) break;
        if (g_tcache[k].tex && g_tcache[k].offset == t->offset && g_tcache[k].format == t->color
            && g_tcache[k].w == t->width && g_tcache[k].h == t->height)
            return g_tcache[k].content;
    }
    return 0;
}
/* The sheet's tex_content: the same in every mission measured (City Rush,
 * Dogenzaka Hill, the Fortified Residential Zone), wherever it is loaded. */
#ifndef WIDE_HUD_SHEET
#define WIDE_HUD_SHEET 0x35732389u
#endif
static int wide_hud_sheet(const TexStage *t)
{
    static uint32_t said[8]; static int nsaid;
    static int mode = -1; static uint32_t want;
    uint32_t c;
    int i, hit;
    if (!t->enabled || t->width != 256 || t->height != 256 || t->color != 0x0E) return 0;
    if (mode < 0) {
        /* RECOMP_WIDE_HUD_SHEET (test only): another hash, or @address. */
        const char *v = getenv("RECOMP_WIDE_HUD_SHEET");
        mode = (v && v[0] == '@') ? 1 : 0;
        want = v ? (uint32_t)strtoul(v + mode, NULL, 16) : WIDE_HUD_SHEET;
    }
    c = tcache_content_of(t);
    hit = mode ? t->offset == want : (c != 0 && c == want);
    for (i = 0; i < nsaid; i++) if (said[i] == (t->offset ^ c)) break;
    if (i == nsaid && nsaid < 8) {
        said[nsaid++] = t->offset ^ c;
        fprintf(stderr, "[WIDEHUD] 256x256 DXT3 at %08X, content %08X: %s\n", t->offset, c,
                hit ? "the HUD's sheet" : "not the HUD's");
    }
    return hit;
}

/* One attribute array a draw reads, resolved once per draw: see do_draw. */
typedef struct {
    const uint8_t *base;
    uint32_t stride;
    uint8_t  type, size, idx;
} VtxLive;

/* One vertex, from the console's formats into four floats an attribute. */
static void vtx_convert(const VtxLive *live, int nlive, uint32_t src_i, float *dst)
{
    int j;
    for (j = 0; j < nlive; j++) {
        const uint8_t *p = live[j].base + (size_t)src_i * live[j].stride;
        float *o = dst + j * 4;
        uint32_t c, sz = live[j].size;
        o[0] = o[1] = o[2] = 0.0f; o[3] = 1.0f;
        switch (live[j].type) {
        case 0:   /* D3DCOLOR: one dword, B,G,R,A in memory order */
            o[0] = p[2] * (1.0f / 255.0f); o[1] = p[1] * (1.0f / 255.0f);
            o[2] = p[0] * (1.0f / 255.0f); o[3] = p[3] * (1.0f / 255.0f);
            break;
        case 1:   /* signed short, normalised */
            for (c = 0; c < sz; c++)
                o[c] = ((const int16_t *)p)[c] * (1.0f / 32767.0f);
            break;
        case 2:   /* float */
            for (c = 0; c < sz; c++) o[c] = ((const float *)p)[c];
            break;
        case 4:   /* unsigned byte, normalised, RGBA order */
            for (c = 0; c < sz; c++) o[c] = p[c] * (1.0f / 255.0f);
            break;
        case 5:   /* signed short, not normalised */
            for (c = 0; c < sz; c++) o[c] = (float)((const int16_t *)p)[c];
            break;
        case 6: { /* packed 11/11/10, signed and normalised */
            uint32_t v = *(const uint32_t *)p;
            o[0] = ((int32_t)(v << 21) >> 21) * (1.0f / 1023.0f);
            o[1] = ((int32_t)(v << 10) >> 21) * (1.0f / 1023.0f);
            o[2] = ((int32_t)(v)       >> 22) * (1.0f / 511.0f);
            break;
        }
        default: break;
        }
    }
}

/* The bytes of one vertex that vtx_convert reads for an attribute. */
static uint32_t vtx_bytes(const VtxLive *l)
{
    switch (l->type) {
    case 0: case 6: return 4;
    case 1: case 5: return 2u * l->size;
    case 2:         return 4u * l->size;
    case 4:         return l->size;
    default:        return 0;
    }
}

/*
 * ── geometry kept between frames ─────────────────────────────────────────
 *
 * Every draw used to convert each vertex it drew from the console's formats
 * into floats -- once per index, so a vertex shared by six triangles was
 * converted six times -- and hand the driver a fresh buffer of them: fifteen
 * megabytes a frame in the attract sequence's busy scenes, converted and
 * copied again sixty times a second, though nearly all of it is the city,
 * which never changes. On the cloud test machine that was nine milliseconds
 * of every frame's sixteen.
 *
 * So a draw that indexes its arrays converts the vertices it names once
 * each -- the range from its lowest index to its highest -- and draws them
 * through an index list; and what it converted is kept, so the next frame's
 * draw of the same thing converts and copies nothing. "The same thing" is
 * the same arrays at the same addresses, laid out the same way, over the same
 * range, with the same index list; and it is still the same if the bytes it
 * was converted from still hash as they did. The hash is the price -- one
 * pass over the source bytes, a fraction of what converting them cost -- and
 * it is what makes this safe: geometry the title rewrites (its dynamic
 * buffers, the particles) no longer matches, is converted again, and its
 * buffer refilled.
 *
 * A draw seen once and never again (a particle burst at an address it will
 * not use twice) is not worth a buffer of its own: a draw is kept the second
 * time it is seen, and until then goes through one shared buffer. Small
 * draws, inline vertices, and the 2D draws the widescreen squeeze measures
 * take the old path, as does every draw while a debugging switch is on:
 * those read the converted vertices back, and a kept draw has none.
 *
 * RECOMP_GL_VCACHE=0 turns it off; RECOMP_GL_VCACHE_MB sets how much it may
 * keep (128 MB, 64 on OpenGL ES), the least recently drawn going first.
 */
#define VC_MAX     8192        /* entries, at most */
#define VC_NHASH   16384       /* buckets: a power of two */
#define VC_NSEEN   8192        /* the second-sighting filter: a power of two */
#define VC_MIN_N   32          /* fewer vertices drawn: not worth a lookup */

typedef struct {
    uint64_t key, content;
    uint32_t vb, bytes, last_frame;
    int32_t  next;             /* in its bucket */
    int32_t  newer, older;     /* in drawing order */
    uint8_t  used;
    uint8_t *shadow;           /* RECOMP_GL_VCACHE_VERIFY: what it holds */
} VcEntry;

static VcEntry  *g_vc;
static int32_t   g_vc_bucket[VC_NHASH];
static int32_t   g_vc_newest = -1, g_vc_oldest = -1, g_vc_free = -1;
static int       g_vc_n;
static uint64_t  g_vc_seen[VC_NSEEN];
static uint64_t  g_vc_held, g_vc_budget;
static uint32_t  g_vc_stream;         /* the shared buffer */
static uint8_t  *g_vc_buf;            /* converted vertices, then indices */
static size_t    g_vc_buf_cap;
static int       g_vc_on = -1;
static struct {
    unsigned long hit, changed, kept, stream, old_path, evicted;
    unsigned long long hashed, converted, uploaded;
} g_vc_st;

static uint64_t vc_rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

/* A 64-bit hash of n bytes, four lanes wide (xxHash64's round): a few GB/s,
 * which is what makes checking the source of every kept draw affordable.
 * From NV2A_HASH_MIN bytes on x86-64, the wider one in nv2a_hash.h. */
static uint64_t vc_hash(const void *data, size_t n, uint64_t seed)
{
    const uint64_t P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full,
                   P3 = 0x165667B19E3779F9ull;
    const uint8_t *p = (const uint8_t *)data;
    uint64_t a = seed + P1 + P2, b = seed + P2, c = seed, d = seed - P1, h, w;
    size_t len = n;
#if defined(NV2A_HASH_WIDE)
    if (n >= NV2A_HASH_MIN) return nv2a_hash_wide(data, n, seed);
#endif
    while (n >= 32) {
        uint64_t w0, w1, w2, w3;
        memcpy(&w0, p, 8); memcpy(&w1, p + 8, 8);
        memcpy(&w2, p + 16, 8); memcpy(&w3, p + 24, 8);
        a = vc_rotl(a + w0 * P2, 31) * P1;
        b = vc_rotl(b + w1 * P2, 31) * P1;
        c = vc_rotl(c + w2 * P2, 31) * P1;
        d = vc_rotl(d + w3 * P2, 31) * P1;
        p += 32; n -= 32;
    }
    h = vc_rotl(a, 1) + vc_rotl(b, 7) + vc_rotl(c, 12) + vc_rotl(d, 18) + (uint64_t)len;
    while (n >= 8) {
        memcpy(&w, p, 8);
        h ^= vc_rotl(w * P2, 31) * P1;
        h = vc_rotl(h, 27) * P1 + P3;
        p += 8; n -= 8;
    }
    while (n) { h ^= (uint64_t)(*p++) * P3; h = vc_rotl(h, 11) * P1; n--; }
    h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
    return h;
}

static uint64_t vc_mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xC2B2AE3D27D4EB4Full;
}

/* The debugging switches that read a draw's converted vertices back. All
 * of them are RECOMP_GL_*; these are the RECOMP_GL_* that are not. */
#if defined(_WIN32)
#  define VC_ENVIRON _environ
#elif defined(__APPLE__)
#  include <crt_externs.h>
#  define VC_ENVIRON (*_NSGetEnviron())
#else
extern char **environ;
#  define VC_ENVIRON environ
#endif
static const char *vc_debug_switch(void)
{
    static const char *const fine[] = {
        "SCALE", "READBACK", "CURTAIN", "PRESENTER", "PROFILE", "VCACHE",
        "VCACHE_MB", "VCACHE_VERIFY", "CONST_PART", "ANISO", "MIPMAP", "DEPTHCLAMP",
        "TEXHASH_FULL", "CONTIG_ALL", "HEAP_FIRST", "STICKY_ATTRS",
        /* the frame dumps: taken at the flip, from the picture */
        "DUMP_FB", "DUMP_AFTER", "DUMP_EVERY", "DUMP_3D", "DUMP_TRIS",
        "DUMP_CHARS", "DUMP_DEPTH",
        /* the timeline probe: a draw's number, surface and vertex count */
        "TIMELINE",
        /* DXT textures decoded rather than handed over as they are */
        "DXT", NULL
    };
    static char name[64];
    char **e;
    for (e = VC_ENVIRON; e && *e; e++) {
        const char *s = *e, *eq;
        size_t n;
        int k, ok = 0;
        if (strncmp(s, "RECOMP_GL_", 10)) continue;
        eq = strchr(s, '=');
        n = eq ? (size_t)(eq - s) : strlen(s);
        for (k = 0; fine[k]; k++)
            if (strlen(fine[k]) == n - 10 && !strncmp(s + 10, fine[k], n - 10)) { ok = 1; break; }
        if (ok) continue;
        snprintf(name, sizeof name, "%.*s", (int)n, s);
        return name;
    }
    return NULL;
}

static int vc_on(void)
{
    const char *v, *mb, *why = NULL;
    int i;
    if (g_vc_on >= 0) return g_vc_on;
    v = getenv("RECOMP_GL_VCACHE");
    mb = getenv("RECOMP_GL_VCACHE_MB");
    g_vc_on = (v ? atoi(v) != 0 : 1)
           && G->vbuf_new && G->vbuf_free && G->vbuf_fill && G->vbuf_bind && G->draw_indexed;
    if (g_vc_on && !v && (why = vc_debug_switch()) != NULL) g_vc_on = 0;
#if defined(NV2A_GLES)
    g_vc_budget = (uint64_t)(mb && atoi(mb) > 0 ? atoi(mb) : 64) << 20;
#else
    g_vc_budget = (uint64_t)(mb && atoi(mb) > 0 ? atoi(mb) : 128) << 20;
#endif
    if (g_vc_on) {
        g_vc = (VcEntry *)calloc(VC_MAX, sizeof *g_vc);
        g_vc_stream = g_vc ? G->vbuf_new(1) : 0;
        if (!g_vc || !g_vc_stream) { free(g_vc); g_vc = NULL; g_vc_on = 0; }
    }
    if (g_vc_on) {
        for (i = 0; i < VC_NHASH; i++) g_vc_bucket[i] = -1;
        for (i = 0; i < VC_MAX; i++) g_vc[i].next = i + 1 < VC_MAX ? i + 1 : -1;
        g_vc_free = 0;
    }
    if (g_vc_on)
        fprintf(stderr, "  [GL] vertex cache: on, up to %u MB\n",
                (unsigned)(g_vc_budget >> 20));
    else if (why)
        fprintf(stderr, "  [GL] vertex cache: off while %s is set (it reads "
                "converted vertices back)\n", why);
    else
        fprintf(stderr, "  [GL] vertex cache: off\n");
    return g_vc_on;
}

static int32_t vc_find(uint64_t key)
{
    int32_t e = g_vc_bucket[key & (VC_NHASH - 1)];
    while (e >= 0 && g_vc[e].key != key) e = g_vc[e].next;
    return e;
}

static void vc_unlink(int32_t e)
{
    if (g_vc[e].newer >= 0) g_vc[g_vc[e].newer].older = g_vc[e].older;
    else g_vc_newest = g_vc[e].older;
    if (g_vc[e].older >= 0) g_vc[g_vc[e].older].newer = g_vc[e].newer;
    else g_vc_oldest = g_vc[e].newer;
}

static void vc_link_newest(int32_t e)
{
    g_vc[e].newer = -1;
    g_vc[e].older = g_vc_newest;
    if (g_vc_newest >= 0) g_vc[g_vc_newest].newer = e;
    g_vc_newest = e;
    if (g_vc_oldest < 0) g_vc_oldest = e;
}

static void vc_touch(int32_t e)
{
    if (g_vc_newest != e) { vc_unlink(e); vc_link_newest(e); }
    g_vc[e].last_frame = S.frames;
}

/*
 * RECOMP_GL_VCACHE_VERIFY=1: check every draw the cache serves against the
 * old path -- the vertices converted again from the title's memory, one per
 * index, as they always were -- and say so if any differs. A kept draw's
 * contents are shadowed in memory for it. A test of the cache's idea of
 * "the same draw", not something to play with.
 */
static int vc_verify_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("RECOMP_GL_VCACHE_VERIFY"); v = (e && atoi(e)) ? 1 : 0; }
    return v;
}

static void vc_verify(const VtxLive *live, int nlive, uint32_t stride_floats,
                      uint32_t fan, uint32_t out_n, const uint8_t *data,
                      uint32_t vbytes, int seq, int idx32, const char *what)
{
    static unsigned long checked, bad;
    static const uint32_t q1[6] = { 0, 1, 2, 2, 3, 0 }, q2[6] = { 0, 1, 3, 3, 2, 0 };
    float want[NUM_ATTRS * 4];
    const float *verts = (const float *)data;
    const uint8_t *ib = data + vbytes;
    uint32_t k;
    if (!nlive) return;
    checked++;
    for (k = 0; k < out_n; k++) {
        uint32_t src_i = fan == 1 ? S.idx[(k / 6) * 4 + q1[k % 6]]
                       : fan == 2 ? S.idx[(k / 6) * 2 + q2[k % 6]] : S.idx[k];
        uint32_t at = seq ? k : idx32 ? ((const uint32_t *)ib)[k] : ((const uint16_t *)ib)[k];
        vtx_convert(live, nlive, src_i, want);
        if (memcmp(want, verts + (size_t)at * stride_floats, (size_t)nlive * 16)) {
            if (bad++ < 20)
                fprintf(stderr, "[VCVERIFY] %s: draw %u, vertex %u of %u (index %u) "
                        "is not what the title's memory holds now\n",
                        what, S.draws, k, out_n, src_i);
            break;
        }
    }
    if (checked % 50000 == 0)
        fprintf(stderr, "[VCVERIFY] %lu draws checked, %lu differed\n", checked, bad);
}

/* GL keeps a deleted buffer's storage until the draws already queued from it
 * are done, so this is safe at any point in a frame. */
static void vc_evict(int32_t e)
{
    int32_t *pp = &g_vc_bucket[g_vc[e].key & (VC_NHASH - 1)];
    while (*pp >= 0 && *pp != e) pp = &g_vc[*pp].next;
    if (*pp == e) *pp = g_vc[e].next;
    vc_unlink(e);
    G->vbuf_free(g_vc[e].vb);
    free(g_vc[e].shadow);
    g_vc[e].shadow = NULL;
    g_vc_held -= g_vc[e].bytes;
    g_vc[e].used = 0;
    g_vc[e].next = g_vc_free;
    g_vc_free = e;
    g_vc_n--;
    g_vc_st.evicted++;
}

static int32_t vc_insert(uint64_t key, uint64_t content, uint32_t bytes)
{
    int32_t e;
    uint32_t vb;
    while (g_vc_oldest >= 0 && (g_vc_n >= VC_MAX || g_vc_held + bytes > g_vc_budget))
        vc_evict(g_vc_oldest);
    if (g_vc_free < 0 || bytes > g_vc_budget) return -1;
    vb = G->vbuf_new(0);
    if (!vb) return -1;
    e = g_vc_free;
    g_vc_free = g_vc[e].next;
    g_vc[e].key = key;
    g_vc[e].content = content;
    g_vc[e].vb = vb;
    g_vc[e].bytes = bytes;
    g_vc[e].used = 1;
    g_vc[e].next = g_vc_bucket[key & (VC_NHASH - 1)];
    g_vc_bucket[key & (VC_NHASH - 1)] = e;
    vc_link_newest(e);
    g_vc[e].last_frame = S.frames;
    g_vc_n++;
    g_vc_held += bytes;
    return e;
}

/* The hash of what a draw's vertices are made from: for each array, the
 * bytes from its lowest vertex to its highest. Attributes interleaved in one
 * array (the same stride, spans that meet) are hashed as one span. */
static uint64_t vc_content(const VtxLive *live, int nlive, uint32_t lo, uint32_t hi)
{
    struct { const uint8_t *from, *to; uint32_t stride; } sp[NUM_ATTRS];
    int ns = 0, j, s;
    uint64_t h = 0x3C6EF372FE94F82Bull;
    for (j = 0; j < nlive; j++) {
        uint32_t sz = vtx_bytes(&live[j]);
        const uint8_t *from, *to;
        if (!sz) continue;
        from = live[j].base + (size_t)lo * live[j].stride;
        to   = live[j].base + (size_t)hi * live[j].stride + sz;
        for (s = 0; s < ns; s++)
            if (sp[s].stride == live[j].stride && from <= sp[s].to && to >= sp[s].from)
                break;
        if (s < ns) {
            if (from < sp[s].from) sp[s].from = from;
            if (to > sp[s].to) sp[s].to = to;
        } else {
            sp[ns].from = from; sp[ns].to = to; sp[ns].stride = live[j].stride;
            ns++;
        }
    }
    for (s = 0; s < ns; s++) {
        h = vc_hash(sp[s].from, (size_t)(sp[s].to - sp[s].from), h);
        g_vc_st.hashed += (unsigned long long)(sp[s].to - sp[s].from);
    }
    return h;
}

/* The index list of a draw over its own range: the batch's indices less its
 * lowest, quads turned into two triangles each as the old path did. */
static void vc_indices(uint32_t fan, uint32_t out_n, uint32_t lo, int idx32, void *dst)
{
    static const uint8_t q1[6] = { 0, 1, 2, 2, 3, 0 }, q2[6] = { 0, 1, 3, 3, 2, 0 };
    const uint32_t *ix = S.idx;
    uint16_t *d16 = (uint16_t *)dst;
    uint32_t *d32 = (uint32_t *)dst;
    uint32_t k, b, j;
    if (!fan) {
        if (idx32) for (k = 0; k < out_n; k++) d32[k] = ix[k] - lo;
        else       for (k = 0; k < out_n; k++) d16[k] = (uint16_t)(ix[k] - lo);
        return;
    }
    for (k = 0, b = 0; k < out_n; k += 6, b += (fan == 1 ? 4u : 2u)) {
        const uint8_t *q = fan == 1 ? q1 : q2;
        for (j = 0; j < 6; j++) {
            uint32_t v = ix[b + q[j]] - lo;
            if (idx32) d32[k + j] = v; else d16[k + j] = (uint16_t)v;
        }
    }
}

/*
 * The kept buffer a draw goes through -- its own, found or made, or the
 * shared one -- or 0 for the old path. *idx32 says what size its indices are
 * and *seq that it has none: its vertices are drawn in order.
 */
static uint32_t vc_prepare(const VtxLive *live, int nlive, uint32_t stride_floats,
                           uint32_t n, uint32_t fan, uint32_t out_n,
                           int *idx32, int *seq_out)
{
    const uint32_t *ix = S.idx;
    uint32_t lo = ix[0], hi = ix[0], first = ix[0], k, nuniq, vbytes, ibytes, vb;
    uint64_t key, content;
    int seq = (fan == 0), i32;
    int32_t e;
    size_t need;

    for (k = 1; k < n; k++) {
        uint32_t v = ix[k];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        seq &= (v == first + k);
    }
    nuniq = hi - lo + 1;
    /* Indices scattered over a far larger range than they draw: converting
     * the range would cost more than converting what is drawn. */
    if (nuniq > 2u * out_n + 64u) return 0;
    i32 = nuniq > 65536u;

    /* What the draw is... */
    key = 0x6A09E667F3BCC909ull;
    key = vc_mix(key, stride_floats | ((uint64_t)fan << 32) | ((uint64_t)nlive << 40));
    for (k = 0; k < (uint32_t)nlive; k++) {
        key = vc_mix(key, (uint64_t)(uintptr_t)live[k].base);
        key = vc_mix(key, live[k].stride | ((uint64_t)live[k].type << 32)
                        | ((uint64_t)live[k].size << 40) | ((uint64_t)live[k].idx << 48));
    }
    key = vc_mix(key, lo | ((uint64_t)nuniq << 32));
    key = vc_mix(key, n);
    if (!seq) {
        key = vc_mix(key, vc_hash(ix, (size_t)n * 4, key));
        g_vc_st.hashed += (unsigned long long)n * 4;
    }
    /* ...and whether what it is made of is still what it was. */
    content = vc_content(live, nlive, lo, hi);

    e = vc_find(key);
    vbytes = nuniq * stride_floats * (uint32_t)sizeof(float);
    ibytes = seq ? 0 : out_n * (i32 ? 4u : 2u);
    if (e >= 0 && g_vc[e].content == content) {
        vc_touch(e);
        g_vc_st.hit++;
        if (g_vc[e].shadow)
            vc_verify(live, nlive, stride_floats, fan, out_n, g_vc[e].shadow,
                      vbytes, seq, i32, "found");
        *idx32 = i32; *seq_out = seq;
        return g_vc[e].vb;
    }

    need = (size_t)vbytes + ibytes;
    if (g_vc_buf_cap < need) {
        free(g_vc_buf);
        g_vc_buf_cap = need + 65536;
        g_vc_buf = (uint8_t *)malloc(g_vc_buf_cap);
        if (!g_vc_buf) { g_vc_buf_cap = 0; return 0; }
    }
    for (k = 0; k < nuniq; k++)
        vtx_convert(live, nlive, lo + k, (float *)g_vc_buf + (size_t)k * stride_floats);
    g_vc_st.converted += nuniq;

    if (e >= 0) {
        /* Rewritten since it was kept: the same buffer, new vertices. */
        vb = g_vc[e].vb;
        g_vc[e].content = content;
        vc_touch(e);
        g_vc_st.changed++;
        { PROF_T0(); G->vbuf_fill(vb, (const float *)g_vc_buf, vbytes, NULL, 0); PROF_ADD(vbo); }
        g_vc_st.uploaded += vbytes;
        g_prof.vbo_bytes += vbytes;
        if (g_vc[e].shadow) {
            memcpy(g_vc[e].shadow, g_vc_buf, vbytes);
            vc_verify(live, nlive, stride_floats, fan, out_n, g_vc[e].shadow,
                      vbytes, seq, i32, "rewritten");
        }
        *idx32 = i32; *seq_out = seq;
        return vb;
    }
    if (!seq) vc_indices(fan, out_n, lo, i32, g_vc_buf + vbytes);
    if (vc_verify_on())
        vc_verify(live, nlive, stride_floats, fan, out_n, g_vc_buf, vbytes, seq, i32, "new");
    { uint32_t slot = (uint32_t)(key >> 20) & (VC_NSEEN - 1);
      if (g_vc_seen[slot] == key
          && (e = vc_insert(key, content, vbytes + ibytes)) >= 0) {
          vb = g_vc[e].vb;
          g_vc_st.kept++;
          if (vc_verify_on() && (g_vc[e].shadow = (uint8_t *)malloc(need)) != NULL)
              memcpy(g_vc[e].shadow, g_vc_buf, need);
      } else {
          g_vc_seen[slot] = key;
          vb = g_vc_stream;
          g_vc_st.stream++;
      } }
    { PROF_T0();
      G->vbuf_fill(vb, (const float *)g_vc_buf, vbytes,
                   seq ? NULL : g_vc_buf + vbytes, ibytes);
      PROF_ADD(vbo); }
    g_vc_st.uploaded += (unsigned long long)vbytes + ibytes;
    g_prof.vbo_bytes += (unsigned long)vbytes + ibytes;
    *idx32 = i32; *seq_out = seq;
    return vb;
}

static void vc_report(const char *when)
{
    if (!g_vc) return;
    fprintf(stderr, "  [GL] vertex cache%s: %d kept (%llu KB); draws %lu found, "
            "%lu found rewritten, %lu newly kept, %lu one-off, %lu the old way; "
            "%lu let go; %llu K vertices converted, %llu KB hashed, %llu KB sent\n",
            when, g_vc_n, (unsigned long long)(g_vc_held >> 10),
            g_vc_st.hit, g_vc_st.changed, g_vc_st.kept, g_vc_st.stream,
            g_vc_st.old_path, g_vc_st.evicted,
            g_vc_st.converted / 1000, g_vc_st.hashed >> 10, g_vc_st.uploaded >> 10);
}

/* How big batches get, against the 16384 indices and 16384 inline words
 * that used to be the limit (see MAX_INDICES): every batch that would have
 * been cut then is counted, the first few described; and a batch that fills
 * even today's room says so. */
static void batch_size_note(int inl, uint32_t n)
{
    static unsigned long over_idx, over_inl, said, full;
    static uint32_t big_idx, big_inl;
    if (inl) {
        if (S.inline_count >= MAX_INLINE && full++ < 4)
            fprintf(stderr, "  [GL] draw %u: an inline batch filled %u words and was cut\n", S.draws, MAX_INLINE);
        if (S.inline_count <= 16384) return;
        over_inl++;
        if (S.inline_count > big_inl) big_inl = S.inline_count;
    } else {
        if (n >= MAX_INDICES && full++ < 4)
            fprintf(stderr, "  [GL] draw %u: a batch filled %u indices and was cut\n", S.draws, MAX_INDICES);
        if (n <= 16384) return;
        over_idx++;
        if (n > big_idx) big_idx = n;
    }
    if (said < 12 || (over_idx + over_inl) % 1000 == 0) {
        said++;
        fprintf(stderr, "  [GL] big batch: draw %u flip %u prim %u %s %u %s -- past the old 16384 "
                        "(%lu index batches so far, largest %u; %lu inline, largest %u words)\n",
                S.draws, S.flips, S.prim, S.xf_mode == NV2A_XF_MODE_FIXED ? "ff" : "vp",
                inl ? S.inline_count : n, inl ? "inline words" : "indices",
                over_idx, big_idx, over_inl, big_inl);
    }
}

/* Draws that never reach the backend, and why -- printed with the census
 * below. A missing piece of the world is either never sent, or sent and
 * dropped here, or drawn and hidden; this says whether it is the second. */
enum { DROP_PRIM, DROP_POINTS, DROP_NOVSIZE, DROP_EMPTY, DROP_NOPOS,
       DROP_NOPROG, DROP_SHORT, DROP_NOMEM, DROP_LOST, DROP_N };
static unsigned long g_drop[DROP_N];

/* RECOMP_GL_PRIMLOG=<s>: draws and vertices by primitive type (the
 * NV097_SET_BEGIN_END numbers: 1 points, 2 lines, 3 line loop, 4 line strip,
 * 5 triangles, 6 strip, 7 fan, 8 quads, 9 quad strip, 10 polygon), fixed-
 * function and programmable apart, every s seconds -- what a scene is drawn
 * with, when something in it is missing. */
static void prim_note(int inl)
{
    static int on = -1; static double every, next;
    static unsigned long draws[2][11], verts[2][11];
    uint32_t n = inl ? S.inline_count : S.idx_count, p = S.prim;
    int k, i;
    if (on < 0) { const char *v = getenv("RECOMP_GL_PRIMLOG");
                  on = v ? 1 : 0; every = v ? atof(v) : 0.0; if (every <= 0) every = 10.0;
                  next = mono_now() + every; }
    if (!on || p > 10) return;
    k = (S.xf_mode == NV2A_XF_MODE_FIXED) ? 0 : 1;
    draws[k][p]++; verts[k][p] += n;
    if (mono_now() >= next) {
        next = mono_now() + every;
        for (k = 0; k < 2; k++) {
            fprintf(stderr, "[PRIMS] %s:", k ? "programs" : "fixed-function");
            for (i = 1; i <= 10; i++)
                if (draws[k][i]) fprintf(stderr, " p%d %lu draws %lu verts;", i, draws[k][i], verts[k][i]);
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "[PRIMS] not drawn: %lu unknown primitive, %lu points, %lu inline without "
                        "attributes, %lu empty, %lu no position, %lu no program, %lu too short, "
                        "%lu no memory, %lu surface lost\n",
                g_drop[DROP_PRIM], g_drop[DROP_POINTS], g_drop[DROP_NOVSIZE], g_drop[DROP_EMPTY],
                g_drop[DROP_NOPOS], g_drop[DROP_NOPROG], g_drop[DROP_SHORT], g_drop[DROP_NOMEM],
                g_drop[DROP_LOST]);
        fflush(stderr);
    }
}

/*
 * RECOMP_GL_POINTLOG=<s>: the point lists the title draws -- which are not
 * drawn (see do_draw_body) -- a line each for the first few, with the point
 * state, then a count every s seconds.
 */
static void points_note(int inl)
{
    static int on = -1, lines; static double every, next;
    static unsigned long draws[2], verts[2];
    int k;
    uint32_t n = inl ? S.inline_count : S.idx_count;
    if (on < 0) { const char *v = getenv("RECOMP_GL_POINTLOG");
                  on = v ? 1 : 0; every = v ? atof(v) : 0.0; if (every <= 0) every = 10.0;
                  next = mono_now() + every; }
    if (!on) return;
    k = (S.xf_mode == NV2A_XF_MODE_FIXED) ? 0 : 1;
    draws[k]++; verts[k] += n;
    if (lines < 24) {
        lines++;
        fprintf(stderr, "[POINTS] draw %u flip %u %s start %u, %u %s; size %u/8, params %s "
                "(%g %g %g %g %g %g %g %g), smooth %s; tex0 %s %ux%u fmt %08X; blend %d, depth test %d mask %d\n",
                S.draws, S.flips, k ? "prog" : "ff", S.prog_start, n, inl ? "inline words" : "indices",
                S.point_size, S.point_params_enable ? "on" : "off",
                S.point_params[0], S.point_params[1], S.point_params[2], S.point_params[3],
                S.point_params[4], S.point_params[5], S.point_params[6], S.point_params[7],
                S.point_smooth_enable ? "on" : "off",
                S.tex[0].enabled ? "on" : "off", S.tex[0].width, S.tex[0].height, S.tex[0].format,
                S.blend_enable, S.depth_test, S.depth_mask);
    }
    if (mono_now() >= next) {
        next = mono_now() + every;
        fprintf(stderr, "[POINTS] so far: %lu fixed-function draws (%lu), %lu programmable (%lu)\n",
                draws[0], verts[0], draws[1], verts[1]);
    }
}

/*
 * RECOMP_GL_TEXGENLOG=<s>: the fixed-function batches that generate texture
 * coordinates -- a line for each new arrangement, with its texture matrix,
 * then a count every s seconds. RECOMP_GL_SKIP_TEXGEN=1 leaves them out of the
 * picture, to see what they are (=ab: every other frame). Returns 1 to drop
 * the draw.
 */
static int texgen_note(uint32_t n)
{
    static int on = -1, skip, lines; static double every, next;
    static unsigned long draws, tris;
    static uint32_t seen[64]; static int nseen;
    int s, c, any = 0;
    uint32_t key = 0;
    if (on < 0) { const char *v = getenv("RECOMP_GL_TEXGENLOG");
                  on = v ? 1 : 0; every = v ? atof(v) : 0.0; if (every <= 0) every = 10.0;
                  next = mono_now() + every;
                  { const char *k = getenv("RECOMP_GL_SKIP_TEXGEN");
                    skip = !k ? 0 : (*k == 'a' || *k == 'A') ? 2 : 1; } }
    if (!on && !skip) return 0;
    for (s = 0; s < 4; s++)
        for (c = 0; c < 4; c++)
            if (S.texgen[s][c]) { any = 1; key = key * 31u + (uint32_t)(s * 4 + c) * 0x10001u + S.texgen[s][c]; }
    if (!any) return 0;
    if (on) {
        int i, found = 0;
        draws++; tris += n / 3;
        key = key * 31u + S.ff_texmat_enable * 7u + (uint32_t)S.ff_lighting * 3u
            + (S.attr[2].size ? 1u : 0u) + (S.tex[0].enabled ? 0x100u : 0u) + (S.tex[1].enabled ? 0x200u : 0u);
        for (i = 0; i < nseen; i++) if (seen[i] == key) { found = 1; break; }
        if (!found && nseen < 64 && lines < 40) {
            const float *m = &S.u.ff_texmat[0][0];
            seen[nseen++] = key; lines++;
            fprintf(stderr, "[TEXGEN] draw %u flip %u n %u: stage0 %04X %04X %04X %04X stage1 %04X %04X %04X %04X; "
                    "texmat %X; lit %d normal array %d normalize %u; tex0 %s %ux%u fmt %08X tex1 %s %ux%u fmt %08X; "
                    "texmat0 [%g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g]\n",
                    S.draws, S.flips, n,
                    S.texgen[0][0], S.texgen[0][1], S.texgen[0][2], S.texgen[0][3],
                    S.texgen[1][0], S.texgen[1][1], S.texgen[1][2], S.texgen[1][3],
                    S.ff_texmat_enable, S.ff_lighting, S.attr[2].size ? 1 : 0, S.normalization,
                    S.tex[0].enabled ? "on" : "off", S.tex[0].width, S.tex[0].height, S.tex[0].format,
                    S.tex[1].enabled ? "on" : "off", S.tex[1].width, S.tex[1].height, S.tex[1].format,
                    m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                    m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
        }
        if (mono_now() >= next) {
            next = mono_now() + every;
            fprintf(stderr, "[TEXGEN] so far: %lu draws, %lu triangles\n", draws, tris);
        }
    }
    /* =ab: every other displayed frame without them */
    return skip == 2 ? (int)(S.frames & 1u) : skip;
}

static void do_draw_body(void);
/* A draw, timed whole for the profile (it has a great many ways out). */
static void do_draw(void)
{
    if (PROF_ON()) {
        double t0 = prof_now();
        do_draw_body();
        g_prof.whole_draw += prof_now() - t0;
    } else {
        do_draw_body();
    }
}

static void do_draw_body(void)
{
    static float *vbuf;
    static uint32_t vbuf_cap;
    uint32_t n, i, k, out_n, fan_expand;
    int mode;
    int slot;
    int inl = (S.inline_count != 0);
    uint32_t stride_floats = NUM_ATTRS * 4;
    /* The vertex layout this draw actually needs. See where it is built. */
    VtxLive  live[NUM_ATTRS];
    int      nlive = 0;
    int      attr_at[NUM_ATTRS];      /* float offset in the vertex, or -1 */
    uint32_t const_mask = 0;          /* read, but supplied as a constant */
    /* The kept buffer it is drawn from (the vertex cache), or 0: the old
     * path, vbuf. */
    uint32_t vc_vb = 0;
    int      vc_i32 = 0, vc_seq = 0, wide2d = 0;

    if (!g_ready || !S.prim) return;
    surface_sync();

    mode = nv2a_prim(S.prim, &fan_expand);
    prim_note(inl);
    if (mode == NV2A_PRIM_POINTS) points_note(inl);
    /* Points included: this tested a GLenum against zero, and GL_POINTS is
     * zero, so point lists have never been drawn. Kept that way here so the
     * two backends agree; drawing them is a change to make on purpose. */
    if (!mode || mode == NV2A_PRIM_POINTS) { g_drop[mode ? DROP_POINTS : DROP_PRIM]++; return; }

    /* Indices: an explicit list, a DRAW_ARRAYS range, or the implicit
     * 0..n-1 of an inline batch. */
    if (inl) {
        uint32_t vsize = 0;
        for (i = 0; i < NUM_ATTRS; i++) {
            const Attr *a = &S.attr[i];
            uint32_t bytes = !a->size ? 0
                           : (a->type == 0) ? 4
                           : (a->type == 2) ? 4 * a->size
                           : (a->type == 1 || a->type == 5) ? 2 * a->size
                           : (a->type == 6) ? 4 : a->size;
            if (!bytes) continue;
            bytes = (bytes + 3) & ~3u;
            S.attr[i].offset = vsize;
            vsize += bytes;
        }
        if (!vsize) { g_drop[DROP_NOVSIZE]++; return; }
        for (i = 0; i < NUM_ATTRS; i++)
            if (S.attr[i].size) S.attr[i].stride = vsize;
        n = S.inline_count * 4 / vsize;
        if (n > MAX_INDICES) n = MAX_INDICES;
        for (i = 0; i < n; i++) S.idx[i] = i;
        S.idx_count = n;
    } else {
        /* Both kinds of batch arrive here now: NV097_ARRAY_ELEMENT appends
         * indices, and NV097_DRAW_ARRAYS appends the ranges it names. */
        n = S.idx_count;
    }
    batch_size_note(inl, n);
    if (n < 1) { g_drop[DROP_EMPTY]++; return; }

    /* The attributes this draw fetched from arrays keep its last vertex's
     * values (see sticky_attrs). Done before anything below can drop the
     * draw, because the hardware would have fetched it regardless; the
     * draw's own constant attributes are the ones with no array, which this
     * leaves alone. */
    if (sticky_attrs()) {
        uint32_t last = S.idx[n - 1];
        for (i = 0; i < NUM_ATTRS; i++) {
            const Attr *a = &S.attr[i];
            if (!a->size || !a->stride || (!inl && !a->offset)) continue;
            /* This reads arrays the draw's program may not read -- the
             * hardware fetches every enabled one -- so an array left enabled
             * with a stale address is read here and nowhere else. Only
             * where guest memory is known to be: RAM, the contiguous
             * window, the tiled alias. */
            if (!inl) {
                uint32_t va = resolve(a->offset);
                uint64_t end = (uint64_t)va + (uint64_t)(last + 1) * a->stride;
                if (!((end <= 0x04000000u)
                      || (va >= XBOX_CONTIG_BASE && end <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)
                      || (va >= 0xF0000000u && end <= 0xF4000000u)))
                    continue;
            }
            fetch(a, last, S.const_attr[i], inl);
        }
    }
    /* RECOMP_GL_STICKYLOG=1: every lit fixed-function draw with no normal
     * array, and the normal it inherits; and the fixed-function draws with
     * no diffuse array whose inherited diffuse is not white. */
    { static int sl = -1, nl;
      if (sl < 0) sl = getenv("RECOMP_GL_STICKYLOG") ? 1 : 0;
      if (sl && nl < 120 && S.xf_mode == NV2A_XF_MODE_FIXED) {
          if (S.ff_lighting && !S.attr[2].size) {
              nl++;
              fprintf(stderr, "[STICKY] draw %u flip %u lit, no normal array: "
                      "normal (%.3f %.3f %.3f %.3f) n=%u\n", S.draws, S.flips,
                      S.const_attr[2][0], S.const_attr[2][1],
                      S.const_attr[2][2], S.const_attr[2][3], n);
          }
          if (!S.attr[3].size && (S.const_attr[3][0] != 1.0f || S.const_attr[3][1] != 1.0f
                                  || S.const_attr[3][2] != 1.0f || S.const_attr[3][3] != 1.0f)) {
              nl++;
              fprintf(stderr, "[STICKY] draw %u flip %u %s, no diffuse array: "
                      "diffuse (%.3f %.3f %.3f %.3f)\n", S.draws, S.flips,
                      S.ff_lighting ? "lit" : "unlit",
                      S.const_attr[3][0], S.const_attr[3][1],
                      S.const_attr[3][2], S.const_attr[3][3]);
          }
      } }

    if (!S.attr[0].size) { S.skipped_no_pos++; g_drop[DROP_NOPOS]++; return; }
    /* RECOMP_GL_NO_TEX1: drop every draw that has a second texture stage.
     * JSRF paints its cel shading as a second blended pass over the same
     * geometry, so this shows what is underneath -- which separates "the base
     * pass is wrong" from "the overlay is covering it". */
    { static int no_t1 = -1;
      if (no_t1 < 0) no_t1 = getenv("RECOMP_GL_NO_TEX1") ? 1 : 0;
      if (no_t1 && S.tex[1].enabled) return; }
    /* RECOMP_GL_ONLY_UNTEX / _ONLY_TEX: show one half of the frame's draws.
     * Most of this screen is covered by geometry drawn with no texture stage
     * bound, and the question that matters is what that geometry IS -- the
     * city, which would mean we are losing its texture, or sky and effect
     * planes, which are meant to be flat. Drawing one set without the other
     * answers it in one frame. */
    { static int half = -2;
      if (half == -2) half = getenv("RECOMP_GL_ONLY_UNTEX") ? 1
                           : getenv("RECOMP_GL_ONLY_TEX")   ? 2 : 0;
      if (half == 1 && S.psh.tex_bound[0]) return;
      if (half == 2 && !S.psh.tex_bound[0]) return; }
    /* RECOMP_GL_ONLY_XF=<mode>: draw only the batches that went through one
     * of the two transform pipelines.
     *
     * The scenery is fixed-function and the characters are programmable, so
     * this separates them cleanly -- which is the question behind the people
     * standing in mid-air. Drawing the level alone says whether the surface
     * they should be on is missing; drawing the characters alone says
     * whether they are where the level would have put them. One frame each
     * answers it, and neither answer can be reached by looking at the two
     * drawn on top of each other. */
    { static long only_xf = -2;
      if (only_xf == -2) { const char *v = getenv("RECOMP_GL_ONLY_XF");
                           only_xf = v ? atol(v) : -1; }
      if (only_xf >= 0 && (long)S.xf_mode != only_xf) return; }
    if (S.xf_mode == NV2A_XF_MODE_FIXED && texgen_note(n)) return;
    /* RECOMP_GL_SKIPVOL=1 / =2 (a probe): leave out the shadow volumes drawn
     * by vertex programs (the characters') / by the fixed-function pipeline
     * (the scenery's): the batches that only count in the stencil buffer. */
    { static int sv = -1;
      if (sv < 0) { const char *e = getenv("RECOMP_GL_SKIPVOL"); sv = e ? atoi(e) : 0; }
      if (sv && S.stencil_en && S.stencil_func == 0x207
          && (S.stencil_op[2] == 0x1E02 || S.stencil_op[2] == 0x1E03)
          && (sv == 1) == (S.xf_mode != NV2A_XF_MODE_FIXED)) return; }
    /* RECOMP_GL_SKIP_LIT=1: leave out the fixed-function batches the T&L
     * unit lights (=ab: every other displayed frame), to see what they are. */
    { static int sk = -1;
      if (sk < 0) { const char *k = getenv("RECOMP_GL_SKIP_LIT");
                    sk = !k ? 0 : (*k == 'a' || *k == 'A') ? 2 : 1; }
      if (sk && S.xf_mode == NV2A_XF_MODE_FIXED && S.ff_lighting
          && (sk == 1 || (S.frames & 1u))) return; }

    slot = program_for_current();
    if (slot < 0) { g_drop[DROP_NOPROG]++; return; }

    /* RECOMP_GL_ONLY_A0=1 draws only the batches that pick their transform
     * out of the constant palette; =0 draws only the rest.
     *
     * ONLY_XF above splits by transform pipeline, which for this title turns
     * out not to be the interesting line -- almost everything in the 3D
     * scene is programmable, so it separates the 2D screens from the world
     * and nothing else. The line that matters is the one the player drew: the
     * things that move against the things that do not. A moving object picks
     * its matrix with the address register and a piece of the city does not,
     * so that flag is the split, exactly.
     *
     * And it settles the question every theory so far has quietly assumed
     * one way. Draw the world alone and see whether the road under the bus
     * is there. Draw the moving things alone and see where they sit in an
     * empty frame. Five theories have died assuming the objects were
     * misplaced; not one of them checked that the world was complete. */
    { static int only_a0 = -2;
      if (only_a0 == -2) { const char *v = getenv("RECOMP_GL_ONLY_A0");
                           only_a0 = v ? atoi(v) : -1; }
      if (only_a0 >= 0 && (g_pcache[slot].uses_a0 ? 1 : 0) != only_a0) return; }

    /* Quad expansion: 0,1,2 2,3,0 per quad; a strip pairs successive edges. */
    out_n = n;
    if (fan_expand == 1) out_n = (n / 4) * 6;
    if (fan_expand == 2) out_n = (n >= 4) ? ((n - 2) / 2) * 6 : 0;
    if (!out_n) { g_drop[DROP_SHORT]++; return; }

    /*
     * The vertex layout, cut down to what this draw actually needs.
     *
     * Every vertex used to be expanded to all sixteen attributes, four floats
     * each, whether the program read them or not: 256 bytes a vertex and
     * sixteen decode calls a vertex regardless. Measured on an M2 Pro in the
     * title-screen level, that was 39 MB of vertex data and 32 million decode
     * calls a second -- and the profile said 93% of the frame was spent
     * outside the GL calls entirely, which is where those decode calls live.
     *
     * Three things come out of it:
     *
     *   - An attribute the program does not read is not fetched at all.
     *   - An attribute the program reads but the title supplies no array for
     *     is a constant for the whole draw, so it goes in with
     *     glVertexAttrib4f once instead of being written into every vertex.
     *   - The base pointer, stride, type and size are resolved once per draw
     *     rather than recomputed per vertex per attribute. The address
     *     translation alone was running thirty-two million times a second.
     *
     * A JSRF vertex reads four or five attributes, so this is the difference
     * between 256 bytes a vertex and about 80.
     */
    {
        uint32_t inputs = g_pcache[slot].inputs;
        for (i = 0; i < NUM_ATTRS; i++) {
            const Attr *a = &S.attr[i];
            attr_at[i] = -1;
            if (!(inputs & (1u << i))) continue;
            if (!a->size || !a->stride || (!inl && !a->offset)) {
                const_mask |= 1u << i;
                continue;
            }
            live[nlive].base = inl
                ? (const uint8_t *)S.inline_buf + a->offset
                : guest(resolve(a->offset));
            live[nlive].stride = a->stride;
            live[nlive].type   = (uint8_t)a->type;
            live[nlive].size   = (uint8_t)(a->size > 4 ? 4 : a->size);
            live[nlive].idx    = (uint8_t)i;
            attr_at[i] = nlive * 4;
            nlive++;
        }
        stride_floats = (uint32_t)nlive * 4;
        if (!stride_floats) stride_floats = 4;   /* a stride of zero is not a
                                                  * layout GL will accept */
    }

    /* RECOMP_GL_STICKYLOG: the draws that read an inherited attribute value
     * different from the one the title last set -- everything the sticky
     * attributes change. A line each for the first few, then a count per
     * attribute every 120 flips. */
    { static int sl = -1, nl; static uint32_t cnt[NUM_ATTRS], since = 0, any;
      if (sl < 0) sl = getenv("RECOMP_GL_STICKYLOG") ? 1 : 0;
      if (sl && const_mask) {
          for (i = 0; i < NUM_ATTRS; i++) {
              if (!(const_mask & (1u << i))) continue;
              if (!memcmp(S.const_attr[i], S.const_set[i], sizeof S.const_set[i])) continue;
              cnt[i]++; any = 1;
              /* One line per program and attribute the first time it is
               * seen, with the program's disassembly once per program. */
              { static uint32_t seen_key[64]; static int nseen;
                static int seen_slot[64]; static int nslots;
                uint32_t key = ((uint32_t)slot << 8) | i;
                int q, found = 0;
                for (q = 0; q < nseen; q++) if (seen_key[q] == key) { found = 1; break; }
                if (!found && nseen < 64 && nl < 80) {
                    seen_key[nseen++] = key; nl++;
                    fprintf(stderr, "[STICKY] draw %u flip %u %s slot %d start %u n %u attr %u inherited "
                            "(%.3f %.3f %.3f %.3f), set (%.3f %.3f %.3f %.3f); arrays",
                            S.draws, S.flips,
                            S.xf_mode == NV2A_XF_MODE_FIXED ? "ff" : "prog", slot,
                            S.prog_start, n, i,
                            S.const_attr[i][0], S.const_attr[i][1],
                            S.const_attr[i][2], S.const_attr[i][3],
                            S.const_set[i][0], S.const_set[i][1],
                            S.const_set[i][2], S.const_set[i][3]);
                    for (q = 0; q < NUM_ATTRS; q++)
                        if (S.attr[q].size)
                            fprintf(stderr, " v%d(t%u s%u)", q, S.attr[q].type, S.attr[q].size);
                    fprintf(stderr, " inputs %04X\n", g_pcache[slot].inputs);
                    found = 0;
                    for (q = 0; q < nslots; q++) if (seen_slot[q] == slot) { found = 1; break; }
                    if (!found && nslots < 64) {
                        seen_slot[nslots++] = slot;
                        if (g_pcache[slot].dis) fputs(g_pcache[slot].dis, stderr);
                    }
                } }
          }
      }
      if (sl && any && S.flips - since >= 120u) {
          fprintf(stderr, "[STICKY] flips %u..%u, draws reading an inherited value:", since, S.flips);
          for (i = 0; i < NUM_ATTRS; i++)
              if (cnt[i]) fprintf(stderr, " attr%u=%u", i, cnt[i]);
          fprintf(stderr, "\n");
          memset(cnt, 0, sizeof cnt); any = 0; since = S.flips;
      } }

    /*
     * RECOMP_GL_DMACENSUS=1: where every draw's position array is routed.
     *
     * The DMA offset the title puts in SET_VERTEX_DATA_ARRAY_OFFSET is
     * whatever MmGetPhysicalAddress gave it, and here that is the VA itself.
     * A contiguous allocation is a VA in the window (0x80000000 + P, or P if
     * D3D masked it), a heap allocation is a VA from 0x00F80000 up. The
     * resolver decides by "is it below the arena's high-water mark", which
     * is right for P and wrong for a heap VA the moment the arena passes
     * 15.5 MB. Count the draws in each class, with the mark, so the size of
     * the ambiguous class -- heap VA, below the mark, currently read from
     * the window -- is a number and not a theory. For those, also say
     * whether the window bytes it reads are zero (fresh pages: geometry
     * silently absent) and whether the heap bytes it should read are not.
     */
    { static int dc = -1; static uint64_t tri[5], nd[5], zwin, nzheap;
      static uint32_t dlast, mark;
      if (dc < 0) dc = getenv("RECOMP_GL_DMACENSUS") ? 1 : 0;
      if (dc && !inl) {
          uint32_t off = S.attr[0].offset; int cls;
          mark = xbox_ContiguousAllocatedBytes();
          if (off & 0x80000000u)      cls = 0;   /* window VA outright */
          else if (off < GL_HEAP_BASE) cls = off < mark ? 1 : 4;
          else                         cls = off < mark ? 2 : 3;
          tri[cls] += out_n / 3; nd[cls]++;
          if (cls == 2) {
              const uint32_t *w = (const uint32_t *)guest(XBOX_CONTIG_BASE + off);
              const uint32_t *h = (const uint32_t *)guest(off);
              int q, wz = 1, hz = 1;
              for (q = 0; q < 16; q++) { if (w[q]) wz = 0; if (h[q]) hz = 0; }
              zwin += wz; nzheap += !hz;
          }
      }
      if (dc && S.draws - dlast >= 200000u) {
          dlast = S.draws;
          fprintf(stderr, "  [DMA] position arrays by route (arena mark %u bytes = 0x%08X, heap from 0x%08X):\n",
                  mark, mark, GL_HEAP_BASE);
          fprintf(stderr, "  [DMA]   window VA (bit31)      : %8llu draws %10llu tris\n"
                          "  [DMA]   contiguous offset      : %8llu draws %10llu tris\n"
                          "  [DMA]   HEAP VA BELOW MARK     : %8llu draws %10llu tris  (window bytes zero: %llu, heap bytes nonzero: %llu)\n"
                          "  [DMA]   heap VA above mark     : %8llu draws %10llu tris\n"
                          "  [DMA]   low VA above mark      : %8llu draws %10llu tris\n",
                  (unsigned long long)nd[0], (unsigned long long)tri[0],
                  (unsigned long long)nd[1], (unsigned long long)tri[1],
                  (unsigned long long)nd[2], (unsigned long long)tri[2],
                  (unsigned long long)zwin, (unsigned long long)nzheap,
                  (unsigned long long)nd[3], (unsigned long long)tri[3],
                  (unsigned long long)nd[4], (unsigned long long)tri[4]);
          fflush(stderr);
      } }

    /* The 2D draws the widescreen squeeze measures (below): it reads their
     * converted positions back, so they take the old path. */
    wide2d = nv2a_wide_scene() && S.xf_mode != NV2A_XF_MODE_FIXED
          && g_pcache[slot].inputs == 0x1F9B && attr_at[0] >= 0
          && S.clip_w == 640 && S.clip_h == 480;

    { double vbo0 = g_prof.vbo;   /* vc_prepare's uploads count as vbo */
      PROF_T0();
    if (!inl && out_n >= VC_MIN_N && !wide2d && vc_on())
        vc_vb = vc_prepare(live, nlive, stride_floats, n, fan_expand, out_n,
                           &vc_i32, &vc_seq);
    if (!vc_vb) {
        if (g_vc) g_vc_st.old_path++;
        if (vbuf_cap < out_n * stride_floats) {
            vbuf_cap = out_n * stride_floats + 4096;
            free(vbuf);
            vbuf = (float *)malloc(vbuf_cap * sizeof(float));
            if (!vbuf) { vbuf_cap = 0; g_drop[DROP_NOMEM]++; return; }
        }
        for (k = 0; k < out_n; k++) {
            uint32_t src_i;
            if (fan_expand == 1) {
                static const uint32_t q[6] = { 0, 1, 2, 2, 3, 0 };
                src_i = S.idx[(k / 6) * 4 + q[k % 6]];
            } else if (fan_expand == 2) {
                static const uint32_t q[6] = { 0, 1, 3, 3, 2, 0 };
                src_i = S.idx[(k / 6) * 2 + q[k % 6]];
            } else {
                src_i = S.idx[k];
            }
            vtx_convert(live, nlive, src_i, &vbuf[k * stride_floats]);
        }
    }
    if (PROF_ON()) g_prof.verts += prof_now() - _t0 - (g_prof.vbo - vbo0); }

    /* The program behind a slot, and the constants it reads, printed once.
     *
     * Every measurement so far has said the texture coordinate arrives in the
     * vertex buffer correct and leaves the shader zero. Between those two
     * facts sits exactly one program, and the only thing it can be doing is
     * transforming the coordinate by constants that are not what the title
     * thinks they are. Printing the instructions and the numbers together is
     * the whole question in one page. */
    if ((dump_slot() >= 0 && slot == dump_slot())
     || (dump_big() > 0 && (long)out_n >= dump_big())) {
        static int done;
        static long want_n = -2, want_xf = -2;
        if (want_n == -2) { const char *v = getenv("RECOMP_GL_DUMP_N");
                            const char *x = getenv("RECOMP_GL_DUMP_XF");
                            want_n = v ? atol(v) : 1;
                            want_xf = x ? atol(x) : -1; }
        if (done < want_n && (want_xf < 0 || (long)S.xf_mode == want_xf)) {
            done++;
            fprintf(stderr, "===== slot %d, inputs %04X, prog_start %u, "
                    "xf_mode %u, %u verts =====\n",
                    slot, g_pcache[slot].inputs, S.prog_start,
                    S.xf_mode, out_n);
            /* The fixed-function pipeline keeps its matrices in the same
             * constant file the programs read, at slots the hardware fixes:
             * c[0..3] is the composite (model x view x projection) matrix,
             * c[58]/c[59] the viewport scale and offset. Printing them
             * unconditionally says whether a fixed-function batch has a real
             * matrix waiting for it or nothing at all. */
            { int k;
              for (k = 0; k < 4; k++)
                  fprintf(stderr, "  CMAT row %d  c[%d] = %12.4f %12.4f %12.4f %12.4f\n",
                          k, k, S.u.c[k][0], S.u.c[k][1],
                          S.u.c[k][2], S.u.c[k][3]);
              fprintf(stderr, "  VPSCL  c[58] = %12.4f %12.4f %12.4f %12.4f\n"
                              "  VPOFF  c[59] = %12.4f %12.4f %12.4f %12.4f\n"
                              "  T0MAT  c[68] = %12.4f %12.4f %12.4f %12.4f\n",
                      S.u.c[58][0], S.u.c[58][1], S.u.c[58][2], S.u.c[58][3],
                      S.u.c[59][0], S.u.c[59][1], S.u.c[59][2], S.u.c[59][3],
                      S.u.c[68][0], S.u.c[68][1], S.u.c[68][2], S.u.c[68][3]); }
            if (g_pcache[slot].dis) fputs(g_pcache[slot].dis, stderr);
            else fprintf(stderr, "  (no disassembly kept)\n");
            fprintf(stderr, "----- constants this program names -----\n");
            if (g_pcache[slot].dis) {
                const char *q = g_pcache[slot].dis;
                int seen[NV2A_VSH_NUM_CONSTS];
                memset(seen, 0, sizeof seen);
                while ((q = strstr(q, "c[")) != NULL) {
                    int n = atoi(q + 2);
                    q += 2;
                    if (n >= 0 && n < NV2A_VSH_NUM_CONSTS && !seen[n]) {
                        seen[n] = 1;
                        fprintf(stderr, "  c[%3d] = %12.5f %12.5f %12.5f %12.5f\n",
                                n, S.u.c[n][0], S.u.c[n][1],
                                S.u.c[n][2], S.u.c[n][3]);
                    }
                }
            }
            /* A program that indexes the constant file through A0 reads
             * constants no "c[" in its text names: print the user range. */
            if (g_pcache[slot].uses_a0) {
                int n;
                fprintf(stderr, "----- c[96..135] (the program indexes through A0) -----\n");
                for (n = 96; n < 136 && n < NV2A_VSH_NUM_CONSTS; n++)
                    fprintf(stderr, "  c[%3d] = %12.5f %12.5f %12.5f %12.5f\n",
                            n, S.u.c[n][0], S.u.c[n][1], S.u.c[n][2], S.u.c[n][3]);
            }
            fprintf(stderr, "----- vertex 0 as fetched -----\n");
            { uint32_t j;
              for (j = 0; j < NUM_ATTRS; j++)
                  if (g_pcache[slot].inputs & (1u << j))
                      fprintf(stderr, "  v%-2u = %10.4f %10.4f %10.4f %10.4f"
                              "   (off=%08X type=%u size=%u stride=%u)\n", j,
                              vbuf[j * 4 + 0], vbuf[j * 4 + 1],
                              vbuf[j * 4 + 2], vbuf[j * 4 + 3],
                              S.attr[j].offset, S.attr[j].type,
                              S.attr[j].size, S.attr[j].stride); }
            fflush(stderr);
        }
    }

    /* What the first draws are actually made of. A batch that produces
     * nothing has usually failed before the shader: no position stream, a
     * stride the title never set, or vertices that transform off the
     * surface -- and the raw numbers separate those three in one line. */
    {
        static int shown;
        if (verbose() && shown < 6) {
            uint32_t j;
            shown++;
#ifdef NV2A_GL_HAVE_GL
            if (g_is_gl) {
                GLint fb = 0, vp4[4] = {0,0,0,0}, cp = 0, va = 0;
                glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
                glGetIntegerv(GL_VIEWPORT, vp4);
                glGetIntegerv(GL_CURRENT_PROGRAM, &cp);
                glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &va);
                fprintf(stderr, "  [GL] context: fbo=%d (ours %u) viewport=%d,%d %dx%d "
                        "program=%d vao=%d depth=%d blend=%d cull=%d scissor=%d\n",
                        fb, nv2a_gfx_gl_fbo(g_cur_rt), vp4[0], vp4[1], vp4[2], vp4[3], cp, va,
                        glIsEnabled(GL_DEPTH_TEST), glIsEnabled(GL_BLEND),
                        glIsEnabled(GL_CULL_FACE), glIsEnabled(GL_SCISSOR_TEST));
            }
#endif
            fprintf(stderr, "  [GL] draw prim=%u verts=%u inputs=%04X inline=%d\n",
                    S.prim, out_n, g_pcache[slot].inputs, inl);
            for (j = 0; j < NUM_ATTRS; j++)
                if (S.attr[j].size)
                    fprintf(stderr, "        v%u: off=%08X type=%u size=%u stride=%u\n",
                            j, S.attr[j].offset, S.attr[j].type,
                            S.attr[j].size, S.attr[j].stride);
            for (j = 0; j < 3 && j < out_n; j++)
                fprintf(stderr, "        vert%u v0=(%.3f %.3f %.3f %.3f)\n", j,
                        vbuf[j * stride_floats + 0], vbuf[j * stride_floats + 1],
                        vbuf[j * stride_floats + 2], vbuf[j * stride_floats + 3]);
            fprintf(stderr, "        c[0]=(%.3f %.3f %.3f %.3f) c[1]=(%.3f %.3f %.3f %.3f)\n",
                    S.u.ff_mat[0][0], S.u.ff_mat[0][1], S.u.ff_mat[0][2], S.u.ff_mat[0][3],
                    S.u.c[1][0], S.u.c[1][1], S.u.c[1][2], S.u.c[1][3]);
            fprintf(stderr, "        prog_start=%u\n", S.prog_start);
            fprintf(stderr, "        c[96..99] rows: "
                    "(%.3f %.3f %.3f %.3f) (%.3f %.3f %.3f %.3f)\n",
                    S.u.c[96][0], S.u.c[96][1], S.u.c[96][2], S.u.c[96][3],
                    S.u.c[97][0], S.u.c[97][1], S.u.c[97][2], S.u.c[97][3]);
            { int nz = 0, first = -1, k;
              for (k = 0; k < NV2A_VSH_NUM_CONSTS; k++)
                  if (S.u.c[k][0] || S.u.c[k][1] || S.u.c[k][2] || S.u.c[k][3]) {
                      if (first < 0) first = k;
                      nz++;
                  }
              fprintf(stderr, "        %d non-zero constants, first at c[%d]; "
                      "const_load now %u, prog_load %u\n",
                      nz, first, S.const_load, S.prog_load); }
        }
    }

    g_wide_squeeze = 0;
    memset(g_wide_ext, 0, sizeof g_wide_ext);
    if (nv2a_wide_scene() && S.xf_mode != NV2A_XF_MODE_FIXED
        && g_pcache[slot].inputs == 0x1F9B && attr_at[0] >= 0
        && S.clip_w == 640 && S.clip_h == 480) {
        float lo = 1e30f, hi = -1e30f, ylo = 1e30f, yhi = -1e30f;
        uint32_t k3;
        for (k3 = 0; k3 < out_n; k3++) {
            const float *p3 = &vbuf[(size_t)k3 * stride_floats + (uint32_t)attr_at[0]];
            if (p3[0] < lo) lo = p3[0];
            if (p3[0] > hi) hi = p3[0];
            if (p3[1] < ylo) ylo = p3[1];
            if (p3[1] > yhi) yhi = p3[1];
        }
        /* In NDC, through the viewport the epilogue divides by: D3D's own
         * copy of the scene to the display works in the surface's scaled
         * pixels (0..2560 at 4x), the title's overlays in 640x480 ones. */
        { float sx = fabsf(S.u.vp_scale[0]) > 1e-6f ? fabsf(S.u.vp_scale[0]) : 320.0f;
          float sy = fabsf(S.u.vp_scale[1]) > 1e-6f ? fabsf(S.u.vp_scale[1]) : 240.0f;
          lo = (lo - S.u.vp_off[0]) / sx; hi = (hi - S.u.vp_off[0]) / sx;
          ylo = (ylo - S.u.vp_off[1]) / sy; yhi = (yhi - S.u.vp_off[1]) / sy; }
        g_wide_ext[0] = lo; g_wide_ext[1] = hi; g_wide_ext[2] = ylo; g_wide_ext[3] = yhi;
        /* Spanning (nearly) the whole width or the whole height: a backdrop,
         * a fade, a frame -- left to fill the 16:9 screen. Anything smaller
         * is an overlay and keeps its shape. */
        if (hi - lo < 1.8f && yhi - ylo < 1.8f) g_wide_squeeze = 1;
        /* RECOMP_WIDE_NOSQUEEZE=1 (test only): measure, never squeeze. */
        { static int nosq = -1;
          if (nosq < 0) nosq = getenv("RECOMP_WIDE_NOSQUEEZE") ? 1 : 0;
          if (nosq) g_wide_squeeze = 0; }
    }
    /* The HUD's pieces to the screen's edges (see wide_hud_sheet): a flat one
     * by where it is; a 3D one, drawn right after a flat one that moved (or
     * after a 3D one that did), with it. The pause screen draws its banners
     * from the same sheet in 3D, with no flat piece of it before them, and
     * they stay where they are. */
    g_wide_anchor = 0.0f;
    { static uint32_t hud_next = 0xFFFFFFFFu; static int hud_side;
      if (nv2a_wide_scene() && wide_hud_on() && S.clip_w == 640 && S.clip_h == 480
          && g_wide_s < 0.999f && S.tex[0].enabled) {
          int flat = S.xf_mode != NV2A_XF_MODE_FIXED && g_pcache[slot].inputs == 0x1F9B;
          if (flat && g_wide_squeeze && wide_hud_sheet(&S.tex[0])) {
              hud_side = g_wide_ext[0] >= 0.1f ? 1 : g_wide_ext[1] <= -0.1f ? -1 : 0;
              g_wide_anchor = (float)hud_side * (1.0f - g_wide_s);
              hud_next = hud_side ? S.draws + 1 : 0xFFFFFFFFu;
          } else if (!flat && out_n <= 32 && hud_side && S.draws == hud_next
                     && wide_hud_sheet(&S.tex[0])) {
              g_wide_anchor = (float)hud_side * (1.0f - g_wide_s);
              hud_next = S.draws + 1;
          }
          if (g_wide_anchor != 0.0f) {
              static int said;
              if (said < 24) {
                  said++;
                  fprintf(stderr, "[WIDEHUD] flip %u draw %u: %s, %u vertices, x %.2f..%.2f y %.2f..%.2f "
                          "-> %s edge (%+.3f)\n", S.flips, S.since_present, flat ? "flat" : "3D", out_n,
                          g_wide_ext[0], g_wide_ext[1], g_wide_ext[2], g_wide_ext[3],
                          g_wide_anchor > 0.0f ? "the right" : "the left", (double)g_wide_anchor);
              }
          }
      } }
    /*
     * A 3D view inside a 2D frame -- the character select's two windows onto
     * the stage, a split screen. The title points it at a part of the screen
     * with a window clip rectangle (and a viewport to match). Widened, its
     * picture comes out 4:3-shaped once the window stretches it again, as
     * everything 3D does; but its rectangle is where the title put it on a
     * 4:3 screen, while the frame around it is an overlay and was squeezed
     * toward the centre -- so the stretch carried the picture out from under
     * its frame: the character slid out of his window, and the select screen
     * drew a strip of the stage outside the panel altogether. Moved toward
     * the centre by the same 3/4 as the frame, and clipped to the frame's
     * squeezed rectangle, the view lines up and shows what the console's did
     * -- the wider camera fills a narrower window. A view that fills the
     * width is the scene itself and is left alone.
     */
    g_wide_shift = 0.0f;
    if (nv2a_wide_scene() && !g_wide_squeeze && S.clip_w == 640 && S.clip_h == 480
        && !(S.xf_mode != NV2A_XF_MODE_FIXED && g_pcache[slot].inputs == 0x1F9B)
        && S.wclip_valid && S.wclip_type == 0) {
        uint32_t x0 =  S.wclip_h[0]        & 0xFFFu;
        uint32_t x1 = (S.wclip_h[0] >> 16) & 0xFFFu;
        uint32_t y0 =  S.wclip_v[0]        & 0xFFFu;
        uint32_t y1 = (S.wclip_v[0] >> 16) & 0xFFFu;
        if (x1 > x0 && y1 > y0 && x1 - x0 + 1 < 600u)
            g_wide_shift = (1.0f - g_wide_s) * (0.5f * (float)(x0 + x1 + 1) - 320.0f);
    }

    /* The vertices, where each attribute sits in them, and the value of
     * each one the program reads that has no array (see the device). */
    { PROF_T0();
      if (vc_vb) {
          G->vbuf_bind(vc_vb, stride_floats, attr_at, const_mask,
                       (const float (*)[4])S.const_attr);
      } else {
          G->vertices(vbuf, out_n, stride_floats, attr_at, const_mask,
                      (const float (*)[4])S.const_attr);
          g_prof.vbo_bytes += (unsigned long)out_n * stride_floats * sizeof(float);
      }
      PROF_ADD(vbo); }

    if (g_cur_slot != slot) {
        g_prof.n_prog_switch++;
        G->prog_use(g_pcache[slot].prog);
        g_cur_prog = g_pcache[slot].prog;
        g_cur_slot = slot;
        /* Uniforms belong to the program object, so a switch invalidates
         * everything remembered about them. */
        S.consts_dirty = 1;
        g_last.tex_enable = -1;
        g_last.alpha_func = -1;
        g_last.vp_scale[0] = 1e30f;
        g_last.psh_ok = 0;
    }
    /* The viewport the shader's epilogue divides by. The title programs it
     * into the hardware registers and, separately, into the constants its own
     * program multiplies by (c[58]/c[59] in JSRF's case) -- they come from one
     * SetViewport, so they agree, and the registers are the ones that are
     * meaningful even for a program that skips the transform. Falling back to
     * the surface size keeps a title that never sets them from collapsing
     * every vertex onto the origin. */
    if (S.u.vp_scale[0] == 0.0f && S.u.vp_scale[1] == 0.0f) {
        S.u.vp_scale[0] = (float)g_fbo_w * 0.5f;
        S.u.vp_scale[1] = (float)g_fbo_h * -0.5f;
        S.u.vp_scale[2] = 1.0f; S.u.vp_scale[3] = 1.0f;
        S.u.vp_off[0] = (float)g_fbo_w * 0.5f;
        S.u.vp_off[1] = (float)g_fbo_h * 0.5f;
        S.u.vp_off[2] = 0.0f; S.u.vp_off[3] = 0.0f;
    }
    /*
     * Every distinct viewport this title programs, not just the first one.
     *
     * It used to say the first and stop, which was enough while the only
     * question was whether the registers arrived at all. The question now is
     * an orientation one, and orientation lives entirely in the SIGN of the
     * y scale: the epilogue's mode-0 path recovers clip space by dividing by
     * it, so a negative scale flips the picture and a positive one does not.
     * The console's own SetViewport writes a negative one, because screen y
     * counts down while clip y counts up.
     *
     * The 3D scenes come out the right way up and the title's 2D screens --
     * the SEGA logo, the graffiti notice, NOW LOADING -- come out mirrored,
     * from the same renderer and the same read-back flip. Two orientations
     * out of one pipeline means two different viewports, so printing each
     * one as it first appears says which draws get which, and the sign says
     * immediately whether the flip is being applied twice or not at all.
     */
    /*
     * The depth range each batch asks for, once per distinct answer.
     *
     * The bone-lookup theory is measured and dead: 64 bad lookups out of
     * 56185, one in nine hundred, which cannot put most of the moving things
     * in this game in the wrong place. So the objects are probably not in
     * the wrong place at all -- they may be in the right place and simply
     * not being HIDDEN by what is in front of them. A bus on a road behind a
     * tower block, drawn over the tower instead of behind it, reads exactly
     * as a bus floating in the sky, and so does a crowd standing on a plaza
     * that should be out of sight behind a building.
     *
     * That is a depth bug, and depth here is per-batch: the title sets
     * NV097_SET_CLIP_MIN and _MAX before nearly every draw, and the shader
     * epilogue maps oPos.z through whichever of those, or the viewport z, it
     * has. A batch that gets the wrong range lands at the wrong depth while
     * everything around it is correct -- which would show up on small
     * dynamic objects and not on the world, because the world is drawn as
     * few large batches that share one range.
     *
     * Printing every distinct range says immediately whether they are sane
     * and few, or wild and many. It is the same cheap question that found
     * the two viewports.
     */
    { static uint32_t seenz[24]; static int nseenz;
      uint32_t kz = fnv(&S.u.z_clip[0], sizeof S.u.z_clip)
                  ^ (uint32_t)(S.u.vp_scale[2] * 7.0f)
                  ^ ((uint32_t)(S.u.vp_off[2] * 13.0f) * 2654435761u);
      int iz, freshz = 1;
      for (iz = 0; iz < nseenz; iz++) if (seenz[iz] == kz) { freshz = 0; break; }
      if (freshz && nseenz < 24) {
          seenz[nseenz++] = kz;
          fprintf(stderr, "  [DEPTH] #%d: clip %.1f..%.1f, viewport z scale "
                  "%.1f offset %.1f -> %s  (test %d func 0x%X write %d)\n",
                  nseenz, S.u.z_clip[0], S.u.z_clip[1],
                  S.u.vp_scale[2], S.u.vp_off[2],
                  S.u.z_clip[1] > S.u.z_clip[0] ? "uses CLIP_MIN/MAX"
                                                : "falls back to the viewport",
                  S.depth_test, S.depth_func, S.depth_mask);
          fflush(stderr);
      } }
    { static uint32_t seen[16]; static int nseen;
      uint32_t k = fnv(&S.u.vp_scale[0], sizeof S.u.vp_scale)
                 ^ (fnv(&S.u.vp_off[0], sizeof S.u.vp_off) * 16777619u)
                 ^ (uint32_t)(S.xf_mode * 2654435761u);
      int i, fresh = 1;
      for (i = 0; i < nseen; i++) if (seen[i] == k) { fresh = 0; break; }
      if (fresh && nseen < 16) {
          seen[nseen++] = k;
          fprintf(stderr,
              "  [GL] viewport #%d: scale (%.1f %.1f %.1f) offset (%.1f %.1f %.1f)"
              "  %s  surface 0x%08X %ux%u  %s\n",
              nseen, S.u.vp_scale[0], S.u.vp_scale[1], S.u.vp_scale[2],
              S.u.vp_off[0], S.u.vp_off[1], S.u.vp_off[2],
              S.xf_mode == NV2A_XF_MODE_FIXED ? "fixed-function" : "program",
              S.color_offset, g_fbo_w, g_fbo_h,
              S.u.vp_scale[1] < 0.0f ? "y flips (normal)"
                                     : "Y DOES NOT FLIP <-- mirrored output");
          fflush(stderr);
      } }
    /*
     * Which space the program left its position in.
     *
     * Mode 0 says screen pixels and un-applies the viewport; mode 1 says clip
     * space and passes it through. The fixed-function path is definitely
     * mode 0 -- the composite matrix has the viewport multiplied into it, as
     * one vertex's z of exactly 16,777,215 proved. Whether a title's own
     * vertex program is in the same space depends on whether the driver folded
     * the viewport into that program's constants or left it to the hardware
     * stage after the program, and those two produce identical pictures for
     * anything drawn in screen space to begin with -- which the interface is.
     *
     * The geometry that is neither is exactly the geometry that is in the
     * wrong place: the traffic and the characters. RECOMP_GL_POSMODE=9 asks
     * for one mode per pipeline so the two can be compared in one frame.
     */
    { static int mode = -2;
      int use;
      if (mode == -2) { const char *v = getenv("RECOMP_GL_POSMODE");
                        mode = v ? atoi(v) : 0; }
      use = (mode == 9) ? (S.xf_mode == NV2A_XF_MODE_FIXED ? 0 : 1) : mode;
      if (uses(slot, NV2A_U_POSMODE))
          G->uniform_i(NV2A_U_POSMODE, use); }
    /* Whether the fog register carries a real factor or a flat 1.0, and how
     * a fog distance becomes that factor. Uniforms belong to the program
     * object, so these go in per draw like the rest -- the last three only
     * with fog on, since with it off the shader never reads them. */
    if (uses(slot, NV2A_U_FOGENABLE))
        G->uniform_i(NV2A_U_FOGENABLE, S.fog_enable ? 1 : 0);
    if (S.fog_enable && uses(slot, NV2A_U_FOGMODE)) {
        /* NV097_SET_FOG_MODE, folded for the shader: 0 linear, 1 exp,
         * 2 exp2, plus 4 for the forms that take the absolute value. */
        int m;
        switch (S.fog_mode) {
        case 0x0800: m = 1; break;   /* EXP */
        case 0x0801: m = 2; break;   /* EXP2 */
        case 0x0802: m = 5; break;   /* EXP_ABS */
        case 0x0803: m = 6; break;   /* EXP2_ABS */
        case 0x0804: m = 4; break;   /* LINEAR_ABS */
        default:     m = 0; break;   /* LINEAR (0x2601) */
        }
        G->uniform_i(NV2A_U_FOGMODE, m);
    }
    if (S.fog_enable && uses(slot, NV2A_U_FOGPARAM))
        uni2f(NV2A_U_FOGPARAM, S.fog_params[0], S.fog_params[1]);
    if (S.fog_enable && uses(slot, NV2A_U_FOGPLANE))
        G->uniform_f(NV2A_U_FOGPLANE, S.fog_plane, 1);
    /*
     * RECOMP_GL_DRAWID: paint each draw with its own index instead of its
     * colour, so the finished frame says which draw owns which pixel.
     *
     * Every other diagnostic here reports on a draw chosen by some property
     * -- its size, its program, its textures -- and then asks whether that is
     * the geometry covering the screen. This asks the question the other way
     * round, which is the way it was always worth asking: point at the part
     * of the picture that is wrong and read off who drew it.
     */
    /*
     * RECOMP_GL_TINTCLIP: run the middle vertex through the interpreter, work
     * out whether the epilogue's ndc.z leaves [-1,1] -- i.e. whether GL would
     * clip this draw on the near or far plane -- and paint it.
     *
     * Depth clamp forced on did not bring the far road back, and the title
     * both leaves w-buffering off for the scene and asks to CULL past the far
     * plane, so the "far-plane clipping" story is unproven. This settles it by
     * location, not by argument: red for draws GL would clip on z, green for
     * draws that survive. If the missing road fills red, it is clipped and the
     * fix is in the epilogue's z mapping; if the missing road stays empty
     * while red lands elsewhere, that geometry is simply not submitted and the
     * search moves to the pushbuffer.
     */
    { static int tc = -1;
      if (tc < 0) tc = getenv("RECOMP_GL_TINTCLIP") ? 1 : 0;
      if (tc && uses(slot, NV2A_U_DRAWTINT) && out_n >= 3
             && S.xf_mode != NV2A_XF_MODE_FIXED) {
          Nv2aVshProgram vp;
          uint32_t pstart = S.prog_start < NV2A_VSH_MAX_INSNS ? S.prog_start : 0;
          int len = nv2a_vsh_decode(S.prog + pstart * 4,
                                    (int)(NV2A_VSH_MAX_INSNS - pstart), &vp);
          int clipped = 0, ok = 0, k2; uint32_t vi2;
          for (vi2 = 0; vi2 < out_n && vi2 < 24; vi2++) {
              float vin[16][4], op[4];
              memset(vin, 0, sizeof vin);
              for (k2 = 0; k2 < NUM_ATTRS; k2++) {
                  if (attr_at[k2] >= 0)
                      memcpy(vin[k2], &vbuf[(size_t)vi2 * stride_floats
                                            + attr_at[k2]], 16);
                  else memcpy(vin[k2], S.const_attr[k2], 16);
              }
              if (!nv2a_vsh_interp(&vp, vin, S.u.c, op)) continue;
              /* The epilogue's own z mapping, mode 0: ndc.z from the clip
               * range, then compared against the clip volume in [-1,1]. */
              { float z01 = (S.u.z_clip[1] > S.u.z_clip[0])
                          ? (op[2] - S.u.z_clip[0]) / (S.u.z_clip[1] - S.u.z_clip[0])
                          : op[2] / 16777215.0f;
                float ndcz = z01 * 2.0f - 1.0f;   /* GL clip is [-w,w]/[-1,1] */
                if (ndcz < -1.001f || ndcz > 1.001f) clipped++; else ok++; }
          }
          uni4f(NV2A_U_DRAWTINT,
                      clipped > ok ? 0.95f : 0.1f,
                      clipped > ok ? 0.1f  : 0.9f, 0.1f, 1.0f);
          goto tint_done;
      } }
    if (uses(slot, NV2A_U_DRAWTINT)) {
        static int drawid = -1;
        if (drawid < 0) drawid = getenv("RECOMP_GL_DRAWID") ? 1 : 0;
        static int tintxf = -1, tintge = -1;
        if (tintxf < 0) { const char *v = getenv("RECOMP_GL_TINT_XF");
                          tintxf = v ? atoi(v) : 0;
                          if (v && !tintxf) tintxf = 1;
                          tintge = getenv("RECOMP_GL_TINT_GEQUAL") ? 1 : 0; }
        /* RECOMP_GL_TINT_GEQUAL: paint every draw that asks for the inverted
         * depth comparison red, and leave the rest alone.
         *
         * A sixth of all triangles are drawn with GEQUAL -- "farther wins" --
         * in the same buffer as the LEQUAL ones. On real hardware that is
         * only sane if those draws carry an inverted z, or are meant to fill
         * only where nothing nearer has been drawn (a backdrop). In this
         * renderer, with z not inverted for them, GEQUAL geometry passes the
         * test exactly where it is FARTHER than what is already there --
         * which is a way to get far things painted over near things, and is
         * therefore worth seeing. Red says what they are. */
        if (tintge) {
            /* Flat colours, since the tint replaces the combiner: GEQUAL in
             * red, everything else in a grey that still shows its shape. */
            if (S.depth_func == 0x206)
                uni4f(NV2A_U_DRAWTINT, 0.9f, 0.1f, 0.1f, 1.0f);
            else
                uni4f(NV2A_U_DRAWTINT, 0.35f, 0.35f, 0.4f, 1.0f);
        } else if (tintxf) {
            /* Which pipeline drew which surface, in one frame.
             *
             * Filtering one pipeline out answers the same question but
             * changes the timing enough to destabilise the run -- three
             * attempts in a row died before the scene appeared. Tinting keeps
             * every draw and every GL call exactly where they were, and the
             * picture still separates the two: the hardware T&L unit's
             * geometry comes out green, the programmable one's red. If the
             * characters are standing on nothing, the space beneath them is
             * empty of green. */
            /* RECOMP_GL_TINT_XF=2 marks only the programmable geometry and
             * leaves the rest of the picture alone, which is what is wanted
             * once the question is "where is that character standing"
             * rather than "which pipeline drew this surface". */
            int fixed = (S.xf_mode == NV2A_XF_MODE_FIXED);
            /* RECOMP_GL_TINT_XF=3: mark the programmable geometry, but leave
             * the full-screen quad that composites the frame alone.
             *
             * Tinting every programmable draw turned the displayed frame
             * uniformly red and said nothing, because the last thing drawn
             * into the display buffer is a four-vertex quad that samples the
             * off-screen surface the scene was built in -- and that quad is
             * programmable too. Excluding the small quads keeps the picture
             * and marks the characters, the traffic and the effects, which is
             * the comparison that was wanted. */
            if (tintxf == 3) {
                if (fixed || out_n <= 6)
                    uni4f(NV2A_U_DRAWTINT,
                                1.0f, 1.0f, 1.0f, 1.0f);
                else
                    uni4f(NV2A_U_DRAWTINT,
                                3.0f, 0.2f, 0.2f, 1.0f);
            } else if (tintxf == 2) {
                if (fixed) uni4f(NV2A_U_DRAWTINT,
                                       1.0f, 1.0f, 1.0f, 1.0f);
                else       uni4f(NV2A_U_DRAWTINT,
                                       3.0f, 0.2f, 0.2f, 1.0f);
            } else
            uni4f(NV2A_U_DRAWTINT,
                        fixed ? 0.25f : 1.0f, fixed ? 1.0f : 0.25f,
                        0.25f, 1.0f);
        } else if (drawid) {
            uint32_t i = S.since_present;
            uni4f(NV2A_U_DRAWTINT,
                        (i & 0xFF) / 255.0f, ((i >> 8) & 0xFF) / 255.0f,
                        0.0f, 1.0f);
        } else {
            uni4f(NV2A_U_DRAWTINT, 1.0f, 0.0f, 1.0f, 1.0f);
        }
    }
    tint_done: ;
    /* The fixed-function transform, for the shaders that use it. Uploaded
     * whenever it changed or the program did, since uniforms belong to the
     * program object. */
    if (uses(slot, NV2A_U_ZCLIP))
        uni2f(NV2A_U_ZCLIP, S.u.z_clip[0], S.u.z_clip[1]);
    if (uses(slot, NV2A_U_LITAMBIENT)) {
        /* RECOMP_GL_FF_LIGHT=<f>: how much light to add to a batch the title
         * asked the hardware to light. 0 keeps today's behaviour exactly. */
        static float amt = -1.0f;
        if (amt < 0.0f) { const char *v = getenv("RECOMP_GL_FF_LIGHT");
                          amt = v ? (float)atof(v) : 0.0f; }
        uni1f(NV2A_U_LITAMBIENT, amt);
    }
    /* The fixed-function lighting's state, for a lit batch. Uploaded every
     * draw: the title changes the light's colour between batches (its pause
     * map colours each element that way), and uniforms are per program. */
    /* Model-view 0: the lighting's eye space, and the fog distance's. */
    if (uses(slot, NV2A_U_FFMV))
        G->uniform_f(NV2A_U_FFMV, &S.ff_mv[0][0], 4);
    /* Its inverse: the lighting's normals, and the generated texture
     * coordinates' of an unlit batch. */
    if (uses(slot, NV2A_U_FFIMV))
        G->uniform_f(NV2A_U_FFIMV, &S.ff_imv[0][0], 4);
    if (uses(slot, NV2A_U_LTSCENEAMB)) {
        G->uniform_f(NV2A_U_LTSCENEAMB, S.scene_ambient, 1);
        G->uniform_f(NV2A_U_LTMATEM, S.material_emission, 1);
        uni1f(NV2A_U_LTMATALPHA, S.material_alpha);
        G->uniform_f(NV2A_U_LTAMB, &S.light_amb[0][0], 8);
        G->uniform_f(NV2A_U_LTDIF, &S.light_dif[0][0], 8);
        G->uniform_f(NV2A_U_LTSPC, &S.light_spc[0][0], 8);
        G->uniform_f(NV2A_U_LTDIR, &S.light_dir[0][0], 8);
        G->uniform_f(NV2A_U_LTHALF, &S.light_half[0][0], 8);
        G->uniform_f(NV2A_U_LTPOS, &S.light_pos[0][0], 8);
        G->uniform_f(NV2A_U_LTATT, &S.light_att[0][0], 8);
        G->uniform_f(NV2A_U_LTRANGE, S.light_range, 8);
        G->uniform_f(NV2A_U_LTSPOTDIR, &S.light_spot_dir[0][0], 8);
        { float eye[4]; memcpy(eye, S.eye_position, sizeof eye);
          if (eye[3] == 0.0f) eye[3] = 1.0f;
          G->uniform_f(NV2A_U_LTEYE, eye, 1); }
        /* The exponent: the title hands the hardware six coefficients of
         * its own approximation (NV097_SET_SPECULAR_PARAMS), which xemu
         * fits back to an exponent. Not reconstructed here yet -- every lit
         * batch this title draws has a black specular colour. */
        uni1f(NV2A_U_LTSPECPOW, 16.0f);
    }
    if (g_zrev && S.xf_mode == NV2A_XF_MODE_FIXED && uses(slot, NV2A_U_ZREV)) {
        float r[4];
        zrev_row(r);
        G->uniform_f(NV2A_U_ZREV, r, 1);
    }
    if (uses(slot, NV2A_U_FFMAT)) {
        G->uniform_f(NV2A_U_FFMAT, &S.u.ff_mat[0][0], 4);
        if (uses(slot, NV2A_U_FFVPOFF)) {
            /* The viewport offset register less the driver's 0.53125 nudge,
             * as the programmable path takes it off (vpRect): the nudge
             * moves the console's pixel centres onto its rasteriser's, and
             * here it moved every fixed-function draw -- the menus, the
             * Garage's graffiti tiles, the scenery -- half a pixel right and
             * down of the vertex programs' draws and of the texels they were
             * laid out for, which a texture shows as a seam or a blur.
             * RECOMP_GL_FF_NUDGE=1 keeps it, as before. */
            static int keep = -1;
            float o[4];
            if (keep < 0) { const char *v = getenv("RECOMP_GL_FF_NUDGE");
                            keep = v && *v && *v != '0'; }
            memcpy(o, S.vp_off_reg, sizeof o);
            if (!keep) { o[0] -= 0.53125f; o[1] -= 0.53125f; }
            G->uniform_f(NV2A_U_FFVPOFF, o, 1);
        }
        if (uses(slot, NV2A_U_FFTEXMAT))
            G->uniform_f(NV2A_U_FFTEXMAT, &S.u.ff_texmat[0][0], 16);
    }
    if (!G->uniform_f_part) {
        if (S.consts_dirty) {
            PROF_T0();
            G->uniform_f(NV2A_U_C, &S.u.c[0][0], NV2A_VSH_NUM_CONSTS);
            S.consts_dirty = 0;
            PROF_ADD(unif);
            g_prof.n_const_upload++;
            g_prof.unif_vec4 += NV2A_VSH_NUM_CONSTS;
        }
    } else if (uses(slot, NV2A_U_C)) {
        /* What changed since this program last had the constants (see
         * g_cstamp), in runs; a gap of a few unchanged ones inside a run is
         * sent along rather than split around. */
        uint32_t from = g_pcache[slot].csync;
        PROF_T0();
        if (from && g_clast >= from) {
            int i = 0, ok = 1;
            while (ok && i < NV2A_VSH_NUM_CONSTS) {
                int j, g;
                if (g_cstamp[i] < from) { i++; continue; }
                j = i + 1;
                for (;;) {
                    while (j < NV2A_VSH_NUM_CONSTS && g_cstamp[j] >= from) j++;
                    for (g = j; g < NV2A_VSH_NUM_CONSTS && g < j + 4 && g_cstamp[g] < from; g++) ;
                    if (g < NV2A_VSH_NUM_CONSTS && g < j + 4) { j = g; continue; }
                    break;
                }
                ok = G->uniform_f_part(NV2A_U_C, i, &S.u.c[i][0], j - i);
                g_prof.n_const_upload++;
                g_prof.unif_vec4 += (unsigned long)(j - i);
                i = j;
            }
            if (!ok) from = 0;
        }
        if (!from) {
            G->uniform_f(NV2A_U_C, &S.u.c[0][0], NV2A_VSH_NUM_CONSTS);
            g_prof.n_const_upload++;
            g_prof.unif_vec4 += NV2A_VSH_NUM_CONSTS;
        }
        /* Up to date now. A change since the clock last ticked moves it on,
         * so that the next change is told apart from what was just sent. */
        if (g_clast == g_cclock) {
            if (++g_cclock == 0xFFFFFFF0u) {
                /* Four billion uploads in: start the clock again, and every
                 * program from nothing. */
                int k;
                memset(g_cstamp, 0, sizeof g_cstamp);
                for (k = 0; k < PROG_CACHE; k++) g_pcache[k].csync = 0;
                g_cclock = 1; g_clast = 0;
            }
        }
        g_pcache[slot].csync = g_cclock;
        S.consts_dirty = 0;
        PROF_ADD(unif);
    }
    /* Widescreen's 2D squeeze (see nv2a_widescreen): ndc.x is
     * (x - vpOff.x) / vpScale.x, so 1/s more scale is s the width (at 16:9,
     * 4/3 more scale and 3/4 the width). */
    float vp_scale_eff[4];
    float wshift = g_wide_shift;
    memcpy(vp_scale_eff, S.u.vp_scale, sizeof vp_scale_eff);
    if (g_wide_squeeze) vp_scale_eff[0] *= 1.0f / g_wide_s;
    /* A HUD piece's move to the edge (g_wide_anchor, NDC) in the shift's
     * title pixels: the fixed-function epilogue divides by half the surface,
     * a program's by its viewport scale -- squeezed or not -- and then maps
     * the viewport onto the surface (vpRect). */
    if (g_wide_anchor != 0.0f) {
        float half = 0.5f * (float)g_fbo_w;
        if (S.xf_mode == NV2A_XF_MODE_FIXED) wshift -= g_wide_anchor * half;
        else {
            float rz = fabsf(S.u.vp_scale[0]) > 1e-6f ? fabsf(S.u.vp_scale[0]) : half;
            wshift -= g_wide_anchor * vp_scale_eff[0] * half / rz;
        }
    }
    if (memcmp(g_last.vp_scale, vp_scale_eff, sizeof vp_scale_eff)
     || memcmp(g_last.vp_off, S.u.vp_off, sizeof S.u.vp_off)
     || g_last.wide_shift != wshift
     || g_last.fbo_w != g_fbo_w || g_last.fbo_h != g_fbo_h) {
        /* The viewport, whenever it or the surface it is on changes: vpSurface
         * and vpRect hold the surface's size, and a title can draw to a
         * surface of another size without writing the viewport again.
         *
         * The depth buffer was measured holding the entire scene inside one
         * millionth of its range, which makes depth testing a coin toss and is
         * exactly what an object hanging in mid-air in front of the building
         * it should be behind looks like. The epilogue turns screen-space z
         * into NDC with these two numbers, so they are the first thing to
         * look at. */
        static int shown;
        if (shown < 6) {
            shown++;
            fprintf(stderr, "  [GL] viewport scale (%.1f %.1f %.1f %.1f) "
                    "offset (%.1f %.1f %.1f %.1f)  xf_mode=%u\n",
                    S.u.vp_scale[0], S.u.vp_scale[1], S.u.vp_scale[2], S.u.vp_scale[3],
                    S.u.vp_off[0], S.u.vp_off[1], S.u.vp_off[2], S.u.vp_off[3],
                    S.xf_mode);
        }
        G->uniform_f(NV2A_U_VPSCALE, vp_scale_eff, 1);
        G->uniform_f(NV2A_U_VPOFF, S.u.vp_off, 1);
        uni2f(NV2A_U_VPSURFACE, (float)g_fbo_w, (float)g_fbo_h);
        {   /* Where the viewport is on the surface, for a vertex program's
             * screen-space oPos (see the epilogue): its centre -- the offset
             * register less the driver's 0.53125 nudge, which puts a whole-
             * surface viewport's centre at exactly half the surface -- and
             * its half size. Unscaled by the widescreen squeeze, which acts
             * inside the viewport through vpScale. */
            float r[4];
            r[0] = S.u.vp_off[0] - 0.53125f;
            r[1] = S.u.vp_off[1] - 0.53125f;
            r[2] = fabsf(S.u.vp_scale[0]);
            r[3] = fabsf(S.u.vp_scale[1]);
            if (r[2] < 1e-6f || r[3] < 1e-6f) {      /* no viewport: the surface */
                r[0] = (float)g_fbo_w * 0.5f; r[1] = (float)g_fbo_h * 0.5f;
                r[2] = (float)g_fbo_w * 0.5f; r[3] = (float)g_fbo_h * 0.5f;
            }
            G->uniform_f(NV2A_U_VPRECT, r, 1);
        }
        uni1f(NV2A_U_WIDESHIFT, wshift);
        memcpy(g_last.vp_scale, vp_scale_eff, sizeof vp_scale_eff);
        memcpy(g_last.vp_off, S.u.vp_off, sizeof S.u.vp_off);
        g_last.wide_shift = wshift;
        g_last.fbo_w = g_fbo_w; g_last.fbo_h = g_fbo_h;
    }

    /* Bind every stage the combiner shader samples.
     *
     * Coordinates are normalised for a swizzled or compressed texture and in
     * texels for a linear one, so the scale that turns them into GL's [0,1]
     * belongs to the texture and not to the draw. Sampling a surface the
     * title rendered into a moment ago is render-to-texture, and the live
     * framebuffer is the answer rather than the bytes at that address. */
    {
        int st, srcslot[4] = { -1, -1, -1, -1 };
        for (st = 0; st < 4; st++) {
            uint32_t tex = 0;
            float sx = 1.0f, sy = 1.0f;
            int flip;

            int sslot = -1;

            if (S.psh.tex_bound[st]) {
                tex = surface_texture_for(&S.tex[st], &sx, &sy, &sslot);
                if (!tex) {
                    sx = sy = 1.0f;
                    g_upload_bias_bg = (S.psh.tex_signed[st] & 0x10) != 0;
                    tex = upload_texture(&S.tex[st]);
                    g_upload_bias_bg = 0;
                    if (tex && !fmt_is_swz(S.tex[st].color)
                            && !fmt_is_dxt(S.tex[st].color)) {
                        sx = 1.0f / (float)S.tex[st].width;
                        sy = 1.0f / (float)S.tex[st].height;
                    }
                }
            }
            G->texture(st, tex);
            /* A surface this backend rendered holds its picture the GL way
             * up: the vertex stage sends screen y = 0 to NDC +1, so the top
             * row of the picture is the LAST row of the framebuffer. A
             * texture decoded from guest memory is the other way up (row 0
             * is the top), and the coordinates are the title's, so sampling
             * a surface has to turn t around: t' = 1 - t. (On OpenGL, that
             * is: Metal keeps a target top row first, the same way up as a
             * texture, and nothing is turned -- see rt_bottom_up.)
             *
             * Without it every render-to-texture pass turned the picture
             * over. JSRF's normal frame passes through exactly one (the
             * off-screen frame copied into the display buffer), which the
             * window and the BMP writer had been compensating for by showing
             * framebuffers upside down. The boost dash adds a second pass --
             * the world rendered to a 512x512 and a 128x128 target and
             * copied into the frame -- so during a boost the world was drawn
             * upside down under a HUD that was the right way up. */
            if (uses(slot, (NV2A_U_TEX0 + st)))
                G->uniform_i((NV2A_U_TEX0 + st), st);
            flip = (sslot >= 0 && G->rt_bottom_up);
            if (uses(slot, (NV2A_U_TEXSCALE0 + st)))
                uni2f((NV2A_U_TEXSCALE0 + st), sx, flip ? -sy : sy);
            if (uses(slot, (NV2A_U_TEXOFF0 + st)))
                uni2f((NV2A_U_TEXOFF0 + st), 0.0f, flip ? 1.0f : 0.0f);
            if (uses(slot, (NV2A_U_BUMPMAT0 + st)))
                G->uniform_f((NV2A_U_BUMPMAT0 + st), S.tex[st].bump_mat, 1);
            if (uses(slot, (NV2A_U_BUMPSCALE0 + st)))
                uni1f((NV2A_U_BUMPSCALE0 + st), S.tex[st].bump_scale);
            if (uses(slot, (NV2A_U_BUMPOFF0 + st)))
                uni1f((NV2A_U_BUMPOFF0 + st), S.tex[st].bump_off);
            srcslot[st] = sslot;
        }
        g_last.tex0 = 0;    /* unit 0 is no longer what the cache remembers */

        /* [PASS]: every draw while nv2a_gl_trace_frames() is active, and the
         * first draws that use a BUMPENVMAP stage: what each stage samples,
         * from where, and the bump matrices. */
        { static int bump_said;
          int bump = 0, k;
          for (k = 1; k < 4; k++)
              if (S.psh.tex_bound[k] && (S.psh.tex_mode[k] == 6 || S.psh.tex_mode[k] == 7)) bump = 1;
          if ((g_pass_trace_until && S.frames < g_pass_trace_until)
           || (bump && bump_said < 12)) {
              char line[1400]; int len = 0;
              if (bump) bump_said++;
              len += snprintf(line + len, sizeof line - len,
                      "[PASS] f%u d%u surf %08X %ux%u prim %u n %u modes %05X blend %d %X/%X ztest %d cm %08X comb %08X |",
                      S.frames, S.draws, S.color_offset, S.clip_w, S.clip_h, S.prim,
                      S.inline_count ? S.inline_count : S.idx_count,
                      S.shader_stage_prog, S.blend_enable, S.blend_src, S.blend_dst,
                      S.depth_test, S.color_mask, S.psh.control);
              for (k = 0; k < 4; k++) {
                  const TexStage *t = &S.tex[k];
                  if (!S.psh.tex_bound[k]) continue;
                  len += snprintf(line + len, sizeof line - len,
                          " t%d %08X %ux%u c%02X p%u f%08X a%08X %s", k, t->offset,
                          t->width, t->height, t->color, t->pitch, t->filter, t->addr,
                          srcslot[k] >= 0 ? "FBO" : "mem");
                  if (S.psh.tex_mode[k] == 6 || S.psh.tex_mode[k] == 7)
                      len += snprintf(line + len, sizeof line - len,
                              " bump<-t%d m(%.4f %.4f %.4f %.4f) s%.3f o%.3f",
                              S.psh.bump_src[k], t->bump_mat[0], t->bump_mat[1],
                              t->bump_mat[2], t->bump_mat[3], t->bump_scale, t->bump_off);
                  if (len >= (int)sizeof line - 1) break;
              }
              fprintf(stderr, "%s\n", line);
          } }
    }
    if (uses(slot, NV2A_U_FOGCOLOR))
        uni4f(NV2A_U_FOGCOLOR,
                    (S.fog_color & 0xFF) / 255.0f,          /* R: low byte */
                    ((S.fog_color >> 8) & 0xFF) / 255.0f,
                    ((S.fog_color >> 16) & 0xFF) / 255.0f,  /* B */
                    ((S.fog_color >> 24) & 0xFF) / 255.0f);
    psh_consts_send(slot);
    {
        /* RECOMP_GL_NOALPHATEST=1 passes every fragment.
         *
         * The props are drawn and the surface they stand on is not, and the
         * surface is static world geometry that the world-only render shows
         * arriving correctly elsewhere. So the deck is probably being
         * submitted and then thrown away per-fragment, and alpha test is the
         * only thing in this pipeline that throws away a fragment of an
         * otherwise perfect triangle. A road texture whose alpha decodes to
         * zero would vanish completely while every bollard and bus standing
         * on it stayed exactly where it is -- which is the picture. */
        static int noalpha = -1;
        int af;
        if (noalpha < 0) noalpha = getenv("RECOMP_GL_NOALPHATEST") ? 1 : 0;
        af = (S.alpha_test && !noalpha) ? (S.alpha_func - 0x0200 + 1) : 0;
        if (af != g_last.alpha_func || S.alpha_ref != g_last.alpha_ref) {
            G->uniform_i(NV2A_U_ALPHAFUNC, af);
            uni1f(NV2A_U_ALPHAREF, S.alpha_ref);
            g_last.alpha_func = af; g_last.alpha_ref = S.alpha_ref;
        }
    }

    /* Everything about this draw that is neither a shader nor a uniform,
     * handed over in the console's own terms. What OpenGL does with it is in
     * nv2a_gfx_gl.c; what Metal does is different enough that the two cannot
     * share the code, and similar enough that they must not disagree about
     * what the registers meant. */
    {
        Nv2aRenderState rs;
        memset(&rs, 0, sizeof rs);
        rs.depth_test  = S.depth_test;
        rs.depth_write = S.depth_mask;
        rs.depth_func  = S.depth_func;

        /*
         * The far crowd is not floating. It is far away, at ground level, and
         * drawn on top of the nearer buildings that should hide it.
         *
         * Cropping one frame into near and far halves made it plain. Near:
         * everything correct. Far: a horizontal line of pedestrians at
         * mid-frame overlapping the building facades, buses in a line above
         * them. For a camera at street level, distant ground projects to the
         * horizon and the horizon is mid-frame -- so a line of people at
         * mid-frame is exactly what a crowd on a distant street looks like.
         * They are placed right. They are simply not being occluded.
         *
         * Buildings occlude each other correctly, which is what hid this all
         * day: I checked that the depth RANGE was uniform and never checked
         * whether the world and the entities are drawn with the same depth
         * STATE. The world drawn without writing depth, or the buffer cleared
         * between the world pass and the entity pass, or the entities drawn
         * with the test off -- any of those gives this picture and leaves
         * building-on-building occlusion intact.
         *
         * Two things here. RECOMP_GL_DEPTHCENSUS counts draws and triangles
         * by the depth state the title actually asked for, split by whether
         * the program indexes the constant palette (entities) or not (mostly
         * world). RECOMP_GL_FORCE_DEPTH ignores the title and turns the test
         * and the write on for every draw: if the far crowd goes behind the
         * buildings, the state is the bug and the census says which draws.
         */
        { static int census = -1, force = -1;
          if (census < 0) { census = getenv("RECOMP_GL_DEPTHCENSUS") ? 1 : 0;
                            force  = getenv("RECOMP_GL_FORCE_DEPTH") ? 1 : 0; }
          if (census) {
              /* And WHICH SURFACE each class is drawn into.
               *
               * Forcing the depth state on for every draw changed nothing,
               * so the state is not it. But the frame is built off-screen
               * and composited into the display by a full-screen quad -- and
               * a quad copies colour, not depth. If the world goes into one
               * surface and the entities into another, the display's depth
               * buffer never holds the world's depth, the entities are tested
               * against an empty buffer, and nothing ever occludes them. That
               * is this bug exactly, and it survives any depth STATE because
               * the two classes are never in the same buffer to be compared.
               * The surface offset in the key says whether that is what is
               * happening. */
              enum { DC = 32 };
              static struct { int t, f, m, a0, wb; uint32_t surf, cm;
                              uint64_t draws, tris; } dc[DC];
              static int ndc; static uint32_t last;
              int a0 = (slot >= 0 && g_pcache[slot].uses_a0) ? 1 : 0, i;
              for (i = 0; i < ndc; i++)
                  if (dc[i].t == S.depth_test && dc[i].f == S.depth_func
                   && dc[i].m == S.depth_mask && dc[i].a0 == a0
                   && dc[i].surf == S.color_offset
                   && dc[i].cm == S.color_mask
                   && dc[i].wb == (int)S.psh.w_buffer) break;
              if (i == ndc && ndc < DC) { dc[ndc].t = S.depth_test;
                  dc[ndc].f = S.depth_func; dc[ndc].m = S.depth_mask;
                  dc[ndc].a0 = a0; dc[ndc].surf = S.color_offset;
                  dc[ndc].cm = S.color_mask; dc[ndc].wb = (int)S.psh.w_buffer;
                  ndc++; }
              if (i < ndc) { dc[i].draws++; dc[i].tris += out_n / 3; }
              if (S.draws - last >= 150000u) {
                  last = S.draws;
                  fprintf(stderr, "  [DCENSUS] depth state by draw class and "
                          "surface:\n");
                  for (i = 0; i < ndc; i++)
                      fprintf(stderr, "  [DCENSUS]   surface %08X  %-3s %s test=%d "
                              "func=0x%03X zwrite=%2d colour=%08X   %8llu draws %10llu tris\n",
                              dc[i].surf, dc[i].a0 ? "A0" : "  ",
                              dc[i].wb ? "W-BUF" : "z    ",
                              dc[i].t, dc[i].f, dc[i].m, dc[i].cm,
                              (unsigned long long)dc[i].draws,
                              (unsigned long long)dc[i].tris);
                  fflush(stderr);
              }
          }
          if (force) { rs.depth_test = 1; rs.depth_write = 1;
                       rs.depth_func = 0x203; /* LEQUAL */ }
          /* RECOMP_GL_GEQUAL_AS_LEQUAL: draw the "farther wins" class as if
           * it were "nearer wins".
           *
           * Tinted red, that class does not appear in a single frame -- and
           * a sixth of the run's triangles do not vanish by accident. With
           * their z not inverted, GEQUAL passes only where the fragment is
           * FARTHER than what the buffer already holds, which against a
           * buffer cleared to far and filled by nearer LEQUAL geometry is
           * almost nowhere. So the whole class is being thrown away. If it
           * is the road decks, this puts them back under the buses. */
          { static int ge2le = -1;
            if (ge2le < 0) ge2le = getenv("RECOMP_GL_GEQUAL_AS_LEQUAL") ? 1 : 0;
            if (ge2le && rs.depth_func == 0x206) rs.depth_func = 0x203; }
          /* Reversed depth: nearer is larger (zrev_start). */
          if (g_zrev) rs.depth_func = zrev_func((uint32_t)rs.depth_func);
        }
        rs.blend_enable = S.blend_enable;
        rs.blend_src    = S.blend_src;
        rs.blend_dst    = S.blend_dst;
        rs.alpha_test   = S.alpha_test;
        rs.cull_enable  = S.cull_enable;
        rs.color_mask   = surf_color_mask();
        rs.zclamp       = S.zminmax;
        rs.w_buffer     = S.psh.w_buffer;
        rs.cull_face    = S.cull_face;
        rs.front_face   = S.front_face;
        /* Polygon offset, which used to be left out: a decal laid on the
         * surface it marks is drawn a little nearer by it, so as not to fight
         * that surface for the pixel. The slope's part is per pixel
         * of the console's raster, so it is taken times the internal
         * resolution's scale; with reversed depth nearer is larger, and the
         * offset is turned round. RECOMP_GL_NOPOLYOFFSET=1: none, as before. */
        { static int nopo = -1;
          if (nopo < 0) nopo = getenv("RECOMP_GL_NOPOLYOFFSET") ? 1 : 0;
          if (!nopo && S.poly_off_fill
           && (S.poly_off_factor != 0.0f || S.poly_off_units != 0.0f)) {
              float sgn = g_zrev ? -1.0f : 1.0f;
              rs.poly_offset = 1;
              rs.poly_offset_factor = sgn * S.poly_off_factor * (float)gl_scale();
              rs.poly_offset_units  = sgn * S.poly_off_units;
              { static int said;
                if (said < 4) { said++;
                    fprintf(stderr, "  [GL] polygon offset: factor %g, units %g (draw %u)\n",
                            S.poly_off_factor, S.poly_off_units, S.draws); } }
          } }
        /* Stencil. RECOMP_GL_NOSTENCIL=1: ignore it, as before. */
        { static int nost = -1;
          if (nost < 0) nost = getenv("RECOMP_GL_NOSTENCIL") ? 1 : 0;
          rs.stencil_test  = nost ? 0 : (S.stencil_en ? 1 : 0);
          rs.stencil_func  = S.stencil_func;
          rs.stencil_ref   = S.stencil_ref;
          rs.stencil_rmask = S.stencil_fmask;
          rs.stencil_wmask = S.stencil_wmask;
          rs.stencil_op[0] = S.stencil_op[0];
          rs.stencil_op[1] = S.stencil_op[1];
          rs.stencil_op[2] = S.stencil_op[2]; }
        /* The window clip. A rectangle that covers the surface is the same as
         * none and costs a state change per draw to say so, which is why the
         * test is here and not in a backend. */
        { static int off = -1;
          /* In the title's pixels, the far edges exclusive. */
          uint32_t cx0 = 0, cy0 = 0, cx1 = g_fbo_w, cy1 = g_fbo_h;
          int clip = 0;
          if (off < 0) off = getenv("RECOMP_GL_NOWCLIP") ? 1 : 0;
          if (!off && S.wclip_valid && S.wclip_type == 0) {
              uint32_t x0 =  S.wclip_h[0]        & 0xFFFu;
              uint32_t x1 = (S.wclip_h[0] >> 16) & 0xFFFu;
              uint32_t y0 =  S.wclip_v[0]        & 0xFFFu;
              uint32_t y1 = (S.wclip_v[0] >> 16) & 0xFFFu;
              if (x1 > x0 && y1 > y0
               && (x0 > 0 || y0 > 0 || x1 + 1 < g_fbo_w || y1 + 1 < g_fbo_h)) {
                  /* Inclusive corners: the far edges one further. */
                  cx0 = x0; cy0 = y0; cx1 = x1 + 1; cy1 = y1 + 1;
                  clip = 1;
                  S.wclip_applied++;
              }
          }
          /* Widescreen: what moved toward the centre takes its rectangle
           * with it, and an overlay stays inside the 4:3 screen it was
           * drawn for -- the title parks some just past its edges, out of
           * sight on the console and in plain view once squeezed. */
          if (g_wide_squeeze || g_wide_shift != 0.0f) {
              float c = 0.5f * (float)g_fbo_w;
              float a = c + g_wide_s * ((float)cx0 - c), b = c + g_wide_s * ((float)cx1 - c);
              /* A HUD piece moved to an edge takes its 4:3 screen with it:
               * what the title parks past that screen's edge stays out of
               * sight, past the wide screen's. */
              if (g_wide_squeeze) { a += g_wide_anchor * c; b += g_wide_anchor * c; }
              cx0 = a > 0.0f ? (uint32_t)floorf(a) : 0u;
              cx1 = b > 0.0f ? (uint32_t)ceilf(b) : 0u;
              clip = 1;
          }
          if (clip) {
              /* The device takes raster pixels. */
              uint32_t sc = gl_scale();
              rs.scissor_enable = 1;
              rs.scissor[0] = cx0 * sc; rs.scissor[1] = cy0 * sc;
              rs.scissor[2] = cx1 * sc; rs.scissor[3] = cy1 * sc;
          } }
        g_draw_rs = rs;
        G->state(&rs);
    }

    if (inl) S.draws_inline++; else S.draws_array++;
    S.draws_xf[S.xf_mode & 3]++;
    if (S.ff_skin_mode && S.xf_mode == NV2A_XF_MODE_FIXED)
        S.draws_skinned++;
    if (S.depth_test) S.draws_depth_on++;
    {
        static int shown;
        if (verbose() && !inl && shown < 8) {
            float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
            uint32_t j; int k;
            shown++;
            for (j = 0; j < out_n; j++)
                for (k = 0; k < 3; k++) {
                    float v = vbuf[j * stride_floats + k];
                    if (v < lo[k]) lo[k] = v;
                    if (v > hi[k]) hi[k] = v;
                }
            fprintf(stderr, "  [GL] array batch: %u verts v0 x[%.1f..%.1f] "
                    "y[%.1f..%.1f] z[%.1f..%.1f] attr0(off=%08X type=%u size=%u "
                    "stride=%u) depth=%d func=%04X zclear=%08X\n",
                    out_n, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2],
                    S.attr[0].offset, S.attr[0].type, S.attr[0].size,
                    S.attr[0].stride, S.depth_test, S.depth_func,
                    S.clear_zstencil);
        }
    }
    /* RECOMP_GL_DUMP_Z=1: what the transform actually produces, in numbers.
     *
     * The depth buffer holds the whole scene inside a millionth of its range,
     * and there are two ways that happens: the composite matrix leaves z in
     * D3D's [0,1] clip range while the epilogue divides it by the viewport's
     * z scale of 16,777,215 as though it were already in screen units, or the
     * matrix really does scale it and something else loses it. Multiplying one
     * real vertex by the matrix on the CPU says which, and costs one line. */
    { static int dz = -1; static int shown;
      if (dz < 0) dz = getenv("RECOMP_GL_DUMP_Z") ? 1 : 0;
      if (dz && shown < 10 && out_n > 16 && attr_at[0] == 0
          && S.xf_mode == NV2A_XF_MODE_FIXED && S.draws > 20000) {
          const float *v = &vbuf[0];
          float px = v[0]*S.u.ff_mat[0][0] + v[1]*S.u.ff_mat[0][1]
                   + v[2]*S.u.ff_mat[0][2] + v[3]*S.u.ff_mat[0][3];
          float py = v[0]*S.u.ff_mat[1][0] + v[1]*S.u.ff_mat[1][1]
                   + v[2]*S.u.ff_mat[1][2] + v[3]*S.u.ff_mat[1][3];
          float pz = v[0]*S.u.ff_mat[2][0] + v[1]*S.u.ff_mat[2][1]
                   + v[2]*S.u.ff_mat[2][2] + v[3]*S.u.ff_mat[2][3];
          float pw = v[0]*S.u.ff_mat[3][0] + v[1]*S.u.ff_mat[3][1]
                   + v[2]*S.u.ff_mat[3][2] + v[3]*S.u.ff_mat[3][3];
          /* The whole batch, not one vertex, and only the vertices that
           * land on screen: an off-screen vertex sits at whatever the
           * projection does to something behind the camera, and averaging
           * those in says nothing about the depth range the scene occupies. */
          { uint32_t q; float zlo = 1e30f, zhi = -1e30f, wlo = 1e30f, whi = -1e30f; int on = 0;
            for (q = 0; q < out_n; q++) {
                const float *vv = &vbuf[q * stride_floats];
                float ax = vv[0]*S.u.ff_mat[0][0] + vv[1]*S.u.ff_mat[0][1]
                         + vv[2]*S.u.ff_mat[0][2] + vv[3]*S.u.ff_mat[0][3];
                float ay = vv[0]*S.u.ff_mat[1][0] + vv[1]*S.u.ff_mat[1][1]
                         + vv[2]*S.u.ff_mat[1][2] + vv[3]*S.u.ff_mat[1][3];
                float az = vv[0]*S.u.ff_mat[2][0] + vv[1]*S.u.ff_mat[2][1]
                         + vv[2]*S.u.ff_mat[2][2] + vv[3]*S.u.ff_mat[2][3];
                float aw = vv[0]*S.u.ff_mat[3][0] + vv[1]*S.u.ff_mat[3][1]
                         + vv[2]*S.u.ff_mat[3][2] + vv[3]*S.u.ff_mat[3][3];
                float sx, sy, sz;
                if (aw <= 0.0f) continue;
                sx = ax/aw; sy = ay/aw; sz = az/aw;
                if (sx < 0 || sx > 640 || sy < 0 || sy > 480) continue;
                on++;
                if (sz < zlo) zlo = sz;
                if (sz > zhi) zhi = sz;
                if (aw < wlo) wlo = aw;
                if (aw > whi) whi = aw;
            }
            if (on)
                fprintf(stderr, "  [Z] batch of %u: %d on screen, "
                        "z/w %.0f..%.0f  -> depth %.6f..%.6f  "
                        "(%.4f%% of the range); w %.3f..%.3f (w-buffer %s)\n",
                        out_n, on, (double)zlo, (double)zhi,
                        (double)(zlo/16777215.0f), (double)(zhi/16777215.0f),
                        (double)(100.0f*(zhi-zlo)/16777215.0f),
                        (double)wlo, (double)whi, S.psh.w_buffer ? "on" : "off");
          }
          if (!shown)
              fprintf(stderr, "  [Z] composite matrix rows:\n"
                  "  [Z]   (%.4f %.4f %.4f %.4f)\n"
                  "  [Z]   (%.4f %.4f %.4f %.4f)\n"
                  "  [Z]   (%.4f %.4f %.4f %.4f)\n"
                  "  [Z]   (%.4f %.4f %.4f %.4f)\n",
                  S.u.ff_mat[0][0],S.u.ff_mat[0][1],S.u.ff_mat[0][2],S.u.ff_mat[0][3],
                  S.u.ff_mat[1][0],S.u.ff_mat[1][1],S.u.ff_mat[1][2],S.u.ff_mat[1][3],
                  S.u.ff_mat[2][0],S.u.ff_mat[2][1],S.u.ff_mat[2][2],S.u.ff_mat[2][3],
                  S.u.ff_mat[3][0],S.u.ff_mat[3][1],S.u.ff_mat[3][2],S.u.ff_mat[3][3]);
          shown++;
          fprintf(stderr, "  [Z] xf=%u v0=(%.2f %.2f %.2f %.2f) -> "
                  "p=(%.2f %.2f %.4f %.4f)  p/w=(%.1f %.1f %.6f)  "
                  "ndc.z=%.6f\n", S.xf_mode, v[0], v[1], v[2], v[3],
                  px, py, pz, pw,
                  pw != 0 ? px/pw : 0.0f, pw != 0 ? py/pw : 0.0f,
                  pw != 0 ? pz/pw : 0.0f,
                  pw != 0 ? 2.0f * (pz/pw) / 16777215.0f - 1.0f : 0.0f);
          fflush(stderr);
      } }
    /* The full-screen black curtain.
     *
     * One draw is responsible for the black tutorial: four vertices covering
     * exactly the viewport, no texture on any stage, diffuse RGB zero, alpha
     * around 0.93, blended SRC_ALPHA/ONE_MINUS_SRC_ALPHA. It takes the frame
     * from a mean of 80 to a mean of 5, which is black to the eye, and the
     * HUD drawn after it survives -- exactly what the picture shows.
     *
     * Two things are wanted here and neither is a fix yet: what its alpha
     * does over time (a fade that ramps and stops is a different bug from one
     * that never ramps), and what the frame looks like without it.
     *
     * RECOMP_GL_CURTAIN=log   report the alpha once a second
     * RECOMP_GL_CURTAIN=skip  drop the draw entirely, and report
     */
    int curtain_skip = 0;
    { static int mode_c = -1;
      if (mode_c < 0) { const char *v = getenv("RECOMP_GL_CURTAIN");
                        mode_c = !v ? 0 : !strcmp(v, "skip") ? 2 : 1; }
      if (mode_c && out_n == 4 && S.blend_enable
          && !S.tex[0].enabled && !S.tex[1].enabled
          && attr_at[3] >= 0 && attr_at[0] >= 0) {
          const float *c = &vbuf[(size_t)attr_at[3]];
          const float *p0 = &vbuf[(size_t)attr_at[0]];
          const float *p3 = &vbuf[(size_t)3 * stride_floats + (size_t)attr_at[0]];
          if (c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f && c[3] > 0.05f
              && p0[0] == 0.0f && p0[1] == 0.0f
              && p3[0] >= 320.0f && p3[1] >= 240.0f) {
              static double last; static unsigned seen;
              static uint32_t cur_frame = 0xFFFFFFFFu, per_frame, worst;
              static float first_alpha;
              double now = prof_now();
              seen++;
              /* How many times PER FRAME, not per second.
               *
               * This is the question that decides what the bug is. One quad
               * at alpha 0.93 dims a frame to 7%, which is dark but not
               * black; the same quad drawn three times leaves 0.03%, which is
               * black. A per-second count cannot tell those apart, and a
               * per-second count is what the first version of this printed.
               * If the answer is one, the alpha itself is wrong; if it is
               * more than one, the title is asking for one curtain and this
               * renderer is drawing several. */
              if (S.frames != cur_frame) {
                  if (per_frame > worst) worst = per_frame;
                  cur_frame = S.frames; per_frame = 0; first_alpha = c[3];
              }
              per_frame++;
              if (now - last > 1.0) {
                  last = now;
                  /* What it was TRYING to sample.
                   *
                   * This quad carries texture coordinates spanning the whole
                   * screen, which a fade to black has no use for. That is the
                   * shape of a composite -- the offscreen surface being drawn
                   * back over the display -- and the character draws all go to
                   * surface 0x00084000 while the display framebuffer is
                   * 0x001B2000. If this quad is the blit that brings that
                   * surface across, then skipping it (the current workaround
                   * for the black tutorial) is also what removes the
                   * characters, and the real bug is that its source is not
                   * being bound. Print every stage's offset even when the
                   * stage reads as disabled, because "disabled" is exactly
                   * what the broken case would look like. */
                  fprintf(stderr, "  [CURTAIN] full-screen quad %ux%u: "
                          "alpha %.3f, %u this frame (worst %u), %u total%s | "
                          "tex0 en=%d off=%08X %ux%u fmt=%08X | tex1 en=%d "
                          "off=%08X | surf=%08X | stencil %u func %X ref %X op %X/%X/%X"
                          " | blend %04X/%04X depth %d/%d func %X z %.1f w %.3f cm %08X\n",
                          (unsigned)p3[0], (unsigned)p3[1], c[3],
                          per_frame, worst, seen,
                          mode_c == 2 ? " -- skipping" : "",
                          S.tex[0].enabled, S.tex[0].offset,
                          S.tex[0].width, S.tex[0].height, S.tex[0].format,
                          S.tex[1].enabled, S.tex[1].offset, S.color_offset,
                          S.stencil_en, S.stencil_func, S.stencil_ref,
                          S.stencil_op[0], S.stencil_op[1], S.stencil_op[2],
                          S.blend_src, S.blend_dst, S.depth_test, S.depth_mask,
                          (unsigned)S.depth_func, p0[2], p0[3], S.color_mask);
                  (void)first_alpha;
                  fflush(stderr);
              }
              /* With the stencil emulated, a curtain drawn under the
               * stencil test is the shadow pass, masked to the shadows: it
               * stays. One without it is a real fade, and is still skipped
               * under RECOMP_GL_CURTAIN=skip. */
              if (mode_c == 2 && !(S.stencil_en && !getenv("RECOMP_GL_NOSTENCIL")))
                  curtain_skip = 1;
          }
      } }
    /* Skipped by suppressing the draw call, NOT by returning.
     *
     * The first version of this returned from here, which quietly skipped the
     * eleven hundred lines of bookkeeping that follow every draw in this
     * function -- counters, and whatever state the next draw expects to have
     * been left behind. A renderer workaround that silently changes the state
     * machine is worse than the bug it works around, and harder to see. */
    /* Draws per program, for THIS frame.
     *
     * The question that should have been asked first: which programs draw in
     * the attract scene and not in the tutorial? The slots present in one and
     * missing in the other name the missing objects directly. Counting per
     * frame rather than per run is what makes the comparison honest -- runs
     * diverge, so two runs compared by dump number are two different scenes,
     * which is exactly the mistake that cost a day. Written out beside the
     * screenshot, so the census and the picture are the same frame. */
    if (slot >= 0 && slot < SLOTCENSUS_MAX) g_slot_draws[slot]++;

    /* Which surface each CLASS of geometry is drawn into.
     *
     * The characters in the tutorial are submitted, positioned sensibly,
     * correctly transformed, textured and blended -- and invisible. Every one
     * of those checks verifies that a draw is well formed; none of them asks
     * whether it is going to the surface that ends up on screen. Every
     * character draw logged so far says surf=00084000, and the display
     * framebuffer in the same run is 0x001B2000. If the world goes to one and
     * the movers to another, that is the entire bug and nothing else needs
     * explaining.
     *
     * Split by mover vs world, because that is the split that matters, and
     * printed every five seconds so a playthrough produces it without any
     * switches. */
    { static struct { uint32_t surf; unsigned long mover, world; } cls[8];
      static int ncls; static double last_cls;
      int mover = (slot >= 0 && g_pcache[slot].uses_a0
                   && S.xf_mode != NV2A_XF_MODE_FIXED);
      int ci;
      double now_cls = g_frame_clock;
      for (ci = 0; ci < ncls; ci++) if (cls[ci].surf == S.color_offset) break;
      if (ci == ncls && ncls < 8) { cls[ncls].surf = S.color_offset; ncls++; }
      if (ci < ncls) { if (mover) cls[ci].mover++; else cls[ci].world++; }
      if (now_cls - last_cls > 5.0) {
          last_cls = now_cls;
          for (ci = 0; ci < ncls; ci++)
              fprintf(stderr, "  [SURFACE] %08X: %lu mover draws, %lu world "
                      "draws\n", cls[ci].surf, cls[ci].mover, cls[ci].world);
          fflush(stderr);
      } }

    /* Where the movers are being PUT, once every few seconds, always on.
     *
     * The player's reading of the tutorial is that the characters are in the
     * wrong place rather than missing, and that is a different bug from
     * everything tested so far -- every check to date has verified that the
     * machinery is correct, never that the answer it produces is sensible.
     * The object matrix's fourth column is the world position, so printing it
     * for a few movers says immediately whether they are standing next to the
     * camera or a kilometre underground.
     *
     * Always on and rate-limited rather than behind a switch, because the
     * scripted input reaches the tutorial perhaps one run in four while he
     * reaches it every time. Instrumentation he can produce by playing is
     * worth more than instrumentation only I can trigger. */
    if (slot >= 0 && g_pcache[slot].uses_a0 && out_n >= 3
        && S.xf_mode != NV2A_XF_MODE_FIXED) {
        static double last_pos; static int per_burst;
        double now_pos = g_frame_clock;
        if (now_pos - last_pos > 5.0) { last_pos = now_pos; per_burst = 0; }
        if (per_burst < 4 && now_pos - last_pos < 0.25) {
            Nv2aVshProgram vp2;
            uint32_t ps2 = S.prog_start < NV2A_VSH_MAX_INSNS ? S.prog_start : 0;
            int len2 = nv2a_vsh_decode(S.prog + ps2 * 4,
                                       (int)(NV2A_VSH_MAX_INSNS - ps2), &vp2);
            int q3, base3 = -1, a0s3[8], na3, k3;
            float vin3[16][4];
            for (q3 = 0; q3 < len2; q3++)
                if (vp2.insns[q3].rel_addr) { base3 = vp2.insns[q3].const_index; break; }
            if (base3 >= 0) {
                /* The draw's first vertex, converted here: a draw from the
                 * vertex cache has no converted copy in vbuf to read. */
                float v03[NUM_ATTRS * 4];
                vtx_convert(live, nlive, S.idx[0], v03);
                memset(vin3, 0, sizeof vin3);
                for (k3 = 0; k3 < NUM_ATTRS; k3++) {
                    if (attr_at[k3] >= 0)
                        memcpy(vin3[k3], &v03[attr_at[k3]], 16);
                    else memcpy(vin3[k3], S.const_attr[k3], 16);
                }
                na3 = nv2a_vsh_interp_a0(&vp2, vin3, S.u.c, a0s3, 8);
                if (na3 > 0) {
                    int idx3 = base3 + a0s3[0];
                    if (idx3 >= 0 && idx3 + 3 < NV2A_VSH_NUM_CONSTS) {
                        per_burst++;
                        /* Position alone was not enough: the tutorial's
                         * characters turned out to be placed perfectly
                         * sensibly and still not appear. The same programs
                         * (343, 525, 1893) draw VISIBLE characters on the
                         * attract screen, so whatever differs is state, and
                         * the only way to find it is to print the state on
                         * both sides and diff them. Everything that decides
                         * whether a fragment survives goes on the line. */
                        { float dif3[4] = {0,0,0,0};
                          if (attr_at[3] >= 0) memcpy(dif3, &v03[attr_at[3]], 16);
                          else memcpy(dif3, S.const_attr[3], 16);
                        fprintf(stderr, "  [MOVERPOS] slot %4d verts %4u A0=%d "
                                "c[%d] -> world (%.1f %.1f %.1f) | diffuse "
                                "(%.2f %.2f %.2f a=%.2f) | blend=%d(%04X/%04X) "
                                "alphatest=%d func=%04X ref=%.2f | depth=%d/%04X "
                                "write=%d | tex0=%d fmt=%08X %ux%u | surf=%08X "
                                "prim=%u cull=%d/%04X\n",
                                slot, out_n, a0s3[0], idx3,
                                S.u.c[idx3][3], S.u.c[idx3+1][3], S.u.c[idx3+2][3],
                                dif3[0], dif3[1], dif3[2], dif3[3],
                                S.blend_enable, S.blend_src, S.blend_dst,
                                S.alpha_test, (unsigned)S.alpha_func, S.alpha_ref,
                                S.depth_test, S.depth_func, S.depth_mask,
                                S.tex[0].enabled, S.tex[0].format,
                                S.tex[0].width, S.tex[0].height,
                                S.color_offset, S.prim,
                                S.cull_enable, S.cull_face); }
                        fflush(stderr);
                    }
                }
            }
        }
    }
    /* ---- [MOVERBOX] and RECOMP_GL_XRAY ---------------------------------
     *
     * Every theory so far rested on one premise: that programs 343, 525 and
     * 1893 draw VISIBLE characters on the attract screen, so the machinery
     * must work and only the tutorial's state can be wrong. The attract
     * reference run settles that the premise is false. It compiles four
     * programs, not one of which indexes the constant palette, and its
     * surface census counts zero palette-indexed draws in the entire run.
     * Nothing in this port has ever rendered a skinned mesh visibly. The
     * skinning path itself is the suspect; the tutorial is simply the first
     * scene that uses it, which is also why it would fail on every level.
     *
     * Two measurements, in one run, that cannot both come back ambiguous.
     *
     * [MOVERBOX] runs the CPU interpreter over a sample of the draw's REAL
     * vertices rather than vertex zero alone. The generated GLSL is checked
     * against that interpreter instruction by instruction, so it is a
     * faithful stand-in for what the GPU computes. It reports the clip-space
     * box the whole mesh occupies, how many vertices fall behind the eye, how
     * many land on screen, and how many distinct addresses the ARL produces
     * across the sample. Vertex zero cannot distinguish a correctly placed
     * mesh from one whose every other vertex is flung across the map by a bad
     * bone index; the box can. A world draw is measured identically in the
     * same burst, as a control.
     *
     * RECOMP_GL_XRAY (1 by default here) draws every palette-indexed batch
     * with blending off, alpha test off, culling off and the depth comparison
     * always passing -- every way a fragment can be discarded, removed. If
     * the characters appear, the geometry was always right and the loss is in
     * the pixel path. If they still do not, no pixel state was ever the
     * problem. One glance answers it.
     */
    /* [SINTAB] -- is the game's sine table still a sine table?
     *
     * Every joint rotation in this title goes through a 4096-entry table of
     * (sin, cos) pairs that sub_0014C910 builds once at start-up, in memory
     * it gets from VirtualAlloc, and whose address it leaves at 0x264BA4.
     * The characters' skeletons come out folded -- a spine bent a hundred
     * and thirteen degrees -- in the tutorial but not on the title screen,
     * and the player sees them twitch. A table that something else later writes
     * over produces exactly that: correct early, garbage afterwards, and a
     * different garbage every frame as the other owner keeps writing.
     *
     * Two checks. Every five seconds, compare all 4096 entries with the true
     * values and say how many are wrong. And the first time the table exists,
     * arm the store watch on it (RECOMP_WATCH_SINTAB=1): the generator has
     * finished by then, so any writer the watch reports is the culprit. */
    { static double last_st; static int armed_st = -1;
      extern uint32_t g_recomp_watch_lo, g_recomp_watch_span;
      uint32_t tab = *(const uint32_t *)guest(0x264BA4);
      double now_st = g_frame_clock;
      if (armed_st < 0) armed_st = getenv("RECOMP_WATCH_SINTAB") ? 0 : 2;
      if (tab && armed_st == 0) {
          g_recomp_watch_lo = tab; g_recomp_watch_span = 0x8000;
          armed_st = 1;
          fprintf(stderr, "  [SINTAB] table at %08X; store watch armed on it\n", tab);
      }
      if (tab && now_st - last_st > 5.0) {
          const float *t = (const float *)guest(tab);
          int bad = 0, first = -1, i;
          last_st = now_st;
          for (i = 0; i < 4096; i++) {
              double a = 2.0 * 3.14159265358979323846 * i / 4096.0;
              if (fabs(t[2*i] - sin(a)) > 0.002 || fabs(t[2*i+1] - cos(a)) > 0.002) {
                  if (first < 0) first = i;
                  bad++;
              }
          }
          fprintf(stderr, "  [SINTAB] table at %08X: %d of 4096 entries wrong",
                  tab, bad);
          if (first >= 0)
              fprintf(stderr, " (first at %d: sin %.4f cos %.4f, should be %.4f"
                      " %.4f)", first, t[2*first], t[2*first+1],
                      sin(2.0 * 3.14159265358979323846 * first / 4096.0),
                      cos(2.0 * 3.14159265358979323846 * first / 4096.0));
          fprintf(stderr, "\n");
          fflush(stderr);
      } }

    /* [PIPE] -- how much geometry each transform path draws, and how big.
     *
     * Every probe in this file skips fixed-function draws, so that half of
     * the renderer has never been measured at all. It also has a known hole:
     * a batch the title asks the hardware to LIGHT gets `oD0 = v3.rgb +
     * litAmbient`, and litAmbient is zero unless someone sets an environment
     * variable -- there is no lighting in the fixed-function path. Anything
     * substantial drawn that way would come out unlit, which for a character
     * means a dark silhouette. Whether that matters depends entirely on
     * whether characters are ever drawn this way, and nothing so far has
     * asked. Count the draws and their sizes on both paths. */
    /* Character batches in the frame being built, for a capture that aims
     * itself. Guessing a present number to dump at has now missed twice --
     * 9000 was the title screen, 16000 a loading screen -- and each miss
     * costs a three-minute run. A frame with several big skinned batches in
     * it IS the tutorial, so let the frame say so. */
    if (slot >= 0 && g_pcache[slot].uses_a0 && n >= 150
        && S.xf_mode != NV2A_XF_MODE_FIXED) g_frame_chars++;

    { static double last_pipe; static unsigned long fixed_n, prog_n;
      static unsigned long fixed_big, prog_big;
      double now_pipe = g_frame_clock;
      if (S.xf_mode == NV2A_XF_MODE_FIXED) {
          fixed_n++; if (n >= 150) fixed_big++;
      } else {
          prog_n++;  if (n >= 150) prog_big++;
      }
      if (now_pipe - last_pipe > 5.0) {
          last_pipe = now_pipe;
          fprintf(stderr, "  [PIPE] fixed-function %lu draws (%lu of them 150+"
                  " verts) | programmable %lu draws (%lu of them 150+ verts)\n",
                  fixed_n, fixed_big, prog_n, prog_big);
          fflush(stderr);
      } }

    { int is_mover = (slot >= 0 && g_pcache[slot].uses_a0
                      && S.xf_mode != NV2A_XF_MODE_FIXED);
      static double last_box; static int box_mover, box_world;
      int probe_this = 0;
      { double now_box = g_frame_clock;
        if (now_box - last_box > 5.0) { last_box = now_box;
                                        box_mover = 0; box_world = 0; } }
      /* RECOMP_GL_CHARDIAG=1 turns on everything in this block. It runs a
       * CPU interpreter over sampled vertices, reads back occlusion queries
       * (a GPU stall) and prints a great deal, so it stays off in the build
       * that is actually played. */
      static int chardiag = -1;
      if (chardiag < 0) chardiag = getenv("RECOMP_GL_CHARDIAG") ? 1 : 0;
      if (chardiag && out_n >= 3 && slot >= 0 && S.xf_mode != NV2A_XF_MODE_FIXED
          /* Characters, not street furniture. Every burst so far was spent
           * on two-bone lamp posts and bollards, because they are drawn first
           * and the budget ran out before a skater arrived. A character batch
           * is hundreds of vertices; a prop is a few dozen. */
          && (is_mover ? (box_mover < 12 && out_n >= 150) : box_world < 8)) {
          Nv2aVshProgram vpb;
          uint32_t psb = S.prog_start < NV2A_VSH_MAX_INSNS ? S.prog_start : 0;
          int lenb = nv2a_vsh_decode(S.prog + psb * 4,
                                     (int)(NV2A_VSH_MAX_INSNS - psb), &vpb);
          float vinb[16][4], opb[4];
          float lo[3], hi[3], wlo = 1e30f, whi = -1e30f;
          int kb, nb = 0, behind = 0, onscr = 0, a0seen[16], na0 = 0;
          uint32_t qb, step = out_n > 64u ? out_n / 64u : 1u;
          lo[0] = lo[1] = lo[2] =  1e30f;
          hi[0] = hi[1] = hi[2] = -1e30f;
          if (is_mover) box_mover++; else box_world++;
          probe_this = 1;
          for (qb = 0; qb < out_n; qb += step) {
              int a0s[8], nas, j;
              memset(vinb, 0, sizeof vinb);
              for (kb = 0; kb < NUM_ATTRS; kb++) {
                  if (attr_at[kb] >= 0)
                      memcpy(vinb[kb], &vbuf[(size_t)qb * stride_floats
                                             + attr_at[kb]], 16);
                  else memcpy(vinb[kb], S.const_attr[kb], 16);
              }
              nas = nv2a_vsh_interp_a0(&vpb, vinb, S.u.c, a0s, 8);
              for (j = 0; j < nas; j++) {
                  int f, dup = 0;
                  for (f = 0; f < na0; f++)
                      if (a0seen[f] == a0s[j]) { dup = 1; break; }
                  if (!dup && na0 < 16) a0seen[na0++] = a0s[j];
              }
              if (!nv2a_vsh_interp(&vpb, vinb, S.u.c, opb)) continue;
              nb++;
              if (opb[3] < wlo) wlo = opb[3];
              if (opb[3] > whi) whi = opb[3];
              if (opb[3] <= 0.0f) behind++;
              /* The epilogue's arithmetic, not a guess at it.
               *
               * Dividing oPos by w here was wrong and produced depths in the
               * tens of thousands, which reads as "clipped" and is not what
               * the shader does: this title's programs apply the viewport
               * themselves, so oPos.xyz arrives already in screen space and
               * the epilogue only undoes the viewport to get back to NDC.
               * Copying its three lines exactly is the only way the number
               * printed here is the number the GPU used. */
              { float n3[3]; int a;
                n3[0] = (opb[0] - S.u.vp_off[0])
                      / (S.u.vp_scale[0] != 0.0f ? S.u.vp_scale[0] : 1.0f);
                n3[1] = (opb[1] - S.u.vp_off[1])
                      / (S.u.vp_scale[1] != 0.0f ? S.u.vp_scale[1] : 1.0f);
                n3[2] = (S.u.z_clip[1] > S.u.z_clip[0])
                      ? (opb[2] - S.u.z_clip[0])
                        / (S.u.z_clip[1] - S.u.z_clip[0])
                      : opb[2];
                for (a = 0; a < 3; a++) {
                    if (n3[a] < lo[a]) lo[a] = n3[a];
                    if (n3[a] > hi[a]) hi[a] = n3[a];
                }
                if (n3[0] >= -1.0f && n3[0] <= 1.0f
                 && n3[1] >= -1.0f && n3[1] <= 1.0f) onscr++; }
          }
          if (nb > 0) {
              int f;
              /* What the mesh looks like BEFORE any transform, and what the
               * title fed in to build it.
               *
               * The batches that come out right have one bone; the batches
               * that vanish have up to sixteen, and their whole mesh lands
               * inside a handful of pixels -- a 1428-vertex character inside
               * a 6x12 box is not far away, it is collapsed. Either the
               * object-space positions arrive collapsed, which is a vertex
               * decode problem, or they arrive as a character and the blend
               * collapses them, which is the palette. The input attributes
               * and the raw v0 extent separate those two, and nothing
               * printed so far has looked at either. */
              { float p0lo[3], p0hi[3]; int a2; uint32_t q2;
                p0lo[0] = p0lo[1] = p0lo[2] =  1e30f;
                p0hi[0] = p0hi[1] = p0hi[2] = -1e30f;
                for (q2 = 0; q2 < out_n; q2 += step) {
                    const float *pv = attr_at[0] >= 0
                        ? &vbuf[(size_t)q2 * stride_floats + attr_at[0]]
                        : S.const_attr[0];
                    for (a2 = 0; a2 < 3; a2++) {
                        if (pv[a2] < p0lo[a2]) p0lo[a2] = pv[a2];
                        if (pv[a2] > p0hi[a2]) p0hi[a2] = pv[a2];
                    }
                }
                fprintf(stderr, "  [VERTIN]  %-5s slot %4d | v0 object space"
                        " x[%.2f %.2f] y[%.2f %.2f] z[%.2f %.2f] | attrs:",
                        is_mover ? "MOVER" : "world", slot,
                        p0lo[0], p0hi[0], p0lo[1], p0hi[1], p0lo[2], p0hi[2]);
                for (a2 = 0; a2 < NUM_ATTRS; a2++) {
                    const Attr *ad = &S.attr[a2];
                    if (attr_at[a2] >= 0)
                        fprintf(stderr, " v%d=type%u/size%u/stride%u",
                                a2, ad->type, ad->size, ad->stride);
                    else if (g_pcache[slot].inputs & (1u << a2))
                        fprintf(stderr, " v%d=const(%.2f %.2f %.2f %.2f)", a2,
                                S.const_attr[a2][0], S.const_attr[a2][1],
                                S.const_attr[a2][2], S.const_attr[a2][3]);
                }
                /* The scale test.
                 *
                 * A perspective camera has one focal length in pixels, and
                 * every object in the frame obeys it: something h units tall
                 * at distance w covers h * f / w pixels. Turned around,
                 * f = pixels * w / units is the SAME number for every draw in
                 * the frame, whatever its distance and whatever its size.
                 * Printing it per draw makes a mis-scaled batch obvious
                 * without needing to know the camera: world geometry agrees
                 * with itself and sets the reference, and anything that
                 * disagrees is being drawn at the wrong size. That matters
                 * here because the player's reading of the x-ray was "far away or
                 * not where expected", and too small is what too far away
                 * looks like.
                 *
                 * (An object matrix carrying its own scale shifts this, so a
                 * few percent means nothing. A factor of two or three does.) */
                { float du = p0hi[1] - p0lo[1];
                  float dn = hi[1] - lo[1];
                  float wm = 0.5f * (wlo + whi);
                  if (du > 0.05f && dn > 0.0f && wm > 0.0f)
                      fprintf(stderr, " | focal %.0f px",
                              dn * 0.5f * (float)g_fbo_h * wm / du);
                }
                fprintf(stderr, "\n"); }

              /* [BONES] -- every matrix this batch actually reads.
               *
               * The player's screenshots settle what the numbers could not. The
               * dog renders correctly. A GG renders too -- but perhaps a
               * third of the size it should be, lying flat on the pavement
               * with its limbs splayed. Two speaking characters are absent
               * from their own dialogue shots. That is not a missing draw,
               * a rejected fragment or a misplaced object: that is a
               * skeleton being posed wrongly, and the simple models that
               * survive are the ones with few bones.
               *
               * So print the palette entries themselves, one per distinct
               * address the ARL produced, with the length of each basis
               * vector and the determinant. A rigid bone matrix has three
               * unit-length rows and a determinant of +1. A row that is
               * short flattens whatever it drives; a determinant near zero
               * collapses the limb; a wrong row order lays a character on
               * the floor -- which is the picture. */
              /* [POSE] -- the character's real dimensions, in world space.
               *
               * "Lying down" is a reading of a screenshot; this is the
               * measurement. Skin each sampled vertex on the CPU with the
               * first bone its own program selects, and take the extent of
               * the result. A standing character is about seventeen units
               * tall and two or three deep. One that is seventeen units long
               * along the ground and three tall is face down, and the number
               * says so without anyone having to squint at a frame. */
              if (is_mover && na0 > 0) {
                  int base_p = -1, q5;
                  for (q5 = 0; q5 < lenb; q5++)
                      if (vpb.insns[q5].rel_addr) {
                          base_p = vpb.insns[q5].const_index; break;
                      }
                  if (base_p >= 0) {
                      float wlo2[3], whi2[3]; int any = 0, a5;
                      uint32_t q6;
                      wlo2[0] = wlo2[1] = wlo2[2] =  1e30f;
                      whi2[0] = whi2[1] = whi2[2] = -1e30f;
                      for (q6 = 0; q6 < out_n; q6 += step) {
                          int a0s2[8], nas2, kk, idx2;
                          float vv[16][4], wp[3];
                          memset(vv, 0, sizeof vv);
                          for (kk = 0; kk < NUM_ATTRS; kk++) {
                              if (attr_at[kk] >= 0)
                                  memcpy(vv[kk], &vbuf[(size_t)q6 * stride_floats
                                                       + attr_at[kk]], 16);
                              else memcpy(vv[kk], S.const_attr[kk], 16);
                          }
                          nas2 = nv2a_vsh_interp_a0(&vpb, vv, S.u.c, a0s2, 8);
                          if (nas2 < 1) continue;
                          idx2 = base_p + a0s2[0];
                          if (idx2 < 0 || idx2 + 2 >= NV2A_VSH_NUM_CONSTS) continue;
                          for (a5 = 0; a5 < 3; a5++)
                              wp[a5] = S.u.c[idx2+a5][0] * vv[0][0]
                                     + S.u.c[idx2+a5][1] * vv[0][1]
                                     + S.u.c[idx2+a5][2] * vv[0][2]
                                     + S.u.c[idx2+a5][3];
                          any = 1;
                          for (a5 = 0; a5 < 3; a5++) {
                              if (wp[a5] < wlo2[a5]) wlo2[a5] = wp[a5];
                              if (wp[a5] > whi2[a5]) whi2[a5] = wp[a5];
                          }
                      }
                      if (any)
                          fprintf(stderr, "  [POSE]    slot %4d %2d bones:"
                                  " world size %.1f wide x %.1f tall x %.1f"
                                  " deep, centred (%.0f %.0f %.0f)\n",
                                  slot, na0,
                                  whi2[0] - wlo2[0], whi2[1] - wlo2[1],
                                  whi2[2] - wlo2[2],
                                  0.5f * (wlo2[0] + whi2[0]),
                                  0.5f * (wlo2[1] + whi2[1]),
                                  0.5f * (wlo2[2] + whi2[2]));
                  }
              }
              if (is_mover && na0 > 0) {
                  int bi, base_c = -1, q4;
                  for (q4 = 0; q4 < lenb; q4++)
                      if (vpb.insns[q4].rel_addr) {
                          base_c = vpb.insns[q4].const_index; break;
                      }
                  fprintf(stderr, "  [BONES]   slot %4d base c[%d], %d bones\n",
                          slot, base_c, na0);
                  for (bi = 0; bi < na0 && bi < 6 && base_c >= 0; bi++) {
                      int idx = base_c + a0seen[bi], r2;
                      float len[3], det;
                      if (idx < 0 || idx + 3 >= NV2A_VSH_NUM_CONSTS) {
                          fprintf(stderr, "  [BONES]     A0=%d -> c[%d] OUT OF"
                                  " RANGE\n", a0seen[bi], idx);
                          continue;
                      }
                      for (r2 = 0; r2 < 3; r2++) {
                          const float *rw = S.u.c[idx + r2];
                          len[r2] = sqrtf(rw[0]*rw[0] + rw[1]*rw[1] + rw[2]*rw[2]);
                      }
                      { const float *a = S.u.c[idx], *b2 = S.u.c[idx+1],
                                    *c2 = S.u.c[idx+2];
                        det = a[0]*(b2[1]*c2[2] - b2[2]*c2[1])
                            - a[1]*(b2[0]*c2[2] - b2[2]*c2[0])
                            + a[2]*(b2[0]*c2[1] - b2[1]*c2[0]); }
                      fprintf(stderr, "  [BONES]     A0=%3d c[%3d..%3d] row"
                              " lengths %.3f %.3f %.3f det %+.3f | translation"
                              " (%.2f %.2f %.2f) | row3 (%.2f %.2f %.2f %.2f)\n",
                              a0seen[bi], idx, idx + 3,
                              len[0], len[1], len[2], det,
                              S.u.c[idx][3], S.u.c[idx+1][3], S.u.c[idx+2][3],
                              S.u.c[idx+3][0], S.u.c[idx+3][1],
                              S.u.c[idx+3][2], S.u.c[idx+3][3]);
                  }
              }
              /* [VPCHK] -- the viewport the PROGRAM applied versus the one the
               * epilogue removes.
               *
               * In Corn's dialogue shot every character batch lands at the
               * right distance and dead centre horizontally, but with NDC y
               * between -1 and -3.5: below the bottom of the screen. This
               * title's programs apply a viewport themselves, from their own
               * constants, and the epilogue then undoes it using the hardware
               * viewport REGISTERS. Those two only have to disagree once --
               * say for a letterboxed cutscene, whose black bars are exactly
               * what that shot has -- for everything drawn by a program to
               * slide vertically while the fixed-function world, which does
               * its own divide, stays put. Print both, and the raw position
               * of vertex zero, for every probed draw. */
              { float vin0[16][4], op0[4]; int k0;
                memset(vin0, 0, sizeof vin0);
                for (k0 = 0; k0 < NUM_ATTRS; k0++) {
                    if (attr_at[k0] >= 0)
                        memcpy(vin0[k0], &vbuf[(size_t)attr_at[k0]], 16);
                    else memcpy(vin0[k0], S.const_attr[k0], 16);
                }
                if (nv2a_vsh_interp(&vpb, vin0, S.u.c, op0))
                    fprintf(stderr, "  [VPCHK] %-5s slot %4d | raw oPos (%.2f %.2f"
                            " %.1f w=%.3f) | c58 (%.2f %.2f %.1f %.2f) c59 (%.2f"
                            " %.2f %.1f %.2f) | regs scale (%.1f %.1f) off (%.1f"
                            " %.1f)\n", is_mover ? "MOVER" : "world", slot,
                            op0[0], op0[1], op0[2], op0[3],
                            S.u.c[58][0], S.u.c[58][1], S.u.c[58][2], S.u.c[58][3],
                            S.u.c[59][0], S.u.c[59][1], S.u.c[59][2], S.u.c[59][3],
                            S.u.vp_scale[0], S.u.vp_scale[1],
                            S.u.vp_off[0], S.u.vp_off[1]); }

              /* [CAMDUMP] -- the program and every constant it reads, once in
               * a dialogue close-up and once on the title screen.
               *
               * The viewport the program applies matches the registers, so
               * the epilogue is innocent: it is the program itself that puts
               * Corn's vertices at screen y 823 and 2468 on a 480-line frame.
               * A program only knows the camera through its constants, so the
               * constants are the question. Same program, same slot, two
               * scenes -- one where the characters land correctly and one
               * where they fall off the bottom -- and a diff of what it read. */
              { static int shown_close, shown_far;
                float wv = 0.0f;
                { float vin1[16][4], op1[4]; int k1;
                  memset(vin1, 0, sizeof vin1);
                  for (k1 = 0; k1 < NUM_ATTRS; k1++) {
                      if (attr_at[k1] >= 0)
                          memcpy(vin1[k1], &vbuf[(size_t)attr_at[k1]], 16);
                      else memcpy(vin1[k1], S.const_attr[k1], 16);
                  }
                  if (nv2a_vsh_interp(&vpb, vin1, S.u.c, op1)) wv = op1[3]; }
                if (is_mover && slot == 343
                    && ((wv > 0.0f && wv < 20.0f && shown_close < 2)
                     || (wv > 100.0f && shown_far < 1))) {
                    if (wv < 20.0f) shown_close++; else shown_far++;
                    fprintf(stderr, "===== CAMDUMP slot %d %s (vertex 0 w=%.2f)"
                            " =====\n", slot, wv < 20.0f ? "DIALOGUE" : "TITLE",
                            wv);
                    if (g_pcache[slot].dis) fputs(g_pcache[slot].dis, stderr);
                    fprintf(stderr, "----- non-palette constants it names -----\n");
                    if (g_pcache[slot].dis) {
                        const char *q = g_pcache[slot].dis;
                        int seen2[NV2A_VSH_NUM_CONSTS];
                        memset(seen2, 0, sizeof seen2);
                        while ((q = strstr(q, "c[")) != NULL) {
                            int n2 = atoi(q + 2);
                            q += 2;
                            if (n2 >= 0 && n2 < 107 && !seen2[n2]) {
                                seen2[n2] = 1;
                                fprintf(stderr, "  c[%3d] = %12.5f %12.5f %12.5f"
                                        " %12.5f\n", n2, S.u.c[n2][0],
                                        S.u.c[n2][1], S.u.c[n2][2], S.u.c[n2][3]);
                            }
                        }
                    }
                    fprintf(stderr, "----- fixed-function composite (the world's"
                            " camera) -----\n");
                    { int r3;
                      for (r3 = 0; r3 < 4; r3++)
                          fprintf(stderr, "  ff[%d] = %12.5f %12.5f %12.5f"
                                  " %12.5f\n", r3, S.u.ff_mat[r3][0],
                                  S.u.ff_mat[r3][1], S.u.ff_mat[r3][2],
                                  S.u.ff_mat[r3][3]); }
                    /* Where the model's own up axis points, as the camera
                     * sees it. The palette maps model space straight to view
                     * space (the projection in c[103..106] carries no
                     * rotation), so column 1 of a bone matrix IS the model's
                     * up direction in camera terms. Upright relative to the
                     * camera reads (0, 1, 0); a character lying on the ground
                     * in front of it reads (0, 0, +-1). */
                    { int bi2;
                      for (bi2 = 0; bi2 < na0 && bi2 < 4; bi2++) {
                          int ix = 107 + a0seen[bi2];
                          if (ix < 0 || ix + 2 >= NV2A_VSH_NUM_CONSTS) continue;
                          fprintf(stderr, "  bone A0=%2d: model up -> view (%.3f"
                                  " %.3f %.3f)  model fwd -> view (%.3f %.3f"
                                  " %.3f)  at (%.1f %.1f %.1f)\n", a0seen[bi2],
                                  S.u.c[ix][1], S.u.c[ix+1][1], S.u.c[ix+2][1],
                                  S.u.c[ix][2], S.u.c[ix+1][2], S.u.c[ix+2][2],
                                  S.u.c[ix][3], S.u.c[ix+1][3], S.u.c[ix+2][3]);
                      } }
                    /* [FINDMAT] removed: it scanned all of guest RAM and
                     * faulted on the first unmapped page, which crashed the
                     * game at the first dialogue close-up. It found what it
                     * was for -- the constant shadow at 0x19BD98. */
                    fprintf(stderr, "===== end CAMDUMP =====\n");
                    fflush(stderr);
                } }

              /* [DIST] -- how far everything in the frame is from the eye.
               *
               * The characters measure 16 units tall in world space, standing,
               * with clean bones -- so they are modelled and posed correctly
               * and they render about twenty pixels tall only because they sit
               * 150 to 800 units away. Either that is where they belong, or
               * they are being placed far behind where the camera expects
               * them. The world geometry settles it: the pavement directly
               * under the camera in the player's screenshots is a few units away,
               * so if world draws reach small distances and no skinned draw
               * ever does, the characters are in the wrong place. Printed for
               * a wide sample of both, one burst every five seconds. */
              /* Does this program write the fog output at all, and what is
               * the fog state around it? The isolated render -- characters
               * alone on black -- showed vehicles and trees in full colour
               * and every humanoid as a near-black silhouette, which is a
               * colour fault, not a geometry one. The combiner's fog register
               * is the one input that can black out a whole batch, so print
               * who writes it. */
              /* [RIG] -- is each bone driving one part of the body?
               *
               * In a correct rig the vertices that share a bone are a
               * contiguous lump of the model: an upper arm, a shin, the head.
               * If the per-vertex bone index is being decoded wrongly, the
               * vertices on any given bone are scattered over the whole mesh
               * instead, and the character comes out as a splayed tangle with
               * roughly the right overall size -- which is precisely what
               * the player's fourth screenshot shows, and which every measurement
               * of bounding boxes and bone matrices so far would call healthy.
               *
               * So measure it: group the sampled vertices by the address
               * their own program's ARL produces, and print how large a slice
               * of the model each bone covers. A bone whose vertices span the
               * entire model is the bug, stated as a number. */
              if (is_mover && na0 > 3) {
                  float mdl[3], mlo[3], mhi[3]; int b2, a7; uint32_t q9;
                  mlo[0] = mlo[1] = mlo[2] =  1e30f;
                  mhi[0] = mhi[1] = mhi[2] = -1e30f;
                  for (q9 = 0; q9 < out_n; q9 += step) {
                      const float *pv2 = attr_at[0] >= 0
                          ? &vbuf[(size_t)q9 * stride_floats + attr_at[0]]
                          : S.const_attr[0];
                      for (a7 = 0; a7 < 3; a7++) {
                          if (pv2[a7] < mlo[a7]) mlo[a7] = pv2[a7];
                          if (pv2[a7] > mhi[a7]) mhi[a7] = pv2[a7];
                      }
                  }
                  mdl[0] = mhi[0] - mlo[0];
                  mdl[1] = mhi[1] - mlo[1];
                  mdl[2] = mhi[2] - mlo[2];
                  fprintf(stderr, "  [RIG]    slot %4d, model %.1f x %.1f x"
                          " %.1f, %d bones:\n", slot, mdl[0], mdl[1], mdl[2],
                          na0);
                  for (b2 = 0; b2 < na0 && b2 < 6; b2++) {
                      float blo[3], bhi[3]; int n2 = 0, a6; uint32_t q8;
                      blo[0] = blo[1] = blo[2] =  1e30f;
                      bhi[0] = bhi[1] = bhi[2] = -1e30f;
                      for (q8 = 0; q8 < out_n; q8 += step) {
                          int a0s3[8], nas3, kk3;
                          float vv3[16][4];
                          memset(vv3, 0, sizeof vv3);
                          for (kk3 = 0; kk3 < NUM_ATTRS; kk3++) {
                              if (attr_at[kk3] >= 0)
                                  memcpy(vv3[kk3], &vbuf[(size_t)q8 * stride_floats
                                                         + attr_at[kk3]], 16);
                              else memcpy(vv3[kk3], S.const_attr[kk3], 16);
                          }
                          nas3 = nv2a_vsh_interp_a0(&vpb, vv3, S.u.c, a0s3, 8);
                          if (nas3 < 1 || a0s3[0] != a0seen[b2]) continue;
                          n2++;
                          for (a6 = 0; a6 < 3; a6++) {
                              if (vv3[0][a6] < blo[a6]) blo[a6] = vv3[0][a6];
                              if (vv3[0][a6] > bhi[a6]) bhi[a6] = vv3[0][a6];
                          }
                      }
                      if (n2 > 0)
                          fprintf(stderr, "  [RIG]      bone A0=%3d drives %3d"
                                  " verts spanning %.1f x %.1f x %.1f  (%.0f%%"
                                  " of the model's height)\n",
                                  a0seen[b2], n2, bhi[0]-blo[0], bhi[1]-blo[1],
                                  bhi[2]-blo[2],
                                  mdl[1] > 0.01f
                                      ? 100.0f * (bhi[1]-blo[1]) / mdl[1] : 0.0f);
                  }
              }

              /* [COLOUR] -- what the program hands the combiner.
               *
               * Rendering the skinned draws alone on black showed the answer
               * the statistics never could: vehicles and trees come out in
               * full colour and every humanoid comes out a near-black
               * silhouette. So the fault is the colour, and the colour that
               * matters is the vertex diffuse the program computes. Print it
               * for the same vertex whose position was just interpreted, with
               * specular and fog beside it, for skinned draws and world
               * draws alike so the two can be compared directly. */
              { float d0[4], d1[4], fg[4]; int kc;
                float vinc[16][4], opc[4]; int kk2;
                memset(vinc, 0, sizeof vinc);
                for (kk2 = 0; kk2 < NUM_ATTRS; kk2++) {
                    if (attr_at[kk2] >= 0)
                        memcpy(vinc[kk2], &vbuf[(size_t)attr_at[kk2]], 16);
                    else memcpy(vinc[kk2], S.const_attr[kk2], 16);
                }
                nv2a_vsh_interp(&vpb, vinc, S.u.c, opc);
                for (kc = 0; kc < 4; kc++) {
                    d0[kc] = g_interp_out[NV2A_OREG_DIFFUSE][kc];
                    d1[kc] = g_interp_out[NV2A_OREG_SPECULAR][kc];
                    fg[kc] = g_interp_out[NV2A_OREG_FOG][kc];
                }
                fprintf(stderr, "  [COLOUR] %-5s slot %4d: diffuse"
                        " (%.3f %.3f %.3f a=%.3f)%s specular"
                        " (%.3f %.3f %.3f) fog %.3f%s | combiner %05X"
                        " tex0 %d\n",
                        is_mover ? "MOVER" : "world", slot,
                        d0[0], d0[1], d0[2], d0[3],
                        (g_interp_written & (1u << NV2A_OREG_DIFFUSE))
                            ? "" : " [NOT WRITTEN]",
                        d1[0], d1[1], d1[2], fg[0],
                        (g_interp_written & (1u << NV2A_OREG_FOG))
                            ? "" : " [NOT WRITTEN]",
                        S.shader_stage_prog, S.tex[0].enabled); }

              { int writes_fog = 0, q7;
                for (q7 = 0; q7 < lenb; q7++)
                    if ((vpb.insns[q7].mac_mask && vpb.insns[q7].mac_temp == NV2A_OREG_FOG)
                     || (vpb.insns[q7].out_mask && vpb.insns[q7].out_reg == NV2A_OREG_FOG))
                        { writes_fog = 1; break; }
                fprintf(stderr, "  [DIST] %-5s slot %4d verts %5u: distance"
                        " %.0f..%.0f | writes fog: %s | fog enable %d colour"
                        " %08X\n", is_mover ? "MOVER" : "world", slot,
                        out_n, wlo, whi, writes_fog ? "yes" : "NO",
                        S.fog_enable, S.fog_color); }
              fprintf(stderr, "  [MOVERBOX] %-5s slot %4d verts %5u sampled %3d"
                      " | ndc x[%6.2f %6.2f] y[%6.2f %6.2f] z[%.6f %.6f]"
                      " w[%9.2f %9.2f] | behind %3d onscreen %3d | prog %3d"
                      " | A0 x%d (", is_mover ? "MOVER" : "world", slot,
                      out_n, nb, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2],
                      wlo, whi, behind, onscr, lenb, na0);
              for (f = 0; f < na0 && f < 8; f++)
                  fprintf(stderr, "%s%d", f ? "," : "", a0seen[f]);
              fprintf(stderr, ") | blend=%d(%04X/%04X) at=%d/%04X ref=%.2f"
                      " depth=%d/%04X w=%d tex0=%d cull=%d/%04X/%04X"
                      " surf=%08X prim=%u\n",
                      S.blend_enable, S.blend_src, S.blend_dst,
                      S.alpha_test, (unsigned)S.alpha_func, S.alpha_ref,
                      S.depth_test, S.depth_func, S.depth_mask,
                      S.tex[0].enabled, S.cull_enable, S.cull_face,
                      S.front_face, S.color_offset, S.prim);
              fflush(stderr);
          }
      }

      /* [SAMPLES] -- how many fragments actually survive, counted by the
       * hardware rather than argued about.
       *
       * Everything upstream of the rasteriser now checks out: the bone
       * indices vary per vertex, the mesh lands on screen, the pixel state
       * the title asks for is identical to the state of world geometry that
       * renders. So the question is no longer "is it drawn" but "what
       * happens to its fragments", and an occlusion query answers exactly
       * that: it counts the samples that reach the depth test, after
       * culling, after the fragment shader's alpha-test discard.
       *
       * Twice, for the same batch. Once with the state the title asked for,
       * once with every reject removed. Zero and zero means nothing is being
       * rasterised at all and the loss is earlier than this. Zero and a large
       * number names the per-fragment path and, between the two probes, which
       * switch in it. A large number twice means the characters ARE being
       * painted and are either the wrong colour or painted over afterwards.
       *
       * Both probes write to neither colour nor depth, so the frame they
       * measure is the frame that would have been drawn anyway.
       */
      /* (OpenGL only: an occlusion query is a GL object, and the probe sets
       * GL state behind the device's back and puts it back afterwards.) */
#ifdef NV2A_GL_HAVE_GL
      if (g_is_gl && probe_this && !curtain_skip && !g_surf_lost && slot >= 0) {
          GLuint qo = 0;
          glGenQueries(1, &qo);
          if (qo) {
              /* The four switches are settled: the alpha test rejects
               * nothing (as-drawn and no-alphatest counts are identical on
               * every batch), depth rejects a little, culling about half, and
               * hundreds to tens of thousands of fragments DO reach the
               * framebuffer. Yet the characters are invisible, and removing
               * blending is what makes them appear. So the fragments are
               * being blended to nothing: SRC_ALPHA/ONE_MINUS_SRC_ALPHA with
               * an alpha just above zero passes an alpha test of "> 0" and
               * still contributes nothing to the picture.
               *
               * The same occlusion query measures that alpha for free. Run
               * the batch with the test set to "greater than t" for a ladder
               * of thresholds and the surviving counts ARE the histogram of
               * the combiner's output alpha -- no readback, no guessing. */
              int af = S.alpha_test ? (S.alpha_func - 0x0200 + 1) : 0;
              /* Alpha turned out to be innocent too: the histogram runs
               * above 0.9 on almost every batch. Depth is the only switch
               * whose removal changes the counts -- but "the depth test
               * rejects it" is not yet a bug, because a character genuinely
               * behind a building SHOULD be rejected.
               *
               * What separates the two is where the character sits relative
               * to what is already in the buffer, and five depth comparisons
               * measure that directly: ALWAYS is the area the mesh covers,
               * and the LESS/GREATER pair says which side of the existing
               * depth it lands on. Nearer than what is there and still
               * rejected is impossible; farther than everything, for a
               * character standing in the foreground, names the culprit as
               * whatever wrote that depth. */
              static const GLenum dfn[5] = { GL_ALWAYS, GL_LEQUAL, GL_LESS,
                                             GL_GEQUAL, GL_GREATER };
              unsigned r[5]; int pass;
              static const char *names[5] = { "covers", "<=(as drawn)",
                                              "nearer", ">=", "farther" };
              glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
              glDepthMask(GL_FALSE);
              for (pass = 0; pass < 5; pass++) {
                  /* One switch at a time, then all of them, so the counts
                   * name the culprit instead of merely proving there is one.
                   * Each pass starts from the state the title asked for. */
                  if (S.depth_test) { glEnable(GL_DEPTH_TEST);
                                      glDepthFunc(gl_depth_func((uint32_t)S.depth_func)); }
                  else glDisable(GL_DEPTH_TEST);
                  if (S.cull_enable) glEnable(GL_CULL_FACE);
                  else glDisable(GL_CULL_FACE);
                  if (uses(slot, NV2A_U_ALPHAFUNC))
                      G->uniform_i(NV2A_U_ALPHAFUNC, af);
                  /* Every pass uses "greater than t" so the counts are
                   * directly comparable; culling stays off throughout so a
                   * back face does not masquerade as a transparent one. */
                  glDisable(GL_CULL_FACE);
                  if (uses(slot, NV2A_U_ALPHAFUNC))
                      G->uniform_i(NV2A_U_ALPHAFUNC, 0);
                  glEnable(GL_DEPTH_TEST);
                  glDepthFunc(dfn[pass]);

                  r[pass] = 0;
#if defined(NV2A_GLES)
                  /* OpenGL ES only says whether any passed: 0 or 1 here. */
                  glBeginQuery(GL_ANY_SAMPLES_PASSED, qo);
                  glDrawArrays(gl_mode_of(mode), 0, (GLsizei)out_n);
                  glEndQuery(GL_ANY_SAMPLES_PASSED);
#else
                  glBeginQuery(GL_SAMPLES_PASSED, qo);
                  glDrawArrays(gl_mode_of(mode), 0, (GLsizei)out_n);
                  glEndQuery(GL_SAMPLES_PASSED);
#endif
                  glGetQueryObjectuiv(qo, GL_QUERY_RESULT, &r[pass]);
              }
              glDeleteQueries(1, &qo);

              fprintf(stderr, "  [SAMPLES] %-5s slot %4d verts %5u |", 
                      is_mover ? "MOVER" : "world", slot, out_n);
              for (pass = 0; pass < 5; pass++)
                  fprintf(stderr, " %s %u |", names[pass], r[pass]);
              fprintf(stderr, "\n");
              fflush(stderr);

              /* Put back exactly what this draw asked for. The state cache
               * believes it already set all of this, so restoring it here is
               * not optional and cannot be left to the next draw. */
              if (S.depth_test) { glEnable(GL_DEPTH_TEST);
                                  glDepthFunc(gl_depth_func((uint32_t)S.depth_func)); }
              else glDisable(GL_DEPTH_TEST);
              glDepthMask(S.depth_mask ? GL_TRUE : GL_FALSE);
              if (S.blend_enable) { glEnable(GL_BLEND);
                                    glBlendFunc(gl_blend((uint32_t)S.blend_src),
                                                gl_blend((uint32_t)S.blend_dst)); }
              else glDisable(GL_BLEND);
              if (S.cull_enable) glEnable(GL_CULL_FACE);
              else glDisable(GL_CULL_FACE);
              if (uses(slot, NV2A_U_ALPHAFUNC)) {
                  G->uniform_i(NV2A_U_ALPHAFUNC, af);
                  uni1f(NV2A_U_ALPHAREF, S.alpha_ref);
              }
              glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
              /* And the device's own idea of the state, exactly as given. */
              G->state_invalidate();
              G->state(&g_draw_rs);
          }
      }
#endif /* NV2A_GL_HAVE_GL */

      /* [TIMELINE] -- the order things happen inside one frame.
       *
       * The characters' pixels are written, opaque, nearer than what is
       * already there, and passing the depth test -- and still never reach
       * the screen. That leaves what happens AFTER the draw, and the first
       * thing to rule in or out is the obvious one. This title builds its
       * scene in surface 00084000 and composites it into the display buffer
       * 001B2000 with a full-screen quad. If the skinned batches are
       * submitted after that composite has already run, their pixels land in
       * a surface nobody reads again -- which looks exactly like this, and
       * would explain why the scenery around them is perfectly fine.
       *
       * One frame every five seconds, printed as it happens: every surface
       * the frame switches to, every skinned batch, and every small quad
       * (six vertices or fewer), which is what a composite looks like.
       * Only with RECOMP_GL_TIMELINE=1: up to 200 lines every five seconds,
       * flushed, from the GPU thread, they were nearly half of every log. */
      { static double last_tl; static uint32_t tl_frame = 0xFFFFFFFFu;
        static uint32_t tl_surf; static int tl_events;
        static uint32_t tl_seen = 0xFFFFFFFFu; static int tl_arm;
        static int tl_on = -1;
        double now_tl = g_frame_clock;
        if (tl_on < 0) { const char *v = getenv("RECOMP_GL_TIMELINE");
                         tl_on = v ? atoi(v) : 0; }
        /* Arming in the middle of a frame shows only its tail, which is
         * exactly the part that cannot answer the question. Wait for the
         * frame to turn over, then record that whole frame. */
        if (tl_on && tl_frame == 0xFFFFFFFFu && !tl_arm && now_tl - last_tl > 5.0)
            tl_arm = 1;
        if (tl_arm && S.frames != tl_seen) {
            tl_arm = 0; last_tl = now_tl; tl_frame = S.frames;
            tl_surf = 0xFFFFFFFFu; tl_events = 0;
            fprintf(stderr, "  [TIMELINE] ---- frame %u ----\n", S.frames);
        }
        tl_seen = S.frames;
        if (tl_frame != 0xFFFFFFFFu) {
            if (S.frames != tl_frame || tl_events >= 200) {
                if (tl_frame != 0xFFFFFFFFu && S.frames != tl_frame)
                    fprintf(stderr, "  [TIMELINE] frame ends at draw %u\n",
                            S.draws);
                tl_frame = 0xFFFFFFFFu;
                fflush(stderr);
            } else {
                if (S.color_offset != tl_surf) {
                    tl_surf = S.color_offset;
                    fprintf(stderr, "  [TIMELINE]   draw %6u: now drawing into"
                            " surface %08X\n", S.draws, S.color_offset);
                    tl_events++;
                }
                if (is_mover) {
                    fprintf(stderr, "  [TIMELINE]   draw %6u: SKINNED slot %d,"
                            " %u verts\n", S.draws, slot, out_n);
                    tl_events++;
                } else if (out_n <= 6) {
                    fprintf(stderr, "  [TIMELINE]   draw %6u: quad, slot %d,"
                            " %u verts%s\n", S.draws, slot, out_n,
                            curtain_skip ? " [curtain, skipped]" : "");
                    tl_events++;
                }
            }
        } }

      /* The x-ray itself: draw the skinned batches a second time with every
       * per-fragment reject removed, so the picture on screen says the same
       * thing the counter above says. Off by default now that the counter
       * exists; RECOMP_GL_XRAY=1 turns it back on. */
      { static int xray = -1;
        if (xray < 0) { const char *v = getenv("RECOMP_GL_XRAY");
                        xray = v ? atoi(v) : 0; }
        if (xray && is_mover && !curtain_skip && !g_surf_lost) {
            Nv2aRenderState xr = g_draw_rs;
            xr.blend_enable = 0;
            xr.cull_enable = 0;
            xr.depth_func = 0x0207;              /* ALWAYS */
            G->state(&xr);
            if (uses(slot, NV2A_U_ALPHAFUNC))
                G->uniform_i(NV2A_U_ALPHAFUNC, 0);
            PROF_T0(); G->draw(mode, out_n); PROF_ADD(draw);
            G->state_invalidate();
            g_last.alpha_func   = -1;
            goto xray_done;
        } }
    }
    if (!curtain_skip && !g_surf_lost) {
        PROF_T0();
        if (vc_vb && !vc_seq) G->draw_indexed(mode, out_n, vc_i32);
        else                  G->draw(mode, out_n);
        PROF_ADD(draw);
    } else if (g_surf_lost) {
        g_drop[DROP_LOST]++;
    }
    xray_done: ;
    /*
     * RECOMP_GL_DARKHUNT=<n>: which draw makes the frame dark.
     *
     * This scene renders about two and a half times darker than the reference
     * footage, and displacing the programmable geometry brightens it -- but
     * the programmable path is a hundred and twelve thousand draws, so that
     * narrows it to half the frame. Reading the picture back after every draw
     * for one frame and reporting where the average falls off a cliff narrows
     * it to one. Ruinously slow, which is why it runs for a single frame.
     */
    { static int hunt = -2; static uint32_t hunt_frame; static double prev_mean;
      static int hunt_tris = -2;
#define hunt_at g_darkhunt_at
      if (hunt == -2) { const char *v = getenv("RECOMP_GL_DARKHUNT");
                        hunt = v ? atoi(v) : -1;
                        hunt_frame = 0; }
      /* RECOMP_GL_DARKHUNT_TRIS=<n>: hunt in the first frame that has a scene
       * in it rather than in a frame number guessed in advance.
       *
       * A frame number is the wrong handle for this. Which frame the level
       * starts on depends on how long the loading took, which depends on the
       * machine and on the run, so a number read off one run points at a
       * loading screen in the next -- and a loading screen has nothing to
       * darken. Triangles per frame separates a logo from a city by three
       * orders of magnitude and does not care how long the disc took. */
      if (hunt_tris == -2) { const char *v = getenv("RECOMP_GL_DARKHUNT_TRIS");
                             hunt_tris = v ? atoi(v) : -1; }
      /* RECOMP_GL_DARKHUNT_AFTER=<presents>: don't arm before here.
       *
       * Triangles alone pick the first scene in the run, and the first scene
       * is the title screen -- a whole city, thousands of triangles, and
       * perfectly bright. The frame actually worth hunting is the one AFTER
       * the player has started a level, which no triangle count can
       * distinguish from the menu it came from. A present count can: it is
       * the one number that only ever goes up and that a dump run already
       * reports, so the frame to hunt can be read off a previous run's
       * screenshots rather than guessed. */
      { static int after = -2;
        if (after == -2) { const char *v = getenv("RECOMP_GL_DARKHUNT_AFTER");
                           after = v ? atoi(v) : 0; }
        if (after > 0 && S.flips < (uint32_t)after) hunt_tris = hunt_tris; /* not yet */
        else if (hunt_tris > 0 && hunt_at < 0 && S.frame_tris >= (uint32_t)hunt_tris
                 && (after <= 0 || S.flips >= (uint32_t)after)) {
            hunt_at = (int)S.frames + 1;
            fprintf(stderr, "  [DARK] present %u frame %u had %u triangles;"
                            " hunting frame %d\n",
                    S.flips, S.frames, S.frame_tris, hunt_at);
        }
      }
      if (0 && hunt_tris > 0 && hunt_at < 0 && S.frame_tris >= (uint32_t)hunt_tris) {
          hunt_at = (int)S.frames + 1;   /* the next whole frame, from its start */
          fprintf(stderr, "  [DARK] frame %u had %u triangles; hunting frame %d\n",
                  S.frames, S.frame_tris, hunt_at);
      }
      if ((hunt >= 0 && (int)S.frames == hunt)
       || (hunt_at >= 0 && (int)S.frames == hunt_at)) {
          static uint8_t small[64 * 48 * 4];
          double m = 0.0; int q;
          /* One row every ph/48, one column every pw/64: a thumbnail without
           * a downscale pass. A readback cannot stride, so read a strip. */
          G->target_read(g_cur_rt, 0, (g_fbo_ph ? g_fbo_ph : g_fbo_h) / 2,
                         64, 48, 0, small);
          for (q = 0; q < 64 * 48; q++)
              m += small[q*4] + small[q*4+1] + small[q*4+2];
          m /= (64.0 * 48.0 * 3.0);
          if (hunt_frame == 0) prev_mean = m;
          hunt_frame++;
          /* Skip the start of the frame: a title paints its background with
           * a full-screen quad, and that legitimately takes the average from
           * the previous frame's picture down to nothing. What is wanted is a
           * draw that darkens a frame already being built. */
          /* The floor used to be 8.0, which hid the end of the story: the
           * frame this was written for falls 80 -> 5.3 on one draw and then
           * 5.3 -> 0 on another, and only the first was ever reported. A
           * picture at 5 is still not black; whatever finishes the job is as
           * interesting as whatever started it. */
          if (hunt_frame > 30 && prev_mean > 0.4 && m < prev_mean * 0.80) {
              fprintf(stderr, "  [DARK] draw %u of this frame: mean %.1f -> "
                      "%.1f  xf=%u  verts=%u  blend=%d(%04X/%04X) "
                      "depth=%d/%04X alpha=%d prog=%d\n",
                      hunt_frame, prev_mean, m, S.xf_mode, out_n,
                      S.blend_enable, S.blend_src, S.blend_dst,
                      S.depth_test, S.depth_func, S.alpha_test, slot);
              /* What the culprit is actually made of.
               *
               * Knowing WHICH draw darkens the frame is only half an answer.
               * A full-screen alpha-blended quad that covers the scene is
               * doing its job if its alpha is right and destroying the frame
               * if its alpha is wrong, and the two are indistinguishable from
               * the outside. So print where every attribute of this draw came
               * from -- per-vertex array, or the constant the title left in
               * SET_VERTEX_DATA4F -- and its value. Attribute 3 is diffuse;
               * its w is the alpha the fade is asking for. */
              { int a; uint32_t q;
                for (a = 0; a < NUM_ATTRS; a++) {
                    if (attr_at[a] >= 0) {
                        const float *v0 = &vbuf[(size_t)attr_at[a]];
                        fprintf(stderr, "  [DARK]   attr %-2d per-vertex:", a);
                        for (q = 0; q < out_n && q < 4; q++) {
                            const float *vv = &vbuf[(size_t)q * stride_floats
                                                    + (size_t)attr_at[a]];
                            fprintf(stderr, " (%.3f %.3f %.3f %.3f)",
                                    vv[0], vv[1], vv[2], vv[3]);
                        }
                        (void)v0;
                        fprintf(stderr, "\n");
                    } else if (S.const_attr[a][0] != 0.0f
                            || S.const_attr[a][1] != 0.0f
                            || S.const_attr[a][2] != 0.0f
                            || S.const_attr[a][3] != 0.0f) {
                        fprintf(stderr, "  [DARK]   attr %-2d constant : "
                                "(%.3f %.3f %.3f %.3f)\n", a,
                                S.const_attr[a][0], S.const_attr[a][1],
                                S.const_attr[a][2], S.const_attr[a][3]);
                    }
                }
                /* And what it is sampling. A full-screen quad whose diffuse
                 * is black gets its colour entirely from its texture, so a
                 * texture that resolved to nothing paints the screen black --
                 * which is a completely different bug from an alpha that is
                 * too high, and the two look identical from outside. */
                { int st;
                  for (st = 0; st < NUM_STAGES; st++) {
                      uint32_t surf;
                      if (!S.tex[st].enabled) {
                          fprintf(stderr, "  [DARK]   tex %d: disabled\n", st);
                          continue;
                      }
                      surf = surface_texture(S.tex[st].offset, 0, 0);
                      fprintf(stderr, "  [DARK]   tex %d: offset 0x%08X %ux%u "
                              "fmt 0x%08X -> %s\n", st,
                              S.tex[st].offset, S.tex[st].width,
                              S.tex[st].height, S.tex[st].format,
                              surf ? "a render-target surface"
                                   : "uploaded from guest memory");
                  } }
                fprintf(stderr, "  [DARK]   positions:");
                for (q = 0; q < out_n && q < 4; q++) {
                    const float *vv = &vbuf[(size_t)q * stride_floats];
                    fprintf(stderr, " (%.1f %.1f %.1f %.1f)",
                            vv[0], vv[1], vv[2], vv[3]);
                }
                fprintf(stderr, "\n");
              }
          }
          prev_mean = m;
          if (hunt_frame % 50 == 0)
              fprintf(stderr, "  [DARK]   after %u draws: mean %.1f\n",
                      hunt_frame, m);
#undef hunt_at
      } }
    /*
     * RECOMP_GL_SKINDBG=1: where a skinned draw's constant reads land.
     *
     * The characters come apart into spikes, and the programs that draw them
     * pick a bone matrix out of the constant file through the address
     * register. Two earlier versions of this check guessed at which vertex
     * attribute held the index and which constant scaled it, and both guessed
     * wrong -- the first read the position as an index, the second used one
     * program's scale for every program's indices. Neither is knowable from
     * the outside: different programs put the index in different registers
     * and scale it by different constants.
     *
     * So run the program. The CPU interpreter already exists for checking the
     * translation against the shader, and it can report every address its ARL
     * instructions set, which is exactly the number in question. A draw is
     * reported only when one of those addresses reaches outside the file, or
     * lands on four constants whose last row is not (0,0,0,1) -- which every
     * affine transform's is.
     */
    /*
     * RECOMP_GL_OBJMAT=1: the matrix each program actually uses to place its
     * object, printed once per program.
     *
     * The player's description split the bug cleanly in two: buildings and roads
     * are in the right place, and people, buses and everything else are not.
     * Static world geometry carries its position in its vertices; everything
     * that floats is placed by a per-object transform that lives in the
     * constant file and is uploaded before the draw. So the question is not
     * how the vertices are decoded or how the program is translated -- both
     * of those would break the buildings too -- it is whether the matrix the
     * program reads is the matrix the title uploaded, at the index the title
     * uploaded it to.
     *
     * Which constants those are is not a guess: a program builds oPos with
     * four DP4s against four consecutive constants, and the instruction
     * stream says which. The last row of any affine transform is (0,0,0,1)
     * and the translation is the fourth column, so a matrix that is stale,
     * missing, or read one slot off announces itself immediately -- and a
     * translation of exactly zero on something that is visibly not at the
     * world origin is the whole bug in one line.
     */
    { static int on = -1; static uint32_t seen[64]; static int nseen;
      if (on < 0) on = getenv("RECOMP_GL_OBJMAT") ? 1 : 0;
      if (on && slot >= 0 && nseen < 64 && out_n >= 3
             && S.xf_mode != NV2A_XF_MODE_FIXED) {
          int i, fresh = 1;
          for (i = 0; i < nseen; i++) if (seen[i] == (uint32_t)slot) { fresh = 0; break; }
          if (fresh) {
              Nv2aVshProgram vp;
              uint32_t pstart = S.prog_start < NV2A_VSH_MAX_INSNS ? S.prog_start : 0;
              int len = nv2a_vsh_decode(S.prog + pstart * 4,
                                        (int)(NV2A_VSH_MAX_INSNS - pstart), &vp);
              int q, pos_c[4], npos = 0, rel = 0;
              for (q = 0; q < len && npos < 4; q++) {
                  const Nv2aVshInsn *in = &vp.insns[q];
                  if (in->mac != NV2A_MAC_DP4) continue;
                  if (!in->out_is_oreg || in->out_reg != NV2A_OREG_POS) continue;
                  if (in->src[0].type != NV2A_PARAM_C
                   && in->src[1].type != NV2A_PARAM_C) continue;
                  pos_c[npos++] = in->const_index;
                  if (in->rel_addr) rel = 1;
              }
              seen[nseen++] = (uint32_t)slot;
              if (npos == 4) {
                  int base = pos_c[0], k, ok = 1;
                  for (k = 1; k < 4; k++) if (pos_c[k] != base + k) ok = 0;
                  fprintf(stderr,
                      "  [OBJMAT] program slot %d writes oPos from c[%d..%d]%s"
                      "%s\n", slot, pos_c[0], pos_c[3],
                      rel ? " (indexed by A0)" : "",
                      ok ? "" : "  <-- not four consecutive constants");
                  if (ok && base >= 0 && base + 3 < NV2A_VSH_NUM_CONSTS) {
                      const float *r3 = S.u.c[base + 3];
                      for (k = 0; k < 4; k++)
                          fprintf(stderr, "           c[%3d] = %10.3f %10.3f "
                                  "%10.3f %10.3f\n", base + k,
                                  S.u.c[base + k][0], S.u.c[base + k][1],
                                  S.u.c[base + k][2], S.u.c[base + k][3]);
                      /* The row that gives the game away. Every affine
                       * transform ends (0,0,0,1); a projection does not, and
                       * this title's programs multiply by a combined
                       * world-view-projection, so say what was found rather
                       * than pronouncing it wrong. */
                      fprintf(stderr, "           last row %s\n",
                          (fabsf(r3[0]) < 1e-4f && fabsf(r3[1]) < 1e-4f
                           && fabsf(r3[2]) < 1e-4f && fabsf(r3[3] - 1.0f) < 1e-3f)
                              ? "is (0,0,0,1): an affine transform"
                              : "is not (0,0,0,1): a projection, or not a matrix");
                  }
              } else {
                  fprintf(stderr, "  [OBJMAT] program slot %d: oPos not built "
                          "from four constant DP4s (%d found)\n", slot, npos);
              }
              fflush(stderr);
          }
      } }
    /* A rate, not four examples.
     *
     * The cap of four was right while the question was "does this ever
     * happen". It is wrong now: with the address resolver fixed, the
     * question is whether it still happens and how often, and four samples
     * taken from the first few seconds cannot answer either. Every draw that
     * uses the address register is counted, and the ones that go wrong are
     * counted separately, so the summary is a proportion. The detail is
     * still capped, because twelve of these is already more than anyone
     * reads. */
    /*
     * RECOMP_GL_POSRANGE=1: what space the program actually leaves oPos in.
     *
     * Two attempts at the depth fix have now been guesses about that, and
     * both produced a black screen -- which is the cost of reasoning about
     * a coordinate space from a projection matrix instead of measuring the
     * numbers the program emits. The interpreter that found the bone
     * indices can answer this directly: run the title's own program on a
     * few vertices and print the range of each component of oPos.
     *
     * The four cases are distinguishable at a glance and want different
     * fixes. x and y spanning 0..640 and 0..480 with w near 1 means screen
     * space, and z should span the declared clip range. x and y in -1..1
     * means normalised device coordinates. x and y large with w large means
     * homogeneous clip space, and everything needs dividing by w. And z
     * spanning 0..1 while x and y span the screen means exactly the mismatch
     * this is chasing: a title that applied the viewport to x and y and left
     * z alone.
     */
    /*
     * RECOMP_GL_DISTCENSUS=1: triangles by distance from the camera, world
     * against entity.
     *
     * The clearest frame of the day: traffic in a line continuing the
     * highway's curve, at its exact height, receding into the distance --
     * and the deck simply stops while the vehicles carry on along where it
     * should be. Near road is drawn. Far road is not. Entities at every
     * distance are. So the question is no longer where anything is; it is
     * whether the distant world geometry reaches this renderer at all.
     *
     * The interpreter gives each draw's w, which is its distance. Bucket the
     * triangles by it, split by class. If world triangles fall to nothing
     * beyond some distance while entity triangles continue, the far world is
     * never submitted and the cause is upstream -- the game's own visibility
     * logic on the recompiled CPU, or a clip this renderer applies. If world
     * triangles ARE there at far distance, they are submitted and something
     * downstream makes them invisible: fog, a decode that collapses them, a
     * texture. Opposite ends of the port, and the buckets say which.
     *
     * Every fourth draw, vertex zero only: enough to bucket by, cheap enough
     * to leave on for a whole run.
     */
    /*
     * RECOMP_GL_WORLDY: the world height of every dynamic object, from its
     * own transform.
     *
     * The whole GPU raster pipeline is eliminated -- nothing is clipped,
     * culled, depth-tested or w-buffered away -- so a floating vehicle is
     * placed where its transform puts it, and the transform is computed by
     * the recompiled CPU and uploaded as constants. A dynamic object reads a
     * bone/object matrix at c[A0+base]; when that matrix is a genuine affine
     * world transform (last row 0,0,0,1) its fourth column is the object's
     * world position, and column Y is its height. On the ground that is
     * small. If the floaters cluster at a large Y, the CPU is computing their
     * height wrong -- which is the bug, located, on the correct side of the
     * emulator at last.
     *
     * Histogram the height across all such draws, sampled, so the shape of
     * the distribution shows whether objects sit at a sensible spread of
     * ground and rooftop heights or pile up at an impossible one.
     */
    /* RECOMP_GL_MOVERS=<n>: where the things that move actually end up.
     *
     * Two runs over the same frames settled that everything visible is drawn
     * WITHOUT the constant palette, and everything drawn WITH it -- which is
     * how this title places anything that moves -- renders nothing at all.
     * That is the missing characters. It leaves exactly one fork: either the
     * transform sends them somewhere off screen, or the transform is fine and
     * the pixels are lost afterwards.
     *
     * The CPU interpreter answers it directly. Run the program over real
     * vertices of a real mover draw, divide by w the way the hardware does,
     * and print where it lands next to the state that decides whether a pixel
     * survives. On-screen coordinates with nothing visible means the loss is
     * in the pixel path; coordinates off in the weeds means it is the matrix.
     */
    { static int mv = -1; static int shown; static uint32_t last_frame = ~0u;
      if (mv < 0) { const char *v = getenv("RECOMP_GL_MOVERS");
                    mv = v ? atoi(v) : 0; }
      if (S.frames != last_frame) { last_frame = S.frames; }
      if (mv > 0 && shown < mv && slot >= 0 && g_pcache[slot].uses_a0
          && out_n >= 3 && S.xf_mode != NV2A_XF_MODE_FIXED) {
          Nv2aVshProgram vp;
          uint32_t pstart = S.prog_start < NV2A_VSH_MAX_INSNS ? S.prog_start : 0;
          int len = nv2a_vsh_decode(S.prog + pstart * 4,
                                    (int)(NV2A_VSH_MAX_INSNS - pstart), &vp);
          float vin[16][4], op[4];
          int k; uint32_t q;
          int on = 0, behind = 0;
          float sx0 = 0, sy0 = 0, sw0 = 0;
          for (q = 0; q < out_n && q < 32; q++) {
              memset(vin, 0, sizeof vin);
              for (k = 0; k < NUM_ATTRS; k++) {
                  if (attr_at[k] >= 0)
                      memcpy(vin[k], &vbuf[(size_t)q * stride_floats + attr_at[k]], 16);
                  else memcpy(vin[k], S.const_attr[k], 16);
              }
              if (!nv2a_vsh_interp(&vp, vin, S.u.c, op)) continue;
              if (op[3] <= 0.0f) { behind++; continue; }
              { float sx = op[0] / op[3], sy = op[1] / op[3];
                if (!q) { sx0 = sx; sy0 = sy; sw0 = op[3]; }
                if (sx >= -1.5f && sx <= 641.5f && sy >= -1.5f && sy <= 481.5f) on++; }
          }
          shown++;
          /* Those coordinates are NORMALISED (x/w), not pixels -- a first
           * reading of them as pixels says every mover collapses onto the top
           * left corner, which is wrong and sends you after the matrix. They
           * sit inside [-1,1], so the transform is fine and the geometry is on
           * screen. What is left is the pixel path, so print the things that
           * decide whether a pixel survives it: the colour the vertices carry
           * (attribute 3 is diffuse; its w is alpha) and the alpha test the
           * title asked for. An alpha of zero under SRC_ALPHA blending is a
           * perfectly drawn, perfectly invisible character. */
          /* And the matrix it is actually reading.
           *
           * Everything measured so far verifies the machinery -- the program
           * translates correctly, A0 is set, the reads are well formed, the
           * vertices land on screen. None of it has compared the CONSTANTS a
           * mover reads in the tutorial against a matrix known to be good.
           * The one dump that showed sane object matrices was taken on the
           * attract screen, where characters do render, so it proves nothing
           * about the scene where they do not. A0, the base it indexes from,
           * and the four rows it lands on are the missing evidence: a row of
           * zeroes, or a last row that is not (0,0,0,1), is the whole bug. */
          { int rel_base = -1, q2, na2, a0s2[8];
            float vin2[16][4];
            for (q2 = 0; q2 < len; q2++)
                if (vp.insns[q2].rel_addr) { rel_base = vp.insns[q2].const_index; break; }
            if (rel_base >= 0) {
                memset(vin2, 0, sizeof vin2);
                for (k = 0; k < NUM_ATTRS; k++) {
                    if (attr_at[k] >= 0)
                        memcpy(vin2[k], &vbuf[(size_t)attr_at[k]], 16);
                    else memcpy(vin2[k], S.const_attr[k], 16);
                }
                na2 = nv2a_vsh_interp_a0(&vp, vin2, S.u.c, a0s2, 8);
                if (na2 > 0) {
                    int idx = rel_base + a0s2[0];
                    fprintf(stderr, "  [MOVER]   A0=%d base=%d -> c[%d..%d]\n",
                            a0s2[0], rel_base, idx, idx + 3);
                    if (idx >= 0 && idx + 3 < NV2A_VSH_NUM_CONSTS) {
                        int r;
                        for (r = 0; r < 4; r++)
                            fprintf(stderr, "  [MOVER]     c[%3d] = %9.3f %9.3f "
                                    "%9.3f %9.3f\n", idx + r,
                                    S.u.c[idx+r][0], S.u.c[idx+r][1],
                                    S.u.c[idx+r][2], S.u.c[idx+r][3]);
                    } else {
                        fprintf(stderr, "  [MOVER]     index out of the "
                                        "constant file\n");
                    }
                } else {
                    fprintf(stderr, "  [MOVER]   no ARL executed for this "
                                    "draw (A0 stays 0, base=%d)\n", rel_base);
                }
            } else {
                fprintf(stderr, "  [MOVER]   no relative read found\n");
            } }
          { float dif[4] = {0,0,0,0};
            if (attr_at[3] >= 0) memcpy(dif, &vbuf[(size_t)attr_at[3]], 16);
            else memcpy(dif, S.const_attr[3], 16);
            fprintf(stderr, "  [MOVER] draw %u slot %d verts %u: v0 ndc "
                  "(%.2f %.2f) w %.1f | diffuse (%.3f %.3f %.3f a=%.3f)%s | "
                  "blend=%d(%04X/%04X) alphatest=%d func=%04X ref=%.3f "
                  "depth=%d/%04X write=%d tex0=%d(fmt 0x%08X)\n",
                  S.draws, slot, out_n, sx0, sy0, sw0,
                  dif[0], dif[1], dif[2], dif[3],
                  attr_at[3] >= 0 ? "" : " [constant]",
                  S.blend_enable, S.blend_src, S.blend_dst,
                  S.alpha_test, (unsigned)S.alpha_func, S.alpha_ref,
                  S.depth_test, S.depth_func, S.depth_mask,
                  S.tex[0].enabled, S.tex[0].format);
            (void)on; (void)behind; }
          fflush(stderr);
      } }

    { static int wy = -1; static uint64_t hist[12]; static uint32_t wlast;
      static const float edges[11] = { -100,-20,-5,0,5,20,50,100,200,500,1000 };
      if (wy < 0) wy = getenv("RECOMP_GL_WORLDY") ? 1 : 0;
      if (wy && slot >= 0 && g_pcache[slot].uses_a0 && out_n >= 3
             && S.xf_mode != NV2A_XF_MODE_FIXED && (S.draws & 1u) == 0) {
          Nv2aVshProgram vp;
          uint32_t pstart = S.prog_start < NV2A_VSH_MAX_INSNS ? S.prog_start : 0;
          int len = nv2a_vsh_decode(S.prog + pstart * 4,
                                    (int)(NV2A_VSH_MAX_INSNS - pstart), &vp);
          int q, rel_base = -1;
          for (q = 0; q < len; q++)
              if (vp.insns[q].rel_addr) { rel_base = vp.insns[q].const_index; break; }
          if (rel_base >= 0) {
              float vin[16][4]; int a0s[8], na, k;
              memset(vin, 0, sizeof vin);
              for (k = 0; k < NUM_ATTRS; k++) {
                  if (attr_at[k] >= 0)
                      memcpy(vin[k], &vbuf[(size_t)(out_n/2) * stride_floats
                                           + attr_at[k]], 16);
                  else memcpy(vin[k], S.const_attr[k], 16);
              }
              na = nv2a_vsh_interp_a0(&vp, vin, S.u.c, a0s, 8);
              if (na > 0) {
                  int idx = rel_base + a0s[0];
                  if (idx >= 0 && idx + 3 < NV2A_VSH_NUM_CONSTS) {
                      const float *r3 = S.u.c[idx + 3];
                      /* only genuine affine world matrices */
                      if (fabsf(r3[0]) < 1e-4f && fabsf(r3[1]) < 1e-4f
                       && fabsf(r3[2]) < 1e-4f && fabsf(r3[3]-1.0f) < 1e-3f) {
                          float wyv = S.u.c[idx + 1][3];   /* col 3 of row Y */
                          int b; for (b = 0; b < 11 && wyv >= edges[b]; b++) ;
                          hist[b]++;
                      }
                  }
              }
          }
      }
      if (wy && S.draws - wlast >= 200000u) {
          int b; wlast = S.draws;
          fprintf(stderr, "  [WORLDY] object world height, all dynamic objects:\n");
          fprintf(stderr, "  [WORLDY]   <-100 <-20 <-5 <0 <5 <20 <50 <100 <200 "
                          "<500 <1000 1000+\n  [WORLDY]  ");
          for (b = 0; b < 12; b++) fprintf(stderr, " %8llu",
                                           (unsigned long long)hist[b]);
          fprintf(stderr, "\n"); fflush(stderr);
      } }

    /*
     * RECOMP_GL_DUMP_WORLD=1: the scene in world space, once per frame dump.
     *
     * Every picture so far is the camera's. This is the map. For each draw
     * of the frame after a framebuffer dump, take a sample of its vertices,
     * put each through the object matrix the program would use -- the four
     * rows at c[A0+base] for a palette draw, c[107..110] for a fixed one --
     * and write the world position, alongside the screen position the whole
     * program produces. Plotted from above, the road network is a road
     * network and a bus is a dot that is either on it or not; plotted from
     * the side, a vehicle is either at the height of the road under it or
     * hanging above nothing. No reference needed for that comparison: the
     * road and the bus are in the same file in the same coordinates.
     *
     * The frame's camera goes in too (c[103..106]) so the view can be
     * reconstructed, and each draw carries its slot, texture and matrix
     * kind so the dots can be coloured by what drew them.
     */
    { static int wd = -1; static FILE *fp; static uint32_t start_flip; static int shot;
      extern int g_world_dump_pending;
      if (wd < 0) wd = getenv("RECOMP_GL_DUMP_WORLD") ? 1 : 0;
      if (wd && g_world_dump_pending && !fp) {
          char name[256]; const char *pfx = getenv("RECOMP_GL_DUMP_FB");
          snprintf(name, sizeof name, "%s_world_%03d.txt", pfx ? pfx : "dump", shot++);
          fp = fopen(name, "w"); start_flip = S.flips;
          if (fp) { int r;
              fprintf(fp, "# frame at flip %u draws %u\n", S.flips, S.draws);
              for (r = 103; r <= 106; r++)
                  fprintf(fp, "# c[%d] %.6g %.6g %.6g %.6g\n", r, S.u.c[r][0], S.u.c[r][1], S.u.c[r][2], S.u.c[r][3]);
              fprintf(fp, "# c[58] %.6g %.6g %.6g %.6g\n# c[59] %.6g %.6g %.6g %.6g\n",
                      S.u.c[58][0], S.u.c[58][1], S.u.c[58][2], S.u.c[58][3],
                      S.u.c[59][0], S.u.c[59][1], S.u.c[59][2], S.u.c[59][3]);
              fprintf(fp, "# columns: draw slot a0 matrix_base tex0 prim nverts | wx wy wz | sx sy sz sw | vx vy vz\n");
          }
      }
      if (fp && S.flips - start_flip >= 3) { fclose(fp); fp = NULL; g_world_dump_pending = 0; }
      /* The fixed-function half of the scene: the composite matrix and the
       * homogeneous result for the same vertex sample, so the two pipelines'
       * screen positions can be laid side by side. */
      if (fp && slot >= 0 && out_n >= 3 && S.xf_mode == NV2A_XF_MODE_FIXED) {
          uint32_t step = out_n / 48, vtx; int r; if (!step) step = 1;
          fprintf(fp, "f %u %d %08X %u %u vpoff %.4g %.4g %.4g vpscl %.4g %.4g %.4g\n", S.draws, slot,
                  S.tex[0].offset, S.prim, out_n, S.u.vp_off[0], S.u.vp_off[1], S.u.vp_off[2],
                  S.u.vp_scale[0], S.u.vp_scale[1], S.u.vp_scale[2]);
          for (r = 0; r < 4; r++)
              fprintf(fp, "m %.6g %.6g %.6g %.6g\n", S.u.ff_mat[r][0], S.u.ff_mat[r][1],
                      S.u.ff_mat[r][2], S.u.ff_mat[r][3]);
          for (vtx = 0; vtx < out_n; vtx += step) {
              const float *v = &vbuf[(size_t)vtx * stride_floats + (attr_at[0] >= 0 ? attr_at[0] : 0)];
              float p[4];
              for (r = 0; r < 4; r++)
                  p[r] = v[0]*S.u.ff_mat[r][0] + v[1]*S.u.ff_mat[r][1]
                       + v[2]*S.u.ff_mat[r][2] + v[3]*S.u.ff_mat[r][3];
              fprintf(fp, "w %.5g %.5g %.5g %.5g %.5g %.5g %.5g\n", p[0], p[1], p[2], p[3], v[0], v[1], v[2]);
          }
      }
      if (fp && slot >= 0 && out_n >= 3 && S.xf_mode != NV2A_XF_MODE_FIXED) {
          Nv2aVshProgram vp;
          uint32_t pstart = S.prog_start < NV2A_VSH_MAX_INSNS ? S.prog_start : 0;
          int len = nv2a_vsh_decode(S.prog + pstart * 4,
                                    (int)(NV2A_VSH_MAX_INSNS - pstart), &vp);
          int q, rel_base = -1, fixed_base = -1;
          for (q = 0; q < len; q++)
              if (vp.insns[q].rel_addr) { rel_base = vp.insns[q].const_index; break; }
          if (rel_base < 0 && g_pcache[slot].dis && strstr(g_pcache[slot].dis, "c[107]"))
              fixed_base = 107;
          if (rel_base >= 0 || fixed_base >= 0) {
              uint32_t step = out_n / 48; if (!step) step = 1;
              uint32_t vtx;
              fprintf(fp, "d %u %d %d %d %08X %u %u vpoff %.4g %.4g %.4g c59 %.4g %.4g %.4g c58 %.4g %.4g %.4g\n",
                      S.draws, slot, rel_base >= 0,
                      rel_base >= 0 ? rel_base : fixed_base, S.tex[0].offset, S.prim, out_n,
                      S.u.vp_off[0], S.u.vp_off[1], S.u.vp_off[2],
                      S.u.c[59][0], S.u.c[59][1], S.u.c[59][2],
                      S.u.c[58][0], S.u.c[58][1], S.u.c[58][2]);
              for (vtx = 0; vtx < out_n; vtx += step) {
                  float vin[16][4], op[4]; int k, base = fixed_base;
                  memset(vin, 0, sizeof vin);
                  for (k = 0; k < NUM_ATTRS; k++) {
                      if (attr_at[k] >= 0)
                          memcpy(vin[k], &vbuf[(size_t)vtx * stride_floats + attr_at[k]], 16);
                      else memcpy(vin[k], S.const_attr[k], 16);
                  }
                  if (rel_base >= 0) {
                      int a0s[8], na = nv2a_vsh_interp_a0(&vp, vin, S.u.c, a0s, 8);
                      if (na < 1) continue;
                      base = rel_base + a0s[0];
                  }
                  if (base < 0 || base + 3 >= NV2A_VSH_NUM_CONSTS) continue;
                  if (!nv2a_vsh_interp(&vp, vin, S.u.c, op)) continue;
                  { const float *v = vin[0]; float w[3]; int r;
                    for (r = 0; r < 3; r++)
                        w[r] = S.u.c[base+r][0]*v[0] + S.u.c[base+r][1]*v[1]
                             + S.u.c[base+r][2]*v[2] + S.u.c[base+r][3]*v[3];
                    fprintf(fp, "v %d %.4g %.4g %.4g %.4g %.4g %.4g %.4g %.4g %.4g %.4g\n",
                            base, w[0], w[1], w[2], op[0], op[1], op[2], op[3], v[0], v[1], v[2]);
                  }
              }
          }
      } }

    /*
     * RECOMP_GL_HUGE=1: small program draws that land far outside the screen.
     *
     * The title screen flashes: whole frames where the 2D elements come out
     * as screen-wide black bars and white slabs -- a logo or a line of text
     * magnified until only its strokes are visible. A draw of a few vertices
     * whose screen box is several times the surface is that draw, so print
     * everything about it: which program, which texture, the viewport it
     * was given, the raw vertex, the w it produced. The first few say
     * whether it is a wrong constant, a wrong viewport, or the title
     * genuinely drawing off-screen and relying on a clip we do not apply.
     */
    /* RECOMP_GL_CHARLOG=<dir>: the characters' draws -- programmable ones
     * that pick their matrices with the address register -- one entry per
     * distinct program, combiner and set of textures (the first 40): every
     * texture stage's words, the combiners, what the program writes for the
     * first three vertices (run on the CPU), the program and the fragment
     * shader once each, and each texture written into the directory as it
     * sits in memory, level 0. Which texture a skin samples, and where, is
     * what this answers. */
    { static const char *dir = (const char *)1; static uint32_t seen[64]; static int nseen;
      static uint32_t seen_prog[64], seen_psh[64]; static int nprog, npsh;
      if (dir == (const char *)1) dir = getenv("RECOMP_GL_CHARLOG");
      if (dir && nseen < 40 && slot >= 0 && S.xf_mode != NV2A_XF_MODE_FIXED
          && g_pcache[slot].uses_a0 && out_n >= 30 && !vc_vb) {
          uint32_t ph = psh_hash(), key, i;
          int q, found = 0, k;
          key = S.prog_start * 2654435761u ^ ph;
          for (i = 0; i < 4; i++) key = key * 31u + (S.psh.tex_bound[i] ? S.tex[i].offset : 0u);
          for (q = 0; q < nseen; q++) if (seen[q] == key) { found = 1; break; }
          if (!found) {
              Nv2aVshProgram vp;
              uint32_t pstart = S.prog_start < NV2A_VSH_MAX_INSNS ? S.prog_start : 0;
              int have_vp = nv2a_vsh_decode(S.prog + pstart * 4, (int)(NV2A_VSH_MAX_INSNS - pstart), &vp) > 0;
              seen[nseen++] = key;
              fprintf(stderr, "[CHAR] #%d draw %u flip %u: prog start %u psh %08X n %u prim %u | blend %d %04X/%04X | alpha test %d func %X ref %.3f"
                              " | depth %d func %X mask %d | cull %d face %X | stages 0x%05X\n",
                      nseen - 1, S.draws, S.flips, S.prog_start, ph, out_n, S.prim, S.blend_enable, S.blend_src, S.blend_dst,
                      S.alpha_test, S.alpha_func, S.alpha_ref, S.depth_test, S.depth_func, S.depth_mask,
                      S.cull_enable, S.cull_face, S.shader_stage_prog);
              for (i = 0; i < 4; i++) {
                  fprintf(stderr, "[CHAR]   tex%u off %08X fmt %08X (colour %02X) %ux%u levels %u addr %08X ctl0 %08X filt %08X en %d bound %d mode %u\n",
                          i, S.tex[i].offset, S.tex[i].format, S.tex[i].color, S.tex[i].width, S.tex[i].height, S.tex[i].levels,
                          S.tex[i].addr, S.tex[i].control0, S.tex[i].filter, S.tex[i].enabled, S.psh.tex_bound[i],
                          (unsigned)S.psh.tex_mode[i]);
                  /* the texture itself, level 0, as the title stored it */
                  if (S.psh.tex_bound[i] && S.tex[i].width && S.tex[i].height
                      && S.tex[i].width <= 1024 && S.tex[i].height <= 1024) {
                      uint32_t w = S.tex[i].width, h = S.tex[i].height;
                      uint8_t *rgba = (uint8_t *)malloc((size_t)w * h * 4);
                      if (rgba && decode_level(S.tex[i].color, guest(resolve(S.tex[i].offset)), w, h,
                                               S.tex[i].pitch, rgba)) {
                          char nm[512]; FILE *f;
                          snprintf(nm, sizeof nm, "%s/char%02d_t%u_%08X_f%02X_%ux%u.ppm", dir, nseen - 1, i,
                                   S.tex[i].offset, S.tex[i].color, w, h);
                          f = fopen(nm, "wb");
                          if (f) { uint32_t p; fprintf(f, "P6\n%u %u\n255\n", w, h);
                                   for (p = 0; p < w * h; p++) fwrite(rgba + p * 4, 1, 3, f); fclose(f); }
                          snprintf(nm, sizeof nm, "%s/char%02d_t%u_%08X_alpha.pgm", dir, nseen - 1, i, S.tex[i].offset);
                          f = fopen(nm, "wb");
                          if (f) { uint32_t p; fprintf(f, "P5\n%u %u\n255\n", w, h);
                                   for (p = 0; p < w * h; p++) fputc(rgba[p * 4 + 3], f); fclose(f); }
                      }
                      free(rgba);
                  }
              }
              fprintf(stderr, "[CHAR]   combiners control %08X final %08X %08X c0 %08X c1 %08X\n", S.psh.control,
                      S.psh.final_abcd, S.psh.final_efg, S.psh.final_c0, S.psh.final_c1);
              for (k = 0; k < (int)(S.psh.control & 0xF) && k < NV2A_PSH_STAGES; k++)
                  fprintf(stderr, "[CHAR]   stage %d rgb icw %08X ocw %08X | alpha icw %08X ocw %08X | c0 %08X c1 %08X\n",
                          k, S.psh.rgb_icw[k], S.psh.rgb_ocw[k], S.psh.alpha_icw[k], S.psh.alpha_ocw[k], S.psh.c0[k], S.psh.c1[k]);
              if (have_vp) {
                  uint32_t v;
                  for (v = 0; v < out_n && v < 3; v++) {
                      float vin[16][4], op[4]; int a;
                      memset(vin, 0, sizeof vin);
                      for (a = 0; a < NUM_ATTRS; a++) {
                          if (attr_at[a] >= 0) memcpy(vin[a], &vbuf[(size_t)v * stride_floats + attr_at[a]], 16);
                          else memcpy(vin[a], S.const_attr[a], 16);
                      }
                      if (nv2a_vsh_interp(&vp, vin, S.u.c, op))
                          fprintf(stderr, "[CHAR]   vertex %u: oD0 %.3f %.3f %.3f %.3f oD1 %.3f %.3f %.3f %.3f | oT0 %.4f %.4f %.4f %.4f"
                                          " | oT1 %.4f %.4f %.4f %.4f | oT2 %.4f %.4f %.4f %.4f | oT3 %.4f %.4f %.4f %.4f\n", v,
                                  g_interp_out[3][0], g_interp_out[3][1], g_interp_out[3][2], g_interp_out[3][3],
                                  g_interp_out[4][0], g_interp_out[4][1], g_interp_out[4][2], g_interp_out[4][3],
                                  g_interp_out[9][0], g_interp_out[9][1], g_interp_out[9][2], g_interp_out[9][3],
                                  g_interp_out[10][0], g_interp_out[10][1], g_interp_out[10][2], g_interp_out[10][3],
                                  g_interp_out[11][0], g_interp_out[11][1], g_interp_out[11][2], g_interp_out[11][3],
                                  g_interp_out[12][0], g_interp_out[12][1], g_interp_out[12][2], g_interp_out[12][3]);
                  }
              }
              for (q = 0; q < nprog; q++) if (seen_prog[q] == S.prog_start) break;
              if (q == nprog && nprog < 64 && have_vp) {
                  static char dis[16384];
                  seen_prog[nprog++] = S.prog_start;
                  if (nv2a_vsh_disasm(&vp, dis, sizeof dis)) fprintf(stderr, "[CHAR] ----- program at %u -----\n%s", S.prog_start, dis);
              }
              for (q = 0; q < npsh; q++) if (seen_psh[q] == ph) break;
              if (q == npsh && npsh < 64) {
                  char *fs = (char *)malloc(96 * 1024);
                  seen_psh[npsh++] = ph;
                  if (fs && nv2a_psh_emit_glsl(&S.psh, fs, 96 * 1024))
                      fprintf(stderr, "[CHAR] ----- combiners %08X -----\n%s\n", ph, fs);
                  free(fs);
              }
              fflush(stderr);
          }
      } }
    /* RECOMP_GL_SPY_TEX=<hex offset>: everything about the first draws that
     * sample the texture at that address -- combiner words, texture words,
     * blend and test state, the vertex program, and the fragment shader as
     * emitted. For a draw that comes out opaque black where the title meant
     * a soft streak, one of these is where the alpha went. */
    { static uint32_t spy; static int sinit, shown;
      if (!sinit) { const char *v = getenv("RECOMP_GL_SPY_TEX"); sinit = 1;
                    spy = v ? (uint32_t)strtoul(v, 0, 16) : 0; }
      if (spy && shown < 4 && slot >= 0 && S.tex[0].offset == spy
          && S.xf_mode != NV2A_XF_MODE_FIXED) {
          int i; char *fs;
          /* Only the magnified ones: vertex 0 well outside the screen. */
          { Nv2aVshProgram vq; float vin[16][4], op[4]; int k;
            uint32_t pstart = S.prog_start < NV2A_VSH_MAX_INSNS ? S.prog_start : 0;
            if (nv2a_vsh_decode(S.prog + pstart * 4, (int)(NV2A_VSH_MAX_INSNS - pstart), &vq) <= 0) goto spy_done;
            memset(vin, 0, sizeof vin);
            for (k = 0; k < NUM_ATTRS; k++) {
                if (attr_at[k] >= 0) memcpy(vin[k], &vbuf[attr_at[k]], 16);
                else memcpy(vin[k], S.const_attr[k], 16);
            }
            if (!nv2a_vsh_interp(&vq, vin, S.u.c, op)) goto spy_done;
            if (fabsf(op[0] - 320.0f) < 900.0f && fabsf(op[1] - 240.0f) < 700.0f) goto spy_done; }
          fs = (char *)malloc(96 * 1024);
          shown++;
          fprintf(stderr, "  [SPY] draw %u flip %u surf %08X xf %u slot %d prim %u n %u | blend %d %04X/%04X | alpha test %d func %X ref %.3f"
                          " | depth %d func %X mask %d | cull %d face %X | colour mask %08X | stages prog 0x%05X | fog en %u mode %u col %08X\n",
                  S.draws, S.flips, S.color_offset, S.xf_mode, slot, S.prim, out_n, S.blend_enable, S.blend_src, S.blend_dst,
                  S.alpha_test, S.alpha_func, S.alpha_ref, S.depth_test, S.depth_func, S.depth_mask,
                  S.cull_enable, S.cull_face, S.color_mask, S.shader_stage_prog, S.fog_enable, S.fog_mode, S.fog_color);
          for (i = 0; i < 4; i++)
              fprintf(stderr, "  [SPY]   tex%d off %08X fmt %08X addr %08X ctl0 %08X ctl1 %08X filt %08X rect %08X en %d %ux%u bound %d\n",
                      i, S.tex[i].offset, S.tex[i].format, S.tex[i].addr, S.tex[i].control0, S.tex[i].control1,
                      S.tex[i].filter, S.tex[i].image_rect, S.tex[i].enabled, S.tex[i].width, S.tex[i].height, S.psh.tex_bound[i]);
          fprintf(stderr, "  [SPY]   combiner control %08X final %08X %08X\n", S.psh.control, S.psh.final_abcd, S.psh.final_efg);
          for (i = 0; i < (int)(S.psh.control & 0xF) && i < NV2A_PSH_STAGES; i++)
              fprintf(stderr, "  [SPY]   stage %d rgb icw %08X ocw %08X | alpha icw %08X ocw %08X | c0 %08X c1 %08X\n",
                      i, S.psh.rgb_icw[i], S.psh.rgb_ocw[i], S.psh.alpha_icw[i], S.psh.alpha_ocw[i], S.psh.c0[i], S.psh.c1[i]);
          if (S.xf_mode != NV2A_XF_MODE_FIXED) {
              Nv2aVshProgram vp; static char dis[16384];
              uint32_t pstart = S.prog_start < NV2A_VSH_MAX_INSNS ? S.prog_start : 0;
              if (nv2a_vsh_decode(S.prog + pstart * 4, (int)(NV2A_VSH_MAX_INSNS - pstart), &vp) > 0
               && nv2a_vsh_disasm(&vp, dis, sizeof dis))
                  fprintf(stderr, "%s", dis);
              fprintf(stderr, "  [SPY]   c97 %.3g %.3g %.3g %.3g  c96 %.3g %.3g %.3g %.3g\n",
                      S.u.c[97][0], S.u.c[97][1], S.u.c[97][2], S.u.c[97][3], S.u.c[96][0], S.u.c[96][1], S.u.c[96][2], S.u.c[96][3]);
              { int r; for (r = 103; r <= 110; r++)
                    fprintf(stderr, "  [SPY]   c%d %.6g %.6g %.6g %.6g\n", r, S.u.c[r][0], S.u.c[r][1], S.u.c[r][2], S.u.c[r][3]); }
              fprintf(stderr, "  [SPY]   zclip %.1f..%.1f  zminmax ctl %08X  vpscale %.1f %.1f %.1f  vpoff %.2f %.2f %.2f\n",
                      S.u.z_clip[0], S.u.z_clip[1], S.zminmax, S.u.vp_scale[0], S.u.vp_scale[1], S.u.vp_scale[2],
                      S.u.vp_off[0], S.u.vp_off[1], S.u.vp_off[2]);
              { uint32_t v; for (v = 0; v < out_n && v < 6; v++) {
                    float vin[16][4], op[4]; int k;
                    memset(vin, 0, sizeof vin);
                    for (k = 0; k < NUM_ATTRS; k++) {
                        if (attr_at[k] >= 0) memcpy(vin[k], &vbuf[(size_t)v * stride_floats + attr_at[k]], 16);
                        else memcpy(vin[k], S.const_attr[k], 16);
                    }
                    if (nv2a_vsh_interp(&vp, vin, S.u.c, op))
                        fprintf(stderr, "  [SPY]   oPos[%u] = %.4g %.4g %.6g %.4g  (z/2^24 = %.4g)\n", v, op[0], op[1], op[2], op[3], op[2] / 16777215.0f);
              } }
          }
          if (fs && nv2a_psh_emit_glsl(&S.psh, fs, 96 * 1024)) fprintf(stderr, "----- fragment shader -----\n%s\n", fs);
          free(fs);
          { uint32_t v; int k;
            for (v = 0; v < out_n && v < 6; v++) {
                fprintf(stderr, "  [SPY]   vertex %u:", v);
                for (k = 0; k < NUM_ATTRS; k++)
                    if (attr_at[k] >= 0) {
                        const float *a = &vbuf[(size_t)v * stride_floats + attr_at[k]];
                        fprintf(stderr, "  v%d(%.4g %.4g %.4g %.4g)", k, a[0], a[1], a[2], a[3]);
                    }
                fprintf(stderr, "\n");
            } }
          fflush(stderr);
          spy_done: ;
      } }

    { static int hg = -1; static int shown;
      if (hg < 0) hg = getenv("RECOMP_GL_HUGE") ? 1 : 0;
      if (hg && shown < 40 && slot >= 0 && out_n >= 3 && out_n <= 96
             && S.xf_mode != NV2A_XF_MODE_FIXED) {
          Nv2aVshProgram vp;
          uint32_t pstart = S.prog_start < NV2A_VSH_MAX_INSNS ? S.prog_start : 0;
          int len = nv2a_vsh_decode(S.prog + pstart * 4,
                                    (int)(NV2A_VSH_MAX_INSNS - pstart), &vp);
          if (len > 0) {
              float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f }, wlo = 1e30f, whi = -1e30f;
              float v0first[4] = { 0, 0, 0, 0 }; uint32_t vtx; int ok = 0;
              for (vtx = 0; vtx < out_n; vtx++) {
                  float vin[16][4], op[4]; int k;
                  memset(vin, 0, sizeof vin);
                  for (k = 0; k < NUM_ATTRS; k++) {
                      if (attr_at[k] >= 0)
                          memcpy(vin[k], &vbuf[(size_t)vtx * stride_floats + attr_at[k]], 16);
                      else memcpy(vin[k], S.const_attr[k], 16);
                  }
                  if (!vtx) memcpy(v0first, vin[0], 16);
                  if (!nv2a_vsh_interp(&vp, vin, S.u.c, op)) continue;
                  ok++;
                  if (op[0] < lo[0]) lo[0] = op[0]; if (op[0] > hi[0]) hi[0] = op[0];
                  if (op[1] < lo[1]) lo[1] = op[1]; if (op[1] > hi[1]) hi[1] = op[1];
                  if (op[3] < wlo) wlo = op[3]; if (op[3] > whi) whi = op[3];
              }
              if (ok && S.draws > 30000 && (hi[0] - lo[0] > 2700.0f || hi[1] - lo[1] > 2000.0f
                         || lo[0] < -2000.0f || hi[0] > 3000.0f || lo[1] < -2000.0f || hi[1] > 3000.0f)) {
                  shown++;
                  fprintf(stderr, "  [HUGE] draw %u flip %u surf %08X slot %d prim %u n %u: x %.0f..%.0f y %.0f..%.0f w %.3g..%.3g"
                                  " | v0 %.4g %.4g %.4g %.4g | tex0 %08X %ux%u en%d | vpoff %.1f %.1f c59 %.1f %.1f c58 %.1f %.1f"
                                  " | blend %d alpha %d ztest %d prog %d insns\n",
                          S.draws, S.flips, S.color_offset, slot, S.prim, out_n, lo[0], hi[0], lo[1], hi[1], wlo, whi,
                          v0first[0], v0first[1], v0first[2], v0first[3],
                          S.tex[0].offset, S.tex[0].width, S.tex[0].height, S.tex[0].enabled,
                          S.u.vp_off[0], S.u.vp_off[1], S.u.c[59][0], S.u.c[59][1], S.u.c[58][0], S.u.c[58][1],
                          S.blend_enable, S.alpha_test, S.depth_test, len);
                  fprintf(stderr, "  [HUGE]   c97 %.3g %.3g %.3g %.3g | blend %04X/%04X | alpha func %X ref %.2f | depth func %X mask %d | stages 0x%05X | cull %d | tex0 fmt %02X\n",
                          S.u.c[97][0], S.u.c[97][1], S.u.c[97][2], S.u.c[97][3],
                          S.blend_src, S.blend_dst, S.alpha_func, S.alpha_ref, S.depth_func, S.depth_mask,
                          S.shader_stage_prog, S.cull_enable, S.tex[0].format & 0xFF);
                  if (shown <= 3) { static char dis[16384];
                      if (nv2a_vsh_disasm(&vp, dis, sizeof dis)) fprintf(stderr, "%s", dis); }
                  fflush(stderr);
              }
          }
      } }

    { static int dcen = -1;
      static uint64_t dw[2][8], dn[2][8]; static uint32_t dlast;
      static const float edges[7] = { 50, 100, 200, 400, 800, 1600, 3200 };
      if (dcen < 0) dcen = getenv("RECOMP_GL_DISTCENSUS") ? 1 : 0;
      if (dcen && slot >= 0 && out_n >= 3 && (S.draws & 3u) == 0
             && S.xf_mode != NV2A_XF_MODE_FIXED) {
          Nv2aVshProgram vp;
          uint32_t pstart = S.prog_start < NV2A_VSH_MAX_INSNS ? S.prog_start : 0;
          int len = nv2a_vsh_decode(S.prog + pstart * 4,
                                    (int)(NV2A_VSH_MAX_INSNS - pstart), &vp);
          if (len > 0) {
              float vin[16][4], op[4]; int k2, cls, b;
              memset(vin, 0, sizeof vin);
              for (k2 = 0; k2 < NUM_ATTRS; k2++) {
                  if (attr_at[k2] >= 0)
                      memcpy(vin[k2], &vbuf[(size_t)(out_n / 2) * stride_floats
                                            + attr_at[k2]], 16);
                  else
                      memcpy(vin[k2], S.const_attr[k2], 16);
              }
              if (nv2a_vsh_interp(&vp, vin, S.u.c, op)) {
                  float w = op[3] < 0 ? -op[3] : op[3];
                  cls = g_pcache[slot].uses_a0 ? 1 : 0;
                  for (b = 0; b < 7 && w >= edges[b]; b++) ;
                  dw[cls][b] += out_n / 3; dn[cls][b]++;
                  /* And per program. The A0 split turned out not to be
                   * world-against-entity -- nearly all 3D geometry indexes
                   * the palette -- so the class that matters has to be found
                   * by what draws it. A program's triangles by distance, with
                   * its largest single draw, says whether the big chunks that
                   * a road deck is made of exist at distance at all. */
                  { enum { PS = 48 };
                    static struct { int slot; uint64_t tri[8]; uint32_t maxn;
                                    float maxw; uint64_t total; } ps[PS];
                    static int nps; int i;
                    for (i = 0; i < nps; i++) if (ps[i].slot == slot) break;
                    if (i == nps && nps < PS) { ps[nps].slot = slot; nps++; }
                    if (i < nps) {
                        ps[i].tri[b] += out_n / 3; ps[i].total += out_n / 3;
                        if (out_n > ps[i].maxn) ps[i].maxn = out_n;
                        if (w > ps[i].maxw) ps[i].maxw = w;
                    }
                    if (S.draws - dlast >= 200000u - 4u) {
                        int j, shown = 0; char done[PS]; memset(done, 0, PS);
                        fprintf(stderr, "  [DIST] by program, biggest first "
                                "(tris <50/<100/<200/<400/<800/<1600/<3200/3200+"
                                " | largest draw | farthest w):\n");
                        for (j = 0; j < nps && shown < 14; j++) {
                            int best = -1;
                            for (i = 0; i < nps; i++)
                                if (!done[i] && (best < 0 || ps[i].total > ps[best].total)) best = i;
                            if (best < 0) break;
                            done[best] = 1; shown++;
                            fprintf(stderr, "  [DIST]   slot %4d %s:", ps[best].slot,
                                    g_pcache[ps[best].slot].uses_a0 ? "A0" : "  ");
                            for (i = 0; i < 8; i++)
                                fprintf(stderr, " %7llu", (unsigned long long)ps[best].tri[i]);
                            fprintf(stderr, " | %5u verts | w %.0f\n",
                                    ps[best].maxn, ps[best].maxw);
                        }
                        fflush(stderr);
                    } }
              }
          }
      }
      if (dcen && S.draws - dlast >= 200000u) {
          int c, b;
          dlast = S.draws;
          fprintf(stderr, "  [DIST] triangles by distance (w), sampled 1 in 4:\n");
          fprintf(stderr, "  [DIST]   %-7s  <50      <100     <200     <400     "
                          "<800     <1600    <3200    3200+\n", "");
          for (c = 0; c < 2; c++) {
              fprintf(stderr, "  [DIST]   %-7s", c ? "entity" : "world");
              for (b = 0; b < 8; b++)
                  fprintf(stderr, " %8llu", (unsigned long long)dw[c][b]);
              fprintf(stderr, "   (draws:");
              for (b = 0; b < 8; b++)
                  fprintf(stderr, " %llu", (unsigned long long)dn[c][b]);
              fprintf(stderr, ")\n");
          }
          fflush(stderr);
      } }

    { static int on = -1; static int shown_pr;
      static uint32_t pr_seen[12]; static int pr_nseen;
      int pr_fresh = 1, pr_i;
      if (on < 0) on = getenv("RECOMP_GL_POSRANGE") ? 1 : 0;
      /* One report per PROGRAM, not per draw.
       *
       * The first version took the first six draws that matched and all six
       * turned out to be the same program drawing the same small thing --
       * sixteen vertices whose x never moved off the viewport centre. I read
       * that as the interpreter failing to run, which it may well not have
       * been: z varied across those same vertices, so the machine was
       * running and the inputs were varying, and a program whose x is
       * genuinely constant is just a program drawing something degenerate.
       * Six samples of one program cannot tell those apart. Twelve
       * different programs can. */
      for (pr_i = 0; pr_i < pr_nseen; pr_i++)
          if (pr_seen[pr_i] == (uint32_t)slot) { pr_fresh = 0; break; }
      if (on && pr_fresh && shown_pr < 12 && slot >= 0 && out_n >= 16
             && S.xf_mode != NV2A_XF_MODE_FIXED) {
          if (pr_nseen < 12) pr_seen[pr_nseen++] = (uint32_t)slot;
          Nv2aVshProgram vp;
          uint32_t pstart = S.prog_start < NV2A_VSH_MAX_INSNS ? S.prog_start : 0;
          int len = nv2a_vsh_decode(S.prog + pstart * 4,
                                    (int)(NV2A_VSH_MAX_INSNS - pstart), &vp);
          if (len > 0) {
              float lo[4] = { 1e30f, 1e30f, 1e30f, 1e30f };
              float hi[4] = { -1e30f, -1e30f, -1e30f, -1e30f };
              /* The whole batch, sampled, not its first sixty-four
               * vertices. Sixty-four consecutive vertices of a city mesh are
               * one corner of one building, and a depth span measured across
               * one corner says nothing about the mesh -- which matters here,
               * because the span is the headline number. Every seventh
               * vertex keeps the cost of interpreting a program in software
               * roughly where it was. */
              uint32_t vi2, step = out_n > 448u ? out_n / 64u : 1u;
              int k2, any = 0;
              for (vi2 = 0; vi2 < out_n; vi2 += step) {
                  float vin[16][4], op[4];
                  memset(vin, 0, sizeof vin);
                  for (k2 = 0; k2 < NUM_ATTRS; k2++) {
                      if (attr_at[k2] >= 0)
                          memcpy(vin[k2], &vbuf[(size_t)vi2 * stride_floats
                                                + attr_at[k2]], 4 * sizeof(float));
                      else
                          memcpy(vin[k2], S.const_attr[k2], 4 * sizeof(float));
                  }
                  if (!nv2a_vsh_interp(&vp, vin, S.u.c, op)) continue;
                  any = 1;
                  for (k2 = 0; k2 < 4; k2++) {
                      if (op[k2] < lo[k2]) lo[k2] = op[k2];
                      if (op[k2] > hi[k2]) hi[k2] = op[k2];
                  }
              }
              if (any) {
                  shown_pr++;
                  fprintf(stderr, "  [POS] slot %d, %u verts: x %.2f..%.2f  "
                          "y %.2f..%.2f  z %.4f..%.4f  w %.3f..%.3f"
                          "   (z/w %.5f..%.5f)\n", slot, out_n,
                          lo[0], hi[0], lo[1], hi[1], lo[2], hi[2],
                          lo[3], hi[3],
                          hi[3] != 0.0f ? lo[2] / hi[3] : 0.0f,
                          lo[3] != 0.0f ? hi[2] / lo[3] : 0.0f);
                  fflush(stderr);
              }
          }
      } }

    { static int on = -1; static int shown;
      static uint64_t a0_draws, a0_bad; static uint32_t last_report;
      static uint64_t g_a0_hist[9];
      if (on < 0) on = getenv("RECOMP_GL_SKINDBG") ? 1 : 0;
      if (on && slot >= 0 && g_pcache[slot].uses_a0) a0_draws++;
      if (on && S.draws - last_report >= 200000u) {
          int hb;
          last_report = S.draws;
          fprintf(stderr, "  [SKIN] of %llu draws that index the constant "
                  "file, %llu picked a matrix that is not one (%.3f%%)\n",
                  (unsigned long long)a0_draws, (unsigned long long)a0_bad,
                  a0_draws ? 100.0 * (double)a0_bad / (double)a0_draws : 0.0);
          /* The gap in the check above, spelled out.
           *
           * "Is it a matrix" is a much weaker question than "is it the RIGHT
           * matrix", and an index that is off by a few lands on a perfectly
           * valid neighbouring matrix and passes silently. An object placed
           * by another object's transform is whole, correctly shaped, and
           * somewhere else entirely -- which is what a bus in mid-air is.
           * So: the distribution. A bone palette based at 107 with four
           * constants each, inside a file of 192, can only reach about
           * twenty entries, so sane indices are small and land on multiples
           * of the matrix stride. A spread across hundreds means the value
           * feeding ARL is being scaled wrongly, and a spread that is dense
           * everywhere means it is not an index at all. */
          fprintf(stderr, "  [SKIN] address register values seen, by range:");
          for (hb = 0; hb < 8; hb++)
              if (g_a0_hist[hb])
                  fprintf(stderr, "  %d-%d x%llu", hb * 32,
                          hb * 32 + 31, (unsigned long long)g_a0_hist[hb]);
          if (g_a0_hist[8])
              fprintf(stderr, "  256+ x%llu", (unsigned long long)g_a0_hist[8]);
          fprintf(stderr, "\n");
          fflush(stderr);
      }
      if (on && slot >= 0 && g_pcache[slot].uses_a0 && out_n >= 8
             && S.xf_mode != NV2A_XF_MODE_FIXED) {
          Nv2aVshProgram vp;
          uint32_t pstart = S.prog_start < NV2A_VSH_MAX_INSNS ? S.prog_start : 0;
          int len = nv2a_vsh_decode(S.prog + pstart * 4,
                                    (int)(NV2A_VSH_MAX_INSNS - pstart), &vp);
          int rel_base = -1, q;
          for (q = 0; q < len; q++)
              if (vp.insns[q].rel_addr) { rel_base = vp.insns[q].const_index; break; }
          if (len > 0 && rel_base >= 0) {
              uint32_t vi;
              int bad = 0, bad_a0 = 0, bad_at = 0;
              for (vi = 0; vi < out_n && !bad; vi++) {
                  float vin[16][4]; int a0s[8], na, k;
                  memset(vin, 0, sizeof vin);
                  for (k = 0; k < NUM_ATTRS; k++) {
                      if (attr_at[k] >= 0)
                          memcpy(vin[k], &vbuf[(size_t)vi * stride_floats
                                               + attr_at[k]], 4 * sizeof(float));
                      else
                          memcpy(vin[k], S.const_attr[k], 4 * sizeof(float));
                  }
                  na = nv2a_vsh_interp_a0(&vp, vin, S.u.c, a0s, 8);
                  for (k = 0; k < na; k++) {
                      int hh = a0s[k] < 0 ? 0 : (a0s[k] >> 5);
                      g_a0_hist[hh > 8 ? 8 : hh]++;
                  }
                  for (k = 0; k < na; k++) {
                      int idx = rel_base + a0s[k];
                      const float *r;
                      if (idx < 0 || idx + 3 > NV2A_VSH_NUM_CONSTS - 1) {
                          bad = 1; bad_a0 = a0s[k]; bad_at = idx; break;
                      }
                      r = S.u.c[idx + 3];
                      if (fabsf(r[0]) > 1e-4f || fabsf(r[1]) > 1e-4f
                       || fabsf(r[2]) > 1e-4f || fabsf(r[3] - 1.0f) > 1e-3f) {
                          bad = 1; bad_a0 = a0s[k]; bad_at = idx; break;
                      }
                  }
              }
              if (bad) {
                  int c;
                  a0_bad++;
                  /* Every vertex's address, not just the one that failed.
                   *
                   * One anomalous vertex among hundreds of good ones and a
                   * whole batch of bad ones are completely different bugs,
                   * and the report has never been able to tell them apart:
                   * it stops at the first failure and describes that vertex
                   * alone. If vertex 0 picks bone 3, vertex 1 picks 678 and
                   * vertex 2 picks 3 again, the data at one element is
                   * corrupt. If every vertex picks something in the
                   * hundreds, the index is being read or scaled wrongly for
                   * the whole batch and nothing is corrupt at all. The first
                   * wants a memory fix and the second wants a decode fix,
                   * and guessing between them is how this diagnostic has
                   * been wrong three times.
                   *
                   * Cheap because it is only ever reached on a draw that has
                   * already failed. */
                  if (shown < 12) {
                      uint32_t vq; int nin = 0, nout = 0, first_bad = -1;
                      fprintf(stderr, "         addresses across the batch:");
                      for (vq = 0; vq < out_n && vq < 24; vq++) {
                          float vin2[16][4]; int a2[8], na2, kq;
                          memset(vin2, 0, sizeof vin2);
                          for (kq = 0; kq < NUM_ATTRS; kq++) {
                              if (attr_at[kq] >= 0)
                                  memcpy(vin2[kq], &vbuf[(size_t)vq * stride_floats
                                                         + attr_at[kq]], 16);
                              else
                                  memcpy(vin2[kq], S.const_attr[kq], 16);
                          }
                          na2 = nv2a_vsh_interp_a0(&vp, vin2, S.u.c, a2, 8);
                          if (na2 > 0) {
                              int ok2 = (rel_base + a2[0] >= 0)
                                     && (rel_base + a2[0] + 3 < NV2A_VSH_NUM_CONSTS);
                              fprintf(stderr, " %d%s", a2[0], ok2 ? "" : "!");
                              if (ok2) nin++; else { nout++;
                                  if (first_bad < 0) first_bad = (int)vq; }
                          }
                      }
                      fprintf(stderr, "\n         %d in range, %d out%s\n",
                              nin, nout,
                              nout && nin ? " -- a few bad vertices among good ones,"
                                            " so the DATA is wrong at those elements"
                            : nout ? " -- the whole batch, so the index is being"
                                     " read or scaled wrongly, not corrupted"
                                   : "");
                  }
                  if (shown >= 12) goto skin_done;
                  shown++;
                  fprintf(stderr, "  [SKIN] draw %u, %u verts, slot %d, "
                          "program reads c[A0%+d]; vertex %u set A0=%d, which "
                          "is c[%d] -- %s\n", S.draws, out_n, slot, rel_base,
                          vi - 1, bad_a0, bad_at,
                          (bad_at < 0 || bad_at + 3 > NV2A_VSH_NUM_CONSTS - 1)
                              ? "outside the constant file"
                              : "not a matrix");
                  fprintf(stderr, "         attributes:");
                  for (c = 0; c < nlive; c++)
                      fprintf(stderr, " v%u=type%u/size%u/stride%u",
                              live[c].idx, live[c].type, live[c].size,
                              live[c].stride);
                  fprintf(stderr, "\n");
                  /* The vertex that went wrong and its neighbours, as this
                   * decoder produced them.
                   *
                   * An index of eighty in a four-byte attribute is either a
                   * bone number this title cannot possibly have -- eighty
                   * bones would need four hundred and twenty-seven constants
                   * and there are a hundred and ninety-two -- or it is not an
                   * index at all and these bytes are something else read from
                   * the wrong place. The neighbours separate the two: data
                   * that belongs together looks alike. */
                  { int wi = (int)vi - 1, d;
                    for (d = wi - 2; d <= wi + 2; d++) {
                        int k2;
                        uint32_t si;
                        if (d < 0 || d >= (int)out_n) continue;
                        if (fan_expand == 1) {
                            static const uint32_t qq[6] = { 0, 1, 2, 2, 3, 0 };
                            si = S.idx[(d / 6) * 4 + qq[d % 6]];
                        } else if (fan_expand == 2) {
                            static const uint32_t qq[6] = { 0, 1, 3, 3, 2, 0 };
                            si = S.idx[(d / 6) * 2 + qq[d % 6]];
                        } else {
                            si = S.idx[d];
                        }
                        /* The source index as well as the decoded vertex.
                         *
                         * A vertex that decodes to nothing has two possible
                         * explanations and they are not alike: the array is
                         * empty where it points, or it points at element
                         * zero. If the empty vertices all say zero here, the
                         * index list is what is wrong, not the data. */
                        fprintf(stderr, "         vertex %-4d <- src %-5u", d, si);
                        for (k2 = 0; k2 < nlive; k2++) {
                            const float *vv = &vbuf[(size_t)d * stride_floats
                                                    + (size_t)k2 * 4];
                            fprintf(stderr, "  v%u(%.4f %.4f %.4f %.4f)",
                                    live[k2].idx, vv[0], vv[1], vv[2], vv[3]);
                        }
                        fprintf(stderr, "\n");
                    } }
                  /* Where those bytes came from, and what is at the same
                   * offset read the other way.
                   *
                   * Most of the vertices in these batches decode to zero --
                   * position, index and normal all zero -- which is not data
                   * a title would draw. Either the array is not where this
                   * thinks it is, or it has not been written yet. This
                   * runtime keeps contiguous memory in a window of its own at
                   * 0x80000000 rather than aliasing RAM, and every array
                   * offset below sixty-four megabytes is resolved into that
                   * window; if these buffers are ordinary memory instead, the
                   * bytes read are whatever happens to be in the window, and
                   * zero is exactly what that looks like. Print both. */
                  { int k3;
                    uint32_t bad_si, max_si = 0, q2;
                    /* The address the failing vertex was actually read from.
                     *
                     * This used to offset by the vertex's position in the
                     * OUTPUT, not by its source index, which for an indexed
                     * draw are different numbers -- the failing vertex here
                     * is output 936 and source 920 -- so it has been
                     * printing the bytes of an unrelated vertex and calling
                     * them the evidence. The source index is the one that
                     * multiplies the stride. */
                    { static const uint32_t qq2[6] = { 0, 1, 3, 3, 2, 0 };
                      int dd = (int)vi - 1;
                      uint32_t ii = (S.prim == 8 && S.idx_count >= 2)
                                  ? (uint32_t)((dd / 6) * 2) + qq2[dd % 6]
                                  : (uint32_t)dd;
                      bad_si = (dd >= 0 && ii < S.idx_count) ? S.idx[ii] : 0u; }
                    for (q2 = 0; q2 < S.idx_count; q2++)
                        if (S.idx[q2] > max_si) max_si = S.idx[q2];
                    fprintf(stderr, "         batch uses source indices up to "
                            "%u across %u of them; the failing one is %u\n",
                            max_si, S.idx_count, bad_si);
                    /* The same element read at neighbouring strides.
                     *
                     * These failures cluster on vertex ONE of small batches,
                     * never vertex zero -- and vertex zero is at offset zero
                     * whatever the stride is, so it is right by construction
                     * and proves nothing. Vertex one is the first read that
                     * the stride can get wrong. If a neighbouring stride
                     * makes the bone index land back in range while the
                     * declared one does not, the stride is what is wrong,
                     * and that is a completely different bug from an index
                     * running off the end of a buffer -- which is the other
                     * half of these failures.
                     *
                     * Printed for the attribute the program actually indexes
                     * with, decoded the way that attribute's type says, so
                     * the answer is a bone number rather than four bytes to
                     * squint at. */
                    { int k4;
                      for (k4 = 0; k4 < nlive; k4++) {
                          int dstride;
                          if (live[k4].type != 4) continue;   /* ubyte4 index */
                          fprintf(stderr, "         v%u (the index) at "
                                  "neighbouring strides:", live[k4].idx);
                          for (dstride = -8; dstride <= 8; dstride += 4) {
                              uint32_t st = (uint32_t)((int)live[k4].stride + dstride);
                              const uint8_t *q;
                              if ((int)live[k4].stride + dstride <= 0) continue;
                              q = guest(resolve(S.attr[live[k4].idx].offset))
                                + (size_t)bad_si * st;
                              fprintf(stderr, "  %u:(%u %u %u %u)%s",
                                      st, q[0], q[1], q[2], q[3],
                                      dstride == 0 ? "<-declared" : "");
                          }
                          fprintf(stderr, "\n");
                      } }
                    for (k3 = 0; k3 < nlive; k3++) {
                        uint32_t off = S.attr[live[k3].idx].offset;
                        uint32_t va  = resolve(off);
                        size_t   at  = (size_t)bad_si * live[k3].stride;
                        const uint8_t *a1 = guest(va)  + at;
                        const uint8_t *a2 = guest(off) + at;
                        /* Read the same element both ways. Contiguous memory
                         * lives in a window of its own at 0x80000000 rather
                         * than aliasing RAM, so an array the title filled
                         * through one route and this reads through the other
                         * disagrees -- and zeros are what that looks like.
                         * If the two agree and both are junk, the index is
                         * simply past the end of the data. */
                        fprintf(stderr, "         v%u array %08X -> %08X, "
                                "element %u at +%zu;"
                                " window: %02X %02X %02X %02X %02X %02X %02X %02X"
                                " | ram: %02X %02X %02X %02X %02X %02X %02X %02X%s\n",
                                live[k3].idx, off, va, bad_si, at,
                                a1[0],a1[1],a1[2],a1[3],a1[4],a1[5],a1[6],a1[7],
                                a2[0],a2[1],a2[2],a2[3],a2[4],a2[5],a2[6],a2[7],
                                memcmp(a1, a2, 8) ? "   <-- the two routes disagree" : "");
                    } }
                  for (c = rel_base; c < rel_base + 8 && c < 192; c++)
                      if (c >= 0)
                          fprintf(stderr, "         c[%d] = (%9.3f %9.3f "
                                  "%9.3f %9.3f)\n", c, S.u.c[c][0],
                                  S.u.c[c][1], S.u.c[c][2], S.u.c[c][3]);
                  fflush(stderr);
              }
              skin_done: ;
          }
      } }
    g_prof.n_draws++;
    S.since_present++;
    /* A GL error turns every later call into a no-op, so a single bad enum
     * early on reads as "nothing draws" for the rest of the run. The check
     * used to follow every draw, and only stopped after eight errors: with no
     * error it never stopped. glGetError makes a driver that works on its own
     * thread (Mesa's, AMD's and NVIDIA's on Windows) catch up with the game
     * before it can answer, a wait at every one of a frame's hundreds of
     * draws. It is asked once a frame now, in present();
     * RECOMP_GL_ERRORS_PER_DRAW=1 asks after every draw again, to name the
     * one that broke. */
#ifdef NV2A_GL_HAVE_GL
    if (g_is_gl && g_gl_err_reports < 8 && gl_err_per_draw()) {
        GLenum e = glGetError();
        if (e != GL_NO_ERROR) {
            fprintf(stderr, "  [GL] glGetError 0x%04X after draw "
                    "(prim=%u verts=%u fbo=%ux%u status=0x%04X)\n",
                    e, S.prim, out_n, g_fbo_w, g_fbo_h,
                    glCheckFramebufferStatus(GL_FRAMEBUFFER));
            g_gl_err_reports++;
        }
    }
#endif
    /* RECOMP_GL_STATE_AT=<draw number>: the full per-draw state for a few
     * draws at a chosen point in the run. Early draws answer questions about
     * bring-up; a title that renders its logos and then goes blank needs the
     * state from where it went blank. */
    {
        static long at = -2, at3d = -2, atflip = -2, atverts = -2; static int shown;
        if (at == -2) { const char *v = getenv("RECOMP_GL_STATE_AT");
                        const char *w = getenv("RECOMP_GL_STATE_AT3D");
                        const char *f = getenv("RECOMP_GL_STATE_AT_FLIP");
                        const char *n = getenv("RECOMP_GL_STATE_AT_VERTS");
                        atverts = n ? atol(n) : -1;
                        at = v ? atol(v) : -1;
                        at3d = w ? atol(w) : -1;
                        atflip = f ? atol(f) : -1; }
        /* RECOMP_GL_STATE_AT_SINCE=<n>: the nth draw of a frame, which is how
         * the draw-identity buffer names the one that owns the screen.
         * RECOMP_GL_STATE_AT3D then acts purely as a gate, so the trigger
         * lands in the phase being looked at rather than the first frame
         * that happens to be long enough. */
        /* RECOMP_GL_STATE_AT_OVERLAY: the draw that covers the screen, named
         * by what it is rather than by where it falls in the frame. Its index
         * moves between frames; its signature does not -- a screen-space
         * viewport, blending on, and far more vertices than the little UI
         * quads that share that viewport. */
        { static long atov = -2;
          if (atov == -2) { const char *v = getenv("RECOMP_GL_STATE_AT_OVERLAY");
                            atov = v ? atol(v) : -1; }
          if (atov >= 0 && shown < 6 && (long)S.flips >= atov
           && S.u.vp_off[0] < 100.0f && out_n >= 40
           && S.blend_enable && S.blend_src == 0x0302
           && S.tex[0].enabled)
              at = (long)S.draws; }
        { static long atsince = -2;
          if (atsince == -2) { const char *v = getenv("RECOMP_GL_STATE_AT_SINCE");
                               atsince = v ? atol(v) : -1; }
          /* RECOMP_GL_STATE_AT_SEC=<s>: only from s seconds after the
           * renderer started -- a gate by time, for a phase whose draw
           * counts are not known in advance. */
          static double atsec = -2.0, t0;
          if (atsec == -2.0) { const char *v = getenv("RECOMP_GL_STATE_AT_SEC");
                               atsec = v ? atof(v) : -1.0; t0 = mono_now(); }
          if (atsince >= 0) {
              if ((long)S.since_present == atsince && shown < 6
               && (at3d < 0 || (long)S.draws_3d >= at3d)
               && (atsec < 0 || mono_now() - t0 >= atsec))
                  at = (long)S.draws;
          } else if (at3d >= 0 && (long)S.draws_3d >= at3d && shown < 6) {
              at = (long)S.draws;
          } }
        /* By presented frame, for the parts of a title that are a handful of
         * draws each -- a logo screen never reaches a draw or scene-draw count
         * worth naming, but it always has a frame number. */
        if (atflip >= 0 && (long)S.flips >= atflip && shown < 6) at = (long)S.draws;
        /* By batch size: the geometry that covers the screen is the geometry
         * with the most vertices in it, whatever phase the title is in. */
        if (atverts >= 0 && (long)out_n >= atverts && shown < 6) at = (long)S.draws;
        /* By which inputs the program reads: the family that renders flat is
         * identified by its input mask, and nothing else about the draw
         * distinguishes it from the family beside it that renders correctly. */
        { static long atin = -2;
          if (atin == -2) { const char *v = getenv("RECOMP_GL_STATE_AT_INPUTS");
                            atin = v ? strtol(v, 0, 16) : -1; }
          if (atin >= 0 && (long)g_pcache[slot].inputs == atin && shown < 6)
              at = (long)S.draws; }
        if (at >= 0 && (long)S.draws >= at && shown < 6) {
            shown++;
            fprintf(stderr,
                "  [GL] draw#%u prim=%u verts=%u prog=%d tex(off=%08X %ux%u "
                "fmt=%02X en=%d ctl0=%08X) alpha(t=%d f=%04X ref=%.2f) "
                "blend(%d %04X %04X) depth(t=%d f=%04X m=%d) surf=%08X "
                "clear=%08X z=%08X\n",
                S.draws, S.prim, out_n, slot, S.tex[0].offset, S.tex[0].width,
                S.tex[0].height, S.tex[0].color, S.tex[0].enabled,
                S.tex[0].control0, S.alpha_test, S.alpha_func, S.alpha_ref,
                S.blend_enable, S.blend_src, S.blend_dst, S.depth_test,
                S.depth_func, S.depth_mask, S.color_offset, S.clear_color,
                S.clear_zstencil);
            { uint32_t st;
              for (st = 0; st < 4; st++)
                  if (S.tex[st].enabled || S.tex[st].offset)
                      fprintf(stderr, "        tex%u off=%08X %ux%u fmt=%02X "
                              "en=%d pitch=%u ctl0=%08X ctl1=%08X filt=%08X "
                              "src=%s\n",
                              st, S.tex[st].offset, S.tex[st].width,
                              S.tex[st].height, S.tex[st].color,
                              S.tex[st].enabled, S.tex[st].pitch,
                              S.tex[st].control0, S.tex[st].control1,
                              S.tex[st].filter,
                              /* Which of the two ways this stage gets its
                               * pixels. A stage reading a surface the title
                               * rendered into is render-to-texture, and an
                               * empty one there looks exactly like a texture
                               * that decoded to black. */
                              surface_texture(S.tex[st].offset, 0, 0)
                                  ? "surface" : "memory"); }
            /* The combiner wiring for this draw, at the draw rather than at
             * shader-compile time: a program compiles once and is cached, so
             * by the time the phase under investigation arrives there is no
             * compile left to hook. */
            { uint32_t k;
              fprintf(stderr, "        combiners: %u stage(s) ctl %08X "
                      "final %08X/%08X\n",
                      (unsigned)(S.psh.control & 0xF), S.psh.control,
                      S.psh.final_abcd, S.psh.final_efg);
              for (k = 0; k < (S.psh.control & 0xF) && k < NV2A_PSH_STAGES; k++)
                  fprintf(stderr, "          stage %u: rgb %08X -> %08X | "
                          "a %08X -> %08X | c0 %08X c1 %08X\n",
                          k, S.psh.rgb_icw[k], S.psh.rgb_ocw[k],
                          S.psh.alpha_icw[k], S.psh.alpha_ocw[k],
                          S.psh.c0[k], S.psh.c1[k]); }
            fprintf(stderr, "        viewport scale=(%.2f %.2f %.2f %.2f) "
                    "offset=(%.2f %.2f %.2f %.2f)\n",
                    S.u.vp_scale[0], S.u.vp_scale[1], S.u.vp_scale[2], S.u.vp_scale[3],
                    S.u.vp_off[0], S.u.vp_off[1], S.u.vp_off[2], S.u.vp_off[3]);
            {
                uint32_t j;
                fprintf(stderr, "        program inputs=%04X, arrays:",
                        g_pcache[slot].inputs);
                for (j = 0; j < NUM_ATTRS; j++)
                    if (S.attr[j].size)
                        fprintf(stderr, " v%u(t%u s%u st%u)", j, S.attr[j].type,
                                S.attr[j].size, S.attr[j].stride);
                fprintf(stderr, "\n");
                /* Every attribute the program reads, as the title described
                 * it -- including the ones it described as absent. "Never
                 * mentioned" and "mentioned, size zero" are different bugs. */
                for (j = 0; j < NUM_ATTRS; j++)
                    if (g_pcache[slot].inputs & (1u << j))
                        fprintf(stderr, "        v%-2u fmt=%08X %s off=%08X\n",
                                j, S.attr[j].raw,
                                S.attr[j].seen ? "set" : "NEVER SET",
                                S.attr[j].offset);
                /* The vertex buffer as bytes.
                 *
                 * An attribute that reads zero while its neighbour in the
                 * same vertex reads correctly is either absent from memory or
                 * being read from the wrong place, and those need different
                 * fixes. Printing the bytes the title actually put there
                 * settles which, and interpreting them as floats shows
                 * whether the coordinates are present at some other offset. */
                for (j = 0; j < NUM_ATTRS; j++) {
                    if (!(g_pcache[slot].inputs & (1u << j))) continue;
                    if (!S.attr[j].size || !S.attr[j].stride) continue;
                    { const uint8_t *base = guest(resolve(S.attr[j].offset));
                      uint32_t v, k;
                      if (!base) { fprintf(stderr, "        v%-2u -> unmapped\n", j);
                                   continue; }
                      fprintf(stderr, "        v%-2u bytes:", j);
                      for (v = 0; v < 2; v++) {
                          const uint8_t *q = base + (size_t)v * S.attr[j].stride;
                          for (k = 0; k < 8 && k < S.attr[j].size * 4u; k++)
                              fprintf(stderr, " %02X", q[k]);
                          fprintf(stderr, " |");
                      }
                      fprintf(stderr, "\n"); }
                }
                /* And the whole first vertex, so a coordinate sitting at an
                 * offset we did not expect is visible rather than inferred. */
                if (S.attr[0].stride) {
                    const uint8_t *base = guest(resolve(S.attr[0].offset));
                    if (base) {
                        uint32_t k;
                        fprintf(stderr, "        vertex 0 as floats:");
                        for (k = 0; k + 4 <= S.attr[0].stride && k < 64; k += 4) {
                            float f; memcpy(&f, base + k, 4);
                            fprintf(stderr, " %+.3f", f);
                        }
                        fprintf(stderr, "\n");
                    }
                }
                for (j = 0; j < NUM_ATTRS; j++)
                    if ((g_pcache[slot].inputs & (1u << j)) || S.attr[j].size)
                        fprintf(stderr, "        v%-2u = (%.3f %.3f %.3f %.3f)\n",
                                j, vbuf[j*4+0], vbuf[j*4+1],
                                vbuf[j*4+2], vbuf[j*4+3]);
            }
        }
    }

    /* RECOMP_GL_CAPTURE=<prefix>: everything one draw needs, to a file.
     *
     * Written so a draw can be replayed outside the game: the microcode, the
     * whole constant bank, the viewport, and the packed vertices. Each
     * hypothesis about why a frame is wrong otherwise costs a five-minute run
     * of the title to reach the same draw again. */
    {
        static int caps;
        static const char *cpfx; static int cinit;
        static long cat;
        static long cat3d = -1;
        if (!cinit) { cinit = 1; cpfx = getenv("RECOMP_GL_CAPTURE");
                      { const char *v = getenv("RECOMP_GL_STATE_AT");
                        cat = v ? atol(v) : 0; }
                      { const char *v = getenv("RECOMP_GL_CAPTURE_3D");
                        cat3d = v ? atol(v) : -1; } }
        /* When capturing the scene rather than bring-up, wait for a batch big
         * enough to be scenery: a twelve-vertex sprite replays fine and says
         * nothing about why the streets are dark. */
        if (cpfx && caps < 4
            && (cat3d >= 0 ? ((long)S.draws_3d >= cat3d && out_n >= 300)
                           : (long)S.draws >= cat)) {
            char nm[128];
            FILE *f;
            snprintf(nm, sizeof nm, "%s%d.bin", cpfx, caps++);
            f = fopen(nm, "wb");
            if (f) {
                uint32_t hdr[8];
                hdr[0] = 0x42544143u;          /* "CATB" */
                hdr[1] = out_n;
                hdr[2] = stride_floats;
                hdr[3] = g_pcache[slot].inputs;
                hdr[4] = S.prim;
                hdr[5] = g_fbo_w;
                hdr[6] = g_fbo_h;
                hdr[7] = NV2A_VSH_MAX_INSNS;
                fwrite(hdr, sizeof hdr, 1, f);
                fwrite(S.u.vp_scale, sizeof S.u.vp_scale, 1, f);
                fwrite(S.u.vp_off, sizeof S.u.vp_off, 1, f);
                fwrite(S.prog, sizeof S.prog, 1, f);
                fwrite(S.u.c, sizeof S.u.c, 1, f);
                fwrite(vbuf, sizeof(float) * out_n * stride_floats, 1, f);
                fclose(f);
                fprintf(stderr, "  [GL] captured %s (%u verts, prim %u)\n",
                        nm, out_n, S.prim);
            }
        }
    }

    /* RECOMP_GL_DUMP_DRAW=<prefix>: the surface as it stands after a draw,
     * which is the only way to tell geometry that never rasterised from
     * geometry that was overwritten before the frame ended. */
    {
        static int shots;
        static const char *pfx; static int init;
        if (!init) { init = 1; pfx = getenv("RECOMP_GL_DUMP_DRAW"); }
        if (pfx && (S.draws % 4000) == 3999) dump_fbo(g_cur_rt, pfx, &shots, 12);
    }
    /* RECOMP_GL_DRAW_LOG=<n>: one line per draw for the first frame that
     * contains at least n of them.
     *
     * The identity buffer names the draw that owns the screen by its index
     * within the frame, so the log has to start at that frame's first draw
     * for the indices to line up. Latching on a running total instead --
     * draws so far, frames so far -- needs a number guessed in advance, and
     * the pace of a run here varies threefold with whatever else is using the
     * two cores; most of those guesses missed the phase in one direction or
     * the other. How many draws are in a frame is a property of the phase
     * rather than of how far the run got. */
    /* Six frames, not one: the frame after a busy one is often a single
     * full-screen fade, and latching onto exactly the next frame kept
     * catching that instead. Each line carries its frame number so the busy
     * one can be picked out afterwards. */
    if (g_draw_log_flip >= 0 && (long)S.flips >= g_draw_log_flip
     && (long)S.flips < g_draw_log_flip + 6) {
        /* The texture coordinate as it comes out of the vertex buffer.
         *
         * Everything measured so far about the flat backdrop was measured
         * downstream of here -- what the shader wrote, what the sampler
         * returned, which pixels ended up which colour -- and each of those
         * has a whole chain behind it. This is the one number with nothing
         * behind it: attribute 9 is TEXCOORD0, fetch() has already run, and
         * if the span is zero then the coordinate never existed in the first
         * place and no amount of shader work will conjure it. If the span is
         * wide, the data is there and the loss is downstream. One line
         * splits the search in half. */
        float t0lo[2] = { 1e30f, 1e30f }, t0hi[2] = { -1e30f, -1e30f };
        /* And the positions as they came in: whether two draws are the
         * same polygons (a second pass, a decal over its wall) or merely
         * close to each other. */
        float plo[3] = { 1e30f, 1e30f, 1e30f }, phi[3] = { -1e30f, -1e30f, -1e30f }, p0[3] = { 0, 0, 0 };
        uint32_t v; int c;
        for (v = 0; v < out_n; v++)
            for (c = 0; c < 2; c++) {
                float t = vbuf[(size_t)v * stride_floats + 9 * 4 + c];
                if (t < t0lo[c]) t0lo[c] = t;
                if (t > t0hi[c]) t0hi[c] = t;
            }
        if (attr_at[0] >= 0 && out_n) {
            for (v = 0; v < out_n; v++)
                for (c = 0; c < 3; c++) {
                    float t = vbuf[(size_t)v * stride_floats + (size_t)attr_at[0] + (size_t)c];
                    if (t < plo[c]) plo[c] = t;
                    if (t > phi[c]) phi[c] = t;
                }
            for (c = 0; c < 3; c++) p0[c] = vbuf[(size_t)attr_at[0] + (size_t)c];
        }
        fprintf(stderr, "[DRAW] f%ld %4u verts %5u prim %u prog %2d "
                "vp %s tex0 %08X %ux%u f%02X en%d tex1 en%d "
                "blend %d %04X/%04X depth %d/%d ctl %08X final %08X/%08X "
                "| xf%u a9 off=%08X t%u s%u st%u u[%.3f..%.3f] v[%.3f..%.3f] "
                "in%04X | sten %u f%X r%X m%X w%X op%X/%X/%X dfunc %X cm %08X | cull %d %X %X zc %X"
                " | a0 off=%08X st%u p0 %.3f %.3f %.3f box %.3f..%.3f %.3f..%.3f %.3f..%.3f%s%s%s\n",
                (long)S.flips, S.since_present, out_n, S.prim, slot,
                S.u.vp_off[0] > 100.0f ? "3D" : "2D",
                S.tex[0].offset, S.tex[0].width, S.tex[0].height,
                S.tex[0].color, S.tex[0].enabled, S.tex[1].enabled,
                S.blend_enable, S.blend_src, S.blend_dst,
                S.depth_test, S.depth_mask,
                S.psh.control, S.psh.final_abcd, S.psh.final_efg,
                S.xf_mode,
                S.attr[9].offset, S.attr[9].type, S.attr[9].size,
                S.attr[9].stride, t0lo[0], t0hi[0], t0lo[1], t0hi[1],
                g_pcache[slot].inputs,
                S.stencil_en, S.stencil_func, S.stencil_ref, S.stencil_fmask,
                S.stencil_wmask, S.stencil_op[0], S.stencil_op[1], S.stencil_op[2],
                (unsigned)S.depth_func, S.color_mask,
                S.cull_enable, (unsigned)S.cull_face, (unsigned)S.front_face, S.zminmax,
                S.attr[0].offset, S.attr[0].stride, p0[0], p0[1], p0[2],
                plo[0], phi[0], plo[1], phi[1], plo[2], phi[2],
                g_imm_drawing ? " | IMMEDIATE" : "",
                g_wide_squeeze ? " | WIDE-SQUEEZED" : "",
                g_wide_anchor > 0.0f ? " | HUD-RIGHT" : g_wide_anchor < 0.0f ? " | HUD-LEFT" : "");
        if (nv2a_wide_scene() && g_pcache[slot].inputs == 0x1F9B)
            fprintf(stderr, "[DRAW] f%ld %4u   2D ndc x[%.2f..%.2f] y[%.2f..%.2f]%s\n",
                    (long)S.flips, S.since_present, g_wide_ext[0], g_wide_ext[1],
                    g_wide_ext[2], g_wide_ext[3], g_wide_squeeze ? " squeezed" : "");
        /* And, for a fixed-function batch, how it is lit. */
        if (S.xf_mode == NV2A_XF_MODE_FIXED)
            fprintf(stderr, "[DRAW] f%ld %4u   lit %d mask %X colmat %02X ctl %X spec %u | sceneAmb %.3f %.3f %.3f matEm %.3f %.3f %.3f"
                    " | l0 amb %.3f %.3f %.3f dif %.3f %.3f %.3f dir %.3f %.3f %.3f | normal %u(t%u) diffuse %u specular %u"
                    " | const normal %.3f %.3f %.3f %.3f | imv %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f\n",
                    (long)S.flips, S.since_present, S.ff_lighting, S.light_enable_mask, S.color_material,
                    S.light_control, S.specular_enable,
                    S.scene_ambient[0], S.scene_ambient[1], S.scene_ambient[2],
                    S.material_emission[0], S.material_emission[1], S.material_emission[2],
                    S.light_amb[0][0], S.light_amb[0][1], S.light_amb[0][2],
                    S.light_dif[0][0], S.light_dif[0][1], S.light_dif[0][2],
                    S.light_dir[0][0], S.light_dir[0][1], S.light_dir[0][2],
                    S.attr[2].size, S.attr[2].type, S.attr[3].size, S.attr[4].size,
                    S.const_attr[2][0], S.const_attr[2][1], S.const_attr[2][2], S.const_attr[2][3],
                    S.ff_imv[0][0], S.ff_imv[0][1], S.ff_imv[0][2], S.ff_imv[1][0], S.ff_imv[1][1], S.ff_imv[1][2],
                    S.ff_imv[2][0], S.ff_imv[2][1], S.ff_imv[2][2]);
        /* RECOMP_GL_VOLDUMP=1: a fixed-function shadow volume (stencil
         * incremented or decremented, colour untouched) of the first logged
         * frame, every vertex: as sent, then through the composite matrix
         * (clip x y z w, and z/w). */
        { static int vd = -1;
          if (vd < 0) vd = getenv("RECOMP_GL_VOLDUMP") ? 1 : 0;
          if (vd && (long)S.flips <= g_draw_log_flip + 1 && S.xf_mode == NV2A_XF_MODE_FIXED
              && S.stencil_en && (S.stencil_op[2] == 0x1E02 || S.stencil_op[2] == 0x1E03)
              && attr_at[0] >= 0) {
              const float (*m)[4] = S.u.ff_mat;
              uint32_t q;
              fprintf(stderr, "[VOLM] f%ld %u mat %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g | vpoff %g %g clip %g %g\n",
                      (long)S.flips, S.since_present,
                      m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3],
                      m[2][0], m[2][1], m[2][2], m[2][3], m[3][0], m[3][1], m[3][2], m[3][3],
                      S.vp_off_reg[0], S.vp_off_reg[1], S.u.z_clip[0], S.u.z_clip[1]);
              for (q = 0; q < out_n && q < 1200; q++) {
                  const float *vv = &vbuf[(size_t)q * stride_floats + (size_t)attr_at[0]];
                  float c4[4]; int r;
                  for (r = 0; r < 4; r++)
                      c4[r] = vv[0] * m[r][0] + vv[1] * m[r][1] + vv[2] * m[r][2] + vv[3] * m[r][3];
                  fprintf(stderr, "[VOLV] f%ld %u %u %.3f %.3f %.3f %.3f | %.3f %.3f %.1f %.4f | %.2f %.2f %.1f\n",
                          (long)S.flips, S.since_present, q, vv[0], vv[1], vv[2], vv[3],
                          c4[0], c4[1], c4[2], c4[3],
                          c4[3] != 0.0f ? c4[0] / c4[3] : 0.0f, c4[3] != 0.0f ? c4[1] / c4[3] : 0.0f,
                          c4[3] != 0.0f ? c4[2] / c4[3] : 0.0f);
              }
          } }
    }

    /* Record what this draw was, for the order dump. The extent only while
     * a dump can print it: it was a pass over every vertex of every draw.
     * A draw from the vertex cache has no converted vertices to pass over,
     * and is listed without one. */
    {
        uint32_t k = g_ring_n % DRAW_RING, v;
        float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f;
        static int want = -1;
        if (want < 0) want = getenv("RECOMP_GL_DUMP_FB") ? 1 : 0;
        if (want && !vc_vb)
        for (v = 0; v < out_n; v++) {
            float x = vbuf[(size_t)v * stride_floats + 0];
            float y = vbuf[(size_t)v * stride_floats + 1];
            if (x < mnx) mnx = x; if (x > mxx) mxx = x;
            if (y < mny) mny = y; if (y > mxy) mxy = y;
        }
        g_ring[k].draw = S.draws;   g_ring[k].verts = out_n;
        g_ring[k].prim = S.prim;
        g_ring[k].tex0_off = S.tex[0].offset;
        g_ring[k].tex0_w = S.tex[0].width; g_ring[k].tex0_h = S.tex[0].height;
        g_ring[k].blend = S.blend_enable;  g_ring[k].src = S.blend_src;
        g_ring[k].dst = S.blend_dst;       g_ring[k].depth_test = S.depth_test;
        g_ring[k].depth_mask = S.depth_mask;
        g_ring[k].alpha_test = S.alpha_test;
        g_ring[k].stages = S.psh.control & 0xF;
        g_ring[k].minx = mnx; g_ring[k].maxx = mxx;
        g_ring[k].miny = mny; g_ring[k].maxy = mxy;
        g_ring_n++;
    }
    S.draws++;
    /* RECOMP_GL_LITLOG=<s>: from s seconds after the renderer started, the
     * lighting state of each lit fixed-function draw whenever it changes
     * (first 80 changes; directions and matrices are shown, not compared). */
    { static int on = -1, n; static float last[64]; static double from, t0;
      if (on < 0) { const char *v = getenv("RECOMP_GL_LITLOG");
                    on = v ? 1 : 0; from = v ? atof(v) : 0.0; t0 = mono_now(); }
      if (on && n < 80 && S.xf_mode == NV2A_XF_MODE_FIXED && S.ff_lighting
          && mono_now() - t0 >= from) {
          float cur[64]; int k = 0, li;
          memset(cur, 0, sizeof cur);
          cur[k++] = (float)S.light_enable_mask; cur[k++] = (float)S.color_material;
          cur[k++] = (float)S.light_control; cur[k++] = (float)S.specular_enable;
          cur[k++] = (float)S.normalization;
          memcpy(&cur[k], S.scene_ambient, 12); k += 3;
          memcpy(&cur[k], S.material_emission, 12); k += 3;
          cur[k++] = S.material_alpha;
          for (li = 0; li < 2; li++) {
              memcpy(&cur[k], S.light_amb[li], 12); k += 3;
              memcpy(&cur[k], S.light_dif[li], 12); k += 3;
              memcpy(&cur[k], S.light_spc[li], 12); k += 3;
          }
          cur[k++] = (float)out_n; cur[k++] = (float)S.tex[0].enabled;
          cur[k++] = (float)S.attr[2].size; cur[k++] = (float)S.attr[3].size;
          if (memcmp(cur, last, sizeof cur)) {
              memcpy(last, cur, sizeof cur); n++;
              fprintf(stderr, "[LIT] draw %u flip %u verts %u: mask %08X colmat %08X ctl %08X spec %u norm %u | "
                      "sceneAmb %.3f %.3f %.3f matEm %.3f %.3f %.3f matA %.3f | normal %s diffuse %s tex0 %d\n",
                      S.draws, S.flips, out_n, S.light_enable_mask, S.color_material, S.light_control,
                      S.specular_enable, S.normalization,
                      S.scene_ambient[0], S.scene_ambient[1], S.scene_ambient[2],
                      S.material_emission[0], S.material_emission[1], S.material_emission[2], S.material_alpha,
                      S.attr[2].size ? "yes" : "no", S.attr[3].size ? "yes" : "no", S.tex[0].enabled);
              for (li = 0; li < 8; li++) {
                  uint32_t mode = (S.light_enable_mask >> (2 * li)) & 3;
                  if (!mode) continue;
                  fprintf(stderr, "[LIT]   light%d mode %u amb %.3f %.3f %.3f dif %.3f %.3f %.3f spc %.3f %.3f %.3f"
                          " dir %.3f %.3f %.3f half %.3f %.3f %.3f pos %.1f %.1f %.1f att %.3f %.3f %.3f range %g\n",
                          li, mode, S.light_amb[li][0], S.light_amb[li][1], S.light_amb[li][2],
                          S.light_dif[li][0], S.light_dif[li][1], S.light_dif[li][2],
                          S.light_spc[li][0], S.light_spc[li][1], S.light_spc[li][2],
                          S.light_dir[li][0], S.light_dir[li][1], S.light_dir[li][2],
                          S.light_half[li][0], S.light_half[li][1], S.light_half[li][2],
                          S.light_pos[li][0], S.light_pos[li][1], S.light_pos[li][2],
                          S.light_att[li][0], S.light_att[li][1], S.light_att[li][2], S.light_range[li]);
              }
              fprintf(stderr, "[LIT]   mv0 %.3f %.3f %.3f %.3f | imv0 %.3f %.3f %.3f %.3f | specpar %.3f %.3f %.3f %.3f %.3f %.3f\n",
                      S.ff_mv[0][0], S.ff_mv[0][1], S.ff_mv[0][2], S.ff_mv[0][3],
                      S.ff_imv[0][0], S.ff_imv[0][1], S.ff_imv[0][2], S.ff_imv[0][3],
                      S.specular_params[0], S.specular_params[1], S.specular_params[2],
                      S.specular_params[3], S.specular_params[4], S.specular_params[5]);
          }
      } }
    /* RECOMP_GL_FOGLOG=1: each distinct fog state draws run with, the first
     * time one does (up to 256 of them) -- enable, mode, how the distance is
     * made, colour, the three parameters, the plane, and which pipeline
     * drew. Distinct states rather than changes: this title switches fog on
     * and off between neighbouring batches, and a log of changes was used up
     * on the first level frames. */
    { static int on = -1, n; static uint32_t seen[256][12];
      if (on < 0) on = getenv("RECOMP_GL_FOGLOG") ? 1 : 0;
      if (on && n < 256) {
          uint32_t cur[12]; int k;
          cur[0] = S.fog_enable; cur[1] = S.fog_mode; cur[2] = S.fog_gen_mode; cur[3] = S.fog_color;
          memcpy(&cur[4], S.fog_params, 12); memcpy(&cur[7], S.fog_plane, 16);
          cur[11] = S.xf_mode == NV2A_XF_MODE_FIXED;
          for (k = 0; k < n; k++) if (!memcmp(cur, seen[k], sizeof cur)) break;
          if (k == n) {
              memcpy(seen[n++], cur, sizeof cur);
              fprintf(stderr, "[FOG] draw %u flip %u: enable %u mode %04X gen %u colour %08X params %g %g %g"
                      " plane %g %g %g %g | %s\n",
                      S.draws, S.flips, S.fog_enable, S.fog_mode, S.fog_gen_mode, S.fog_color,
                      S.fog_params[0], S.fog_params[1], S.fog_params[2],
                      S.fog_plane[0], S.fog_plane[1], S.fog_plane[2], S.fog_plane[3],
                      S.xf_mode == NV2A_XF_MODE_FIXED ? "fixed-function" : "program");
          }
      } }
    /* The title alternates viewports within a frame: a half-pixel offset for
     * its screen-space passes, a centred one for the scene. Counting the
     * centred ones is the cheapest "are we in 3D yet" signal there is, and it
     * is what the frame dumper triggers on -- guessing a flip number for a
     * phase that arrives minutes in wastes a whole run per guess. */
    if (S.u.vp_off[0] > 100.0f) S.draws_3d++;
    S.tris += out_n / 3;
    /* Which pipeline is drawing the picture.
     *
     * Displacing the programmable geometry made the city two and a half times
     * brighter, which only makes sense if the programmable path is drawing
     * more of the scene than the traffic and the interface. Counting settles
     * it: "the characters are programmable and the scenery is fixed-function"
     * is an assumption, and this is the first line that checks it. */
    /*
     * RECOMP_GL_INTERP=1: run the title's own vertex program on the CPU and
     * compare with where the shader put the vertex.
     *
     * Everything cheap has been eliminated. What remains is that either the
     * translation of this program is wrong or the constants it reads are, and
     * those two produce the same picture. This interpreter is written from the
     * decoded instruction stream rather than from the generated shader, so a
     * mistake would have to be made twice, independently; if it agrees with
     * what is on screen, the translation is right and the data is wrong.
     */
    { static int on = -1; static int shown;
      if (on < 0) on = getenv("RECOMP_GL_INTERP") ? 1 : 0;
      if (on && shown < 6 && out_n >= 3 && S.xf_mode != NV2A_XF_MODE_FIXED) {
          Nv2aVshProgram vp;
          uint32_t start = S.prog_start < NV2A_VSH_MAX_INSNS ? S.prog_start : 0;
          int len = nv2a_vsh_decode(S.prog + start * 4,
                                    (int)(NV2A_VSH_MAX_INSNS - start), &vp);
          if (len > 0 && (vp.outputs_written & 1u)) {
              float vin[16][4], pos[4];
              int a, cmp;
              memset(vin, 0, sizeof vin);
              for (a = 0; a < NUM_ATTRS; a++) {
                  if (attr_at[a] >= 0)
                      memcpy(vin[a], &vbuf[attr_at[a]], 4 * sizeof(float));
                  else
                      memcpy(vin[a], S.const_attr[a], 4 * sizeof(float));
              }
              cmp = nv2a_vsh_interp(&vp, vin, S.u.c, pos);
              if (cmp) {
                  float sx = pos[3] != 0.0f ? pos[0] / pos[3] : pos[0];
                  float sy = pos[3] != 0.0f ? pos[1] / pos[3] : pos[1];
                  float sz = pos[3] != 0.0f ? pos[2] / pos[3] : pos[2];
                  shown++;
                  if (shown == 1) {
                      char dis[4096];
                      int nz = 0, q;
                      if (nv2a_vsh_disasm(&vp, dis, sizeof dis))
                          fprintf(stderr, "  [INTERP] the program:\n%s", dis);
                      for (q = 0; q < NV2A_VSH_NUM_CONSTS; q++)
                          if (S.u.c[q][0] || S.u.c[q][1]
                           || S.u.c[q][2] || S.u.c[q][3]) nz++;
                      fprintf(stderr, "  [INTERP] %d non-zero constants; "
                              "c[0]=(%.3f %.3f %.3f %.3f)\n", nz,
                              S.u.c[0][0], S.u.c[0][1],
                              S.u.c[0][2], S.u.c[0][3]);
                  }
                  fprintf(stderr, "  [INTERP] %d-instruction program, "
                          "v0=(%.1f %.1f %.1f %.1f) -> oPos=(%.1f %.1f %.1f "
                          "%.4f)  /w=(%.1f %.1f %.1f)  verts=%u\n",
                          len, vin[0][0], vin[0][1], vin[0][2], vin[0][3],
                          pos[0], pos[1], pos[2], pos[3], sx, sy, sz, out_n);
                  fflush(stderr);
              }
          }
      } }

    /* RECOMP_GL_ATTRFMT=1: which vertex formats each pipeline uses.
     *
     * A position decoded in the wrong format is displaced rather than
     * destroyed, which is exactly what the traffic and the characters look
     * like -- and they are the programmable pipeline's geometry. If they are
     * arriving in a format this decoder guesses at, that is the bug; if they
     * are plain floats, it is not. */
    { static int on = -1;
      static uint32_t seen[2][8];
      static uint32_t n;
      if (on < 0) on = getenv("RECOMP_GL_ATTRFMT") ? 1 : 0;
      if (on) {
          int pipe = (S.xf_mode == NV2A_XF_MODE_FIXED) ? 0 : 1, j;
          for (j = 0; j < nlive; j++)
              if (live[j].idx == 0 && live[j].type < 8)
                  seen[pipe][live[j].type]++;
          if (++n % 200000 == 0) {
              static const char *nm[8] = { "D3DCOLOR", "short-normalised",
                  "float", "?3", "ubyte-normalised", "short", "packed-11/11/10",
                  "?7" };
              int k, q;
              for (k = 0; k < 2; k++) {
                  fprintf(stderr, "  [ATTR] position format, %s pipeline:",
                          k ? "programmable" : "fixed-function");
                  for (q = 0; q < 8; q++)
                      if (seen[k][q]) fprintf(stderr, "  %s x%u", nm[q], seen[k][q]);
                  fprintf(stderr, "\n");
              }
              fflush(stderr);
          }
      } }
    if (S.xf_mode == NV2A_XF_MODE_FIXED) { S.tris_ff += out_n / 3;
                                           if (S.ff_lighting) S.tris_ff_lit += out_n / 3; }
    else                                 S.tris_pm += out_n / 3;
}

/* ---- clear and present -------------------------------------------------- */

static void present(int is_frame);
/* RECOMP_GL_FLIPLOG=<n>: what each present is actually for.
 *
 * present() runs at the end of a render pass, not at the end of a displayed
 * frame -- a title that builds shadows, reflections and a UI layer into
 * separate targets ends several passes per frame, and each one looked like a
 * flip. That is why capping the frame rate stretched this title's boot by six
 * times: the cap was counting passes. This says which is which. */
static void fliplog(const char *why);

static void do_clear(uint32_t param)
{
    unsigned mask = 0;
    float rgba[4] = { 0, 0, 0, 1 };
    float depth = 1.0f;
    uint32_t rect[4], *rectp = NULL;

    if (!g_ready) return;
    surface_sync();

    /* A clear that covers the surface ends the previous frame.
     *
     * Which flip method a title uses to mark the end of a frame varies -- some
     * stall at the end, some at the start of the next one -- so keying the
     * capture to FLIP_STALL alone loses the frame on any title that stalls
     * first and then clears. The clear itself is unambiguous: whatever was on
     * the surface a moment ago was the finished frame, and this is the last
     * instant it exists. Capture it, then wipe. */
    if ((param & 0xF0) && S.since_present) {
        fliplog("clear");
        /* Not a displayed frame, unless this title never stalls -- see the
         * note on S.seen_flip_stall. */
        present(!S.seen_flip_stall);
        S.since_present = 0;
    }
    if (param & 0xF0) {
        float a, r, g, b;
        /*
         * The clear value is in the SURFACE's format, not in ARGB8888.
         *
         * JSRF renders into a 16-bit R5G6B5 surface, and clears it to white
         * by writing 0x0000FFFF -- sixteen bits of white in the low half.
         * Read as ARGB8888 that is (0, 255, 255): cyan. The Smilebit logo
         * screen, which is white on the console, came out on a cyan field,
         * and every fade from it scaled that cyan rather than white.
         *
         * Nothing about it looks like a bug in the frame: a solid, smooth,
         * correctly-fading colour is exactly what a working clear produces.
         */
        switch (S.surf_format & 0xF) {
        case 1: case 2: {          /* X1R5G5B5 -- 5 bits each, no alpha */
            uint32_t v = S.clear_color & 0xFFFFu;
            r = ((v >> 10) & 0x1F) / 31.0f;
            g = ((v >> 5)  & 0x1F) / 31.0f;
            b = ( v        & 0x1F) / 31.0f;
            a = 1.0f;
            break;
        }
        case 3: {                  /* R5G6B5 -- green gets the spare bit */
            uint32_t v = S.clear_color & 0xFFFFu;
            r = ((v >> 11) & 0x1F) / 31.0f;
            g = ((v >> 5)  & 0x3F) / 63.0f;
            b = ( v        & 0x1F) / 31.0f;
            a = 1.0f;
            break;
        }
        default:                   /* the 32-bit formats */
            /* X8R8G8B8 has no alpha either: see surf_has_alpha. */
            a = surf_has_alpha(S.surf_format) ? ((S.clear_color >> 24) & 0xFF) / 255.0f : 1.0f;
            r = ((S.clear_color >> 16) & 0xFF) / 255.0f;
            g = ((S.clear_color >> 8)  & 0xFF) / 255.0f;
            b = ( S.clear_color        & 0xFF) / 255.0f;
            break;
        }
        rgba[0] = r; rgba[1] = g; rgba[2] = b; rgba[3] = a;
        mask |= 1u;
    }
    if (param & 0x01) {
        /* Z is the top 24 bits of the combined z/stencil clear value. */
        depth = (float)((double)(S.clear_zstencil >> 8) / 16777215.0);
        /* Reversed depth keeps 1 - z (zrev_start); done in double, so that
         * the far plane's 1 comes out exactly 0. */
        if (g_zrev) depth = (float)(1.0 - (double)(S.clear_zstencil >> 8) / 16777215.0);
        mask |= 2u;
    }
    /* NV097_CLEAR_SURFACE_STENCIL, bit 1, is its own request: this used to
     * clear depth for it too, so a stencil-only clear wiped the depth. */
    if (param & 0x02)
        mask |= 4u;
    if (!mask) return;

    /* A clear is bounded by the clear rectangle, not by the surface -- the
     * device applies it. An empty rectangle means the whole surface, which
     * the interface spells as no rectangle at all. In raster pixels: the
     * title's own, times the internal resolution.
     *
     * The rectangle's far corner is INCLUSIVE, as the console has it (xemu
     * clears xmax - xmin + 1 pixels): this title clears 0..639 x 0..479.
     * Taken as exclusive, the last column and the last row were never
     * cleared and kept whatever the target was made with -- the "1-pixel
     * dark line along the right edge" (and the bottom one, less seen), and
     * on Metal every clear was a drawn triangle instead of a free load
     * action, because none of them covered the whole target. */
    if (S.clear_x1 > S.clear_x0 && S.clear_y1 > S.clear_y0) {
        uint32_t sc = gl_scale();
        rect[0] = S.clear_x0 * sc;       rect[1] = S.clear_y0 * sc;
        rect[2] = (S.clear_x1 + 1) * sc; rect[3] = (S.clear_y1 + 1) * sc;
        rectp = rect;
    }
    if (!g_surf_lost)
        G->clear(mask, rgba, depth, S.clear_zstencil & 0xFFu, rectp);
    S.clears++;
}

/* Copy what GL drew into the title's own surface, so the rest of the runtime
 * -- the framebuffer dump, the presentation window -- sees a finished frame
 * without knowing a GPU was involved. */
/* Monotonic seconds, as a double. */
static double mono_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void fliplog(const char *why)
{
    static int budget = -1;
    static uint32_t prev_draws;
    if (budget < 0) {
        const char *e = getenv("RECOMP_GL_FLIPLOG");
        budget = e ? atoi(e) : 0;
    }
    /* RECOMP_GL_FLIPLOG_AFTER=<flips>: start counting the budget only once
     * this many presents have gone by -- the title scene is fifteen
     * thousand presents into a run. */
    { static long after = -1;
      if (after < 0) { const char *a = getenv("RECOMP_GL_FLIPLOG_AFTER"); after = a ? atol(a) : 0; }
      if ((long)S.flips < after) { prev_draws = S.draws; return; } }
    if (budget <= 0) { prev_draws = S.draws; return; }
    budget--;
    fprintf(stderr, "  [FLIP] %-14s flip=%u surface=%08X %ux%u  draws=%u  fbo=%u\n",
            why, S.flips, S.color_offset, S.clip_w, S.clip_h, S.draws - prev_draws, (unsigned)g_cur_rt);
    prev_draws = S.draws;
    fflush(stderr);
}

/*
 * Frame pacing.
 *
 * The console's frame loop is bounded by the display: a title swaps and
 * waits. Nothing here made it wait, so the guest was presenting sixteen
 * hundred times a second -- twenty-seven times faster than it was written
 * to run. Everything paced by real time loses that race: streaming, the
 * loader, the music, and any thread that sleeps for a fixed number of
 * milliseconds expecting the world to have moved on. Some runs sat on the
 * loading screen forever with seven shaders compiled while the frame loop
 * spun; the loader was not stuck, it was outnumbered.
 *
 * RECOMP_FPS sets the rate; 0 removes the limit, which is what the
 * diagnostic runs want when they are trying to reach a late phase of a
 * title quickly.
 */
/* How the pacing went, for the report: frames that reached the flip after
 * their moment, and the time from one released frame to the next. */
static struct {
    uint64_t frames, late, restarts, iv[5], overslept, widened;
    double   late_max, last, overslept_max, ran_over_max, margin_max;
} g_pace_st;

/* Sleep for ts -- on Windows, for about as long as that.
 *
 * nanosleep there is winpthreads': it waits whole milliseconds, and after each
 * wait asks GetTickCount64, which moves in 15.6 ms steps, whether the time is
 * up. Measured on Windows 11 with the timer at 1 ms, the 1 ms slices below took
 * 2-3 ms on average and up to 17.5, where Linux takes 1.06: seven waits in ten
 * ended over half a millisecond past the frame's deadline, and the title's
 * vblank handler, which waits for this thread between slices, held the guest
 * lock up to a third of the time. A high-resolution waitable timer (Windows 10
 * 1803 on) wakes within a fraction of a millisecond; before that, a plain one
 * still does without the rounding. */
#if defined(_WIN32)
static void pace_sleep(const struct timespec *ts)
{
    static HANDLE timer;
    LARGE_INTEGER due;
    if (!timer)
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    due.QuadPart = -((LONGLONG)ts->tv_sec * 10000000 + ts->tv_nsec / 100);  /* relative */
    if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
        WaitForSingleObject(timer, INFINITE);
    else
        nanosleep(ts, NULL);
}
#else
#define pace_sleep(ts) nanosleep((ts), NULL)
#endif

/* How much of the wait before a frame is spun rather than slept: the part a
 * sleep cannot be trusted to deliver.
 *
 * 1.2 ms everywhere, and on Linux that spin -- the clock read in a loop -- was
 * measured at about 7% of a core on the GPU thread, every frame, where Linux's
 * sleeps barely needed it: one frame in 130,000 let go late, by half a
 * millisecond. So on desktop Linux each sleep goes to an absolute time, which
 * the slices before it cannot push later, and the spin starts 0.3 ms before
 * the frame's moment. A sleep that runs more than three quarters of the way
 * into the spin widens it at once, to what that sleep overran by and 0.2 ms
 * more (1.2 ms at most); frames on time narrow it back by 2 us each.
 * RECOMP_PACE_SPIN_US fixes it instead (1200: as before). Windows, macOS, iOS
 * and Android keep 1.2 ms. (From spriteice's measurements on a ROG Xbox Ally
 * X.) */
#if defined(__linux__) && !defined(__ANDROID__)
#include <errno.h>
#define PACE_ABSOLUTE 1
static double g_pace_margin = -1.0, g_pace_margin_min;
static int    g_pace_margin_fixed;

static double pace_margin(void)
{
    if (g_pace_margin < 0.0) {
        const char *v = getenv("RECOMP_PACE_SPIN_US");
        g_pace_margin_fixed = v != NULL;
        g_pace_margin_min = v ? atof(v) * 1e-6 : 0.0003;
        if (g_pace_margin_min < 0.0) g_pace_margin_min = 0.0;
        if (g_pace_margin_min > 0.005) g_pace_margin_min = 0.005;
        g_pace_margin = g_pace_margin_min;
        g_pace_st.margin_max = g_pace_margin;
    }
    return g_pace_margin;
}

/* The last sleep before the spin ended `over` past the spin's start. */
static void pace_margin_learn(double over)
{
    if (over > g_pace_st.ran_over_max) g_pace_st.ran_over_max = over;
    if (g_pace_margin_fixed) return;
    if (over > g_pace_margin * 0.75) {
        g_pace_margin = over + 0.0002 < 0.0012 ? over + 0.0002 : 0.0012;
        g_pace_st.widened++;
        if (g_pace_margin > g_pace_st.margin_max) g_pace_st.margin_max = g_pace_margin;
    } else if (g_pace_margin > g_pace_margin_min) {
        g_pace_margin -= 0.000002;
        if (g_pace_margin < g_pace_margin_min) g_pace_margin = g_pace_margin_min;
    }
}

/* Asleep until `until` on mono_now()'s clock. */
static void pace_sleep_until(double until)
{
    struct timespec ts;
    ts.tv_sec = (time_t)until;
    ts.tv_nsec = (long)((until - (double)ts.tv_sec) * 1e9);
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) { }
}
#endif

static void frame_pace(void)
{
        static double period = -1.0;
        static double deadline;
        double now;
        if (period < 0.0) {
            const char *v = getenv("RECOMP_FPS");
            double fps = v ? atof(v) : 60.0;
            period = (fps > 0.0) ? 1.0 / fps : 0.0;
        }
        /*
         * No cap while the title is not drawing a scene.
         *
         * The cap exists so that gameplay runs at the speed it was written
         * for. A logo screen and a loading screen have no gameplay in them --
         * and this title's loader is driven by its frame loop, so capping the
         * frame rate caps the loading with it. Measured on an M2 Pro: uncapped
         * the level arrives in about a hundred seconds; capped at sixty it had
         * not arrived after three minutes, and the guest had done six times
         * less work in the same wall time.
         *
         * A frame with a handful of draws in it is a logo or a loading screen.
         * A frame of this game is four hundred. The threshold does not have to
         * be precise because nothing sits between those two numbers.
         */
        {
            /*
             * Only until the first real scene, and then never again.
             *
             * The rule was "a frame with a handful of draws is a loading
             * screen", and a five-minute soak showed what is wrong with it:
             * when the attract loop dropped to a low-geometry screen the cap
             * disengaged and the emulator ran at six hundred frames a second.
             * A menu has as few draws as a loading screen, and a menu
             * animating ten times too fast is a worse bug than a slow load.
             *
             * Before the title has ever drawn a scene there is no gameplay to
             * run at the wrong speed, and that is the whole of the boot
             * sequence this needed to skip. After it, the cap is the cap.
             */
            static uint32_t prev_draws;
            static int seen_scene;
            uint32_t this_frame = S.draws - prev_draws;
            prev_draws = S.draws;
            if (this_frame >= 100) seen_scene = 1;
            if (!seen_scene) period = -period;        /* remembered, not lost */
            else if (period < 0.0) period = -period;
        }
        if (period > 0.0) {
            /*
             * Pace against a deadline that advances by exactly one period,
             * not against "one period from whenever the last sleep happened
             * to end".
             *
             * nanosleep only promises to sleep at least as long as asked, and
             * on this hardware it routinely overshoots by two or three
             * milliseconds. Restarting the clock afterwards folds every
             * overshoot into the next frame's budget, so a 16.7 ms period
             * became a 19.5 ms one and a 60 fps cap delivered 51. Advancing a
             * fixed deadline instead makes an overshoot cost only itself: the
             * next frame simply sleeps less.
             *
             * The last millisecond is spun rather than slept, because that is
             * the part the sleep cannot deliver accurately, and a millisecond
             * of one core is a cheaper way to hold 60 fps than missing it.
             * (On desktop Linux, less of it: see pace_margin.)
             */
            now = mono_now();
            if (deadline == 0.0 || deadline < now - 1.0) deadline = now;
#if defined(NV2A_SDL_PRESENTER)
            {
                /* In step with the display, when the presenter has measured
                 * one that runs at the game's rate or a whole multiple of it
                 * (pres_lock): its period, and each frame handed over half a
                 * refresh before the one that will show it -- as far as can
                 * be from either, so that each is shown exactly once. Eased
                 * towards, a fifth of the way each frame. */
                double ref = 0.0, refresh = 0.0, lp = pres_lock(period, &ref, &refresh);
                if (lp > 0.0) {
                    deadline += lp;
                    if (ref > 0.0 && now - ref < 0.25) {
                        double ph = fmod(deadline - ref, refresh);
                        if (ph < 0.0) ph += refresh;
                        deadline -= (ph - 0.5 * refresh) * 0.2;
                    }
                } else {
                    deadline += period;
                }
            }
#else
            deadline += period;
#endif
            g_pace_st.frames++;
            if (now > deadline) {
                double l = (now - deadline) * 1000.0;
                g_pace_st.late++;
                if (l > g_pace_st.late_max) g_pace_st.late_max = l;
            }
            if (now > deadline + period * 4.0) {
                g_pace_st.restarts++;
                /* Genuinely behind -- the emulation could not keep up. Start
                 * again from here rather than sprinting to catch up on frames
                 * whose moment has passed. */
                deadline = now + period;
            } else if (now < deadline) {
                int held = xbox_guest_lock_drop ? xbox_guest_lock_drop() : 0;
#if defined(PACE_ABSOLUTE)
                double spin = deadline - pace_margin();
                int slept = 0;
#else
                double spin = deadline - 0.0012;
#endif
                /* Answer the display interrupt while waiting: this thread is
                 * the one that completes its acknowledgement, and the title's
                 * handler holds the guest lock until it does -- see
                 * xbox_nv2a_vblank_service(). So the sleep is taken in slices
                 * of a millisecond with the interrupt serviced between them.
                 * RECOMP_PACE_VBLANK=0: one sleep, as before, for A/B. */
                static int service = -1;
                if (service < 0) {
                    const char *v = getenv("RECOMP_PACE_VBLANK");
                    service = (v ? atoi(v) : 1) && xbox_nv2a_vblank_service;
                }
                while (now < spin) {
#if defined(PACE_ABSOLUTE)
                    /* To an absolute time, the guest lock given up as below. */
                    double until = spin;
                    if (service && until - now > 0.001) until = now + 0.001;
                    pace_sleep_until(until);
                    slept = 1;
                    now = mono_now();
                    if (!service) break;
                    xbox_nv2a_vblank_service();
#else
                    struct timespec ts;
                    double wait = spin - now;
                    if (service && wait > 0.001) wait = 0.001;
                    ts.tv_sec = (time_t)wait;
                    ts.tv_nsec = (long)((wait - (double)ts.tv_sec) * 1e9);
                    /* Wait the way the console waits: with the rest of the
                     * guest still running. This is guest code holding the lock
                     * that serialises guest threads, so sleeping here without
                     * giving it up stops the loader and the sound thread too. */
                    pace_sleep(&ts);
                    if (!service) break;
                    xbox_nv2a_vblank_service();
                    now = mono_now();
#endif
                }
#if defined(PACE_ABSOLUTE)
                if (slept) pace_margin_learn(now - spin);
#endif
                while ((now = mono_now()) < deadline) { if (service) xbox_nv2a_vblank_service(); }
                /* A sleep that overran the deadline lets the frame go late;
                 * counted from half a millisecond. */
                if (now - deadline > 0.0005) {
                    g_pace_st.overslept++;
                    if (now - deadline > g_pace_st.overslept_max) g_pace_st.overslept_max = now - deadline;
                }
                if (held && xbox_guest_lock_retake) xbox_guest_lock_retake(held);
            }
            { double t = mono_now();
              if (g_pace_st.last > 0.0) {
                  double ms = (t - g_pace_st.last) * 1000.0;
                  g_pace_st.iv[ms < 14.0 ? 0 : ms < 19.5 ? 1 : ms < 30.0 ? 2 : ms < 45.0 ? 3 : 4]++;
                  if (ms >= period * 1500.0) {
                      g_ht.hitches++;
                      if (g_ht.logged < 200 || g_ht.hitches % 50 == 0) {
                          double pso = G->pipeline_ms ? G->pipeline_ms() - g_hf.pso_ms0 : 0.0;
                          double other = ms - 1000.0 * (g_hf.compile_s + g_hf.tex_s + g_hf.idle_s) - pso;
                          char pl[96] = "";
                          if (G->pipeline_ms) snprintf(pl, sizeof pl, ", pipelines %.1f ms", pso);
                          char gp[96] = "";
                          if (g_hf.idle_s > 0.0)
                              snprintf(pl + strlen(pl), sizeof pl - strlen(pl), ", %.1f ms building ahead",
                                       1000.0 * g_hf.idle_s);
                          /* Of the rest, the GPU's part: what it was busy with
                           * in the meantime (earlier frames' work included),
                           * and how long the render thread waited for it. */
                          if (G->gpu_ms) {
                              double busy = 0.0, waited = 0.0;
                              G->gpu_ms(&busy, &waited);
                              snprintf(gp, sizeof gp, " (GPU busy %.1f ms, waited for %.1f)",
                                       busy - g_hf.gpu_busy0, waited - g_hf.gpu_wait0);
                          }
                          g_ht.logged++;
                          fprintf(stderr, "[HITCH] frame %u: %.1f ms flip to flip, %u draws | "
                                  "%u programs built in %.1f ms (worst %.1f)%s | %u textures new, "
                                  "%.1f ms in textures | %.1f ms the rest%s | %u of the new "
                                  "textures DXT as they are\n",
                                  S.frames, ms, S.draws - g_hf.draws0, g_hf.compiles,
                                  1000.0 * g_hf.compile_s, 1000.0 * g_hf.compile_worst, pl,
                                  g_hf.tex_new, 1000.0 * g_hf.tex_s, other < 0.0 ? 0.0 : other, gp,
                                  g_hf.tex_dxt);
                      }
                  }
              }
              g_pace_st.last = t;
              memset(&g_hf, 0, sizeof g_hf);
              g_hf.draws0 = S.draws;
              if (G->pipeline_ms) g_hf.pso_ms0 = G->pipeline_ms();
              if (G->gpu_ms) G->gpu_ms(&g_hf.gpu_busy0, &g_hf.gpu_wait0); }
        }
}

/* A frame with little in it -- a logo, a loading screen, a menu -- is where
 * the shader cache builds what earlier runs needed (cache_idle), a few
 * milliseconds at a time, so that the game finds it built. It is done in the
 * frame's slack, before frame_pace waits out the rest of the period, and with
 * the guest lock given up as frame_pace gives it up, so that a loader running
 * behind a loading screen carries on meanwhile. */
static void cache_idle_frame(void)
{
    static uint32_t last_draws;
    static int more = 1;
    uint32_t nd = S.draws - last_draws;
    last_draws = S.draws;
    if (!more || !G || !G->cache_idle) return;
    if (nd < 150) {
        double t0 = mono_now();
        int held = xbox_guest_lock_drop ? xbox_guest_lock_drop() : 0;
        more = G->cache_idle(6.0) > 0;
        if (held && xbox_guest_lock_retake) xbox_guest_lock_retake(held);
        g_hf.idle_s += mono_now() - t0;
    }
}

static void present(int is_frame)
{
    uint32_t va, bpp, x, y;
    uint8_t *dst;

    if (!g_ready || !g_fbo_w || !g_fbo_h || !S.color_offset) return;
#ifdef NV2A_GL_HAVE_GL
    /* The frame's GL errors, asked once (see after the draw). */
    if (is_frame && g_is_gl && g_gl_err_reports < 8 && !gl_err_per_draw()) {
        GLenum e = glGetError();
        if (e != GL_NO_ERROR) {
            fprintf(stderr, "  [GL] glGetError 0x%04X during frame %u (%u draws; "
                    "RECOMP_GL_ERRORS_PER_DRAW=1 names the draw)\n",
                    e, S.frames, S.since_present);
            g_gl_err_reports++;
        }
    }
#endif
    if (is_frame) { S.frames++; g_frame_clock = mono_now();
                    prof_report(); tcache_sweep(); tcache_verify();
                    /* the census counts one frame, not everything since the
                     * last dump -- otherwise it measures the gap, not the
                     * picture next to it */
                    memcpy(g_slot_prev, g_slot_draws, sizeof g_slot_prev);
                    memset(g_slot_draws, 0, sizeof g_slot_draws);
                    g_frame_chars_prev = g_frame_chars; g_frame_chars = 0; }

    /* RECOMP_PACE=1: one line a second saying how the boot is progressing.
     *
     * The question it answers is whether this title's start-up is counted in
     * frames or in seconds. If capping the frame rate stretches the boot in
     * proportion, the loader is being driven by the frame loop; if the boot
     * takes the same wall time either way, it is on a clock and the cap costs
     * nothing. Guessing was cheaper than measuring exactly once. */
    {
        static int pace = -1;
        static struct timespec t0, tprev;
        static uint32_t fprev, dprev;
        static unsigned long vprev;
        if (pace < 0) {
            pace = getenv("RECOMP_PACE") ? 1 : 0;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            tprev = t0;
        }
        if (pace) {
            struct timespec now;
            double dt, since;
            clock_gettime(CLOCK_MONOTONIC, &now);
            dt = (double)(now.tv_sec - tprev.tv_sec)
               + (double)(now.tv_nsec - tprev.tv_nsec) * 1e-9;
            if (dt >= 1.0) {
                since = (double)(now.tv_sec - t0.tv_sec)
                      + (double)(now.tv_nsec - t0.tv_nsec) * 1e-9;
                fprintf(stderr, "[PACE] t=%6.1fs frames=%-7u (%5.1f/s) "
                                "draws=%-8u (%6.0f/s) vbl=%-7lu (%5.1f/s)\n",
                        since, S.frames, (double)(S.frames - fprev) / dt,
                        S.draws, (double)(S.draws - dprev) / dt,
                        (&g_vblank_count ? g_vblank_count : 0UL),
                        (&g_vblank_count
                         ? (double)(g_vblank_count - vprev) / dt : 0.0));
                vprev = (&g_vblank_count ? g_vblank_count : 0UL);
                fflush(stderr);
                tprev = now; fprev = S.frames; dprev = S.draws;
            }
        }
    }

    if (is_frame) { PROF_T0(); cache_idle_frame(); frame_pace(); PROF_ADD(pace); }

    /* Anti-aliasing turned on, off or changed since the last flip: between
     * frames, before this one is shown. */
    if (is_frame) { smaa_take(); msaa_take(); }

    /* Put the finished frame on screen: the surface the title has set up
     * when it flips, which is what the window showed while surfaces were
     * bound as their methods arrived (JSRF: the display buffer it copies
     * each frame into). Surfaces are bound when used now, and the last one
     * used at the flip is the off-screen frame instead. */
    if (is_frame) {
        PROF_T0();
        uint32_t prt = g_cur_rt;
        uint32_t pw = g_fbo_pw ? g_fbo_pw : g_fbo_w;
        uint32_t ph = g_fbo_ph ? g_fbo_ph : g_fbo_h;
        if (g_present_slot >= 0 && g_present_slot < SURF_CACHE && g_surf[g_present_slot].used) {
            prt = g_surf[g_present_slot].rt;
            pw = g_surf[g_present_slot].w * gl_scale();
            ph = g_surf[g_present_slot].h * gl_scale();
        }
        /* With SMAA, what is shown is the copy of it with its edges smoothed. */
        if (g_smaa_now && G->target_smaa) prt = G->target_smaa(prt);
        g_shown_rt = prt;
#if defined(NV2A_GL_USE_CGL)
        /* OpenGL on a Mac: read the finished frame and hand it to the window.
         *
         * Blitting straight into the window's back buffer works -- measurably,
         * the pixels arrive -- and a layer-backed view never presents what
         * another thread drew into its context. So the frame crosses as
         * pixels and the main thread draws it. One readback a frame. (Metal
         * draws into the window's layer itself, in present() below.) */
        if (g_windowed && g_is_gl) {
            static uint8_t *shot;
            static uint32_t cap;
            uint32_t need = pw * ph * 4;
            if (cap < need) { free(shot); shot = (uint8_t *)malloc(need); cap = need; }
            /* At the frame's real size: the window is happy to show more
             * pixels than the title thinks it drew. */
            if (shot && G->target_read(prt, 0, 0, pw, ph, 1, shot))
                nv_window_submit_frame(shot, (int)pw, (int)ph);
            nv_window_present();
        }
#elif defined(NV2A_GL_USE_SDL)
        if (g_window) {
#if defined(NV2A_SDL_PRESENTER)
            if (g_presenter == 1) sdl_hand_over(nv2a_gfx_gl_fbo(prt), (int)pw, (int)ph);
            else
#endif
            sdl_present(nv2a_gfx_gl_fbo(prt), (int)pw, (int)ph);
            sdl_pump();
        }
#endif
        /* The device's end of the frame: Metal shows it in the window's
         * layer, and either way the frame's work is handed to the GPU. */
        G->present(prt, pw, ph);
#if defined(NV2A_GL_USE_CGL)
        if (g_windowed && nv_window_should_close()) {
            fprintf(stderr, "  [GL] window closed; stopping\n");
            nv2a_gl_report();
            exit(0);
        }
#endif
        PROF_ADD(present);
    }

    va = resolve(S.color_offset);
    bpp = (S.surf_pitch && S.clip_w) ? S.surf_pitch / S.clip_w : 4;
    if (bpp != 2 && bpp != 4) return;
    if (!g_readback) return;

    /* Reading the frame back into guest memory costs a full-surface transfer
     * and a pipeline stall, every frame. That is the price of letting the
     * rest of the runtime see a finished frame without knowing about GL, and
     * it is worth paying only when something will look at it: with it on, this
     * title managed 133k draws in two minutes, and with it off, 295k.
     *
     * RECOMP_GL_READBACK=n copies every nth frame (0 = never). A capture is
     * scheduled separately and forces a copy for that frame alone, so arming
     * one no longer costs the whole run its speed.
     */
    {
        static int every = -1, dump_shots;
        static const char *pfx;
        static uint32_t dump_after, dump_every, dump_3d, dump_after_tris;
        int dump_due, chars_ok = 1;

        if (every < 0) {
            const char *v = getenv("RECOMP_GL_READBACK");
            const char *a = getenv("RECOMP_GL_DUMP_AFTER");
            const char *e = getenv("RECOMP_GL_DUMP_EVERY");
            const char *t = getenv("RECOMP_GL_DUMP_3D");
            every = v ? atoi(v) : 1;
            if (every < 0) every = 0;
            pfx = getenv("RECOMP_GL_DUMP_FB");
            dump_after = a ? (uint32_t)strtoul(a, 0, 0) : 0;
            dump_every = e ? (uint32_t)strtoul(e, 0, 0) : 90;
            dump_3d = t ? (uint32_t)strtoul(t, 0, 0) : 0;
            if (!dump_every) dump_every = 90;
        }
        /* RECOMP_GL_DUMP_TRIS=<n>: wait for a frame that actually has a scene
         * in it. A logo screen is a handful of quads; a level is thousands of
         * triangles, so triangles-per-frame separates the two far better than
         * a flip count guessed in advance. */
        { static uint32_t prev_tris; static const char *tinit;
          if (!tinit) { tinit = getenv("RECOMP_GL_DUMP_TRIS"); tinit = tinit ? tinit : ""; }
          S.frame_tris = S.tris - prev_tris;
          prev_tris = S.tris;
          if (*tinit) dump_after_tris = (uint32_t)strtoul(tinit, 0, 0); }

        /* RECOMP_GL_DARKHUNT_DROP=1: hunt the frame where the picture
         * actually goes dark, instead of one named in advance.
         *
         * Aiming the per-draw hunt by hand needs a frame number, and the only
         * way to get one is a previous run's screenshots -- which are numbered
         * by presents, on a timeline that moves the moment anything changes.
         * Two attempts at this landed on bright frames. The frame worth
         * hunting identifies itself perfectly well: it is the one after the
         * mean brightness falls off a cliff. One 64x48 readback per frame is
         * cheap enough to leave on for a whole run, and the fade lasts long
         * enough that the following frame is still dark, so the per-draw hunt
         * gets a frame in which the darkening is still happening. */
        { static int drop = -1; static double prev_mean = -1.0; static int armed;
          static int drop_tris = -1;
          if (drop < 0) drop = getenv("RECOMP_GL_DARKHUNT_DROP") ? 1 : 0;
          /* Only frames with a scene in them.
           *
           * The first attempt at this armed at present 1095, on a frame with
           * THREE triangles: the boot sequence flashes white and then black,
           * which is a fall from 255 to 0 and tells us nothing. A level frame
           * is tens of thousands of triangles, so requiring a real scene on
           * both sides of the fall removes every logo, flash and fade between
           * menus without needing to know where any of them are. */
          if (drop_tris < 0) { const char *v = getenv("RECOMP_GL_DARKHUNT_TRIS");
                               drop_tris = v ? atoi(v) : 5000; }
          if (drop && !armed && S.frame_tris >= (uint32_t)drop_tris) {
              static uint8_t strip[64 * 48 * 4];
              double m = 0.0; int q;
              G->target_read(g_cur_rt, 0, (g_fbo_ph ? g_fbo_ph : g_fbo_h) / 2,
                             64, 48, 0, strip);
              for (q = 0; q < 64 * 48; q++)
                  m += strip[q*4] + strip[q*4+1] + strip[q*4+2];
              m /= (64.0 * 48.0 * 3.0);
              if (prev_mean >= 0.0)
                  fprintf(stderr, "  [DARK] present %u frame %u mean %.1f"
                                  " (was %.1f) tris %u\n",
                          S.flips, S.frames, m, prev_mean, S.frame_tris);
              if (prev_mean > 25.0 && m < prev_mean * 0.45) {
                  armed = 1;
                  fprintf(stderr, "  [DARK] the picture fell from %.1f to %.1f"
                                  " at present %u -- hunting the next frame\n",
                          prev_mean, m, S.flips);
                  { extern int g_darkhunt_at; g_darkhunt_at = (int)S.frames + 1; }
              }
              prev_mean = m;
          } }

        draw_log_latch();
        /* RECOMP_GL_DUMP_CHARS=<n>: only frames with at least n big skinned
         * batches in them, which is what a scene with characters in it looks
         * like and a menu or loading screen never does. */
        { static int cinit; static uint32_t want_chars;
          if (!cinit) { const char *v = getenv("RECOMP_GL_DUMP_CHARS");
                        want_chars = v ? (uint32_t)strtoul(v, 0, 0) : 0;
                        cinit = 1; }
          chars_ok = !want_chars || g_frame_chars_prev >= want_chars; }
        if (g_dump_burst_left > 0 && pfx && is_frame
         && (S.frames % (uint32_t)g_dump_burst_every) == 0 && dump_shots < 72) {
            g_dump_burst_left--;
            dump_due = 1;
        } else
        dump_due = pfx && dump_shots < dump_max() && chars_ok
                && (dump_after_tris ? S.frame_tris >= dump_after_tris
                    : dump_3d ? S.draws_3d >= dump_3d
                    : S.flips >= dump_after)
                && (S.flips % dump_every) == 0;
        /* RECOMP_GL_DUMP_SECS=<s>: none before s seconds into the run --
         * flips are no clock, a loading screen runs hundreds a second. */
        { static double t0 = -1.0, after_s = -1.0;
          if (t0 < 0.0) { const char *v = getenv("RECOMP_GL_DUMP_SECS");
                          t0 = mono_now(); after_s = v ? atof(v) : 0.0; }
          if (dump_due && after_s > 0.0 && mono_now() - t0 < after_s) dump_due = 0; }
        /* RECOMP_GL_DUMP_SHOWN: a shot that falls on the end of a pass rather
         * than of a frame is kept for the next frame. Flips count passes, and
         * a scene that ends k passes a frame puts its frames on one residue
         * mod k: an interval that k divides could fall on a pass every time,
         * and every shot was spent writing nothing. */
        { static int so = -1, owed;
          int shows = is_frame && g_present_slot >= 0 && g_surf[g_present_slot].used;
          if (so < 0) so = getenv("RECOMP_GL_DUMP_SHOWN") ? 1 : 0;
          if (so) {
              if (dump_due && !shows) { owed = 1; dump_due = 0; }
              else if (owed && shows && pfx && dump_shots < dump_max()) { owed = 0; dump_due = 1; }
          } }
        /* RECOMP_GL_DUMP_PAIRS=1: each dump and the frame straight after it --
         * with RECOMP_GL_TEXGEN=ab, the same moment with and without. */
        { static int pairs = -1, second;
          if (pairs < 0) pairs = getenv("RECOMP_GL_DUMP_PAIRS") ? 1 : 0;
          if (pairs && is_frame) {
              if (second && pfx && dump_shots < dump_max()) { dump_due = 1; second = 0; }
              else if (dump_due) second = 1;
          } }

        if (!dump_due && (!every || (S.flips % (uint32_t)every) != 0)) {
            S.flips++;
            S.since_present = 0;
            return;
        }
        if (dump_due) {
            g_world_dump_pending = 1;   /* RECOMP_GL_DUMP_WORLD: next frame's map */
            ring_dump();
            /* Every surface, not just the one that happens to be current: a
             * title that builds its frame off-screen and composites leaves
             * the picture somewhere other than where the capture is pointed,
             * and one image per surface says which. RECOMP_GL_DUMP_SHOWN=1:
             * only what the window is given, for looking at the picture
             * rather than at how it was made. */
            int k;
            static int shown_only = -1;
            if (shown_only < 0) shown_only = getenv("RECOMP_GL_DUMP_SHOWN") ? 1 : 0;
            for (k = 0; k < SURF_CACHE && !shown_only; k++) {
                char pf[256];
                static int per_surf[SURF_CACHE];
                if (!g_surf[k].used) continue;
                snprintf(pf, sizeof pf, "%s_s%d_", pfx, k);
                { uint32_t sw = g_fbo_w, sh = g_fbo_h, spw = g_fbo_pw, sph = g_fbo_ph;
                  g_fbo_w = g_surf[k].w; g_fbo_h = g_surf[k].h;
                  g_fbo_pw = g_surf[k].w * gl_scale(); g_fbo_ph = g_surf[k].h * gl_scale();
                  dump_fbo(g_surf[k].rt, pf, &per_surf[k], 72);
                  g_fbo_w = sw; g_fbo_h = sh; g_fbo_pw = spw; g_fbo_ph = sph; }
            }
            /* And the one the window shows at this flip, under its own name,
             * so a headless run can see what a player would. */
            if (is_frame && g_present_slot >= 0 && g_surf[g_present_slot].used) {
                char pf[256];
                static int shown_n;
                int k2 = g_present_slot;
                uint32_t sw = g_fbo_w, sh = g_fbo_h, spw = g_fbo_pw, sph = g_fbo_ph;
                snprintf(pf, sizeof pf, "%s_shown_", pfx);
                g_fbo_w = g_surf[k2].w; g_fbo_h = g_surf[k2].h;
                g_fbo_pw = g_surf[k2].w * gl_scale(); g_fbo_ph = g_surf[k2].h * gl_scale();
                dump_fbo(g_surf[k2].rt, pf, &shown_n, 72);
                /* And the picture the window was given, where anti-aliasing
                 * made it another one: the same frame, before and after. */
                if (g_shown_rt && g_shown_rt != g_surf[k2].rt) {
                    static int aa_n;
                    snprintf(pf, sizeof pf, "%s_shown_aa_", pfx);
                    dump_fbo(g_shown_rt, pf, &aa_n, 72);
                }
                g_fbo_w = sw; g_fbo_h = sh; g_fbo_pw = spw; g_fbo_ph = sph;
                fprintf(stderr, "  [GL] shown at this flip: slot %d = surface %08X %ux%u (frame %u, %d shown)\n",
                        k2, g_surf[k2].offset, g_surf[k2].w, g_surf[k2].h, S.frames, shown_n);
            }
            { char nm[256]; FILE *sf;
              snprintf(nm, sizeof nm, "%s_slots_%03d.txt",
                       pfx ? pfx : "dump", dump_shots);
              sf = fopen(nm, "w");
              if (sf) {
                  int s2; uint32_t tot = 0;
                  for (s2 = 0; s2 < SLOTCENSUS_MAX; s2++) tot += g_slot_prev[s2];
                  fprintf(sf, "# frame at present %u, %u draws this frame\n",
                          S.flips, tot);
                  for (s2 = 0; s2 < SLOTCENSUS_MAX; s2++)
                      if (g_slot_prev[s2])
                          fprintf(sf, "%d %u\n", s2, g_slot_prev[s2]);
                  fclose(sf);
              } }
            dump_shots++;
            /* dump_fbo did its own read; fall through so the guest surface
             * gets the same frame when a copy is also wanted. */
        }
    }

    dst = (uint8_t *)xbox_GetMemoryOffset() + va;
    for (y = 0; y < g_fbo_h; y++) {
        /* GL's origin is bottom-left and the console's is top-left. */
        const uint8_t *row = g_readback + (size_t)(g_fbo_h - 1 - y) * g_fbo_w * 4;
        uint8_t *out = dst + (size_t)y * S.surf_pitch;
        if (bpp == 4) {
            memcpy(out, row, (size_t)g_fbo_w * 4);
        } else {
            for (x = 0; x < g_fbo_w; x++) {
                uint32_t b = row[x * 4 + 0], g = row[x * 4 + 1], r = row[x * 4 + 2];
                ((uint16_t *)out)[x] = (uint16_t)(((r & 0xF8) << 8)
                                                | ((g & 0xFC) << 3)
                                                | (b >> 3));
            }
        }
    }
    S.flips++;
    S.since_present = 0;
}

/* ---- the state machine -------------------------------------------------- */


/*
 * A batch ends where the geometry's state changes, not only where Begin/End
 * does -- and that means the vertex ARRAYS as much as the constants.
 *
 * The console runs vertices through the transform as they arrive. Writing a
 * vertex program constant between two DRAW_ARRAYS inside one Begin/End is
 * therefore legal and meaningful: the ranges before the write are
 * transformed by the old matrix and the ranges after by the new one. That is
 * how this title puts many objects on screen inside one primitive block --
 * one upload and one range per object.
 *
 * This renderer buffered the whole block and drew it once, with whatever the
 * constants happened to be at the end. Every object in the block therefore
 * got the LAST object's matrix. The last one lands correctly and the rest
 * land wherever their neighbour was, which is not a wild error -- it is a
 * plausible position, a few metres out, which is exactly what "the people
 * and the buses are floating" looks like. Static world geometry is immune
 * because it shares one transform for the whole block, so the buildings and
 * the roads have been right all along, which is the split the player described.
 *
 * So: before new geometry joins a batch whose state has moved since the last
 * vertex arrived, draw what is already there. The check is at the geometry
 * end rather than at the write because a matrix is sixteen dwords and
 * splitting sixteen times per object would be its own bug.
 *
 * The constants turned out not to move mid-block in this title -- measured,
 * zero splits in two runs. The VERTEX ARRAYS are the real case, and the
 * evidence for them is much better than a hunch. SKINDBG catches draws whose
 * address register comes out at 943, or 662, or 80, when a bone index in
 * this title cannot exceed about twenty; the vertices that produce those
 * numbers decode to zeros or to bytes that are plainly not a unit normal,
 * while their neighbours a few source indices earlier decode perfectly.
 * That is an index reading past the end of the data it belongs to -- which
 * is what happens when the title re-points an array part way through a
 * block and every index in the block is then resolved against the last
 * pointer. The vertices belonging to the final chunk come out right and the
 * earlier ones read whatever lies past its end. A character whose vertices
 * pick a wrong bone matrix does not fall apart when they all pick the SAME
 * wrong one: it just moves, somewhere plausible, a few metres off. Which is
 * what a bus hanging in the air looks like.
 *
 * Strips and fans are not split: a strip's later triangles are built from
 * earlier vertices, so cutting one invents geometry that was never drawn.
 * Nothing observed here changes constants mid-strip, and a wrong picture is
 * worth less than a wrong picture that also disagrees with itself.
 */
static void ensure_ready(void);   /* defined just below */
static int gfx_init(uint32_t w, uint32_t h);

/*
 * Reversed depth: the depth buffer holds 1 - z, in 32-bit floating point --
 * floating point's precision where the far end of the scene needs it, which
 * the console's 24-bit z (and z kept as z in a float) puts at the near end.
 * The why is in nv2a_vsh.c (depth_defines); here, the parts that follow from
 * it: the comparisons turned around, clears to 1 - z, and for the
 * fixed-function batches the row that makes 1 - z from the position.
 *
 * On wherever the device can (Metal; OpenGL with glClipControl), unless
 * RECOMP_GL_ZREV=0. Decided once, before the first target and program.
 * (g_zrev is declared near the top.)
 */

static uint32_t zrev_func(uint32_t f)
{
    switch (f) {
    case 0x201: return 0x204;          /* LESS     -> GREATER */
    case 0x203: return 0x206;          /* LEQUAL   -> GEQUAL  */
    case 0x204: return 0x201;          /* GREATER  -> LESS    */
    case 0x206: return 0x203;          /* GEQUAL   -> LEQUAL  */
    default:    return f;              /* NEVER, EQUAL, NOTEQUAL, ALWAYS */
    }
}

/*
 * 1 - z, times w, as one row of the fixed-function transform.
 *
 * The epilogue's z is (p.z / p.w - z0) / (z1 - z0), with p the composite
 * matrix times the position and z0..z1 the screen-space range it maps to
 * 0..1 (CLIP_MIN/MAX, or the viewport's z offset and scale). So
 *
 *     (1 - z) * p.w = (z1 * p.w - p.z) / (z1 - z0)
 *                   = dot(position, (z1 * row_w - row_z) / (z1 - z0))
 *
 * -- a row worked out here in double precision, from the title's own
 * single-precision rows, and one dot product on the GPU. Computed the long
 * way there instead, 1 - z near the far plane would be the difference of two
 * numbers close to 1, and every bit the reversal was for would be gone.
 */
static void zrev_row(float out[4])
{
    double z0, z1;
    int k;
    if (S.u.z_clip[1] > S.u.z_clip[0]) {
        z0 = S.u.z_clip[0];
        z1 = S.u.z_clip[1];
    } else {
        double sc = S.u.vp_scale[2] != 0.0f ? (double)S.u.vp_scale[2] : 1.0;
        z0 = S.u.vp_off[2];
        z1 = z0 + sc;
    }
    for (k = 0; k < 4; k++)
        out[k] = (float)((z1 * (double)S.u.ff_mat[3][k] - (double)S.u.ff_mat[2][k]) / (z1 - z0));
}

static void zrev_start(void)
{
    const char *v = getenv("RECOMP_GL_ZREV");
    int want = v ? atoi(v) != 0 : 1;
    g_zrev = (want && G->zrev) ? (G->zrev(1) ? 1 : 0) : 0;
    nv2a_vsh_zrev = g_zrev;
    nv2a_vsh_gl_clip_z01 = g_zrev && G->lang == NV2A_LANG_GLSL;
    fprintf(stderr, "  [GL] depth: %s\n",
            g_zrev ? "reversed, 32-bit float"
                   : want ? "as the console keeps it (this device cannot reverse it)"
                          : "as the console keeps it (RECOMP_GL_ZREV=0)");
}

static void split_batch_if_state_moved(void)
{
    static int on = -1;
    if (on < 0) { const char *v = getenv("RECOMP_GL_MIDBATCH");
                  on = v ? atoi(v) : 1; }
    if (!on) return;
    if (!S.consts_since_geom && !S.arrays_since_geom) return;
    { int by_array = S.arrays_since_geom;
      S.consts_since_geom = 0;
      S.arrays_since_geom = 0;
      if (!S.prim || (S.idx_count == 0 && S.inline_count == 0)) return;
      if (by_array) S.midbatch_array_splits++; }
    /* 5 = triangle strip, 6 = triangle fan, 2 = line strip, 3 = line loop,
     * 9 = quad strip, 10 = polygon: all carry state between primitives. */
    if (S.prim == 2 || S.prim == 3 || S.prim == 5 || S.prim == 6
     || S.prim == 9 || S.prim == 10) return;
    ensure_ready();
    do_draw();
    S.idx_count = 0;
    S.inline_count = 0;
    S.draw_count = 0;
    S.midbatch_splits++;
}

static void ensure_ready(void)
{
    if (g_ready || g_failed) return;
    if (!S.clip_w || !S.clip_h) return;      /* wait for a surface */
    if (!gfx_init(S.clip_w, S.clip_h)) { g_failed = 1; return; }
    g_ready = 1;
    surface_bind(S.color_offset, S.clip_w, S.clip_h);
}

/*
 * RECOMP_GL_METHVAL=<s>: which VALUES the title writes to a set of methods
 * this backend ignores or only half-handles -- texture coordinate generation
 * (0x03C0..0x03FC per stage S/T/R/Q), stencil (0x032C..0x0378), polygon
 * offset (0x0330..0x0338, 0x0384/0x0388), point sprites (0x0318/0x031C,
 * 0x043C) and the texgen planes (0x0840..0x093C) -- each distinct value
 * with a count, printed every s seconds. The counts alone (RECOMP_GL_UNHANDLED)
 * cannot say whether a feature is ever switched ON, and that is the question.
 */
static void methval_note(uint32_t method, uint32_t param)
{
    enum { NM = 96, NV = 6 };
    static int on = -1; static double every, next;
    static uint32_t meth[NM], val[NM][NV], cnt[NM][NV]; static int nm, nv[NM];
    int i, j;
    if (on < 0) { const char *v = getenv("RECOMP_GL_METHVAL");
                  on = v ? 1 : 0; every = v ? atof(v) : 0.0; if (every <= 0) every = 10.0;
                  next = mono_now() + every; }
    if (!on) return;
    if (!((method >= 0x0318 && method <= 0x0390) || (method >= 0x03C0 && method < 0x0400)
          || method == 0x043C || (method >= 0x0840 && method < 0x0940))) return;
    for (i = 0; i < nm; i++) if (meth[i] == method) break;
    if (i == nm) { if (nm >= NM) return; meth[nm++] = method; }
    for (j = 0; j < nv[i]; j++) if (val[i][j] == param) break;
    if (j == nv[i]) { if (nv[i] >= NV) j = NV - 1; else { val[i][j] = param; nv[i]++; } }
    cnt[i][j]++;
    if (mono_now() >= next) {
        next = mono_now() + every;
        fprintf(stderr, "[METHVAL] draw %u:", S.draws);
        for (i = 0; i < nm; i++) {
            fprintf(stderr, " %04X{", meth[i]);
            for (j = 0; j < nv[i]; j++) fprintf(stderr, "%s%X:%u", j ? "," : "", val[i][j], cnt[i][j]);
            fprintf(stderr, "}");
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    }
}

/*
 * Immediate-mode vertices.
 *
 * Besides its vertex arrays, the NV2A takes vertices one attribute write at a
 * time between SET_BEGIN_END and its END: NV097_SET_VERTEX3F/4F and the
 * SET_VERTEX_DATA* methods write the current value of an attribute, and the
 * write that completes the POSITION (attribute 0) sends a vertex made of every
 * attribute's current value (xemu: pgraph_finish_inline_buffer_vertex). The
 * XDK's D3DDevice_Begin / SetVertexData4f / End is this path.
 *
 * Here those methods only ever set the constant an array-less attribute
 * reads, so a batch sent this way ended with no vertices and drew nothing --
 * among them, the one that darkens what this title's shadow volumes marked in
 * the stencil buffer: the volumes were drawn, the shadows never were.
 *
 * RECOMP_GL_IMMEDIATE=0: as before, nothing drawn.
 */
#define MAX_IMM 2048
static float    g_imm[MAX_IMM][NUM_ATTRS][4];
static uint32_t g_imm_n, g_imm_dropped;

static int imm_on(void)
{
    static int on = -1;
    if (on < 0) { const char *v = getenv("RECOMP_GL_IMMEDIATE"); on = v ? atoi(v) != 0 : 1; }
    return on;
}

/* The position is complete: a vertex, of every attribute's current value. */
static void imm_emit(void)
{
    if (!S.prim || !imm_on()) return;     /* outside Begin/End: a value, not a vertex */
    if (g_imm_n >= MAX_IMM) { g_imm_dropped++; return; }
    memcpy(g_imm[g_imm_n], S.const_attr, sizeof g_imm[0]);
    g_imm_n++;
}

static void imm_set(uint32_t a, int c, float v)
{
    S.const_attr[a][c] = v;
    S.const_set[a][c] = v;
}

/* 1 if `method` is one of the per-vertex attribute methods (handled here). */
static int imm_method(uint32_t method, uint32_t param)
{
    float f;
    uint32_t a, part;
    /* Every method of the pushbuffer passes through here first, and almost
     * none is a per-vertex one: they all lie in two ranges, so anything
     * outside them is answered before the forty tests below. */
    if (!((method >= 0x1500 && method < 0x16D0) ||
          (method >= 0x1880 && method < 0x1B00))) return 0;
    memcpy(&f, &param, 4);
    if (method >= 0x1500 && method < 0x150C) {            /* SET_VERTEX3F */
        part = (method - 0x1500) / 4;
        imm_set(0, (int)part, f); imm_set(0, 3, 1.0f);
        if (part == 2) imm_emit();
        return 1;
    }
    if (method >= 0x1518 && method < 0x1528) {            /* SET_VERTEX4F */
        part = (method - 0x1518) / 4;
        imm_set(0, (int)part, f);
        if (part == 3) imm_emit();
        return 1;
    }
    if (method >= 0x1530 && method < 0x153C) {            /* SET_NORMAL3F */
        imm_set(2, (int)((method - 0x1530) / 4), f);
        return 1;
    }
    if (method >= 0x1540 && method < 0x1548) {            /* SET_NORMAL3S */
        float lo = (float)(int16_t)(param & 0xFFFF) / 32767.0f;
        float hi = (float)(int16_t)(param >> 16) / 32767.0f;
        if (lo < -1.0f) lo = -1.0f;
        if (hi < -1.0f) hi = -1.0f;
        if (method == 0x1540) { imm_set(2, 0, lo); imm_set(2, 1, hi); }
        else imm_set(2, 2, lo);
        return 1;
    }
    /* Diffuse (3) and specular (4): 4F, 3F (alpha 1), 4UB. */
    if ((method >= 0x1550 && method < 0x1560) || (method >= 0x1570 && method < 0x1580)) {
        a = method < 0x1570 ? 3 : 4;
        imm_set(a, (int)((method & 0xF) / 4), f);
        return 1;
    }
    if ((method >= 0x1560 && method < 0x156C) || (method >= 0x1580 && method < 0x158C)) {
        a = method < 0x1570 ? 3 : 4;
        imm_set(a, (int)(((method - (a == 3 ? 0x1560 : 0x1580))) / 4), f);
        imm_set(a, 3, 1.0f);
        return 1;
    }
    if (method == 0x156C || method == 0x158C) {           /* SET_*_COLOR4UB */
        int c;
        a = method == 0x156C ? 3 : 4;
        for (c = 0; c < 4; c++) imm_set(a, c, (float)((param >> (8 * c)) & 0xFF) / 255.0f);
        return 1;
    }
    /* Texture coordinates 0..3 (attributes 9..12): 2F, 2S, 4F, 4S. */
    {
        static const uint32_t b2f[4] = { 0x1590, 0x15B8, 0x15E0, 0x1608 };
        static const uint32_t b2s[4] = { 0x1598, 0x15C0, 0x15E8, 0x1610 };
        static const uint32_t b4f[4] = { 0x15A0, 0x15C8, 0x15F0, 0x1620 };
        static const uint32_t b4s[4] = { 0x15B0, 0x15D8, 0x1600, 0x1630 };
        int t;
        for (t = 0; t < 4; t++) {
            a = 9 + (uint32_t)t;
            if (method >= b2f[t] && method < b2f[t] + 8) {
                imm_set(a, (int)((method - b2f[t]) / 4), f);
                imm_set(a, 2, 0.0f); imm_set(a, 3, 1.0f);
                return 1;
            }
            if (method == b2s[t]) {
                imm_set(a, 0, (float)(int16_t)(param & 0xFFFF));
                imm_set(a, 1, (float)(int16_t)(param >> 16));
                imm_set(a, 2, 0.0f); imm_set(a, 3, 1.0f);
                return 1;
            }
            if (method >= b4f[t] && method < b4f[t] + 16) {
                imm_set(a, (int)((method - b4f[t]) / 4), f);
                return 1;
            }
            if (method >= b4s[t] && method < b4s[t] + 8) {
                part = (method - b4s[t]) / 4;
                imm_set(a, (int)part * 2, (float)(int16_t)(param & 0xFFFF));
                imm_set(a, (int)part * 2 + 1, (float)(int16_t)(param >> 16));
                return 1;
            }
        }
    }
    if (method == 0x1698) { imm_set(5, 0, f); return 1; }  /* SET_FOG_COORD */
    if (method == 0x169C) { imm_set(1, 0, f); return 1; }  /* SET_WEIGHT1F */
    if (method >= 0x16A0 && method < 0x16A8) { imm_set(1, (int)((method - 0x16A0) / 4), f); return 1; }
    if (method >= 0x16B0 && method < 0x16BC) { imm_set(1, (int)((method - 0x16B0) / 4), f); return 1; }
    if (method >= 0x16C0 && method < 0x16D0) { imm_set(1, (int)((method - 0x16C0) / 4), f); return 1; }
    if (method >= 0x1880 && method < 0x1900) {            /* SET_VERTEX_DATA2F_M */
        a = (method - 0x1880) / 8; part = ((method - 0x1880) / 4) & 1;
        imm_set(a, (int)part, f); imm_set(a, 2, 0.0f); imm_set(a, 3, 1.0f);
        if (a == 0 && part == 1) imm_emit();
        return 1;
    }
    if (method >= 0x1900 && method < 0x1940) {            /* SET_VERTEX_DATA2S */
        a = (method - 0x1900) / 4;
        imm_set(a, 0, (float)(int16_t)(param & 0xFFFF));
        imm_set(a, 1, (float)(int16_t)(param >> 16));
        imm_set(a, 2, 0.0f); imm_set(a, 3, 1.0f);
        if (a == 0) imm_emit();
        return 1;
    }
    if (method >= 0x1940 && method < 0x1980) {            /* SET_VERTEX_DATA4UB */
        int c;
        a = (method - 0x1940) / 4;
        for (c = 0; c < 4; c++) imm_set(a, c, (float)((param >> (8 * c)) & 0xFF) / 255.0f);
        if (a == 0) imm_emit();
        return 1;
    }
    if (method >= 0x1980 && method < 0x1A00) {            /* SET_VERTEX_DATA4S_M */
        a = (method - 0x1980) / 8; part = ((method - 0x1980) / 4) & 1;
        imm_set(a, (int)part * 2, (float)(int16_t)(param & 0xFFFF));
        imm_set(a, (int)part * 2 + 1, (float)(int16_t)(param >> 16));
        if (a == 0 && part == 1) imm_emit();
        return 1;
    }
    if (method >= 0x1A00 && method < 0x1B00) {            /* SET_VERTEX_DATA4F_M */
        a = (method - 0x1A00) / 16; part = ((method - 0x1A00) / 4) & 3;
        imm_set(a, (int)part, f);
        if (a == 0 && part == 3) imm_emit();
        return 1;
    }
    return 0;
}

static void do_draw(void);

/* The batch Begin/End sent one vertex at a time, drawn as an inline array of
 * floats: the attributes that differ between its vertices (and the position)
 * as arrays, the rest as the constants they are. */
static void imm_draw(void)
{
    static Attr saved[NUM_ATTRS];
    uint32_t live = 1u, a, v, k = 0;
    for (a = 1; a < NUM_ATTRS; a++)
        for (v = 0; v < g_imm_n; v++)
            if (memcmp(g_imm[v][a], S.const_attr[a], sizeof g_imm[0][0])) { live |= 1u << a; break; }
    memcpy(saved, S.attr, sizeof saved);
    for (a = 0; a < NUM_ATTRS; a++) {
        S.attr[a].size = (live >> a) & 1u ? 4 : 0;
        S.attr[a].type = 2;
        S.attr[a].stride = 0;
    }
    for (v = 0; v < g_imm_n; v++)
        for (a = 0; a < NUM_ATTRS; a++)
            if ((live >> a) & 1u) {
                if (k + 4 > MAX_INLINE) break;
                memcpy(&S.inline_buf[k], g_imm[v][a], 16);
                k += 4;
            }
    S.inline_count = k;
    /* [IMM]: each kind of batch sent this way, once, the first time it is
     * drawn (its primitive and size, which attributes vary from vertex to
     * vertex, the stencil test, the blend), then every kind's count every
     * 30 s -- what this path adds to the picture, at a glance. */
    { enum { NK = 48 };
      static struct { uint64_t key; unsigned long n; } kinds[NK];
      static int nk; static double next;
      uint64_t key = (uint64_t)S.prim | (uint64_t)(g_imm_n < 15 ? g_imm_n : 15) << 4
                   | (uint64_t)live << 8 | (uint64_t)(S.stencil_en ? 1 : 0) << 24
                   | (uint64_t)(S.stencil_func & 0xF) << 25 | (uint64_t)(S.blend_enable ? 1 : 0) << 29
                   | (uint64_t)(S.blend_src & 0xFFF) << 30 | (uint64_t)(S.blend_dst & 0xFFF) << 42
                   | (uint64_t)(S.xf_mode & 3) << 54 | (uint64_t)(S.tex[0].enabled ? 1 : 0) << 56;
      int k;
      for (k = 0; k < nk; k++) if (kinds[k].key == key) break;
      if (k == nk && nk < NK) {
          kinds[nk].key = key; kinds[nk].n = 0; nk++;
          fprintf(stderr, "[IMM] kind %d, first at flip %u draw %u: %u vertices (prim %u), per-vertex "
                  "attributes %04X, first (%.1f %.1f %.3f %.3f) diffuse (%.2f %.2f %.2f %.2f) | stencil %u "
                  "func %X ref %X ops %X/%X/%X | blend %d %04X/%04X depth %d/%d func %X | xf %u tex0 %d%s\n",
                  k, S.flips, S.since_present, g_imm_n, S.prim, live,
                  g_imm[0][0][0], g_imm[0][0][1], g_imm[0][0][2], g_imm[0][0][3],
                  g_imm[0][3][0], g_imm[0][3][1], g_imm[0][3][2], g_imm[0][3][3],
                  S.stencil_en, S.stencil_func, S.stencil_ref,
                  S.stencil_op[0], S.stencil_op[1], S.stencil_op[2],
                  S.blend_enable, S.blend_src, S.blend_dst, S.depth_test, S.depth_mask,
                  (unsigned)S.depth_func, S.xf_mode, S.tex[0].enabled,
                  g_imm_dropped ? " (vertices dropped: batch too long)" : "");
          fflush(stderr);
      }
      if (k < nk) kinds[k].n++;
      if (mono_now() >= next) {
          if (next > 0.0) {
              fprintf(stderr, "[IMM] so far:");
              for (k = 0; k < nk; k++) fprintf(stderr, " kind %d %lu;", k, kinds[k].n);
              fprintf(stderr, "\n");
          }
          next = mono_now() + 30.0;
      } }
    g_imm_drawing = 1;
    /* RECOMP_GL_STENCILVIS=1 (a probe): a batch drawn under a stencil test
     * other than "always" -- this title's shadow passes -- is replaced by
     * the stencil buffer's contents over the area it covers: 1 red, 2 green,
     * 3 blue, 4 and more white, 0 left alone. */
    { static int vis = -1;
      if (vis < 0) { const char *e = getenv("RECOMP_GL_STENCILVIS"); vis = e ? atoi(e) : 0; }
      if (vis && S.stencil_en && S.stencil_func != 0x207) {
          static const float col[4][4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 }, { 1, 1, 1, 1 } };
          uint32_t f = S.stencil_func, r = S.stencil_ref, fm = S.stencil_fmask, o2 = S.stencil_op[2];
          int be = S.blend_enable, cv;
          float keep[4];
          memcpy(keep, S.const_attr[3], sizeof keep);
          S.blend_enable = 0; S.stencil_fmask = 0xFF; S.stencil_op[2] = 0x1E00;
          for (cv = 0; cv < 4; cv++) {
              /* EQUAL k for 1..3, LEQUAL 4 (4 <= stencil) for the rest. The
               * colour as the diffuse constant, unless the batch sends one. */
              S.stencil_func = cv < 3 ? 0x202 : 0x203;
              S.stencil_ref = (uint32_t)cv + 1;
              memcpy(S.const_attr[3], col[cv], sizeof col[cv]);
              if ((live >> 3) & 1u)
                  for (v = 0; v < g_imm_n; v++)
                      memcpy(&S.inline_buf[(v * (uint32_t)__builtin_popcount(live)
                                            + (uint32_t)__builtin_popcount(live & 7u)) * 4],
                             col[cv], 16);
              do_draw();
          }
          memcpy(S.const_attr[3], keep, sizeof keep);
          S.stencil_func = f; S.stencil_ref = r; S.stencil_fmask = fm; S.stencil_op[2] = o2;
          S.blend_enable = be;
          g_imm_drawing = 0;
          memcpy(S.attr, saved, sizeof saved);
          S.inline_count = 0;
          return;
      } }
    do_draw();
    g_imm_drawing = 0;
    memcpy(S.attr, saved, sizeof saved);
    S.inline_count = 0;
}

/* RECOMP_GL_METHTRACE=<from>-<to>: in the frames RECOMP_GL_DRAW_LOG follows,
 * every method between those draws of the frame (the vertex program and
 * constant uploads and the vertex data itself left out). */
static void methtrace_note(uint32_t method, uint32_t param)
{
    static int init; static long lo = -1, hi = -1;
    float f;
    if (!init) { const char *v = getenv("RECOMP_GL_METHTRACE"); init = 1;
                 if (v) { lo = atol(v); { const char *d = strchr(v, '-'); hi = d ? atol(d + 1) : lo; } } }
    if (lo < 0 || g_draw_log_flip < 0) return;
    if ((long)S.flips < g_draw_log_flip || (long)S.flips >= g_draw_log_flip + 2) return;
    if ((long)S.since_present < lo || (long)S.since_present > hi) return;
    if ((method >= 0x0B00 && method < 0x0C00) || method == 0x1800 || method == 0x1808
        || method == 0x1818) return;
    memcpy(&f, &param, 4);
    fprintf(stderr, "[MT] f%u d%u %04X = %08X (%g)\n", S.flips, S.since_present, method, param, f);
}

/*
 * A run of index words in one call.
 *
 * In 99th Street the walk reads about 13 million words a second, and 55% of
 * them are indices: ARRAY_ELEMENT16 or 32, one method written
 * non-incrementing with up to 2047 words (spriteice's count). Each word went
 * through three calls and every check nv2a_gl_method makes ahead of its
 * switch, to land in S.idx.
 *
 * The first word still goes through nv2a_gl_method, which starts the batch,
 * or draws what it holds if a constant or an array moved since
 * (split_batch_if_state_moved). The rest are appended here as nv2a_gl_method
 * would append them, dropped the same way once S.idx is full. Nothing
 * nv2a_gl_method checks can change between two words of one run: only another
 * method moves a constant or an array, and the per-method logs (methval_note,
 * methtrace_note) skip the index methods.
 */
void nv2a_gl_indices(uint32_t method, const uint32_t *p, uint32_t n)
{
    uint32_t room, i;
    nv2a_gl_method(method, p[0]);
    if (n < 2 || !nv2a_gl_enabled() || g_failed) return;
    p++;
    n--;
    if (method == M_ARRAY_ELEMENT16) {
        room = (MAX_INDICES - S.idx_count) / 2;     /* two indices a word */
        if (n > room) n = room;
        for (i = 0; i < n; i++) {
            S.idx[S.idx_count++] = p[i] & 0xFFFF;
            S.idx[S.idx_count++] = p[i] >> 16;
        }
    } else if (method == M_ARRAY_ELEMENT32) {
        room = MAX_INDICES - S.idx_count;
        if (n > room) n = room;
        memcpy(&S.idx[S.idx_count], p, n * sizeof *p);
        S.idx_count += n;
    }
}

void nv2a_gl_method(uint32_t method, uint32_t param)
{
    if (!nv2a_gl_enabled() || g_failed) return;
    methval_note(method, param);
    methtrace_note(method, param);

    if (!S.const_attr_init) {
        int i;
        S.const_attr_init = 1;
        for (i = 0; i < NUM_ATTRS; i++) {
            S.const_attr[i][0] = S.const_attr[i][1] = S.const_attr[i][2] = 0.0f;
            S.const_attr[i][3] = 1.0f;
        }
        /* Diffuse and specular latch to white, which is what an untextured
         * surface with no colour stream is meant to come out as. */
        S.const_attr[3][0] = S.const_attr[3][1] = S.const_attr[3][2] = 1.0f;
        S.const_attr[4][0] = S.const_attr[4][1] = S.const_attr[4][2] = 1.0f;
        memcpy(S.const_set, S.const_attr, sizeof S.const_set);
    }

    /* Ranged methods first: they are the ones a title spends its bandwidth on. */
    /* Both of these arrive as a run of dwords, and which component each dword
     * belongs to must be counted rather than derived from the method address.
     *
     * The pushbuffer has two ways to send a run: incrementing, where the
     * method advances with each dword, and non-incrementing, where every dword
     * arrives at the same address. Deriving the component from the address
     * works for the first and silently drops all but one dword in four for the
     * second -- and the XDK uses non-incrementing writes for exactly the
     * constants a title never re-uploads, including the viewport scale and
     * offset the vertex program multiplies by. The result is a program that
     * multiplies every position by zero, which draws nothing at all and looks
     * like the whole renderer being broken. */
    if (method >= M_SET_TRANSFORM_PROGRAM && method <= M_SET_TRANSFORM_PROGRAM + 0x7C) {
        if (S.prog_load < NV2A_VSH_MAX_INSNS) {
            S.prog[S.prog_load * 4 + S.prog_sub] = param;
            g_vsh_gen++;
        }
        if (++S.prog_sub == 4) { S.prog_sub = 0; S.prog_load++; S.prog_dirty = 1; }
        return;
    }
    if (method >= M_SET_TRANSFORM_CONSTANT && method <= M_SET_TRANSFORM_CONSTANT + 0x7C) {
        /* RECOMP_GL_WATCH_CONST=<n>: report every write to one constant.
         * A program that reads a constant the title never writes is either a
         * capture bug or a genuinely absent upload, and only watching the
         * slot separates the two. */
        { static int wc = -2;
          if (wc == -2) { const char *v = getenv("RECOMP_GL_WATCH_CONST");
                          wc = v ? atoi(v) : -1; }
          if (wc >= 0 && (int)S.const_load == wc) {
              static int n;
              float fv; memcpy(&fv, &param, 4);
              if (n++ < 40)
                  fprintf(stderr, "  [GL] c[%d].%u <- %08X (%.4f)\n",
                          wc, S.const_sub, param, fv);
          } }
        /* RECOMP_GL_CONST_OWNER=1: who writes which constant slot.
         *
         * The fixed-function matrices are mirrored into the low end of this
         * same file, because that is where the hardware puts them. That is
         * only safe if the title's own vertex programs never read from there,
         * and D3D8 says they do not -- an application's c[0] is file index 96.
         * If this title writes below 96, every fixed-function draw is
         * overwriting something one of its programs is about to read, and its
         * characters would transform with the road's matrix. Counting who
         * writes where settles it. */
        { static unsigned char who[NV2A_VSH_NUM_CONSTS];
          static int on = -1, reported;
          if (on < 0) on = getenv("RECOMP_GL_CONST_OWNER") ? 1 : 0;
          if (on && S.const_load < NV2A_VSH_NUM_CONSTS) {
              who[S.const_load] |= 1;              /* written by the title */
              if (++reported % 200000 == 0) {
                  int k, lo = -1, hi = -1, n = 0;
                  for (k = 0; k < NV2A_VSH_NUM_CONSTS; k++)
                      if (who[k] & 1) { if (lo < 0) lo = k; hi = k; n++; }
                  fprintf(stderr, "  [CONST] the title writes %d slots, "
                          "c[%d]..c[%d]; the fixed-function mirror writes "
                          "c[0..3] (composite), c[8/16/24/32] (model-view), "
                          "c[58,59] (viewport), c[68..83] (texture)\n",
                          n, lo, hi);
                  { int clash = 0;
                    for (k = 0; k < 4; k++)  clash |= who[k] & 1;
                    for (k = 8; k <= 32; k += 8) clash |= who[k] & 1;
                    for (k = 68; k < 84; k++) clash |= who[k] & 1;
                    fprintf(stderr, "  [CONST] collision with the mirror: %s\n",
                            clash ? "YES -- the mirror is destroying the "
                                    "title's own constants"
                                  : "no"); }
                  fflush(stderr);
              }
          } }
        if (S.const_load < NV2A_VSH_NUM_CONSTS
            && memcmp(&S.u.c[S.const_load][S.const_sub], &param, 4) != 0) {
            memcpy(&S.u.c[S.const_load][S.const_sub], &param, 4);
            const_changed(S.const_load);
        }
        if (++S.const_sub == 4) { S.const_sub = 0; S.const_load++; }
        S.consts_dirty = 1;
        S.consts_since_geom = 1;
        return;
    }
    if (method >= M_SET_FOG_PARAMS && method < M_SET_FOG_PARAMS + 12) {
        memcpy(&S.fog_params[(method - M_SET_FOG_PARAMS) / 4], &param, 4);
        return;
    }
    if (method >= M_SET_FOG_PLANE && method < M_SET_FOG_PLANE + 16) {
        memcpy(&S.fog_plane[(method - M_SET_FOG_PLANE) / 4], &param, 4);
        return;
    }
    if (method >= M_SET_WINDOW_CLIP_HORZ && method < M_SET_WINDOW_CLIP_HORZ + 32) {
        S.wclip_h[(method - M_SET_WINDOW_CLIP_HORZ) / 4] = param;
        S.wclip_valid = 1; return;
    }
    if (method >= M_SET_WINDOW_CLIP_VERT && method < M_SET_WINDOW_CLIP_VERT + 32) {
        S.wclip_v[(method - M_SET_WINDOW_CLIP_VERT) / 4] = param;
        S.wclip_valid = 1; return;
    }
    if (method >= M_SET_COMBINER_ALPHA_ICW && method < M_SET_COMBINER_ALPHA_ICW + 32) {
        S.psh.alpha_icw[(method - M_SET_COMBINER_ALPHA_ICW) / 4] = param; return;
    }
    if (method >= M_SET_COMBINER_FACTOR0 && method < M_SET_COMBINER_FACTOR0 + 32) {
        S.psh.c0[(method - M_SET_COMBINER_FACTOR0) / 4] = param; return;
    }
    if (method >= M_SET_COMBINER_FACTOR1 && method < M_SET_COMBINER_FACTOR1 + 32) {
        S.psh.c1[(method - M_SET_COMBINER_FACTOR1) / 4] = param; return;
    }
    if (method >= M_SET_COMBINER_ALPHA_OCW && method < M_SET_COMBINER_ALPHA_OCW + 32) {
        S.psh.alpha_ocw[(method - M_SET_COMBINER_ALPHA_OCW) / 4] = param; return;
    }
    if (method >= M_SET_COMBINER_COLOR_ICW && method < M_SET_COMBINER_COLOR_ICW + 32) {
        S.psh.rgb_icw[(method - M_SET_COMBINER_COLOR_ICW) / 4] = param; return;
    }
    if (method >= M_SET_COMBINER_COLOR_OCW && method < M_SET_COMBINER_COLOR_OCW + 32) {
        S.psh.rgb_ocw[(method - M_SET_COMBINER_COLOR_OCW) / 4] = param; return;
    }
    /* The fixed-function matrices arrive by their own methods rather than
     * through the constant file, and the hardware mirrors them into fixed
     * constant slots -- the same arrangement as the viewport registers at 58
     * and 59, which is the one place this backend already had to learn.
     * Putting them where the hardware puts them means the fixed-function
     * shader reads its matrix exactly the way a translated program reads
     * anything else. */
    if (method >= M_SET_COMPOSITE_MATRIX && method < M_SET_COMPOSITE_MATRIX + 64) {
        uint32_t k = (method - M_SET_COMPOSITE_MATRIX) / 4;
        /* The shader transforms with dp4 against these four vectors, so they
         * have to be the matrix's columns. Whether the sixteen floats arrive
         * in that order or the other one is a property of the driver, not
         * something to be reasoned out from a convention -- RECOMP_GL_FF_T
         * swaps it, so both can be looked at rather than argued about. */
        static int tr = -1;
        if (tr < 0) tr = getenv("RECOMP_GL_FF_T") ? 1 : 0;
        if (tr) memcpy(&S.u.ff_mat[k & 3][k / 4], &param, 4);
        else    memcpy(&S.u.ff_mat[k / 4][k & 3], &param, 4);
        S.ff_mats_dirty = 1;
        return;
    }
    if (method >= M_SET_MODEL_VIEW_MATRIX
     && method < M_SET_MODEL_VIEW_MATRIX + 4 * 64) {
        /* The four model-view matrices are not contiguous in the constant
         * file: each is followed by its own inverse, so they sit at 8, 16,
         * 24 and 32 with IMMAT0..3 in the gaps. The hardware blends between
         * them when skinning is on; the composite matrix alone is only the
         * rigid case. */
        /* Not written into the constant file -- the composite matrix covers
         * the transform, and zero of this title's fixed-function batches
         * enable skinning; writing them there was pure damage. Matrix 0 is
         * kept for the lighting, which needs eye-space positions. Stored as
         * sent, four groups of four, each the vector a component is dotted
         * with -- the composite matrix's own arrangement (xemu: no
         * transpose). */
        uint32_t k = (method - M_SET_MODEL_VIEW_MATRIX) / 4;
        if (k < 16) memcpy(&S.ff_mv[k / 4][k & 3], &param, 4);
        return;
    }
    if (method >= 0x0580 && method < 0x0580 + 4 * 64) {  /* NV097_SET_INVERSE_MODEL_VIEW_MATRIX */
        /* Its matrix 0 turns normals into eye space for the lighting:
         * tNormal = (normal, 0) against these, as xemu's vsh-ff does. */
        uint32_t k = (method - 0x0580) / 4;
        if (k < 16) memcpy(&S.ff_imv[k / 4][k & 3], &param, 4);
        return;
    }
    if (method >= M_SET_TEXTURE_MATRIX && method < M_SET_TEXTURE_MATRIX + 4 * 64) {
        uint32_t k = (method - M_SET_TEXTURE_MATRIX) / 4;   /* 0..63 */
        memcpy(&S.u.ff_texmat[(k / 16) * 4 + ((k / 4) & 3)][k & 3], &param, 4);
        S.ff_mats_dirty = 1;
        return;
    }
    if (method >= M_SET_TEXTURE_MATRIX_ENABLE
     && method < M_SET_TEXTURE_MATRIX_ENABLE + 16) {
        uint32_t i = (method - M_SET_TEXTURE_MATRIX_ENABLE) / 4;
        if (param) S.ff_texmat_enable |= 1u << i;
        else       S.ff_texmat_enable &= ~(1u << i);
        return;
    }
    if (method >= M_SET_TEXGEN_S && method < M_SET_TEXGEN_S + 0x40) {
        S.texgen[(method - M_SET_TEXGEN_S) >> 4][((method - M_SET_TEXGEN_S) >> 2) & 3] = param;
        return;
    }
    if (method >= M_SET_TEXGEN_PLANE_S && method < M_SET_TEXGEN_PLANE_S + 0x100) {
        uint32_t k = (method - M_SET_TEXGEN_PLANE_S) >> 2;   /* stage*16 + comp*4 + element */
        memcpy(&S.texgen_plane[k >> 2][k & 3], &param, 4);
        return;
    }
    if (method == M_SET_POINT_PARAMS_ENABLE) { S.point_params_enable = param; return; }
    if (method == M_SET_POINT_SMOOTH_ENABLE) { S.point_smooth_enable = param; return; }
    if (method == M_SET_POINT_SIZE)          { S.point_size = param; return; }
    if (method >= M_SET_POINT_PARAMS && method < M_SET_POINT_PARAMS + 32) {
        memcpy(&S.point_params[(method - M_SET_POINT_PARAMS) >> 2], &param, 4);
        return;
    }
    /* RECOMP_GL_INLINELOG=1: the named per-vertex constant methods
     * (NV097_SET_NORMAL3F 0x1530, _NORMAL3S 0x1540, _DIFFUSE_COLOR* 0x1550..,
     * _SPECULAR_COLOR* ...) -- which of them the title sends, with values. */
    if ((method >= 0x1500 && method < 0x1600 && (method < 0x1500 || method >= 0x1530))
        || method == 0x1890 || method == 0x1894 || method == 0x1908 || method == 0x1948
        || method == 0x1990 || method == 0x1994 || (method >= 0x1A20 && method < 0x1A30)) {
        /* (the inline vertex positions, 0x1500..0x152C, are left out) --
         * and every route to the normal's constant, attribute 2. */
        static int on = -1, n;
        if (on < 0) on = getenv("RECOMP_GL_INLINELOG") ? 1 : 0;
        if (on && n < 200) { float fv; memcpy(&fv, &param, 4); n++;
            fprintf(stderr, "[INLINE] m %04X = %08X (%g) at draw %u flip %u\n", method, param, fv, S.draws, S.flips); }
    }
    /* Every per-vertex attribute method: the attribute's current value, and
     * inside Begin/End the position's last component sends a vertex. */
    if (imm_method(method, param)) return;
    if (method >= M_SET_VERTEX_ARRAY_OFFSET && method < M_SET_VERTEX_ARRAY_OFFSET + 0x40) {
        uint32_t ai = (method - M_SET_VERTEX_ARRAY_OFFSET) / 4;
        if (S.attr[ai].offset != param) S.arrays_since_geom = 1;
        S.attr[ai].offset = param;
        return;
    }
    if (method >= M_SET_VERTEX_ARRAY_FORMAT && method < M_SET_VERTEX_ARRAY_FORMAT + 0x40) {
        Attr *a = &S.attr[(method - M_SET_VERTEX_ARRAY_FORMAT) / 4];
        if (a->raw != param) S.arrays_since_geom = 1;
        a->raw    = param;
        a->seen   = 1;
        a->type   = param & 0xF;
        a->size   = (param >> 4) & 0xF;
        /* STRIDE is 24 bits, not 8. A fat vertex -- position, normal, two
         * texture coordinate sets -- passes 255 bytes easily, and truncating
         * it makes every vertex after the first read from inside its
         * predecessor. */
        a->stride = (param >> 8) & 0xFFFFFF;
        return;
    }
    if (method >= M_SET_TEXTURE_OFFSET && method < M_SET_TEXTURE_OFFSET + NUM_STAGES * 0x40) {
        uint32_t stage = (method - M_SET_TEXTURE_OFFSET) / 0x40;
        uint32_t reg   = (method - M_SET_TEXTURE_OFFSET) % 0x40;
        TexStage *t = &S.tex[stage];
        switch (reg) {
        case 0x00: t->offset = param; break;
        case 0x04:
            t->format = param;
            t->color  = (param >> 8) & 0xFF;
            t->levels = (param >> 16) & 0xF;
            /* A swizzled or compressed texture carries its size as log2 in
             * the format register; a linear one gets it from IMAGE_RECT. */
            if (fmt_is_swz(t->color) || fmt_is_dxt(t->color)) {
                t->width  = 1u << ((param >> 20) & 0xF);
                t->height = 1u << ((param >> 24) & 0xF);
            }
            break;
        case 0x08: t->addr = param; break;
        case 0x0C: t->control0 = param;
                   t->enabled = (int)((param >> 30) & 1); break;
        case 0x10: t->control1 = param; t->pitch = param >> 16; break;
        case 0x14: t->filter = param; break;
        case 0x28: case 0x2C: case 0x30: case 0x34:
            memcpy(&t->bump_mat[(reg - 0x28) / 4], &param, 4); break;
        case 0x38: memcpy(&t->bump_scale, &param, 4); break;
        case 0x3C: memcpy(&t->bump_off, &param, 4); break;
        case 0x1C: t->image_rect = param;
                   if (!fmt_is_swz(t->color) && !fmt_is_dxt(t->color)) {
                       t->width  = param >> 16;
                       t->height = param & 0xFFFF;
                   }
                   break;
        default: break;
        }
        return;
    }
    /* The viewport is four floats, and like the constant bank it can arrive
     * as a non-incrementing run -- every dword at the same method address.
     * Deriving the component from the address then writes all four to the
     * first one and leaves the rest stale, which produces a viewport that is
     * almost right and a picture that is scaled by a few hundred. */
    /* The per-light block: NV097_SET_LIGHT_* at 0x1000 + light * 0x80.
     * Colours and vectors are three floats, the spot direction four. Light 0's
     * values are logged the first few times they change, to see what the
     * title's sun looks like. */
    if (method >= 0x1000 && method < 0x1000 + 8 * 0x80) {
        /* The fields are three floats apiece (the spot direction four), so
         * they do not sit on 16-byte boundaries: ambient 0x00, diffuse 0x0C,
         * specular 0x18, range 0x24, infinite half vector 0x28, infinite
         * direction 0x34, spot falloff 0x40, spot direction 0x4C, local
         * position 0x5C, attenuation 0x68 (xemu nv2a_regs.h). This used to
         * decode by masking off bits 2-3, which only finds fields that start
         * on 16 bytes: everything but the ambient colour was dropped, and
         * every light read as black -- the "null light" of the earlier
         * notes was this decoder. */
        uint32_t li = (method - 0x1000) / 0x80, off = (method - 0x1000) % 0x80; float fv;
        memcpy(&fv, &param, 4);
        if      (off < 0x0C) S.light_amb[li][off / 4] = fv;
        else if (off < 0x18) S.light_dif[li][(off - 0x0C) / 4] = fv;
        else if (off < 0x24) S.light_spc[li][(off - 0x18) / 4] = fv;
        else if (off < 0x28) S.light_range[li] = fv;
        else if (off < 0x34) S.light_half[li][(off - 0x28) / 4] = fv;
        else if (off < 0x40) S.light_dir[li][(off - 0x34) / 4] = fv;
        else if (off < 0x4C) S.light_spot_falloff[li][(off - 0x40) / 4] = fv;
        else if (off < 0x5C) S.light_spot_dir[li][(off - 0x4C) / 4] = fv;
        else if (off < 0x68) S.light_pos[li][(off - 0x5C) / 4] = fv;
        else if (off < 0x74) S.light_att[li][(off - 0x68) / 4] = fv;
        if (li == 0 && off == 0x3C) { static int said; if (said++ < 4)
            fprintf(stderr, "  [LIGHT] light0 amb %.2f %.2f %.2f dif %.2f %.2f %.2f spc %.2f %.2f %.2f dir %.3f %.3f %.3f half %.3f %.3f %.3f at draw %u\n",
                    S.light_amb[0][0], S.light_amb[0][1], S.light_amb[0][2], S.light_dif[0][0], S.light_dif[0][1], S.light_dif[0][2],
                    S.light_spc[0][0], S.light_spc[0][1], S.light_spc[0][2], S.light_dir[0][0], S.light_dir[0][1], S.light_dir[0][2],
                    S.light_half[0][0], S.light_half[0][1], S.light_half[0][2], S.draws); }
        return;
    }
    if (method >= 0x0A50 && method < 0x0A50 + 16) {   /* NV097_SET_EYE_POSITION (was 0x181C: wrong) */
        memcpy(&S.eye_position[(method - 0x0A50) / 4], &param, 4); return;
    }
    if (method >= 0x09E0 && method < 0x09E0 + 24) {   /* NV097_SET_SPECULAR_PARAMS, six floats */
        memcpy(&S.specular_params[(method - 0x09E0) / 4], &param, 4); return;
    }

    if (method >= M_SET_VIEWPORT_OFFSET && method < M_SET_VIEWPORT_OFFSET + 0x10) {
        static uint32_t last, sub;
        sub = (method == last) ? ((sub + 1) & 3)
                               : ((method - M_SET_VIEWPORT_OFFSET) / 4);
        last = method;
        if (vp_trace()) { float fv; memcpy(&fv, &param, 4);
            if (fv != S.u.vp_off[sub])
                fprintf(stderr, "[VP] off m=%04X sub=%u %.5f (draw %u)\n",
                        method, sub, fv, S.draws); }
        /* Interleaved with the per-frame draw log, so the order of viewport
         * writes and draws inside one frame is visible. A value that arrives
         * and a value that is live at the draw are different things. */
        if (g_draw_log_flip >= 0 && (long)S.flips >= g_draw_log_flip
         && (long)S.flips < g_draw_log_flip + 6) {
            float fv; memcpy(&fv, &param, 4);
            fprintf(stderr, "[VPW] f%ld after draw %u  off sub=%u %.5f\n",
                    (long)S.flips, S.since_present, sub, fv);
        }
        /* RECOMP_GL_VP_FORCE: ignore an offset that arrives without a scale
         * to go with it.
         *
         * Across a whole run the scale is written 543 times and is always
         * (320, -240, 16777215, 0); the offset is written 543 times as
         * (320.53, 240.53) -- one per scale write, a matched pair -- and 117
         * further times as (0.53, 0.53) with no scale write beside it. The
         * second combination puts NDC zero at pixel 0.53 of a 640x480 target,
         * which is half a screen out. This is a probe, not a fix: if holding
         * the paired offset makes the scene appear, the unpaired writes are
         * reaching geometry they should not, and the real question becomes
         * what they are. */
        { static int force = -1;
          if (force < 0) force = getenv("RECOMP_GL_VP_FORCE") ? 1 : 0;
          if (force && sub < 2) {
              float fv; memcpy(&fv, &param, 4);
              if (fv < 100.0f && S.u.vp_scale[0] > 100.0f) return;
          } }
        memcpy(&S.u.vp_off[sub], &param, 4);
        memcpy(&S.vp_off_reg[sub], &param, 4);
        /* Only dirty the bank on a real change: the title rewrites the
         * viewport around most draws, and re-uploading 192 vec4s each time
         * costs more than the draw does. */
        if (memcmp(&S.u.c[NV2A_XFCTX_VPOFF][sub], &param, 4) != 0) {
            memcpy(&S.u.c[NV2A_XFCTX_VPOFF][sub], &param, 4);
            const_changed(NV2A_XFCTX_VPOFF);
            S.consts_dirty = 1;
        }
        return;
    }
    if (method >= M_SET_VIEWPORT_SCALE && method < M_SET_VIEWPORT_SCALE + 0x10) {
        static uint32_t last, sub;
        sub = (method == last) ? ((sub + 1) & 3)
                               : ((method - M_SET_VIEWPORT_SCALE) / 4);
        last = method;
        if (vp_trace()) { float fv; memcpy(&fv, &param, 4);
            if (fv != S.u.vp_scale[sub])
                fprintf(stderr, "[VP] scl m=%04X sub=%u %.5f (draw %u)\n",
                        method, sub, fv, S.draws); }
        if (g_draw_log_flip >= 0 && (long)S.flips >= g_draw_log_flip
         && (long)S.flips < g_draw_log_flip + 6) {
            float fv; memcpy(&fv, &param, 4);
            fprintf(stderr, "[VPW] f%ld after draw %u  scl sub=%u %.5f\n",
                    (long)S.flips, S.since_present, sub, fv);
        }
        memcpy(&S.u.vp_scale[sub], &param, 4);
        if (memcmp(&S.u.c[NV2A_XFCTX_VPSCL][sub], &param, 4) != 0) {
            memcpy(&S.u.c[NV2A_XFCTX_VPSCL][sub], &param, 4);
            const_changed(NV2A_XFCTX_VPSCL);
            S.consts_dirty = 1;
        }
        return;
    }

    switch (method) {
    case M_SET_SURFACE_CLIP_H:
        S.clip_x = param & 0xFFFF; S.clip_w = (param >> 16) & 0xFFFF; break;
    case M_SET_SURFACE_CLIP_V:
        S.clip_y = param & 0xFFFF; S.clip_h = (param >> 16) & 0xFFFF;
        ensure_ready();
        /* Bound at the next draw or clear, not here: see surface_sync. */
        S.surf_dirty = 1;
        break;
    case M_SET_SURFACE_FORMAT:       S.surf_format = param; break;
    case M_SET_SURFACE_PITCH:        S.surf_pitch = param & 0xFFFF; break;
    case M_SET_SURFACE_COLOR_OFFSET:
        /* Which surfaces a title draws into, and how big they are. More than
         * two means render-to-texture: the scene is being built off-screen and
         * composited, and a backend with a single framebuffer silently merges
         * all of them -- which looks like the final frame being empty. */
        if (S.color_offset != param) {
            static struct { uint32_t off, w, h, n; } seen[16];
            static int nseen;
            int i;
            for (i = 0; i < nseen; i++)
                if (seen[i].off == param) break;
            if (i == nseen && nseen < 16) {
                seen[nseen].off = param; seen[nseen].w = S.clip_w;
                seen[nseen].h = S.clip_h; nseen++;
                if (verbose())
                    fprintf(stderr, "  [GL] surface %d: offset %08X  %ux%u\n",
                            i, param, S.clip_w, S.clip_h);
            }
            if (i < 16) seen[i].n++;
            S.surface_switches++;
        }
        S.color_offset = param;
        S.surf_dirty = 1;     /* see surface_sync */
        break;
    case M_SET_SURFACE_ZETA_OFFSET:  S.zeta_offset = param; break;

    case M_SET_COMBINER_CONTROL:     S.psh.control = param; break;
    case M_SET_COMBINER_SPECFOG_CW0: S.psh.final_abcd = param; break;
    case M_SET_COMBINER_SPECFOG_CW1: S.psh.final_efg = param; break;
    case 0x1E20: S.psh.final_c0 = param; break;   /* NV097_SET_SPECULAR_FOG_FACTOR */
    case 0x1E24: S.psh.final_c1 = param; break;
    case M_SET_FOG_COLOR:            S.fog_color = param; break;
    /* The texture shaders: what each of the four stages does with its
     * coordinates before the combiners see it. Not decoded before -- every
     * bound stage was a plain 2D sample. Logged per distinct value first,
     * because a stage set to CLIPPLANE discards fragments and a stage set
     * to NONE is never sampled, and either would explain a title screen
     * that flashes whole quads the console never shows. */
    case M_SET_SHADER_STAGE_PROGRAM:
        if (param != S.shader_stage_prog) {
            static uint32_t seen[16]; static int nseen; int i, fresh = 1;
            for (i = 0; i < nseen; i++) if (seen[i] == param) { fresh = 0; break; }
            if (fresh && nseen < 16) { seen[nseen++] = param;
                fprintf(stderr, "  [GL] shader stage program #%d: 0x%05X = stages %u/%u/%u/%u"
                                " (0 none 1 2D 4 passthru 5 clipplane 6 bumpenv) at draw %u\n",
                        nseen, param, param & 31, (param >> 5) & 31, (param >> 10) & 31,
                        (param >> 15) & 31, S.draws); fflush(stderr); }
        }
        S.shader_stage_prog = param; break;
    case M_SET_SHADER_CLIP_PLANE_MODE:
        if (param != S.shader_clip_mode) {
            static int said; if (said++ < 8) fprintf(stderr, "  [GL] shader clip plane mode 0x%08X at draw %u\n", param, S.draws); }
        S.shader_clip_mode = param; break;
    case M_SET_SHADER_OTHER_STAGE_INPUT: S.shader_other_input = param; break;
    case M_SET_FOG_MODE:             S.fog_mode = param; break;
    case M_SET_FOG_GEN_MODE:         S.fog_gen_mode = param; break;
    case M_SET_FOG_ENABLE:           S.fog_enable = param; break;
    case M_SET_CONTROL0: {
        static uint32_t said = 0xFFFFFFFFu;
        S.control0 = param;
        if (param != said) {
            said = param;
            fprintf(stderr, "  [GL] CONTROL0 = %08X at draw %u, flip %u: z_format=%s  "
                    "w-buffering(Z_PERSPECTIVE)=%s  stencil_write=%s\n", param,
                    S.draws, S.flips,
                    (param & (1u << 12)) ? "float" : "fixed",
                    (param & (1u << 16)) ? "ON" : "off",
                    (param & 1u) ? "on" : "off");
            fflush(stderr);
        }
        break; }
    case M_SET_ZMIN_MAX_CONTROL: {
        static uint32_t said = 0xFFFFFFFFu;
        S.zminmax = param;
        if (param != said) {
            said = param;
            fprintf(stderr, "  [GL] ZMIN_MAX_CONTROL = %08X at draw %u: past the near/far "
                    "plane a fragment is %s\n", param, S.draws,
                    (param & 0xF0u) ? "CLAMPED to it (GL_DEPTH_CLAMP)"
                                    : "culled");
            fflush(stderr);
        }
        break; }
    case M_SET_COLOR_CLEAR_VALUE:    S.clear_color = param; break;
    case M_SET_ZSTENCIL_CLEAR_VALUE: S.clear_zstencil = param; break;
    case M_SET_CLEAR_RECT_H:
        S.clear_x0 = param & 0xFFFF; S.clear_x1 = (param >> 16) & 0xFFFF; break;
    case M_SET_CLEAR_RECT_V:
        S.clear_y0 = param & 0xFFFF; S.clear_y1 = (param >> 16) & 0xFFFF; break;
    case M_CLEAR_SURFACE:            ensure_ready(); do_clear(param); break;

    /* Fixed-function transform.
     *
     * The NV2A has a hardware T&L unit as well as the programmable one, and
     * D3D8 uses it for every FVF draw -- SetVertexShader(D3DFVF_...) rather
     * than a real shader. Nothing here has ever read this register, so a
     * fixed-function batch has always been drawn with whatever program
     * happened to be loaded last. That would leave position roughly right
     * (fixed mode keeps its composite matrix in the vertex constant file, so
     * a stale program transforming by those slots still lands) while every
     * other output came from instructions written for different inputs --
     * which is exactly the shape of the flat backdrop: right place, right
     * texture, coordinate stuck at zero. Count it first, before building a
     * fixed-function emitter on a guess. */
    case M_SET_TRANSFORM_EXEC_MODE:   S.xf_mode = param & 3; break;
    case M_SET_WINDOW_CLIP_TYPE: S.wclip_type = param; break;
    case 0x1E98:   /* NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN */
        /* Whether writes to the transform context are enabled.
         *
         * The title sets this a hundred and four thousand times a run -- about
         * once per programmable draw -- and this backend has been ignoring it
         * and writing every constant regardless. If the hardware is dropping
         * some of those writes, the constant file here does not match the one
         * the title's programs actually read, and an object transformed by the
         * wrong constants is an object in the wrong place. Worth knowing what
         * values it carries before assuming either way. */
        { static int said; static uint32_t was = 0xFFFFFFFFu;
          static uint32_t n_on, n_off;
          if (param) n_on++; else n_off++;
          if (param != was && said < 6) {
              said++; was = param;
              fprintf(stderr, "  [GL] transform context writes %s "
                      "(enabled %u times, disabled %u so far)\n",
                      param ? "ENABLED" : "DISABLED", n_on, n_off);
          } }
        break;
    case M_SET_CLIP_MIN: memcpy(&S.u.z_clip[0], &param, 4); break;
    case M_SET_CLIP_MAX:
        memcpy(&S.u.z_clip[1], &param, 4);
        { static int said; static float lo = -1, hi = -1;
          if (said < 6 && (S.u.z_clip[0] != lo || S.u.z_clip[1] != hi)) {
              said++; lo = S.u.z_clip[0]; hi = S.u.z_clip[1];
              fprintf(stderr, "  [GL] depth clip range %.1f .. %.1f "
                      "(the backend assumed 0 .. 16777215)\n",
                      (double)S.u.z_clip[0], (double)S.u.z_clip[1]);
          } }
        break;
    case 0x03BC: /* NV097_SET_LIGHT_ENABLE_MASK */
        if (param != S.light_enable_mask) { static int said;
            if (said++ < 6) fprintf(stderr, "  [LIGHT] enable mask %08X (light0 %u light1 %u light2 %u light3 %u) at draw %u\n",
                                    param, param & 3, (param >> 2) & 3, (param >> 4) & 3, (param >> 6) & 3, S.draws); }
        S.light_enable_mask = param; break;
    case 0x0298: /* NV097_SET_COLOR_MATERIAL */
        if (param != S.color_material) { static int said;
            if (said++ < 6) fprintf(stderr, "  [LIGHT] color material %08X at draw %u\n", param, S.draws); }
        S.color_material = param; break;
    case 0x0294: /* NV097_SET_LIGHT_CONTROL */
        if (param != S.light_control) { static int said;
            if (said++ < 6) fprintf(stderr, "  [LIGHT] light control %08X at draw %u\n", param, S.draws); }
        S.light_control = param; break;
    case 0x03A4: S.normalization = param; break;   /* NV097_SET_NORMALIZATION_ENABLE */
    case 0x03B8: S.specular_enable = param; break; /* NV097_SET_SPECULAR_ENABLE */
    case 0x03B4: memcpy(&S.material_alpha, &param, 4); break;
    case 0x0A10: case 0x0A14: case 0x0A18:
        memcpy(&S.scene_ambient[(method - 0x0A10) / 4], &param, 4);
        if (method == 0x0A18) { static int said; if (said++ < 4)
            fprintf(stderr, "  [LIGHT] scene ambient %.3f %.3f %.3f at draw %u\n", S.scene_ambient[0], S.scene_ambient[1], S.scene_ambient[2], S.draws); }
        break;
    case 0x03A8: case 0x03AC: case 0x03B0:
        memcpy(&S.material_emission[(method - 0x03A8) / 4], &param, 4);
        if (method == 0x03B0) { static int said; if (said++ < 4)
            fprintf(stderr, "  [LIGHT] material emission %.3f %.3f %.3f at draw %u\n", S.material_emission[0], S.material_emission[1], S.material_emission[2], S.draws); }
        break;
    case M_SET_LIGHTING_ENABLE:
        /* Whether the hardware T&L unit is lighting these vertices.
         *
         * If it is, the vertex colour in the stream is only part of the
         * answer and a shader that just passes it through renders the scene
         * at whatever the unlit term happens to be -- which for a title that
         * expects a sun is dark. This scene comes out about two and a half
         * times darker than the reference, so it is worth knowing which. */
        { static int said, was = -1;
          if (S.ff_lighting != (int)param || was < 0) {
              if (said++ < 4)
                  fprintf(stderr, "  [GL] fixed-function lighting %s\n",
                          param ? "ENABLED by the title" : "off");
              was = (int)param;
          } }
        S.ff_lighting = (int)param; break;
    case M_SET_SKIN_MODE:             S.ff_skin_mode = param; break;
    case M_SET_TRANSFORM_PROGRAM_LD:  S.prog_load = param; S.prog_sub = 0; break;
    case M_SET_TRANSFORM_PROGRAM_ST:  S.prog_start = param; break;
    case M_SET_TRANSFORM_CONSTANT_LD:
        /* A histogram of where a title loads constants, reported once. Which
         * slots are written, and how many at a time, is the only way to line
         * the uploads up against the slots the programs read. */
        { static uint32_t hist[192], counts[192]; static uint32_t prev; static int have;
          static int on = -2;
          if (on == -2) on = getenv("RECOMP_GL_CONST_MAP") ? 1 : 0;
          if (on) {
              if (have && prev < 192) {
                  hist[prev]++;
                  counts[prev] = S.const_load > prev ? S.const_load - prev : 0;
              }
              prev = param; have = 1;
              { static uint64_t n;
                if (++n % 20000 == 0) {
                    int k;
                    fprintf(stderr, "  [GL] constant loads seen:\n");
                    for (k = 0; k < 192; k++)
                        if (hist[k])
                            fprintf(stderr, "        load %3d: %u times, %u at a time\n",
                                    k, hist[k], counts[k]);
                } }
          } }
        S.const_load = param; S.const_sub = 0; break;

    case M_SET_BEGIN_END:
        if (param) {
            S.prim = param;
            S.idx_count = 0;
            S.inline_count = 0;
            S.draw_count = 0;
            S.consts_since_geom = 0;
            S.arrays_since_geom = 0;
            g_imm_n = 0;
            g_imm_dropped = 0;
        } else {
            ensure_ready();
            /* No array or inline vertices, but some sent one at a time. */
            if (g_imm_n && !S.idx_count && !S.inline_count) imm_draw();
            else do_draw();
            S.prim = 0;
            S.idx_count = 0;
            S.inline_count = 0;
            g_imm_n = 0;
        }
        break;
    case M_ARRAY_ELEMENT16:
        split_batch_if_state_moved();
        if (S.idx_count + 2 <= MAX_INDICES) {
            S.idx[S.idx_count++] = param & 0xFFFF;
            S.idx[S.idx_count++] = param >> 16;
        }
        break;
    case M_ARRAY_ELEMENT32:
        split_batch_if_state_moved();
        if (S.idx_count < MAX_INDICES) S.idx[S.idx_count++] = param;
        break;
    case M_DRAW_ARRAYS:
        split_batch_if_state_moved();
        {
        /*
         * A run of ranges, not one range.
         *
         * NV097_DRAW_ARRAYS carries a start and a count in a single dword,
         * and the count is eight bits -- at most 256 vertices. Anything
         * larger arrives as a run of these, non-incrementing, one after
         * another inside the same Begin/End, and the hardware concatenates
         * them into one primitive stream. This used to keep the last pair
         * and draw that alone, so a mesh sent as thirteen ranges came out as
         * its final 256 vertices and the other three thousand were simply
         * absent. With a triangle strip that does not leave a hole, it leaves
         * whatever the surviving range happens to span.
         *
         * Appending them to the same index list the element path uses puts
         * both kinds of batch through one piece of code, which is also the
         * only way a title that mixes them in one Begin/End comes out right.
         */
        uint32_t first = param & 0xFFFFFF;
        uint32_t count = ((param >> 24) & 0xFF) + 1;
        uint32_t q;
        /* RECOMP_GL_DRAWARRAYS_LAST=1 puts the old behaviour back -- keep the
         * last range and draw only that -- so the two can be photographed
         * from the same camera on the same run of the attract sequence.
         * "The picture changed" is not the same claim as "the picture is
         * better", and only a pair of frames settles which. */
        static int last_only = -1;
        if (last_only < 0) last_only = getenv("RECOMP_GL_DRAWARRAYS_LAST") ? 1 : 0;
        if (last_only) S.idx_count = 0;
        for (q = 0; q < count && S.idx_count < MAX_INDICES; q++)
            S.idx[S.idx_count++] = first + q;
        S.draw_first = first;
        S.draw_count = 0;
    }
        break;
    case M_INLINE_ARRAY:
        split_batch_if_state_moved();
        if (S.inline_count < MAX_INLINE) S.inline_buf[S.inline_count++] = param;
        break;

    case M_SET_DEPTH_TEST_ENABLE: S.depth_test = (int)param; break;
    case M_SET_DEPTH_MASK:        S.depth_mask = (int)param; break;
    /* Was never decoded. A title that draws a depth-only pass -- colour
     * writes off, to prime the depth buffer or to lay down an invisible
     * occluder -- had that pass painted in full here. */
    case M_SET_COLOR_MASK:        S.color_mask = param; break;
    case 0x032C: S.stencil_en = param;              /* SET_STENCIL_TEST_ENABLE */
        if (g_draw_log_flip >= 0 && (long)S.flips >= g_draw_log_flip
         && (long)S.flips < g_draw_log_flip + 6)
            fprintf(stderr, "[STW] f%ld after draw %u: stencil test %u (func %X ref %X ops %X/%X/%X)\n",
                    (long)S.flips, S.since_present, param, S.stencil_func, S.stencil_ref,
                    S.stencil_op[0], S.stencil_op[1], S.stencil_op[2]);
        break;
    case 0x0360: S.stencil_wmask = param; break;    /* SET_STENCIL_MASK (writes) */
    case 0x0364: S.stencil_func = param;            /* SET_STENCIL_FUNC */
        if (g_draw_log_flip >= 0 && (long)S.flips >= g_draw_log_flip
         && (long)S.flips < g_draw_log_flip + 6)
            fprintf(stderr, "[STW] f%ld after draw %u: stencil func %X (test %u)\n",
                    (long)S.flips, S.since_present, param, S.stencil_en);
        break;
    case 0x0368: S.stencil_ref = param; break;      /* SET_STENCIL_FUNC_REF */
    case 0x036C: S.stencil_fmask = param; break;    /* SET_STENCIL_FUNC_MASK */
    case 0x0370: S.stencil_op[0] = param; break;    /* SET_STENCIL_OP_FAIL */
    case 0x0374: S.stencil_op[1] = param; break;    /* SET_STENCIL_OP_ZFAIL */
    case 0x0378: S.stencil_op[2] = param; break;    /* SET_STENCIL_OP_ZPASS */
    case 0x0338: S.poly_off_fill = param; break;    /* SET_POLY_OFFSET_FILL_ENABLE */
    case 0x0384: memcpy(&S.poly_off_factor, &param, 4); break;  /* SET_POLYGON_OFFSET_SCALE_FACTOR */
    case 0x0388: memcpy(&S.poly_off_units, &param, 4); break;   /* SET_POLYGON_OFFSET_BIAS */
    case M_SET_DEPTH_FUNC:        S.depth_func = (int)param; break;
    case M_SET_ALPHA_TEST_ENABLE: S.alpha_test = (int)param; break;
    case M_SET_ALPHA_FUNC:        S.alpha_func = (int)param; break;
    case M_SET_ALPHA_REF:         S.alpha_ref = (float)param / 255.0f; break;
    case M_SET_BLEND_ENABLE:      S.blend_enable = (int)param; break;
    case M_SET_BLEND_FUNC_SFACTOR:S.blend_src = (int)param; break;
    case M_SET_BLEND_FUNC_DFACTOR:S.blend_dst = (int)param; break;
    case M_SET_CULL_FACE_ENABLE:  S.cull_enable = (int)param; break;
    case M_SET_CULL_FACE:         S.cull_face = (int)param; break;
    case M_SET_FRONT_FACE:        S.front_face = (int)param; break;

    case M_FLIP_STALL:
        /*
         * The end of a displayed frame, as opposed to the end of a pass.
         *
         * present() also runs on a full-surface clear, because a title that
         * stalls at the start of the next frame instead of the end of this one
         * would otherwise lose its last pass -- but a title with off-screen
         * targets clears several times a frame, and JSRF clears a scratch
         * surface twice per frame on top of the two display buffers it
         * alternates. Everything paced by "a frame" was therefore running
         * three times too often: capping the frame rate at 60 gave the title
         * 20 frames a second and stretched its boot sequence from twenty
         * seconds to two minutes.
         *
         * So the stall is the frame, when a title uses one, and the clear
         * stays the frame for a title that does not.
         */
        S.seen_flip_stall = 1;
        /* Which surface the window shows for this frame.
         *
         * The one the title has set up when it flips is the back buffer it
         * is ABOUT to draw: JSRF binds the next display buffer, flips, and
         * only then copies its off-screen frame into it. Showing that buffer
         * (what the window did while surfaces were bound as their methods
         * arrived) is two frames behind the game. The buffer the console is
         * about to scan out is the other one -- same size and format, and
         * drawn since the last flip. If there is no such buffer (a title
         * that draws straight into its back buffer), the bound one it is. */
        { int k, m = -1, best = -1;
          for (k = 0; k < SURF_CACHE; k++)
              if (g_surf[k].used && g_surf[k].offset == S.color_offset
               && g_surf[k].w == S.clip_w && g_surf[k].h == S.clip_h) { m = k; break; }
          if (m >= 0)
              for (k = 0; k < SURF_CACHE; k++) {
                  if (k == m || !g_surf[k].used) continue;
                  if (g_surf[k].w != g_surf[m].w || g_surf[k].h != g_surf[m].h) continue;
                  if (g_surf[k].fmt != (S.surf_format & 0xFFFFu)) continue;
                  if (g_surf[k].last_draw <= g_last_flip_draws) continue;
                  if (best < 0 || g_surf[k].last_draw > g_surf[best].last_draw) best = k;
              }
          g_present_slot = best >= 0 ? best : m;
          g_last_flip_draws = S.draws; }
        if (S.since_present) { fliplog("flip_stall"); present(1); }
        else {
            /* Nothing drawn since the last frame -- a black frame, a fade,
             * a frame spent loading -- but the title still flipped, and on
             * the console that flip waits for the display like any other.
             *
             * Unpaced, these ran at 500 to 1,500 a second. The DJ K scene
             * has seven of them in bursts, one each time it loads a line of
             * speech: its subtitles and animation count frames, its speech
             * plays in real time, so each burst put the picture up to ten
             * seconds ahead of the voice -- the subtitles showed the next
             * line while he was still saying this one, and his mouth moved
             * to the wrong words. Paced, the two stay together. */
            static int pace_nodraw = -1;   /* RECOMP_PACE_NODRAW=0: the old way, for A/B */
            if (pace_nodraw < 0) {
                const char *v = getenv("RECOMP_PACE_NODRAW");
                pace_nodraw = v ? atoi(v) : 1;
            }
            fliplog("stall(nodraw)");
            if (pace_nodraw) frame_pace();
        }
        break;
    default:
        /* RECOMP_GL_UNHANDLED=1: every method this backend ignores, ranked.
         *
         * "The characters are in the wrong place" has a short list of possible
         * causes and most of them are a method the title sends and this file
         * drops on the floor. Counting them is the only way to look at that
         * list rather than guess at it -- and the count matters as much as the
         * method, because a transform method sent once at startup and one sent
         * before every character is not the same suspect. */
        { static unsigned char seen[0x800];
          static uint32_t count[0x800];
          static int on = -1, reported;
          if (on < 0) on = getenv("RECOMP_GL_UNHANDLED") ? 1 : 0;
          if (on) {
              uint32_t k = (method >> 2) & 0x7FF;
              seen[k] = 1; count[k]++;
              if (++reported % 400000 == 0) {
                  int q, shown;
                  fprintf(stderr, "  [UNHANDLED] methods this backend "
                                  "ignores, most frequent first:\n");
                  for (shown = 0; shown < 14; shown++) {
                      int best = -1;
                      for (q = 0; q < 0x800; q++)
                          if (seen[q] == 1
                           && (best < 0 || count[q] > count[best])) best = q;
                      if (best < 0 || !count[best]) break;
                      fprintf(stderr, "  [UNHANDLED]   0x%04X  x%u\n",
                              (unsigned)(best << 2), count[best]);
                      seen[best] = 2;
                  }
                  for (q = 0; q < 0x800; q++) if (seen[q] == 2) seen[q] = 1;
                  fflush(stderr);
              }
          } }
        break;
    }
}

/* One cached render target, for the end-of-run report. */
static const char *surf_desc(int i)
{
    static char buf[8][48];
    if (i < 0 || i >= SURF_CACHE || !g_surf[i].used) return "";
    snprintf(buf[i], sizeof buf[i], " [%d]%08X %ux%u",
             i, g_surf[i].offset, g_surf[i].w, g_surf[i].h);
    return buf[i];
}

/* One number that must keep changing while the game is alive.
 *
 * Presents, not draws: a title that has stopped can still be issuing draws
 * into a buffer nobody shows, and a title that is merely slow still presents.
 * The freeze watchdog in the kernel bridge reads this and nothing else. */
unsigned long nv2a_gl_progress(void);
unsigned long nv2a_gl_progress(void) { return (unsigned long)S.flips; }

void nv2a_gl_report(void)
{
    if (!nv2a_gl_enabled()) return;
    vc_report("");
#if defined(NV2A_SDL_PRESENTER)
    if (g_presenter == 1) ps_line("the whole run", g_ps.on, g_ps.shown, g_ps.dropped);
#endif
    if (g_pace_st.frames) {
        uint64_t n = g_pace_st.iv[0] + g_pace_st.iv[1] + g_pace_st.iv[2]
                   + g_pace_st.iv[3] + g_pace_st.iv[4];
        fprintf(stderr, "  [PACE] %llu frames paced at the flip: %llu reached it late "
                "(worst %.1f ms), %llu let go late by the wait (worst %.1f ms), %llu restarts; "
                "flip to flip: under 14 ms %llu, "
                "a 60th of a second %llu, 19-30 ms %llu, 30-45 ms %llu, longer %llu (%llu)",
                (unsigned long long)g_pace_st.frames, (unsigned long long)g_pace_st.late,
                g_pace_st.late_max, (unsigned long long)g_pace_st.overslept,
                1000.0 * g_pace_st.overslept_max, (unsigned long long)g_pace_st.restarts,
                (unsigned long long)g_pace_st.iv[0], (unsigned long long)g_pace_st.iv[1],
                (unsigned long long)g_pace_st.iv[2], (unsigned long long)g_pace_st.iv[3],
                (unsigned long long)g_pace_st.iv[4], (unsigned long long)n);
#if defined(PACE_ABSOLUTE)
        if (g_pace_margin >= 0.0)
            fprintf(stderr, "; spinning the last %.2f ms (at most %.2f, widened %llu times; "
                    "a sleep ran up to %.2f ms past the spin's start)",
                    1000.0 * g_pace_margin, 1000.0 * g_pace_st.margin_max,
                    (unsigned long long)g_pace_st.widened, 1000.0 * g_pace_st.ran_over_max);
#endif
        fputc('\n', stderr);
    }
    if (g_ht.compiles)
        fprintf(stderr, "  [HITCH] %llu frames half again the period or longer; %llu programs "
                "built in %.0f ms in all (%.1f ms each, worst %.1f)\n",
                g_ht.hitches, g_ht.compiles, 1000.0 * g_ht.compile_s,
                1000.0 * g_ht.compile_s / (double)g_ht.compiles, 1000.0 * g_ht.compile_worst);
    fprintf(stderr,
        "  [GL] %u draws, %u triangles, %u clears, %u presents, "
        "%u programs compiled\n"
        "  [GL] %u batches split mid-block (%u of them because the vertex "
        "arrays moved, the rest the constants)\n"
        "  [GL] skipped: %u without a vertex program, %u without positions, "
        "%u shader failures\n"
        "  [GL] %u inline batches, %u array batches, %u with depth testing\n"
        "  [GL] %u surface switches, %u scene draws\n"
        "  [GL] transform mode: %u fixed-function, %u program (%u/%u other)\n"
        "  [GL] triangles by pipeline: %u fixed-function (%u of them lit), %u program\n"
        "  [GL] %u draws confined by a window clip rectangle\n"
        "  [GL] %u fixed-function draws asked for vertex blending\n"
        "  [GL] surfaces:%s%s%s%s%s%s%s%s\n"
        "  [GL] stage 0 not textured: %u disabled, %u no offset, %u no size; "
        "stage 1: %u/%u/%u\n",
        S.draws, S.tris, S.clears, S.flips, S.prog_switches,
        S.midbatch_splits, S.midbatch_array_splits,
        S.skipped_no_prog, S.skipped_no_pos, S.shader_fails,
        S.draws_inline, S.draws_array, S.draws_depth_on, S.surface_switches,
        S.draws_3d,
        S.draws_xf[0], S.draws_xf[1], S.draws_xf[2], S.draws_xf[3],
        S.tris_ff, S.tris_ff_lit, S.tris_pm, S.wclip_applied,
        S.draws_skinned,
        surf_desc(0), surf_desc(1), surf_desc(2), surf_desc(3),
        surf_desc(4), surf_desc(5), surf_desc(6), surf_desc(7),
        g_tex_dead[0][0], g_tex_dead[0][1], g_tex_dead[0][2],
        g_tex_dead[1][0], g_tex_dead[1][1], g_tex_dead[1][2]);
    if (g_dxt_made[0] || g_dxt_made[1])
        fprintf(stderr, "  [GL] DXT textures: %u handed to the device as they are, %u decoded\n",
                g_dxt_made[1], g_dxt_made[0]);
    if (G) G->report();
    fflush(stderr);
}


/* ---- the device ---------------------------------------------------------
 *
 * nv2a_backend.h draws the line between "what the NV2A was asked to do" and
 * "how this machine does it". Everything above is one copy of the pushbuffer
 * translation; below it there is an OpenGL implementation (nv2a_gfx_gl.c) and
 * a Metal one (nv2a_metal.m), which is what macOS is moving to and what makes
 * iOS possible at all.
 */

/* A build without the Metal backend still has to link. The header's rule is
 * that a backend which cannot be built at all returns NULL, and that is a
 * cheaper way to say "this platform has no Metal" than an #ifdef at every
 * place that might ask for one. */
#ifndef NV2A_HAVE_METAL
const Nv2aGfx *nv2a_gfx_metal(void) { return NULL; }
#endif

/*
 * Which one runs.
 *
 * RECOMP_BACKEND=metal or =opengl asks for one; otherwise, in a window, the
 * stored preference (Video > Renderer on a Mac: Metal unless OpenGL was
 * chosen), and without a window OpenGL. A Metal build that cannot bring a
 * device up gets OpenGL, and says so. Chosen once and kept, because a
 * backend that changed halfway through a frame would leave its surfaces and
 * programs behind in the other one.
 */
/* ---- the shader seed ------------------------------------------------------
 *
 * The shader cache (below) keeps each program as the text it was built from,
 * in this backend's language and for this translator only. So on a first
 * start -- a new player, a new device, or a release whose translator changed
 * -- every shader is built the moment the game first needs it, the game waits
 * for each, and the first minute of every area stutters: a phone takes tens
 * of milliseconds over a shader a Mac builds in one.
 *
 * So what each program was translated from is kept as well (the "states" of
 * nv2a_shader_cache.h: its combiner state, and its vertex program's microcode
 * or fixed-function description, with the blend recipes it was drawn with),
 * which is the same on every platform and for every translator. The engine
 * carries a list of them made by playing through the game (port/shader_seed.dat,
 * handed over by nv2a_set_shader_seed), and each machine keeps its own. At
 * start, every state that this backend's cache does not have yet is translated
 * and added to it -- once per list and translator, as states-<tag>.done says --
 * and the backend builds them ahead, as it does the programs of earlier runs:
 * Metal on two threads in the background from the start, OpenGL a few
 * milliseconds at a time on frames with little in them. Nothing waits for
 * them; a program still missing when the game asks is built then, as before.
 * JSRF_SHADER_SEED=<file> adds a list from a file (tests); RECOMP_SHADER_SEED=0
 * leaves the engine's own out (this machine's are still used). */
static const void *g_seed_data;
static size_t      g_seed_len;
void nv2a_set_shader_seed(const void *data, size_t len);
void nv2a_set_shader_seed(const void *data, size_t len) { g_seed_data = data; g_seed_len = len; }

static uint32_t seed_fnv(uint32_t h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

static void seed_import(const char *dir, const char *tag, int msl)
{
    char mark[1200], want[64], have[96] = "";
    FILE *mf;
    unsigned char *extra = NULL;
    size_t extra_len = 0;
    uint32_t fp = 2166136261u;
    int n, nlocal, done_local = -1, i, before = 0, after = 0, tried = 0, failed = 0;
    double t0 = mono_now();
    char *v, *f;

    nv2a_sc_states_open(dir);
    nlocal = nv2a_sc_states_local();
    { const char *e = getenv("JSRF_SHADER_SEED");
      FILE *sf = (e && *e) ? fopen(e, "rb") : NULL;
      if (sf) {
          long end = (!fseek(sf, 0, SEEK_END)) ? ftell(sf) : -1;
          rewind(sf);
          if (end > 0 && end < (64L << 20) && (extra = (unsigned char *)malloc((size_t)end)) != NULL &&
              fread(extra, 1, (size_t)end, sf) == (size_t)end) extra_len = (size_t)end;
          fclose(sf);
      } }
    { const char *e = getenv("RECOMP_SHADER_SEED");
      if (e && *e && !atoi(e)) g_seed_len = 0; }
    if (g_seed_data && g_seed_len) {
        nv2a_sc_states_add_blob(g_seed_data, g_seed_len);
        fp = seed_fnv(fp, g_seed_data, g_seed_len);
    }
    if (extra_len) {
        nv2a_sc_states_add_blob(extra, extra_len);
        fp = seed_fnv(fp, extra, extra_len);
    }
    free(extra);
    n = nv2a_sc_states_count();
    snprintf(want, sizeof want, "%08X:%lu", (unsigned)fp, (unsigned long)(g_seed_len + extra_len));
    snprintf(mark, sizeof mark, "%s/states-%s.done", dir, tag);
    if ((mf = fopen(mark, "r")) != NULL) {
        if (fgets(have, sizeof have, mf)) {
            char w[64]; int d;
            if (sscanf(have, "%63s %d", w, &d) == 2 && !strcmp(w, want)) done_local = d;
        }
        fclose(mf);
    }
    /* The machine's own come first (0..nlocal), the carried list's after. With
     * the same list translated already, only the machine's newer ones. */
    if (done_local > nlocal) done_local = -1;
    if (done_local == nlocal) {
        fprintf(stderr, "  [SHADERS] states: %d (%d from this machine), translated for %s already\n",
                n, nlocal, tag);
        return;
    }
    if (!nv2a_sc_open(dir, tag)) return;
    v = (char *)malloc(96 * 1024);
    f = (char *)malloc(96 * 1024);
    nv2a_sc_stats(&before, NULL, NULL);
    for (i = (done_local >= 0 ? done_local : 0); v && f && i < (done_local >= 0 ? nlocal : n); i++) {
        uint32_t id, vlen, plen, rc[64];
        const void *vd, *pd;
        int kind, ok = 0, k, nr;
        Nv2aPshState ps;
        if (!nv2a_sc_state_get(i, &id, &kind, &vd, &vlen, &pd, &plen)) continue;
        tried++;
        if (plen != sizeof ps) { failed++; continue; }        /* another engine's layout */
        memcpy(&ps, pd, sizeof ps);
        if (kind == 1 && vlen && vlen % 16 == 0 && vlen <= NV2A_VSH_MAX_INSNS * 16u) {
            Nv2aVshProgram vp;
            if (nv2a_vsh_decode((const uint32_t *)vd, (int)(vlen / 16), &vp) > 0 && (vp.outputs_written & 1u))
                ok = msl ? nv2a_vsh_emit_msl(&vp, v, 96 * 1024) : nv2a_vsh_emit_glsl(&vp, v, 96 * 1024);
        } else if (kind == 2 && vlen == sizeof(Nv2aVshFixed)) {
            Nv2aVshFixed ff;
            memcpy(&ff, vd, sizeof ff);
            ok = msl ? nv2a_vsh_emit_ff_msl(&ff, v, 96 * 1024) : nv2a_vsh_emit_ff_glsl(&ff, v, 96 * 1024);
        }
        if (ok) ok = msl ? nv2a_psh_emit_msl(&ps, f, 96 * 1024) : nv2a_psh_emit_glsl(&ps, f, 96 * 1024);
        if (!ok) { failed++; continue; }
        {
            uint64_t key = nv2a_sc_key(v, f);
            nv2a_sc_add(key, v, f);
            nr = nv2a_sc_state_recipes(id, rc, 64);
            for (k = 0; k < nr; k++) nv2a_sc_recipe_add(key, rc[k]);
        }
    }
    free(v);
    free(f);
    nv2a_sc_stats(&after, NULL, NULL);
    if ((mf = fopen(mark, "w")) != NULL) {
        fprintf(mf, "%s %d\n", want, nlocal);
        fclose(mf);
    }
    fprintf(stderr, "  [SHADERS] states: %d (%d from this machine, the rest carried in the engine); "
            "%d translated for %s in %.0f ms: %d programs new to its cache%s\n",
            n, nlocal, tried, tag, (mono_now() - t0) * 1000.0, after - before,
            failed ? " (some could not be: another engine's)" : "");
}

/* The shader cache (nv2a_shader_cache.h), in <JSRF_SAVE_DIR>/shaders: every
 * launcher sets the save folder, and a run without one has no cache. Its files
 * are named for the backend and for the translator -- a hash of a reference
 * pair of shaders generated now -- so that what an older engine built is
 * never loaded, only left behind. RECOMP_SHADER_CACHE=0 turns it off. */
static void shader_cache_start(void)
{
    const char *save = getenv("JSRF_SAVE_DIR"), *off = getenv("RECOMP_SHADER_CACHE");
    char dir[1024], tag[96];
    char *v, *f;
    uint64_t fp = 0;
    Nv2aVshFixed f0;
    Nv2aPshState p0;
    int msl;
    if (!G || !G->cache_open || (off && !atoi(off)) || !save || !*save) return;
    msl = (G->lang == NV2A_LANG_MSL);
    snprintf(dir, sizeof dir, "%s/shaders", save);
    v = (char *)malloc(96 * 1024);
    f = (char *)malloc(96 * 1024);
    memset(&f0, 0, sizeof f0);
    f0.inputs_read = 1;
    memset(&p0, 0, sizeof p0);
    if (v && f && (msl ? nv2a_vsh_emit_ff_msl(&f0, v, 96 * 1024) : nv2a_vsh_emit_ff_glsl(&f0, v, 96 * 1024))
              && (msl ? nv2a_psh_emit_msl(&p0, f, 96 * 1024) : nv2a_psh_emit_glsl(&p0, f, 96 * 1024)))
        fp = nv2a_sc_key(v, f);
    free(v);
    free(f);
    snprintf(tag, sizeof tag, "%s-%08X", G->name, (unsigned)(fp ^ (fp >> 32)));
    seed_import(dir, tag, msl);
    G->cache_open(dir, tag);
}

/* Anti-aliasing from the start: RECOMP_GL_SMAA and RECOMP_GL_MSAA, else the
 * settings -- the window's (smaa=, msaa=) or, on a Mac or an iPhone, the
 * app's preferences. Before the shader cache opens: the Metal backend builds
 * pipelines ahead from it, and a pipeline is made for one count of samples. */
extern int nv_window_pref_smaa(void) __attribute__((weak));
extern int nv_window_pref_msaa(void) __attribute__((weak));
static void aa_start(void)
{
    const char *vs = getenv("RECOMP_GL_SMAA"), *vm = getenv("RECOMP_GL_MSAA");
    int smaa = 0, msaa = 0;
#if defined(NV2A_GL_USE_SDL)
    smaa = sdl_cfg_get("smaa", 0) != 0;
    msaa = sdl_cfg_get("msaa", 0);
#else
    if (nv_window_pref_smaa) smaa = nv_window_pref_smaa() != 0;
    if (nv_window_pref_msaa) msaa = nv_window_pref_msaa();
#endif
    if (vs && *vs) smaa = atoi(vs) != 0;
    if (vm && *vm) msaa = atoi(vm);
    g_msaa_max = G->samples_max ? G->samples_max() : 1;
#if !defined(NV2A_GLES)
    g_msaa_live = g_is_gl && G->samples != NULL;
#endif
    if (G->samples) g_msaa_now = G->samples(msaa > 1 ? msaa : 1);
    else if (msaa > 1) fprintf(stderr, "  [GL] MSAA: not on the %s backend\n", G->name);
    g_msaa_want = g_msaa_now;
    if (G->target_smaa) g_smaa_now = g_smaa_want = smaa;
    else if (smaa) fprintf(stderr, "  [GL] SMAA: not on the %s backend\n", G->name);
    fprintf(stderr, "  [GL] anti-aliasing: SMAA %s, MSAA %dx (from %s; up to %dx here%s)\n",
            g_smaa_now ? "on" : "off", g_msaa_now,
            (vs && *vs) || (vm && *vm) ? "the environment" : "the settings", g_msaa_max,
            g_msaa_live ? ", changed while running" : ", changed at the next start");
}

static int gfx_init(uint32_t w, uint32_t h)
{
    const char *v = getenv("RECOMP_BACKEND");
    int want_metal = 0, metal_window = 0;
#if defined(NV2A_GL_USE_CGL)
    /* With a window, the window has already decided (it was made for one or
     * the other, from the same preference); without one, only the
     * environment can ask. */
    extern void *nv_window_metal_layer(void) __attribute__((weak));
    extern void  nv_window_use_gl(void) __attribute__((weak));
    if (getenv("RECOMP_WINDOW") && nv_window_is_open())
        g_windowed = 1;
    metal_window = nv_window_metal_layer && nv_window_metal_layer() != NULL;
#endif
    want_metal = (v && *v) ? !strcmp(v, "metal") : metal_window;
#if defined(NV2A_GL_USE_NONE)
    want_metal = 1;                   /* there is nothing else */
#endif
    if (want_metal) {
        const Nv2aGfx *m = nv2a_gfx_metal();
        if (m && m->init()) {
            G = m;
            g_is_gl = 0;
            fprintf(stderr, "  [GL] backend: %s\n", G->name);
            zrev_start();
            aa_start();
            shader_cache_start();
            return 1;
        }
        fprintf(stderr, "  [GL] Metal backend unavailable%s\n",
#if defined(NV2A_GL_USE_NONE)
                "; nothing to draw with"
#else
                ", using OpenGL"
#endif
                );
#if defined(NV2A_GL_USE_CGL)
        if (metal_window && nv_window_use_gl) nv_window_use_gl();
#endif
    }
    if (!gl_init(w, h)) return 0;     /* the context, then the device */
    fprintf(stderr, "  [GL] backend: %s\n", G->name);
    zrev_start();
    aa_start();
    shader_cache_start();
#if defined(NV2A_GL_USE_SDL)
    sdl_menu_update();                /* the menu's anti-aliasing, now known */
#endif
    return 1;
}

/* The present count -- the same number RECOMP_GL_DUMP_AFTER counts in -- for
 * diagnostics outside this file that want to act on one particular frame. */
uint32_t nv2a_gl_flips(void) { return S.flips; }

/* Test hooks for the game side (recomp_manual.c): capture a burst of frames
 * (every surface, as RECOMP_GL_DUMP_FB does) starting now, and log every
 * pass of the next few frames. */
void nv2a_gl_dump_burst(int shots, int every)
{
    g_dump_burst_every = every > 0 ? every : 1;
    g_dump_burst_left = shots;
    fprintf(stderr, "  [GL] dump burst: %d shots every %d frames from frame %u\n",
            shots, g_dump_burst_every, S.frames);
}
void nv2a_gl_trace_frames(int n)
{
    g_pass_trace_until = S.frames + (uint32_t)n;
    fprintf(stderr, "  [GL] pass trace for frames %u..%u\n", S.frames, g_pass_trace_until);
}
