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
#if defined(NV2A_GL_USE_CGL)
#  include <OpenGL/OpenGL.h>
#  include <OpenGL/gl3.h>
#  include <OpenGL/gl3ext.h>
#elif defined(NV2A_GL_USE_SDL)
#  include <SDL2/SDL.h>
#  if defined(__APPLE__)
#    include <OpenGL/gl3.h>
#    include <OpenGL/gl3ext.h>
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

/* The guest-thread lock, if this is linked into the runtime.
 *
 * Weak, because the GL backend also builds standalone for its own capture
 * test harness, where there is no guest and no lock. */
extern int  xbox_guest_lock_drop(void)   __attribute__((weak));
extern unsigned long g_vblank_count      __attribute__((weak));
extern void xbox_guest_lock_retake(int)  __attribute__((weak));

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern uint32_t  xbox_ContiguousAllocatedBytes(void);

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

#define MAX_INDICES   16384
#define MAX_INLINE    16384
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
    uint32_t control0;           /* NV097_SET_CONTROL0 */
    uint32_t zminmax;            /* NV097_SET_ZMIN_MAX_CONTROL */

    /* textures and fixed state */
    TexStage tex[NUM_STAGES];
    int      depth_test, depth_mask, depth_func;
    uint32_t color_mask;         /* NV097_SET_COLOR_MASK, 0x01010101 = all */
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
} S;

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
    unsigned n_draws, n_const_upload, n_prog_switch, n_tex_upload, n_tex_lookup;
    unsigned long vbo_bytes, unif_vec4;
} GlProfile;
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
        fprintf(stderr,
            "[GLPROF] %.0f draws/s | tex %4.1f%%  verts %4.1f%%  vbo %4.1f%%  "
            "uniforms %4.1f%%  glDraw %4.1f%%  state %4.1f%%  (rest %4.1f%%)\n",
            (g_prof.n_draws - prev.n_draws) / dt,
            100 * d.tex / dt, 100 * d.verts / dt, 100 * d.vbo / dt,
            100 * d.unif / dt, 100 * d.draw / dt, 100 * d.state / dt,
            100 * (dt - d.tex - d.verts - d.vbo - d.unif - d.draw - d.state) / dt);
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
            "%u texture lookups\n",
            (g_prof.vbo_bytes - prev.vbo_bytes) / 1024,
            g_prof.unif_vec4 - prev.unif_vec4,
            g_prof.n_const_upload - prev.n_const_upload,
            g_prof.n_prog_switch - prev.n_prog_switch,
            g_prof.n_tex_upload  - prev.n_tex_upload,
            g_prof.n_tex_lookup  - prev.n_tex_lookup);
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
#if defined(NV2A_GL_USE_CGL)
static CGLContextObj g_cgl_ctx;
#elif defined(NV2A_GL_USE_SDL)
static SDL_Window   *g_window;
static SDL_GLContext g_sdl_ctx;
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
#define SURF_CACHE 8
static struct {
    uint32_t offset, w, h;
    GLuint   fbo, tex, depth;
    int      used;
} g_surf[SURF_CACHE];
static int      g_cur_surf = -1;
static GLuint   g_fbo, g_color_tex, g_depth_rb;
static uint32_t g_fbo_w, g_fbo_h;
/* The raster's real size. Equal to the logical size unless RECOMP_GL_SCALE
 * asks for more -- see gl_scale(). Everything the title can observe uses the
 * logical size; only the pixels use this one. */
static uint32_t g_fbo_pw, g_fbo_ph;
static GLuint   g_vao, g_vbo;
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
    GLuint   prog;
    GLint    u_c, u_vpScale, u_vpOff, u_vpSurface, u_posMode, u_drawTint;
    GLint    u_ffMat, u_ffTexMat;   /* fixed-function transform, see S.ff_* */
    GLint    u_ffVpOff;
    GLint    u_litAmbient, u_zClip;
    GLint    u_tex[4], u_texScale[4], u_fogColor, u_alphaFunc, u_alphaRef;
    uint16_t inputs;
    uint8_t  uses_a0;   /* indexes the constant file through the address register */
    uint8_t  a0_input;  /* the v register the index comes from, 0xFF if unknown */
    char    *dis;       /* disassembly, kept only when a dump is armed */
} g_pcache[PROG_CACHE];
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

static GLuint g_cur_prog;
static int    g_cur_slot = -1;

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
    uint32_t layout;        /* which arrays are enabled, and where each sits */
    uint32_t const_mask;    /* attributes currently set as constants */
    int      prog_slot;
    GLuint   tex0;
    int      tex_enable;
    float    tex_sx, tex_sy;
    int      depth_test, depth_func, depth_mask;
    int      blend_enable, blend_src, blend_dst;
    int      alpha_func;
    float    alpha_ref;
    float    vp_scale[4], vp_off[4];
    int      valid;
} g_last;
static int g_last_cm_reset;   /* a clear forced the colour mask on */

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
    uint32_t last_draw;     /* when this entry was last looked up */
    GLuint   tex;
    int      used;
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
 * contiguous memory, is unaffected, which is exactly the split Julien
 * described: the buildings and the roads are right and everything else
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

/* Which backend is running -- OpenGL, or Metal. Defined at the foot of this
 * file, next to the table it returns; declared here because the front half
 * calls it long before then. */
static const Nv2aBackend *backend(void);

static uint32_t fnv(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 2166136261u;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

/* ---- context ------------------------------------------------------------ */

/* Everything that is the same whichever way the context was obtained. */
static int gl_init_common(uint32_t w, uint32_t h)
{
    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);
    glGenFramebuffers(1, &g_fbo);
    glGenTextures(1, &g_color_tex);
    glGenRenderbuffers(1, &g_depth_rb);
    (void)w; (void)h;
    return 1;
}

#if defined(NV2A_GL_USE_CGL)

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
    if (getenv("RECOMP_WINDOW") && nv_window_is_open())
        g_windowed = 1;

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
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    /* macOS gives 3.3 core only when asked for forward-compatible; without
     * this the request quietly returns a 2.1 context and every shader in the
     * game fails to compile. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,
                        SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    g_window = SDL_CreateWindow("Jet Set Radio Future",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                (int)(w ? w : 640), (int)(h ? h : 480),
                                SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_window) {
        fprintf(stderr, "  [GL] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 0;
    }
    g_sdl_ctx = SDL_GL_CreateContext(g_window);
    if (!g_sdl_ctx) {
        fprintf(stderr, "  [GL] no 3.3 core context: %s\n", SDL_GetError());
        return 0;
    }
    SDL_GL_MakeCurrent(g_window, g_sdl_ctx);
    SDL_GL_SetSwapInterval(0);       /* never wait for vblank while profiling */

    fprintf(stderr, "  [GL] %s | %s | GLSL %s\n",
            (const char *)glGetString(GL_VERSION),
            (const char *)glGetString(GL_RENDERER),
            (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));

    return gl_init_common(w, h);
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
    if (slot < 0) slot = 0;                       /* evict the oldest */

    if (!g_surf[slot].used || g_surf[slot].offset != offset
     || g_surf[slot].w != w || g_surf[slot].h != h) {
        if (!g_surf[slot].fbo) {
            glGenFramebuffers(1, &g_surf[slot].fbo);
            glGenTextures(1, &g_surf[slot].tex);
            glGenRenderbuffers(1, &g_surf[slot].depth);
        }
        glBindTexture(GL_TEXTURE_2D, g_surf[slot].tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                     (GLsizei)(w * gl_scale()), (GLsizei)(h * gl_scale()), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindRenderbuffer(GL_RENDERBUFFER, g_surf[slot].depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24,
                              (GLsizei)(w * gl_scale()),
                              (GLsizei)(h * gl_scale()));
        glBindFramebuffer(GL_FRAMEBUFFER, g_surf[slot].fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, g_surf[slot].tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, g_surf[slot].depth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "  [GL] framebuffer incomplete at %ux%u\n", w, h);
            return;
        }
        g_surf[slot].used = 1;
        g_surf[slot].offset = offset;
        g_surf[slot].w = w; g_surf[slot].h = h;
        fprintf(stderr, "  [GL] surface %08X: new %ux%u render target\n",
                offset, w, h);
        g_last.tex0 = 0;      /* the bind above disturbed unit 0 */
    }

    if (g_cur_surf != slot) {
        glBindFramebuffer(GL_FRAMEBUFFER, g_surf[slot].fbo);
        glViewport(0, 0, (GLsizei)(w * gl_scale()), (GLsizei)(h * gl_scale()));
        g_cur_surf = slot;
    }
    g_fbo = g_surf[slot].fbo;
    g_color_tex = g_surf[slot].tex;
    g_depth_rb = g_surf[slot].depth;
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
static GLuint surface_texture(uint32_t offset, uint32_t w, uint32_t h)
{
    int i;
    for (i = 0; i < SURF_CACHE; i++)
        if (g_surf[i].used && g_surf[i].offset == offset
         && (!w || g_surf[i].w == w) && (!h || g_surf[i].h == h)
         && i != g_cur_surf)
            return g_surf[i].tex;
    return 0;
}

/* ---- shaders ------------------------------------------------------------ */

/* RECOMP_GL_SOLID: paint every fragment opaque magenta and ignore depth.
 * When a frame comes back empty this says, in one run, whether the geometry
 * is reaching the surface at all -- which is a different bug from it being
 * shaded to black or hidden behind the depth test. */
static const char *FRAG_SOLID =
"#version 330 core\n"
"in vec4 oD0; in vec4 oD1; in vec4 oT0; in vec4 oT1; in vec4 oT2; in vec4 oT3;\n"
"in float oFogC;\n"
"uniform sampler2D tex0; uniform int texEnable; uniform vec2 texScale;\n"
"uniform int alphaFunc; uniform float alphaRef;\n"
"out vec4 frag;\n"
"uniform vec4 drawTint;\n"
"void main() { frag = drawTint; }\n";

static GLuint compile(GLenum stage, const char *src, const char *what)
{
    GLuint s = glCreateShader(stage);
    GLint ok = 0;
    char *patched = NULL;

    /* RECOMP_GL_ZVIEW turns every fragment shader into a depth view.
     *
     * The define has to land after the #version line, which GLSL requires to
     * come first, so it is spliced in here rather than prepended. The emitter
     * already carries the epilogue behind an #ifdef; this is only the switch.
     */
    { static int zview = -1;
      if (zview < 0) zview = getenv("RECOMP_GL_ZVIEW") ? 1 : 0;
      if (zview && stage == GL_FRAGMENT_SHADER && src
       && strncmp(src, "#version", 8) == 0) {
          const char *nl = strchr(src, '\n');
          if (nl) {
              static const char def[] = "#define NV2A_ZVIEW_ON 1\n";
              size_t head = (size_t)(nl - src) + 1;
              size_t len = strlen(src);
              patched = (char *)malloc(len + sizeof def);
              if (patched) {
                  memcpy(patched, src, head);
                  memcpy(patched + head, def, sizeof def - 1);
                  memcpy(patched + head + sizeof def - 1, src + head,
                         len - head + 1);
                  src = patched;
              }
          }
      } }

    /* RECOMP_GL_Z01: take the program's depth as the 0..1 it already is,
     * rather than dividing it by a clip range it never used. Same splice,
     * the vertex stage this time. */
    { static int z01 = -1;
      if (z01 < 0) z01 = getenv("RECOMP_GL_Z01") ? 1 : 0;
      if (z01 && stage == GL_VERTEX_SHADER && src
       && strncmp(src, "#version", 8) == 0) {
          const char *nl = strchr(src, '\n');
          if (nl) {
              static const char d2[] = "#define NV2A_Z01 1\n";
              size_t head = (size_t)(nl - src) + 1;
              size_t len = strlen(src);
              char *p2 = (char *)malloc(len + sizeof d2);
              if (p2) {
                  memcpy(p2, src, head);
                  memcpy(p2 + head, d2, sizeof d2 - 1);
                  memcpy(p2 + head + sizeof d2 - 1, src + head, len - head + 1);
                  free(patched);
                  src = patched = p2;
              }
          }
      } }

    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        GLsizei n = 0;
        glGetShaderInfoLog(s, sizeof log, &n, log);
        fprintf(stderr, "  [GL] %s failed to compile:\n%.*s\n", what, (int)n, log);
        if (getenv("RECOMP_GL_DUMP_SHADER"))
            fprintf(stderr, "----- source -----\n%s\n------------------\n", src);
        glDeleteShader(s);
        free(patched);
        return 0;
    }
    free(patched);
    return s;
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
        S.psh.tex_bound[i] = (uint8_t)(live ? 1 : 0);
        /* A8 and the swizzled A8: colour comes back as zero from GL and as
         * one from the console. */
        S.psh.tex_alpha_only[i] = (uint8_t)((t->color == 0x19 || t->color == 0x1F)
                                            ? 1 : 0);
    }
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

/* Where this program keeps the things the backend sets every draw.
 *
 * Shared by the two compile paths -- a translated program and the
 * fixed-function transform link against the same combiner shader and take
 * the same uniforms, and a name looked up in one place and not the other is
 * a uniform that silently stays at zero. */
static void pcache_locate_uniforms(int slot)
{
    GLuint prog = g_pcache[slot].prog;
    int i;
    g_pcache[slot].u_c          = glGetUniformLocation(prog, "c");
    g_pcache[slot].u_vpScale    = glGetUniformLocation(prog, "vpScale");
    g_pcache[slot].u_vpOff      = glGetUniformLocation(prog, "vpOff");
    g_pcache[slot].u_vpSurface  = glGetUniformLocation(prog, "vpSurface");
    g_pcache[slot].u_posMode    = glGetUniformLocation(prog, "posMode");
    g_pcache[slot].u_drawTint   = glGetUniformLocation(prog, "drawTint");
    g_pcache[slot].u_ffMat      = glGetUniformLocation(prog, "ffMat");
    g_pcache[slot].u_ffTexMat   = glGetUniformLocation(prog, "ffTexMat");
    g_pcache[slot].u_ffVpOff    = glGetUniformLocation(prog, "ffVpOff");
    g_pcache[slot].u_litAmbient = glGetUniformLocation(prog, "litAmbient");
    g_pcache[slot].u_zClip      = glGetUniformLocation(prog, "zClip");
    for (i = 0; i < 4; i++) {
        char nm[16];
        snprintf(nm, sizeof nm, "tex%d", i);
        g_pcache[slot].u_tex[i] = glGetUniformLocation(prog, nm);
        snprintf(nm, sizeof nm, "texScale%d", i);
        g_pcache[slot].u_texScale[i] = glGetUniformLocation(prog, nm);
    }
    g_pcache[slot].u_fogColor   = glGetUniformLocation(prog, "fogColor");
    g_pcache[slot].u_alphaFunc  = glGetUniformLocation(prog, "alphaFunc");
    g_pcache[slot].u_alphaRef   = glGetUniformLocation(prog, "alphaRef");
}

/* Which inputs a fixed-function batch actually has.
 *
 * The emitted shader declares only the arrays the batch supplies; an
 * attribute the title left as a set constant still reads correctly through
 * fetch(), which copies the constant into the packed vertex, so "has an
 * array" and "has a value" are the same question here. Position is always
 * present -- do_draw() rejects a batch without it before this is reached. */
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
    f->lit = (uint8_t)(S.ff_lighting ? 1 : 0);
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

static int ff_program_for_current(void)
{
    Nv2aVshFixed f;
    uint32_t hash;
    char *vsrc, *fsrc;
    GLuint vs, fs, prog;
    GLint ok = 0;
    int slot;

    ff_describe(&f);
    psh_sync_textures();
    /* A namespace of its own: hashing the descriptor alone could collide with
     * a translated program's microcode hash, and the two are not
     * interchangeable. */
    hash = 0xF1EDF1EDu;   /* a namespace of its own */
    hash = hash * 16777619u + fnv(&f, sizeof f);
    hash = hash * 16777619u + fnv(&S.psh, sizeof S.psh);
    { int hit = pcache_find(hash);
      if (hit >= 0) return hit; }
    slot = pcache_alloc(hash);

    vsrc = (char *)malloc(64 * 1024);
    if (!vsrc) return -1;
    if (!nv2a_vsh_emit_ff_glsl(&f, vsrc, 64 * 1024)) {
        free(vsrc); S.shader_fails++; return -1;
    }
    if (getenv("RECOMP_GL_DUMP_FF")) {
        static int shown;
        if (shown++ < 2)
            fprintf(stderr, "----- fixed-function GLSL (inputs %04X, "
                    "texmat %X) -----\n%s\n",
                    f.inputs_read, f.tex_matrix, vsrc);
    }
    vs = compile(GL_VERTEX_SHADER, vsrc, "fixed-function transform");
    free(vsrc);
    if (!vs) { S.shader_fails++; return -1; }

    fsrc = (char *)malloc(96 * 1024);
    if (!fsrc) { glDeleteShader(vs); return -1; }
    if (!nv2a_psh_emit_glsl(&S.psh, fsrc, 96 * 1024)) {
        free(fsrc); glDeleteShader(vs); S.shader_fails++; return -1;
    }
    fs = compile(GL_FRAGMENT_SHADER, fsrc, "combiners (fixed-function draw)");
    free(fsrc);
    if (!fs) { glDeleteShader(vs); S.shader_fails++; return -1; }

    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(prog, sizeof log, NULL, log);
        fprintf(stderr, "  [GL] fixed-function link failed: %s\n", log);
        glDeleteProgram(prog);
        S.shader_fails++;
        return -1;
    }
    if (g_pcache[slot].used) glDeleteProgram(g_pcache[slot].prog);
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
    pcache_locate_uniforms(slot);
    S.prog_switches++;
    return slot;
}

static int program_for_current(void)
{
    uint32_t hash, start;
    Nv2aVshProgram vp;
    char *vsrc, *fsrc;
    GLuint vs, fs, prog;
    GLint ok = 0;
    int slot, len;

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
    len = nv2a_vsh_decode(S.prog + start * 4,
                          (int)(NV2A_VSH_MAX_INSNS - start), &vp);
    if (len <= 0) { S.skipped_no_prog++; return -1; }
    /* No write to oPos means every vertex lands on the origin. That is not a
     * program the title expects to draw with -- it is the state before one has
     * been uploaded -- and compiling it wastes a slot and a shader. */
    if (!(vp.outputs_written & 1u)) { S.skipped_no_prog++; return -1; }

    psh_sync_textures();
    /* One key for both halves: a title reuses a vertex program with several
     * combiner setups and vice versa, and caching on either alone hands back
     * a shader that is half right. */
    hash = fnv(S.prog + start * 4, (size_t)len * 16);
    hash = hash * 16777619u + fnv(&S.psh, sizeof S.psh);
    { int hit = pcache_find(hash);
      if (hit >= 0) return hit; }
    slot = pcache_alloc(hash);

    vsrc = (char *)malloc(96 * 1024);
    if (!vsrc) return -1;
    if (!nv2a_vsh_emit_glsl(&vp, vsrc, 96 * 1024)) {
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
    if (getenv("RECOMP_GL_DUMP_VSH")) {
        char dis[16384];
        if (nv2a_vsh_disasm(&vp, dis, sizeof dis))
            fprintf(stderr, "----- NV2A program (%d slots, hash %08X) -----\n%s",
                    len, hash, dis);
        fprintf(stderr, "----- GLSL -----\n%s\n", vsrc);
    }

    vs = compile(GL_VERTEX_SHADER, vsrc, "translated vertex program");
    free(vsrc);
    if (!vs) { S.shader_fails++; return -1; }
    fsrc = (char *)malloc(96 * 1024);
    if (!fsrc) { glDeleteShader(vs); return -1; }
    if (!nv2a_psh_emit_glsl(&S.psh, fsrc, 96 * 1024)) {
        free(fsrc); glDeleteShader(vs); S.shader_fails++; return -1;
    }
    if (getenv("RECOMP_GL_DUMP_MSL")) {
        char *mv = (char *)malloc(96 * 1024), *mf = (char *)malloc(96 * 1024);
        if (mv && mf && nv2a_vsh_emit_msl(&vp, mv, 96 * 1024)
                     && nv2a_psh_emit_msl(&S.psh, mf, 96 * 1024))
            dump_msl_pair(hash, mv, mf);
        free(mv); free(mf);
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
       || (at3d >= 0 && (long)S.draws_3d >= at3d && psh_shots < 4)) {
        int k;
        psh_shots++;
        fprintf(stderr, "----- combiners: %u stage(s), control %08X, "
                        "final %08X/%08X -----\n",
                (unsigned)(S.psh.control & 0xF), S.psh.control,
                S.psh.final_abcd, S.psh.final_efg);
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
    fs = compile(GL_FRAGMENT_SHADER, fsrc ? fsrc : FRAG_SOLID,
                 "combiner shader");
    free(fsrc);
    if (!fs) { glDeleteShader(vs); S.shader_fails++; return -1; }

    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        char log[4096]; GLsizei n = 0;
        glGetProgramInfoLog(prog, sizeof log, &n, log);
        fprintf(stderr, "  [GL] link failed:\n%.*s\n", (int)n, log);
        glDeleteProgram(prog);
        S.shader_fails++;
        return -1;
    }

    if (g_pcache[slot].used && g_pcache[slot].prog)
        glDeleteProgram(g_pcache[slot].prog);
    g_pcache[slot].used   = 1;
    g_pcache[slot].hash   = hash;
    g_pcache[slot].last_draw = S.draws;
    g_pcache[slot].prog   = prog;
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
    pcache_locate_uniforms(slot);
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

static uint32_t decode_texel(uint32_t f, const uint8_t *p)
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

static GLuint upload_texture_inner(const TexStage *t);
static GLuint upload_texture(const TexStage *t)
{
    GLuint r; PROF_T0();
    g_prof.n_tex_lookup++;
    r = upload_texture_inner(t);
    PROF_ADD(tex);
    return r;
}
static GLuint upload_texture_inner(const TexStage *t)
{
    uint32_t va = resolve(t->offset);
    const uint8_t *src = guest(va);
    uint32_t w = t->width, h = t->height, i, hash, key;
    GLuint tex;
    int slot = -1;

    if (!w || !h || w > 4096 || h > 4096) return 0;

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

    /* Cache on where it came from and what it looks like. Hashing the whole
     * image would be exact but costs a full read per draw; hashing a sparse
     * sample catches a title that reuses one address for many textures, which
     * is the case that actually matters. */
    {
        uint32_t bytes = fmt_is_dxt(t->color)
                       ? ((w + 3) / 4) * ((h + 3) / 4)
                         * d3d8_format_dxt_block_bytes(t->color)
                       : w * h * (fmt_is_swz(t->color)
                                  ? linear_bpp(swz_to_linear(t->color))
                                  : linear_bpp(t->color));
        uint32_t step = bytes > 4096 ? bytes / 64 : 64;
        hash = t->color * 2654435761u;
        for (i = 0; i < bytes && step; i += step)
            hash = hash * 31u + src[i];
        hash = hash * 31u + bytes;
    }
    key = (t->offset ^ (w << 16) ^ (h << 4) ^ t->color) * 2246822519u;
    for (i = 0; i < TEX_CACHE; i++) {
        uint32_t k = (key + i) % TEX_CACHE;
        if (g_tcache[k].used && g_tcache[k].offset == t->offset
         && g_tcache[k].format == t->color && g_tcache[k].w == w
         && g_tcache[k].h == h && g_tcache[k].hash == hash) {
            g_tcache[k].last_draw = S.draws;
            return g_tcache[k].tex;
        }
        if (!g_tcache[k].used) { slot = (int)k; break; }
    }
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

    if (g_tcache[slot].used && g_tcache[slot].tex)
        glDeleteTextures(1, &g_tcache[slot].tex);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (fmt_is_dxt(t->color)) {
        /* Decoded here rather than handed to the driver compressed.
         *
         * Passing the blocks straight through is faster and exact, but it
         * makes every texture in the game depend on the host advertising
         * S3TC -- and when it does not, glCompressedTexImage2D fails, the
         * texture stays undefined, and every surface samples black. That is
         * a whole-screen failure caused by a driver capability, which is not
         * a trade worth making for an upload that happens once per texture
         * and is cached afterwards. */
        uint8_t *rgba = (uint8_t *)malloc((size_t)w * h * 4);
        uint32_t x, y;

        if (!rgba) { glDeleteTextures(1, &tex); return 0; }
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                uint32_t argb = 0xFF000000u;
                d3d8_dxt_decode_texel(src, t->color, x, y, w, &argb);
                rgba[((size_t)y * w + x) * 4 + 0] = (uint8_t)(argb >> 16);
                rgba[((size_t)y * w + x) * 4 + 1] = (uint8_t)(argb >> 8);
                rgba[((size_t)y * w + x) * 4 + 2] = (uint8_t)argb;
                rgba[((size_t)y * w + x) * 4 + 3] = (uint8_t)(argb >> 24);
            }
        tex_alpha_census(rgba, w, h, t->color, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        free(rgba);
    } else {
        uint32_t lin = fmt_is_swz(t->color) ? swz_to_linear(t->color) : t->color;
        uint32_t bpp = linear_bpp(lin);
        uint8_t *rgba;
        uint32_t x, y;

        if (!bpp) { glDeleteTextures(1, &tex); return 0; }
        rgba = (uint8_t *)malloc((size_t)w * h * 4);
        if (!rgba) { glDeleteTextures(1, &tex); return 0; }
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                const uint8_t *p;
                uint32_t argb;
                if (fmt_is_swz(t->color))
                    p = src + (size_t)swizzle_offset(x, y, w, h) * bpp;
                else
                    p = src + (size_t)y * (t->pitch ? t->pitch : w * bpp)
                            + (size_t)x * bpp;
                argb = decode_texel(lin, p);
                rgba[((size_t)y * w + x) * 4 + 0] = (uint8_t)(argb >> 16);
                rgba[((size_t)y * w + x) * 4 + 1] = (uint8_t)(argb >> 8);
                rgba[((size_t)y * w + x) * 4 + 2] = (uint8_t)argb;
                rgba[((size_t)y * w + x) * 4 + 3] = (uint8_t)(argb >> 24);
            }
        }
        tex_alpha_census(rgba, w, h, t->color, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        free(rgba);
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
        { static uint32_t only; static int oinit;
          if (!oinit) { const char *o = getenv("RECOMP_GL_DUMP_TEX_OFF");
                        oinit = 1; only = o ? (uint32_t)strtoul(o, 0, 16) : 0; }
        if (pfx && shots < 24 && S.draws_3d >= after3d
            && (!only || t->offset == only)) {
            uint8_t *back = (uint8_t *)malloc((size_t)w * h * 4);
            if (back) {
                char nm[128]; FILE *f; uint32_t x, y, row, sz, nz = 0;
                uint8_t hdr[54];
                glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, back);
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
        GLenum mn, mg;
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
         * world goes soft and slightly wrong at once. Julien called it
         * straight away: worse than before.
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
         */
        if (minf >= 3 && minf <= 6) {
            if (want_mips) {
                glGenerateMipmap(GL_TEXTURE_2D);
                mn = minf == 3 ? GL_NEAREST_MIPMAP_NEAREST
                   : minf == 4 ? GL_LINEAR_MIPMAP_NEAREST
                   : minf == 5 ? GL_NEAREST_MIPMAP_LINEAR
                               : GL_LINEAR_MIPMAP_LINEAR;
                /* Anisotropy only makes sense alongside a chain, and it was
                 * the other half of what made distant surfaces look smeared
                 * rather than sharp. It goes with the chain. */
                { static float aniso = -1.0f;
                  if (aniso < 0.0f) {
                      const char *v = getenv("RECOMP_GL_ANISO");
                      GLfloat cap = 1.0f;
                      glGetFloatv(0x84FF, &cap);
                      aniso = v ? (float)atof(v) : 4.0f;
                      if (aniso > (float)cap) aniso = (float)cap;
                      if (aniso < 1.0f) aniso = 1.0f;
                  }
                  if (aniso > 1.0f)
                      glTexParameterf(GL_TEXTURE_2D, 0x84FE, aniso); }
            } else {
                mn = (minf == 3 || minf == 5) ? GL_NEAREST : GL_LINEAR;
            }
        } else {
            mn = minf == 1 ? GL_NEAREST : GL_LINEAR;
        }
        mg = magf == 1 ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)mn);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)mg);
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
     * what Julien saw appear. Clamping is wrong in the same small way it was
     * wrong before, and wrong invisibly rather than wrong in black. */
    { static const GLint wrap_of[8] = {
          GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE,
          GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE,
          GL_CLAMP_TO_EDGE };
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                      wrap_of[t->addr & 7u]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                      wrap_of[(t->addr >> 8) & 7u]); }

    g_prof.n_tex_upload++;
    g_tcache[slot].used = 1;
    g_tcache[slot].offset = t->offset; g_tcache[slot].format = t->color;
    g_tcache[slot].w = w; g_tcache[slot].h = h; g_tcache[slot].pitch = t->pitch;
    g_tcache[slot].hash = hash; g_tcache[slot].tex = tex;
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

static GLenum gl_prim(uint32_t p, uint32_t *fan_expand)
{
    *fan_expand = 0;
    switch (p) {
    case PRIM_POINTS:         return GL_POINTS;
    case PRIM_LINES:          return GL_LINES;
    case PRIM_LINE_LOOP:      return GL_LINE_LOOP;
    case PRIM_LINE_STRIP:     return GL_LINE_STRIP;
    case PRIM_TRIANGLES:      return GL_TRIANGLES;
    case PRIM_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
    case PRIM_TRIANGLE_FAN:   return GL_TRIANGLE_FAN;
    case PRIM_POLYGON:        return GL_TRIANGLE_FAN;
    /* Quads have no core-profile equivalent, so they are expanded into two
     * triangles per quad on the way to the buffer rather than emulated with
     * a geometry shader. */
    case PRIM_QUADS:          *fan_expand = 1; return GL_TRIANGLES;
    case PRIM_QUAD_STRIP:     *fan_expand = 2; return GL_TRIANGLES;
    default:                  return 0;
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
    static long want = -2;
    if (want == -2) { const char *v = getenv("RECOMP_GL_DRAW_LOG");
                      want = v ? atol(v) : -1; }
    if (want >= 0 && g_draw_log_flip < 0 && (long)S.since_present >= want)
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
     * the order glReadPixels returns -- so writing straight through looks
     * like the obviously correct thing to do, and produces an upside-down
     * picture. What is in the framebuffer is already mirrored: the title
     * hands over D3D screen coordinates with y increasing downward, GL
     * rasterises them into a buffer whose y increases upward, and both the
     * window and the guest readback undo that on the way out. */
    for (y = 0; y < ph; y++) {
        const uint8_t *r = buf + (size_t)(ph - 1 - y) * pw * 4;
        uint8_t pad[3] = { 0, 0, 0 };
        for (x = 0; x < pw; x++) fwrite(r + x * 4, 1, 3, f);
        fwrite(pad, 1, row - pw * 3, f);
    }
    fclose(f);
}

/* Write the current framebuffer out as a BMP, for looking at.
 *
 * Reads back rather than sharing the present path's buffer: the two are asked
 * at different moments -- one at the end of a frame, one straight after a
 * draw -- and the whole value of the second is that it is not the first. */
static void dump_fbo(const char *prefix, int *counter, int limit)
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
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    /* Colour first, always. */
    glReadPixels(0, 0, (GLsizei)pw, (GLsizei)ph, GL_BGRA,
                 GL_UNSIGNED_BYTE, buf);
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
              glReadPixels(0, 0, (GLsizei)pw, (GLsizei)ph,
                           GL_DEPTH_COMPONENT, GL_FLOAT, zb);
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
static void do_draw(void)
{
    static float *vbuf;
    static uint32_t vbuf_cap;
    uint32_t n, i, k, out_n, fan_expand;
    GLenum mode;
    int slot;
    int inl = (S.inline_count != 0);
    uint32_t stride_floats = NUM_ATTRS * 4;
    /* The vertex layout this draw actually needs. See where it is built. */
    struct { const uint8_t *base; uint32_t stride; uint8_t type, size, idx; }
            live[NUM_ATTRS];
    int      nlive = 0;
    int      attr_at[NUM_ATTRS];      /* float offset in the vertex, or -1 */
    uint32_t const_mask = 0;          /* read, but supplied as a constant */

    if (!g_ready || !S.prim) return;

    mode = gl_prim(S.prim, &fan_expand);
    if (!mode) return;

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
        if (!vsize) return;
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
    if (n < 1) return;

    if (!S.attr[0].size) { S.skipped_no_pos++; return; }
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

    slot = program_for_current();
    if (slot < 0) return;

    /* RECOMP_GL_ONLY_A0=1 draws only the batches that pick their transform
     * out of the constant palette; =0 draws only the rest.
     *
     * ONLY_XF above splits by transform pipeline, which for this title turns
     * out not to be the interesting line -- almost everything in the 3D
     * scene is programmable, so it separates the 2D screens from the world
     * and nothing else. The line that matters is the one Julien drew: the
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
    if (!out_n) return;

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

    if (vbuf_cap < out_n * stride_floats) {
        vbuf_cap = out_n * stride_floats + 4096;
        free(vbuf);
        vbuf = (float *)malloc(vbuf_cap * sizeof(float));
        if (!vbuf) { vbuf_cap = 0; return; }
    }

    { PROF_T0();
    for (k = 0; k < out_n; k++) {
        uint32_t src_i;
        float *dst = &vbuf[k * stride_floats];
        int j;
        if (fan_expand == 1) {
            static const uint32_t q[6] = { 0, 1, 2, 2, 3, 0 };
            src_i = S.idx[(k / 6) * 4 + q[k % 6]];
        } else if (fan_expand == 2) {
            static const uint32_t q[6] = { 0, 1, 3, 3, 2, 0 };
            src_i = S.idx[(k / 6) * 2 + q[k % 6]];
        } else {
            src_i = S.idx[k];
        }
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
    PROF_ADD(verts); }

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
            {
                GLint fb = 0, vp4[4] = {0,0,0,0}, cp = 0, va = 0;
                glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
                glGetIntegerv(GL_VIEWPORT, vp4);
                glGetIntegerv(GL_CURRENT_PROGRAM, &cp);
                glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &va);
                fprintf(stderr, "  [GL] context: fbo=%d (ours %u) viewport=%d,%d %dx%d "
                        "program=%d vao=%d depth=%d blend=%d cull=%d scissor=%d\n",
                        fb, g_fbo, vp4[0], vp4[1], vp4[2], vp4[3], cp, va,
                        glIsEnabled(GL_DEPTH_TEST), glIsEnabled(GL_BLEND),
                        glIsEnabled(GL_CULL_FACE), glIsEnabled(GL_SCISSOR_TEST));
            }
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

    { PROF_T0();
      glBindVertexArray(g_vao);
      glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
      glBufferData(GL_ARRAY_BUFFER,
                   (GLsizeiptr)out_n * stride_floats * sizeof(float),
                   vbuf, GL_STREAM_DRAW);
      PROF_ADD(vbo);
      g_prof.vbo_bytes += (unsigned long)out_n * stride_floats * sizeof(float); }
    /* The buffer is re-created every draw, so the pointers have to be re-
     * specified only when the layout changes -- the offsets and stride are a
     * property of the layout, not of the buffer. The layout is now the set of
     * attributes that came from arrays and where each one sits, so the key has
     * to be both of those and not just what the program reads: the same
     * program drawn twice with a different attribute supplied as a constant is
     * a different layout. */
    {
        uint32_t layout_key = 0, j;
        for (j = 0; j < (uint32_t)nlive; j++)
            layout_key = layout_key * 17u + (live[j].idx + 1u);
        layout_key = layout_key * 65599u + const_mask;
        if (!g_last.valid || g_last.layout != layout_key) {
            for (i = 0; i < NUM_ATTRS; i++) {
                if (attr_at[i] >= 0) {
                    glEnableVertexAttribArray(i);
                    glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE,
                        (GLsizei)(stride_floats * sizeof(float)),
                        (const void *)(uintptr_t)((uint32_t)attr_at[i] * sizeof(float)));
                } else {
                    glDisableVertexAttribArray(i);
                }
            }
            g_last.layout = layout_key;
            g_last.const_mask = ~const_mask;   /* force the constants below */
        }
        /* An attribute the program reads with no array behind it is one value
         * for the whole draw. GL has a per-attribute constant for exactly
         * this, so it costs one call rather than four floats in every vertex. */
        if (const_mask && g_last.const_mask != const_mask) {
            for (i = 0; i < NUM_ATTRS; i++)
                if (const_mask & (1u << i))
                    glVertexAttrib4fv(i, S.const_attr[i]);
            g_last.const_mask = const_mask;
        }
    }

    if (g_cur_slot != slot) {
        g_prof.n_prog_switch++;
        glUseProgram(g_pcache[slot].prog);
        g_cur_prog = g_pcache[slot].prog;
        g_cur_slot = slot;
        /* Uniforms belong to the program object, so a switch invalidates
         * everything remembered about them. */
        S.consts_dirty = 1;
        g_last.tex_enable = -1;
        g_last.alpha_func = -1;
        g_last.vp_scale[0] = 1e30f;
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
      if (g_pcache[slot].u_posMode >= 0)
          glUniform1i(g_pcache[slot].u_posMode, use); }
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
      if (tc && g_pcache[slot].u_drawTint >= 0 && out_n >= 3
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
          glUniform4f(g_pcache[slot].u_drawTint,
                      clipped > ok ? 0.95f : 0.1f,
                      clipped > ok ? 0.1f  : 0.9f, 0.1f, 1.0f);
          goto tint_done;
      } }
    if (g_pcache[slot].u_drawTint >= 0) {
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
                glUniform4f(g_pcache[slot].u_drawTint, 0.9f, 0.1f, 0.1f, 1.0f);
            else
                glUniform4f(g_pcache[slot].u_drawTint, 0.35f, 0.35f, 0.4f, 1.0f);
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
                    glUniform4f(g_pcache[slot].u_drawTint,
                                1.0f, 1.0f, 1.0f, 1.0f);
                else
                    glUniform4f(g_pcache[slot].u_drawTint,
                                3.0f, 0.2f, 0.2f, 1.0f);
            } else if (tintxf == 2) {
                if (fixed) glUniform4f(g_pcache[slot].u_drawTint,
                                       1.0f, 1.0f, 1.0f, 1.0f);
                else       glUniform4f(g_pcache[slot].u_drawTint,
                                       3.0f, 0.2f, 0.2f, 1.0f);
            } else
            glUniform4f(g_pcache[slot].u_drawTint,
                        fixed ? 0.25f : 1.0f, fixed ? 1.0f : 0.25f,
                        0.25f, 1.0f);
        } else if (drawid) {
            uint32_t i = S.since_present;
            glUniform4f(g_pcache[slot].u_drawTint,
                        (i & 0xFF) / 255.0f, ((i >> 8) & 0xFF) / 255.0f,
                        0.0f, 1.0f);
        } else {
            glUniform4f(g_pcache[slot].u_drawTint, 1.0f, 0.0f, 1.0f, 1.0f);
        }
    }
    tint_done: ;
    /* The fixed-function transform, for the shaders that use it. Uploaded
     * whenever it changed or the program did, since uniforms belong to the
     * program object. */
    if (g_pcache[slot].u_zClip >= 0)
        glUniform2f(g_pcache[slot].u_zClip, S.u.z_clip[0], S.u.z_clip[1]);
    if (g_pcache[slot].u_litAmbient >= 0) {
        /* RECOMP_GL_FF_LIGHT=<f>: how much light to add to a batch the title
         * asked the hardware to light. 0 keeps today's behaviour exactly. */
        static float amt = -1.0f;
        if (amt < 0.0f) { const char *v = getenv("RECOMP_GL_FF_LIGHT");
                          amt = v ? (float)atof(v) : 0.0f; }
        glUniform1f(g_pcache[slot].u_litAmbient, amt);
    }
    if (g_pcache[slot].u_ffMat >= 0) {
        glUniform4fv(g_pcache[slot].u_ffMat, 4, &S.u.ff_mat[0][0]);
        if (g_pcache[slot].u_ffVpOff >= 0)
            glUniform4fv(g_pcache[slot].u_ffVpOff, 1, S.vp_off_reg);
        if (g_pcache[slot].u_ffTexMat >= 0)
            glUniform4fv(g_pcache[slot].u_ffTexMat, 16, &S.u.ff_texmat[0][0]);
    }
    if (S.consts_dirty) {
        PROF_T0();
        glUniform4fv(g_pcache[slot].u_c, NV2A_VSH_NUM_CONSTS, &S.u.c[0][0]);
        S.consts_dirty = 0;
        PROF_ADD(unif);
        g_prof.n_const_upload++;
        g_prof.unif_vec4 += NV2A_VSH_NUM_CONSTS;
    }
    if (memcmp(g_last.vp_scale, S.u.vp_scale, sizeof S.u.vp_scale)
     || memcmp(g_last.vp_off, S.u.vp_off, sizeof S.u.vp_off)) {
        /* The viewport, whenever it changes.
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
        glUniform4fv(g_pcache[slot].u_vpScale, 1, S.u.vp_scale);
        glUniform4fv(g_pcache[slot].u_vpOff, 1, S.u.vp_off);
        glUniform2f(g_pcache[slot].u_vpSurface, (float)g_fbo_w, (float)g_fbo_h);
        memcpy(g_last.vp_scale, S.u.vp_scale, sizeof S.u.vp_scale);
        memcpy(g_last.vp_off, S.u.vp_off, sizeof S.u.vp_off);
    }

    /* Bind every stage the combiner shader samples.
     *
     * Coordinates are normalised for a swizzled or compressed texture and in
     * texels for a linear one, so the scale that turns them into GL's [0,1]
     * belongs to the texture and not to the draw. Sampling a surface the
     * title rendered into a moment ago is render-to-texture, and the live
     * framebuffer is the answer rather than the bytes at that address. */
    {
        int st;
        for (st = 0; st < 4; st++) {
            GLuint tex = 0;
            float sx = 1.0f, sy = 1.0f;

            if (S.psh.tex_bound[st]) {
                tex = surface_texture(S.tex[st].offset, 0, 0);
                if (!tex) tex = upload_texture(&S.tex[st]);
                if (tex && !fmt_is_swz(S.tex[st].color)
                        && !fmt_is_dxt(S.tex[st].color)) {
                    sx = 1.0f / (float)S.tex[st].width;
                    sy = 1.0f / (float)S.tex[st].height;
                }
            }
            glActiveTexture((GLenum)(GL_TEXTURE0 + st));
            glBindTexture(GL_TEXTURE_2D, tex);
            if (g_pcache[slot].u_tex[st] >= 0)
                glUniform1i(g_pcache[slot].u_tex[st], st);
            if (g_pcache[slot].u_texScale[st] >= 0)
                glUniform2f(g_pcache[slot].u_texScale[st], sx, sy);
        }
        g_last.tex0 = 0;    /* unit 0 is no longer what the cache remembers */
    }
    if (g_pcache[slot].u_fogColor >= 0)
        glUniform4f(g_pcache[slot].u_fogColor,
                    (S.fog_color & 0xFF) / 255.0f,          /* R: low byte */
                    ((S.fog_color >> 8) & 0xFF) / 255.0f,
                    ((S.fog_color >> 16) & 0xFF) / 255.0f,  /* B */
                    ((S.fog_color >> 24) & 0xFF) / 255.0f);
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
            glUniform1i(g_pcache[slot].u_alphaFunc, af);
            glUniform1f(g_pcache[slot].u_alphaRef, S.alpha_ref);
            g_last.alpha_func = af; g_last.alpha_ref = S.alpha_ref;
        }
    }

    /* Everything about this draw that is neither a shader nor a uniform,
     * handed over in the console's own terms. What OpenGL does with it is in
     * glb_state; what Metal does is different enough that the two cannot
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
        }
        rs.blend_enable = S.blend_enable;
        rs.blend_src    = S.blend_src;
        rs.blend_dst    = S.blend_dst;
        rs.alpha_test   = S.alpha_test;
        rs.cull_enable  = S.cull_enable;
        rs.color_mask   = S.color_mask;
        rs.zclamp       = S.zminmax;
        rs.w_buffer     = S.psh.w_buffer;
        rs.cull_face    = S.cull_face;
        rs.front_face   = S.front_face;
        /* The window clip. A rectangle that covers the surface is the same as
         * none and costs a state change per draw to say so, which is why the
         * test is here and not in a backend. */
        { static int off = -1;
          if (off < 0) off = getenv("RECOMP_GL_NOWCLIP") ? 1 : 0;
          if (!off && S.wclip_valid && S.wclip_type == 0) {
              uint32_t x0 =  S.wclip_h[0]        & 0xFFFu;
              uint32_t x1 = (S.wclip_h[0] >> 16) & 0xFFFu;
              uint32_t y0 =  S.wclip_v[0]        & 0xFFFu;
              uint32_t y1 = (S.wclip_v[0] >> 16) & 0xFFFu;
              if (x1 > x0 && y1 > y0
               && (x0 > 0 || y0 > 0 || x1 + 1 < g_fbo_w || y1 + 1 < g_fbo_h)) {
                  rs.scissor_enable = 1;
                  rs.scissor[0] = x0; rs.scissor[1] = y0;
                  rs.scissor[2] = x1; rs.scissor[3] = y1;
                  S.wclip_applied++;
              }
          } }
        backend()->state(&rs);
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
          { uint32_t q; float zlo = 1e30f, zhi = -1e30f; int on = 0;
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
            }
            if (on)
                fprintf(stderr, "  [Z] batch of %u: %d on screen, "
                        "z/w %.0f..%.0f  -> depth %.6f..%.6f  "
                        "(%.4f%% of the range)\n",
                        out_n, on, (double)zlo, (double)zhi,
                        (double)(zlo/16777215.0f), (double)(zhi/16777215.0f),
                        (double)(100.0f*(zhi-zlo)/16777215.0f));
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
    { PROF_T0(); glDrawArrays(mode, 0, (GLsizei)out_n); PROF_ADD(draw); }
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
      static int hunt_tris = -2, hunt_at = -1;
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
      if (hunt_tris > 0 && hunt_at < 0 && S.frame_tris >= (uint32_t)hunt_tris) {
          hunt_at = (int)S.frames + 1;   /* the next whole frame, from its start */
          fprintf(stderr, "  [DARK] frame %u had %u triangles; hunting frame %d\n",
                  S.frames, S.frame_tris, hunt_at);
      }
      if ((hunt >= 0 && (int)S.frames == hunt)
       || (hunt_at >= 0 && (int)S.frames == hunt_at)) {
          static uint8_t small[64 * 48 * 4];
          double m = 0.0; int q;
          GLuint keep_read;
          glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, (GLint *)&keep_read);
          glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
          glPixelStorei(GL_PACK_ALIGNMENT, 1);
          /* One row every ph/48, one column every pw/64: a thumbnail without
           * a downscale pass. glReadPixels cannot stride, so read a strip. */
          glReadPixels(0, (GLint)((g_fbo_ph ? g_fbo_ph : g_fbo_h) / 2),
                       64, 48, GL_RGBA, GL_UNSIGNED_BYTE, small);
          glBindFramebuffer(GL_READ_FRAMEBUFFER, keep_read);
          for (q = 0; q < 64 * 48; q++)
              m += small[q*4] + small[q*4+1] + small[q*4+2];
          m /= (64.0 * 48.0 * 3.0);
          if (hunt_frame == 0) prev_mean = m;
          hunt_frame++;
          /* Skip the start of the frame: a title paints its background with
           * a full-screen quad, and that legitimately takes the average from
           * the previous frame's picture down to nothing. What is wanted is a
           * draw that darkens a frame already being built. */
          if (hunt_frame > 30 && prev_mean > 8.0 && m < prev_mean * 0.80)
              fprintf(stderr, "  [DARK] draw %u of this frame: mean %.1f -> "
                      "%.1f  xf=%u  verts=%u  blend=%d(%04X/%04X) "
                      "depth=%d/%04X alpha=%d prog=%d\n",
                      hunt_frame, prev_mean, m, S.xf_mode, out_n,
                      S.blend_enable, S.blend_src, S.blend_dst,
                      S.depth_test, S.depth_func, S.alpha_test, slot);
          prev_mean = m;
          if (hunt_frame % 50 == 0)
              fprintf(stderr, "  [DARK]   after %u draws: mean %.1f\n",
                      hunt_frame, m);
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
     * Julien's description split the bug cleanly in two: buildings and roads
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
     * early on reads as "nothing draws" for the rest of the run. Check the
     * first few draws and say which one broke. */
    {
        static int checked;
        if (checked < 8) {
            GLenum e = glGetError();
            if (e != GL_NO_ERROR) {
                fprintf(stderr, "  [GL] glGetError 0x%04X after draw "
                        "(prim=%u verts=%u fbo=%ux%u status=0x%04X)\n",
                        e, S.prim, out_n, g_fbo_w, g_fbo_h,
                        glCheckFramebufferStatus(GL_FRAMEBUFFER));
                checked++;
            }
        }
    }
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
          if (atsince >= 0) {
              if ((long)S.since_present == atsince && shown < 6
               && (at3d < 0 || (long)S.draws_3d >= at3d))
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
        if (pfx && (S.draws % 4000) == 3999) dump_fbo(pfx, &shots, 12);
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
        uint32_t v; int c;
        for (v = 0; v < out_n; v++)
            for (c = 0; c < 2; c++) {
                float t = vbuf[(size_t)v * stride_floats + 9 * 4 + c];
                if (t < t0lo[c]) t0lo[c] = t;
                if (t > t0hi[c]) t0hi[c] = t;
            }
        fprintf(stderr, "[DRAW] f%ld %4u verts %5u prim %u prog %2d "
                "vp %s tex0 %08X %ux%u f%02X en%d tex1 en%d "
                "blend %d %04X/%04X depth %d/%d ctl %08X final %08X/%08X "
                "| xf%u a9 off=%08X t%u s%u st%u u[%.3f..%.3f] v[%.3f..%.3f] "
                "in%04X\n",
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
                g_pcache[slot].inputs);
    }

    /* Record what this draw was, for the order dump. */
    {
        uint32_t k = g_ring_n % DRAW_RING, v;
        float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f;
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

/* The OpenGL half of a clear: the values are already decoded and the
 * rectangle is already in the title's coordinates, so all that is left is
 * OpenGL's own spelling of "wipe this rectangle". */
static void glb_clear(unsigned mask, const float rgba[4], float depth,
                      uint32_t stencil, const uint32_t rect[4])
{
    GLbitfield bits = 0;
    (void)stencil;

    if (!g_ready) return;
    if (mask & 1u) {
        glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]);
        /* The console's clear picks its channels from the CLEAR_SURFACE mask
         * and ignores COLOR_MASK; GL's honours the write mask. Force it on
         * for the clear and let the next draw re-apply the title's. */
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        g_last_cm_reset = 1;
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if (mask & 2u) {
        glClearDepth((double)depth);
        glDepthMask(GL_TRUE);
        g_last.depth_mask = -1;   /* the clear forced it; re-apply next draw */
        bits |= GL_DEPTH_BUFFER_BIT;
    }
    if (!bits) return;

    /* A clear is bounded by the clear rectangle, not by the surface.
     *
     * Titles clear sub-rectangles constantly -- a letterbox bar, a panel
     * behind some text, a region being re-drawn -- and a backend that ignores
     * the rectangle wipes the whole frame every time one arrives. The symptom
     * is a completely black frame with a healthy draw count, which reads as
     * "nothing is being rasterised" and sends you looking in the wrong place.
     * The rectangle arrives in surface coordinates, which run top-down, so it
     * has to be flipped into GL's bottom-up scissor space. */
    if (rect) {
        uint32_t sc = gl_scale();
        GLint x = (GLint)(rect[0] * sc);
        GLint y = (GLint)((g_fbo_h - rect[3]) * sc);
        GLsizei w = (GLsizei)((rect[2] - rect[0]) * sc);
        GLsizei h = (GLsizei)((rect[3] - rect[1]) * sc);
        if (y < 0) { h += y; y = 0; }
        glEnable(GL_SCISSOR_TEST);
        glScissor(x, y, w > 0 ? w : 0, h > 0 ? h : 0);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    glClear(bits);
    glDisable(GL_SCISSOR_TEST);
}

static void do_clear(uint32_t param)
{
    unsigned mask = 0;
    float rgba[4] = { 0, 0, 0, 1 };
    float depth = 1.0f;
    uint32_t rect[4], *rectp = NULL;

    if (!g_ready) return;

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
            a = ((S.clear_color >> 24) & 0xFF) / 255.0f;
            r = ((S.clear_color >> 16) & 0xFF) / 255.0f;
            g = ((S.clear_color >> 8)  & 0xFF) / 255.0f;
            b = ( S.clear_color        & 0xFF) / 255.0f;
            break;
        }
        rgba[0] = r; rgba[1] = g; rgba[2] = b; rgba[3] = a;
        mask |= 1u;
    }
    if (param & 0x03) {
        /* Z is the top 24 bits of the combined z/stencil clear value. */
        depth = (float)((double)(S.clear_zstencil >> 8) / 16777215.0);
        mask |= 2u;
    }
    if (!mask) return;

    /* A clear is bounded by the clear rectangle, not by the surface -- see
     * glb_clear, which applies it. An empty rectangle means the whole
     * surface, which the interface spells as no rectangle at all. */
    if (S.clear_x1 > S.clear_x0 && S.clear_y1 > S.clear_y0) {
        rect[0] = S.clear_x0; rect[1] = S.clear_y0;
        rect[2] = S.clear_x1; rect[3] = S.clear_y1;
        rectp = rect;
    }
    backend()->clear(mask, rgba, depth, S.clear_zstencil & 0xFFu, rectp);
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
    if (budget <= 0) { prev_draws = S.draws; return; }
    budget--;
    fprintf(stderr, "  [FLIP] %-10s surface=%08X %ux%u  draws=%u\n",
            why, S.color_offset, S.clip_w, S.clip_h, S.draws - prev_draws);
    prev_draws = S.draws;
    fflush(stderr);
}

static void present(int is_frame)
{
    uint32_t va, bpp, x, y;
    uint8_t *dst;

    if (!g_ready || !g_fbo_w || !g_fbo_h || !S.color_offset) return;
    if (is_frame) { S.frames++; prof_report(); }

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
    if (is_frame) {
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
             */
            now = mono_now();
            if (deadline == 0.0 || deadline < now - 1.0) deadline = now;
            deadline += period;
            if (now > deadline + period * 4.0) {
                /* Genuinely behind -- the emulation could not keep up. Start
                 * again from here rather than sprinting to catch up on frames
                 * whose moment has passed. */
                deadline = now + period;
            } else if (now < deadline) {
                int held = xbox_guest_lock_drop ? xbox_guest_lock_drop() : 0;
                double spin = deadline - 0.0012;
                if (now < spin) {
                    struct timespec ts;
                    double wait = spin - now;
                    ts.tv_sec = (time_t)wait;
                    ts.tv_nsec = (long)((wait - (double)ts.tv_sec) * 1e9);
                    /* Wait the way the console waits: with the rest of the
                     * guest still running. This is guest code holding the lock
                     * that serialises guest threads, so sleeping here without
                     * giving it up stops the loader and the sound thread too. */
                    nanosleep(&ts, NULL);
                }
                while (mono_now() < deadline) { }
                if (held && xbox_guest_lock_retake) xbox_guest_lock_retake(held);
            }
        }
    }

#if defined(NV2A_GL_USE_CGL)
    /* Put the finished frame on screen.
     *
     * The frame is built in an offscreen framebuffer whichever way this is
     * running, so showing it is one blit -- and the y flip in the blit is the
     * same one the BMP writer does, because GL's framebuffer counts rows from
     * the bottom and both a window and a bitmap want them from the top. */
    if (g_windowed && is_frame) {
        /* Read the finished frame and hand it to the window.
         *
         * Blitting straight into the window's back buffer works -- measurably,
         * the pixels arrive -- and a layer-backed view never presents what
         * another thread drew into its context. So the frame crosses as
         * pixels and the main thread draws it. One readback a frame. */
        static uint8_t *shot;
        static uint32_t cap;
        uint32_t pw = g_fbo_pw ? g_fbo_pw : g_fbo_w;
        uint32_t ph = g_fbo_ph ? g_fbo_ph : g_fbo_h;
        uint32_t need = pw * ph * 4;
        if (cap < need) { free(shot); shot = (uint8_t *)malloc(need); cap = need; }
        if (shot) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, (GLsizei)pw, (GLsizei)ph,
                         GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, shot);
            glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
            /* At the frame's real size: the window is happy to show more
             * pixels than the title thinks it drew. */
            nv_window_submit_frame(shot, (int)pw, (int)ph);
        }
        nv_window_present();
        if (nv_window_should_close()) {
            fprintf(stderr, "  [GL] window closed; stopping\n");
            nv2a_gl_report();
            exit(0);
        }
    }
#endif

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
        int dump_due;

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

        draw_log_latch();
        dump_due = pfx && dump_shots < 24
                && (dump_after_tris ? S.frame_tris >= dump_after_tris
                    : dump_3d ? S.draws_3d >= dump_3d
                    : S.flips >= dump_after)
                && (S.flips % dump_every) == 0;

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
             * and one image per surface says which. */
            int k;
            GLuint keep = g_fbo;
            for (k = 0; k < SURF_CACHE; k++) {
                char pf[256];
                static int per_surf[SURF_CACHE];
                if (!g_surf[k].used) continue;
                snprintf(pf, sizeof pf, "%s_s%d_", pfx, k);
                glBindFramebuffer(GL_FRAMEBUFFER, g_surf[k].fbo);
                { uint32_t sw = g_fbo_w, sh = g_fbo_h;
                  g_fbo_w = g_surf[k].w; g_fbo_h = g_surf[k].h;
                  dump_fbo(pf, &per_surf[k], 24);
                  g_fbo_w = sw; g_fbo_h = sh; }
            }
            glBindFramebuffer(GL_FRAMEBUFFER, keep);
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
 * the roads have been right all along, which is the split Julien described.
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
    if (!gl_init(S.clip_w, S.clip_h)) { g_failed = 1; return; }
    g_ready = 1;
    backend()->surface(S.color_offset, S.clip_w, S.clip_h, gl_scale());
}

void nv2a_gl_method(uint32_t method, uint32_t param)
{
    if (!nv2a_gl_enabled() || g_failed) return;

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
        if (S.prog_load < NV2A_VSH_MAX_INSNS)
            S.prog[S.prog_load * 4 + S.prog_sub] = param;
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
        if (S.const_load < NV2A_VSH_NUM_CONSTS)
            memcpy(&S.u.c[S.const_load][S.const_sub], &param, 4);
        if (++S.const_sub == 4) { S.const_sub = 0; S.const_load++; }
        S.consts_dirty = 1;
        S.consts_since_geom = 1;
        return;
    }
    if (method >= M_SET_FOG_PARAMS && method < M_SET_FOG_PARAMS + 12) {
        memcpy(&S.fog_params[(method - M_SET_FOG_PARAMS) / 4], &param, 4);
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
        /* Not kept: nothing reads them. The composite matrix covers the
         * rigid case, which is every fixed-function batch this title draws --
         * measured, zero of them enable skinning. Writing them into the
         * constant file was pure damage. */
        (void)param;
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
    if (method >= M_SET_VERTEX_DATA4F && method < M_SET_VERTEX_DATA4F + NUM_ATTRS * 16) {
        uint32_t i = (method - M_SET_VERTEX_DATA4F) / 16;
        memcpy(&S.const_attr[i][((method - M_SET_VERTEX_DATA4F) / 4) & 3], &param, 4);
        return;
    }
    if (method >= M_SET_VERTEX_DATA4UB && method < M_SET_VERTEX_DATA4UB + NUM_ATTRS * 4) {
        uint32_t i = (method - M_SET_VERTEX_DATA4UB) / 4;
        S.const_attr[i][0] = ((param >>  0) & 0xFF) / 255.0f;
        S.const_attr[i][1] = ((param >>  8) & 0xFF) / 255.0f;
        S.const_attr[i][2] = ((param >> 16) & 0xFF) / 255.0f;
        S.const_attr[i][3] = ((param >> 24) & 0xFF) / 255.0f;
        return;
    }
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
        uint32_t li = (method - 0x1000) / 0x80, off = (method - 0x1000) % 0x80, c = (off & 0xC) / 4; float fv;
        memcpy(&fv, &param, 4);
        switch (off & ~0xCu) {
        case 0x00: if (c < 3) S.light_amb[li][c] = fv; break;
        case 0x0C: if (c < 3) S.light_dif[li][c] = fv; break;   /* 0x0C..0x14 */
        case 0x18: if (c < 3) S.light_spc[li][c] = fv; break;   /* 0x18..0x20 */
        case 0x24: if (c == 0) S.light_range[li] = fv; break;
        case 0x28: if (c < 3) S.light_half[li][c] = fv; break;  /* 0x28..0x30 */
        case 0x34: if (c < 3) S.light_dir[li][c] = fv; break;   /* 0x34..0x3C */
        case 0x40: if (c < 3) S.light_spot_falloff[li][c] = fv; break;
        case 0x4C: if (c < 4) S.light_spot_dir[li][c] = fv; break;
        case 0x5C: if (c < 3) S.light_pos[li][c] = fv; break;
        case 0x68: if (c < 3) S.light_att[li][c] = fv; break;
        default: break;
        }
        if (li == 0 && off == 0x3C) { static int said; if (said++ < 4)
            fprintf(stderr, "  [LIGHT] light0 amb %.2f %.2f %.2f dif %.2f %.2f %.2f spc %.2f %.2f %.2f dir %.3f %.3f %.3f half %.3f %.3f %.3f at draw %u\n",
                    S.light_amb[0][0], S.light_amb[0][1], S.light_amb[0][2], S.light_dif[0][0], S.light_dif[0][1], S.light_dif[0][2],
                    S.light_spc[0][0], S.light_spc[0][1], S.light_spc[0][2], S.light_dir[0][0], S.light_dir[0][1], S.light_dir[0][2],
                    S.light_half[0][0], S.light_half[0][1], S.light_half[0][2], S.draws); }
        return;
    }
    if (method >= 0x181C && method < 0x181C + 16) {   /* NV097_SET_EYE_POSITION */
        memcpy(&S.eye_position[(method - 0x181C) / 4], &param, 4); return;
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
        if (g_ready) backend()->surface(S.color_offset, S.clip_w, S.clip_h, gl_scale());
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
        if (g_ready) backend()->surface(param, S.clip_w, S.clip_h, gl_scale());
        break;
    case M_SET_SURFACE_ZETA_OFFSET:  S.zeta_offset = param; break;

    case M_SET_COMBINER_CONTROL:     S.psh.control = param; break;
    case M_SET_COMBINER_SPECFOG_CW0: S.psh.final_abcd = param; break;
    case M_SET_COMBINER_SPECFOG_CW1: S.psh.final_efg = param; break;
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
        } else {
            ensure_ready();
            do_draw();
            S.prim = 0;
            S.idx_count = 0;
            S.inline_count = 0;
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
        if (S.since_present) { fliplog("flip_stall"); present(1); }
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

void nv2a_gl_report(void)
{
    if (!nv2a_gl_enabled()) return;
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
    fflush(stderr);
}


/* ---- the backend interface ---------------------------------------------
 *
 * nv2a_backend.h draws the line between "what the NV2A was asked to do" and
 * "how this machine does it". Everything above is one copy of the pushbuffer
 * translation; below it there is an OpenGL implementation, which is this
 * file, and a Metal one, which is what makes iOS possible at all.
 *
 * The table fills in one entry at a time and the front half calls only the
 * entries that are filled: an operation is routed through here once BOTH
 * backends implement it, and until then the front half keeps calling the
 * OpenGL code directly. The stubs below are what an entry looks like before
 * its turn -- they say so rather than crashing, because "the screen went
 * black halfway through a refactor" is a much worse afternoon than a line of
 * text naming the operation nobody has moved yet.
 */
static void glb_unrouted(const char *what)
{
    static const char *said[16]; static int n; int i;
    for (i = 0; i < n; i++) if (said[i] == what) return;
    if (n < 16) said[n++] = what;
    fprintf(stderr, "  [GL] backend entry '%s' is not routed yet\n", what);
}

static int glb_init(void) { return 1; }   /* the context is already up */

static void glb_surface(uint32_t offset, uint32_t w, uint32_t h, uint32_t scale)
{
    /* The scale is a property of this backend today -- gl_scale() reads the
     * environment once and every size below multiplies by it -- so the
     * parameter is accepted and checked rather than used. When the front half
     * owns the scale, this is where it arrives. */
    if (scale != gl_scale()) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [GL] surface asked for scale %u, backend is at "
                            "%u\n", scale, gl_scale());
    }
    surface_bind(offset, w, h);
}

static int glb_program(const Nv2aVshProgram *vp, const Nv2aVshFixed *ff,
                       const Nv2aPshState *ps, const struct Nv2aRenderState *rs,
                       uint32_t hash)
{
    (void)vp; (void)ff; (void)ps; (void)rs; (void)hash;
    glb_unrouted("program");
    return -1;
}

static void glb_texture(int stage, const void *rgba, uint32_t w, uint32_t h,
                        uint32_t key, const Nv2aSampler *smp)
{
    (void)stage; (void)rgba; (void)w; (void)h; (void)key; (void)smp;
    glb_unrouted("texture");
}

/* The OpenGL half of the per-draw state.
 *
 * Still compared against what was last set rather than set outright: these
 * are the calls a title makes thousands of times a frame with the same
 * values, and OpenGL charges for each one. The comparison is now against a
 * copy of the descriptor, so a backend that is handed identical state twice
 * costs nothing whichever front half produced it. */
static void glb_state(const Nv2aRenderState *rs)
{
    if (rs->depth_test != g_last.depth_test || rs->depth_func != g_last.depth_func) {
        if (rs->depth_test) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(gl_depth_func((uint32_t)rs->depth_func));
        } else glDisable(GL_DEPTH_TEST);
        g_last.depth_test = rs->depth_test;
        g_last.depth_func = rs->depth_func;
    }
    if (rs->depth_write != g_last.depth_mask) {
        glDepthMask(rs->depth_write ? GL_TRUE : GL_FALSE);
        g_last.depth_mask = rs->depth_write;
    }
    /* Past the far plane: clamp or cull, as the title asked -- with
     * RECOMP_GL_DEPTHCLAMP=1/0 to force it either way for the experiment
     * that matters: does the distant road come back when nothing beyond the
     * far plane is thrown away. */
    { static int last_clamp = -1, force = -2;
      int clamp;
      if (force == -2) { const char *v = getenv("RECOMP_GL_DEPTHCLAMP");
                         force = v ? atoi(v) : -1; }
      /* A w-buffered draw must not be clipped on the program's z either:
       * its depth comes from w in the fragment stage, and the console's
       * range test is against w, which this title's clip range never
       * rejects. Clamp instead, and let the fragment shader overwrite. */
      clamp = force >= 0 ? force
            : ((rs->zclamp & 0xF0u) || rs->w_buffer) ? 1 : 0;
      if (clamp != last_clamp) {
          if (clamp) glEnable(GL_DEPTH_CLAMP); else glDisable(GL_DEPTH_CLAMP);
          last_clamp = clamp;
      } }
    { static uint32_t last_cm = 0xFFFFFFFFu;
      uint32_t cm = rs->color_mask ? rs->color_mask : 0x01010101u;
      if (g_last_cm_reset) { last_cm = 0xFFFFFFFFu; g_last_cm_reset = 0; }
      if (cm != last_cm) {
          glColorMask((cm >> 16) & 1 ? GL_TRUE : GL_FALSE,
                      (cm >>  8) & 1 ? GL_TRUE : GL_FALSE,
                      (cm      ) & 1 ? GL_TRUE : GL_FALSE,
                      (cm >> 24) & 1 ? GL_TRUE : GL_FALSE);
          last_cm = cm;
      } }
    { static int nob = -1;
      if (nob < 0) nob = getenv("RECOMP_GL_NOBLEND") ? 1 : 0;
      if (nob) { glDisable(GL_BLEND); g_last.blend_enable = -1; } }
    if (rs->blend_enable != g_last.blend_enable
     || rs->blend_src != g_last.blend_src || rs->blend_dst != g_last.blend_dst) {
        if (rs->blend_enable) {
            glEnable(GL_BLEND);
            glBlendFunc(gl_blend((uint32_t)rs->blend_src),
                        gl_blend((uint32_t)rs->blend_dst));
        } else glDisable(GL_BLEND);
        g_last.blend_enable = rs->blend_enable;
        g_last.blend_src = rs->blend_src;
        g_last.blend_dst = rs->blend_dst;
    }
    g_last.valid = 1;

    /*
     * Culling, with the winding deliberately inverted.
     *
     * A cel-shaded title draws its outlines by rendering the model a second
     * time, expanded along its normals, in black, with the front faces culled
     * so that only the silhouette shows. Cull nothing and that second pass is
     * an opaque black copy of the model sitting in front of the original --
     * which is what the black bars standing over the city were.
     *
     * The winding has to be flipped from what the title asks for. The
     * viewport's y scale is negative, so the transform mirrors the frame
     * vertically, and a mirror reverses the order every triangle's vertices
     * appear in. Passing the console's front-face convention through
     * unchanged would therefore cull exactly the wrong half.
     *
     * RECOMP_GL_NOCULL puts it back, because "geometry is missing" and
     * "geometry that should be hidden is not" look nothing alike, and having
     * both pictures separates them in one run.
     */
    {
        static int cull = -1, flip = -1;
        if (cull < 0) { cull = getenv("RECOMP_GL_NOCULL") ? 0 : 1;
                        flip = getenv("RECOMP_GL_CULLFLIP") ? 1 : 0; }
        /* What the title actually asks for, counted once.
         *
         * Turning culling on removed most of the city, which says the inputs
         * are not what the reasoning above assumed -- and the two registers
         * were being read from the wrong methods until a moment ago, so
         * nothing here has ever seen a real value. Count them before
         * believing any convention. */
        { static uint32_t seen_en[2], seen_cf[8], seen_ff[4]; static uint64_t n;
          static int on = -2;
          if (on == -2) on = getenv("RECOMP_GL_CULL_MAP") ? 1 : 0;
          if (on) {
              seen_en[rs->cull_enable ? 1 : 0]++;
              seen_cf[(rs->cull_face - 0x404) & 7]++;
              seen_ff[(rs->front_face - 0x900) & 3]++;
              if (++n % 50000 == 0)
                  fprintf(stderr, "  [GL] cull: off %u on %u | face F%u B%u "
                          "FB%u other%u | front CW%u CCW%u other %u/%u\n",
                          seen_en[0], seen_en[1], seen_cf[0], seen_cf[1],
                          seen_cf[4], seen_cf[2] + seen_cf[3] + seen_cf[5]
                          + seen_cf[6] + seen_cf[7],
                          seen_ff[0], seen_ff[1], seen_ff[2], seen_ff[3]);
          } }
        if (!cull || !rs->cull_enable) {
            glDisable(GL_CULL_FACE);
        } else {
            /* These registers hold OpenGL's own enumerants: 0x900 is
             * GL_CW, 0x901 GL_CCW, 0x404 GL_FRONT, 0x405 GL_BACK, 0x408
             * GL_FRONT_AND_BACK. The NV2A is a GeForce 3 and its driver
             * spoke GL, so the values pass straight through.
             *
             * Straight through is also right for the winding, which took a
             * run to establish. The transform mirrors y -- the viewport's y
             * scale is negative -- and a mirror reverses the order a
             * triangle's vertices appear in, so correcting for it looked
             * obviously necessary. It is not: the console's own viewport
             * transform has the same negative scale, so the hardware sees
             * the reflected winding too and the register already accounts
             * for it. Correcting again culled the visible side of every
             * surface, which left the city as the dark insides of its own
             * buildings. */
            int ccw = (rs->front_face == 0x901);
            glEnable(GL_CULL_FACE);
            if (flip) ccw = !ccw;
            glFrontFace(ccw ? GL_CCW : GL_CW);
            glCullFace(rs->cull_face == 0x404 ? GL_FRONT
                     : rs->cull_face == 0x408 ? GL_FRONT_AND_BACK : GL_BACK);
        }
    }

    /* The window clip, as a scissor. The rectangle arrives in the title's
     * coordinates, which run top-down; GL's run bottom-up, so the y has to be
     * turned over, and the internal resolution multiplies all four. */
    if (rs->scissor_enable) {
        uint32_t sc = gl_scale();
        GLint gy = (GLint)((g_fbo_h - (rs->scissor[3] + 1)) * sc);
        if (gy < 0) gy = 0;
        glEnable(GL_SCISSOR_TEST);
        glScissor((GLint)(rs->scissor[0] * sc), gy,
                  (GLsizei)((rs->scissor[2] - rs->scissor[0] + 1) * sc),
                  (GLsizei)((rs->scissor[3] - rs->scissor[1] + 1) * sc));
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
}

static void glb_draw(int prog, int prim,
                     const float *verts, uint32_t nverts, uint32_t stride_floats,
                     const Nv2aAttrSlot *slots, int nslots,
                     const Nv2aUniforms *u,
                     uint32_t const_attr_mask, const float const_attr[16][4])
{
    (void)prog; (void)prim; (void)verts; (void)nverts; (void)stride_floats;
    (void)slots; (void)nslots; (void)u; (void)const_attr_mask; (void)const_attr;
    glb_unrouted("draw");
}

static void glb_present(const uint8_t **pixels, uint32_t *w, uint32_t *h)
{
    *pixels = NULL; *w = 0; *h = 0;
    glb_unrouted("present");
}

static void glb_report(void) { nv2a_gl_report(); }

static const Nv2aBackend g_gl_backend = {
    "opengl",
    glb_init, glb_surface, glb_clear, glb_program, glb_texture,
    glb_state, glb_draw, glb_present, glb_report
};

const Nv2aBackend *nv2a_backend_gl(void) { return &g_gl_backend; }

/* A build without the Metal backend still has to link. The header's rule is
 * that a backend which cannot be built at all returns NULL, and that is a
 * cheaper way to say "this platform has no Metal" than an #ifdef at every
 * place that might ask for one. */
#ifndef NV2A_HAVE_METAL
const Nv2aBackend *nv2a_backend_metal(void) { return NULL; }
#endif

/*
 * Which one runs.
 *
 * RECOMP_BACKEND=metal asks for Metal; anything else, or a Metal build that
 * cannot bring a device up, gets OpenGL. Chosen once and remembered, because
 * a backend that changed halfway through a frame would leave its surfaces and
 * programs behind in the other one.
 */
static const Nv2aBackend *backend(void)
{
    static const Nv2aBackend *b;
    if (!b) {
        const char *v = getenv("RECOMP_BACKEND");
        if (v && !strcmp(v, "metal")) {
            b = nv2a_backend_metal();
            if (b && b->init && !b->init()) b = NULL;
            if (!b) fprintf(stderr, "  [GL] Metal backend unavailable, "
                                    "using OpenGL\n");
        }
        if (!b) b = &g_gl_backend;
        fprintf(stderr, "  [GL] backend: %s\n", b->name);
    }
    return b;
}
