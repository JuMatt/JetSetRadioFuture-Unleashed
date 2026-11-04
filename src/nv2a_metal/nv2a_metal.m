/*
 * The NV2A's back half, in Metal.
 *
 * The other implementation of the device in nv2a_backend.h, beside the
 * OpenGL one in nv2a_gfx_gl.c -- same operations, same order, and written to
 * be read side by side with it, because when the two pictures differ the
 * question is always "which of these two functions disagrees". The front half
 * (nv2a_gl.c) is shared, so everything here is about how Metal does a thing,
 * never about what the console asked for.
 *
 * Where Metal differs from OpenGL in this job, structurally:
 *
 *   State lives in objects. Blend state and the colour write mask are
 *   compiled into the render pipeline, depth and stencil state into a
 *   depth-stencil object; both are cached here, keyed on exactly what went
 *   into them, and looked up at draw time from the last state() call. The
 *   rest (cull, winding, scissor, depth clamp, stencil reference) is encoder
 *   state and is set when it changes.
 *
 *   Work is recorded, then submitted. A frame's draws go into one command
 *   buffer, committed at the flip; vertices, constants and index lists are
 *   written into a per-frame arena of shared buffers, three frames of them,
 *   so the CPU never writes a buffer the GPU is still reading.
 *
 *   A render target is drawn in passes. Switching target, copying out of one
 *   or reading it back ends the pass; the next draw begins another that loads
 *   what was there. A clear at the start of a pass is a load action (free on
 *   this hardware); anywhere else it is a triangle drawn with the clear
 *   colour, depth and stencil.
 *
 *   There are no generic vertex attributes and no triangle fans. Vertices are
 *   pulled (the generated vertex function reads a packed float buffer through
 *   a table of offsets), with one value per draw for an attribute that has no
 *   array -- kept here as OpenGL kept it, including the value a draw that
 *   supplies neither inherits. Fans and line loops are drawn indexed.
 *
 *   Pictures are the right way up in memory: a target's row 0 is the top of
 *   the screen, as a texture's is, so sampling a surface turns nothing over
 *   and read-backs flip rows to hand out OpenGL's bottom-up order.
 *
 *   The window is fed by a thread of its own, which waits for the display so
 *   that nothing else has to (see "the window", at mtl_present).
 *
 * Built with ARC (CMakeLists.txt asks for it for this file alone).
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <pthread.h>
#include <TargetConditionals.h>

#include "../nv2a_gl/nv2a_backend.h"
#include "../nv2a_gl/nv2a_shader_cache.h"
#include "../nv2a_gl/nv2a_smaa.h"

#if !__has_feature(objc_arc)
#error "nv2a_metal.m is written for ARC: compile it with -fobjc-arc"
#endif

/* In nv2a_window_mac.m: the window's CAMetalLayer, when there is a window and
 * it was made for Metal; and nv2a_gl.c's widescreen state, for the picture's
 * shape at the flip. */
void *nv_window_metal_layer(void) __attribute__((weak));
int   nv_window_visible(void) __attribute__((weak));
int   nv_window_refresh_hz(void) __attribute__((weak));
/* iOS (port/ios/jsrf_ios.m) returns from this once the app is in front: GPU
 * work submitted from the background is refused there, so a frame finished
 * while the app is away is held until it is back -- and the game with it.
 * Elsewhere this default does nothing. A weak definition rather than a weak
 * reference: Mach-O will not link a weak reference nothing defines. */
__attribute__((weak)) void nv_window_wait_visible(void);
__attribute__((weak)) void nv_window_wait_visible(void) { }
int   nv2a_wide_scene(void);
float nv2a_wide_aspect(void);

#define MTL_FRAMES_IN_FLIGHT 3
#define MTL_ARENA_CHUNK      (8u << 20)
#define MTL_MAX_TEX          8192
#define MTL_MAX_RT           64
#define MTL_MAX_PROG         4096
#define MTL_PSO_CACHE        16384     /* power of two */
#define MTL_DSS_CACHE        1024      /* power of two */
#define MTL_SMP_CACHE        64

static int env_on(const char *n) { const char *v = getenv(n); return v && *v && *v != '0'; }

/* ---- the device ----------------------------------------------------------- */

static id<MTLDevice>               g_dev;
static id<MTLCommandQueue>         g_queue;
static id<MTLCommandBuffer>        g_cb;      /* this frame's, while recording */
static id<MTLRenderCommandEncoder> g_enc;     /* the open pass, if any */
static dispatch_semaphore_t        g_inflight;
static int      g_frame_open;                 /* a frame has begun recording */
static uint32_t g_frame_slot;

/* ---- handles: textures, targets, programs ---------------------------------- */

typedef struct {
    id<MTLTexture>      tex;
    id<MTLSamplerState> smp;
    uint32_t            w, h;
    int                 used;
} MtlTex;
static MtlTex   g_tex[MTL_MAX_TEX];
static uint32_t g_tex_hint;

typedef struct {
    uint32_t       color;      /* texture handle of the colour buffer */
    id<MTLTexture> ds;         /* depth32float + stencil8 */
    /* Multisampled (samples()): the colour and depth/stencil drawn into, and
     * `color` the picture resolved from them at the end of every pass. Then
     * there is no `ds`. nil when not multisampled. */
    id<MTLTexture> mscol, msds;
    int            flat;       /* colour only, never drawn into by the title:
                                  SMAA's own target */
    uint32_t       pw, ph;
    int            used;
} MtlRT;
static MtlRT    g_rt[MTL_MAX_RT];
static uint32_t g_cur_rt;
static int      g_samples = 1;  /* every target's, samples(): set before the first */

typedef struct {
    id<MTLFunction> vf, ff;
    uint64_t        uses;      /* bit per Nv2aUniformId the source reads */
    uint64_t        skey;      /* the shader cache's key for its sources, 0 none */
    uint32_t        sid;       /* the renderer's key for the state it was made from, 0 none */
    uint32_t        gen;       /* changes each time the slot is reused */
    int             used;
} MtlProg;
static MtlProg  g_prog[MTL_MAX_PROG];
static uint32_t g_prog_gen;
static uint32_t g_cur_prog;

typedef struct {
    uint64_t key;
    id<MTLRenderPipelineState> pso;
} MtlPso;
static MtlPso g_pso[MTL_PSO_CACHE];

typedef struct {
    uint64_t key;
    id<MTLDepthStencilState> dss;
    int used;
} MtlDss;
static MtlDss g_dss[MTL_DSS_CACHE];

typedef struct { uint32_t key; id<MTLSamplerState> smp; } MtlSmp;
static MtlSmp g_smp[MTL_SMP_CACHE];
static int    g_nsmp;

static uint32_t g_snap_tex;              /* target_snapshot()'s copy */
static id<MTLTexture>      g_black;      /* what an unbound stage samples */
static id<MTLSamplerState> g_black_smp;

/* The clear and present pipelines, built once. */
static id<MTLLibrary>              g_util_lib;
static id<MTLRenderPipelineState> g_clear_pso[2];      /* colour off / on */
static id<MTLRenderPipelineState> g_blit_pso;          /* to the drawable */
static id<MTLSamplerState>         g_linear_clamp;

/* ---- per-draw state ----------------------------------------------------------- */

static Nv2aRenderState g_rs;              /* the last state() */
static uint32_t g_bound[4];               /* texture handles per unit */

static uint8_t  *g_vu, *g_fu;             /* the two uniform structs */
static uint32_t  g_vu_size, g_fu_size;
static float     g_consts[NV2A_VSH_NUM_CONSTS][4];
static int       g_consts_dirty = 1;
static id<MTLBuffer> g_consts_buf;        /* where the constants last went */
static uint32_t  g_consts_off;

static Nv2aMslAttrTable g_attr;
static float     g_generic[16][4];        /* GL's current attribute values */
static id<MTLBuffer> g_vbuf;              /* the vertices of the coming draws */
static uint32_t  g_voff;

static id<MTLBuffer> g_fan;               /* 0,1,2, 0,2,3, ... */
static uint32_t  g_fan_verts;

/* What the open encoder has been given, so a draw sets only what changed. */
static struct {
    __unsafe_unretained id<MTLRenderPipelineState> pso;
    __unsafe_unretained id<MTLDepthStencilState>   dss;
    int      cull, wind, clip;
    float    bias_u, bias_f;    /* the depth bias set: polygon offset */
    uint32_t stencil_ref;
    MTLScissorRect sc;
    __unsafe_unretained id<MTLBuffer> vbuf, cbuf;
    uint32_t voff, coff;
    uint32_t tex[4];
    int      valid;
} E;

/* Clear load actions waiting for the next pass on the bound target. */
static struct { unsigned mask; float rgba[4]; float depth; uint32_t stencil; } g_pending_clear;
static int g_zrev;              /* reversed depth: see mtl_zrev */

/* Counters for the report. */
static uint64_t g_n_draws, g_n_passes, g_n_frames, g_n_quad_clears, g_n_load_clears;
static uint32_t g_n_progs, g_n_psos, g_n_tex, g_n_readbacks, g_n_snapshots;
static uint64_t g_arena_peak;
/* The window's, kept by two threads (see mtl_present). */
static _Atomic uint64_t g_n_presented, g_n_no_drawable, g_n_skipped, g_n_hidden;
static double   g_ms_compile, g_ms_pso, g_ms_pso_max;   /* time spent building */

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ---- the arena ------------------------------------------------------------------ */

typedef struct {
    id<MTLBuffer> chunk[16];
    uint32_t      nchunk, cur, pos;
    uint64_t      used;
} MtlArena;
static MtlArena g_arena[MTL_FRAMES_IN_FLIGHT];

/* `n` bytes in this frame's arena, aligned to 256 (constant buffers want it,
 * and nothing is the worse for it). */
static void *arena_alloc(uint32_t n, id<MTLBuffer> __strong *buf, uint32_t *off)
{
    MtlArena *a = &g_arena[g_frame_slot];
    uint32_t need = (n + 255u) & ~255u;
    for (;;) {
        if (a->cur < a->nchunk) {
            id<MTLBuffer> b = a->chunk[a->cur];
            if (a->pos + need <= (uint32_t)[b length]) {
                *buf = b;
                *off = a->pos;
                a->pos += need;
                a->used += need;
                if (a->used > g_arena_peak) g_arena_peak = a->used;
                return (uint8_t *)[b contents] + *off;
            }
            a->cur++;
            a->pos = 0;
            continue;
        }
        if (a->nchunk >= 16) {
            static int said;
            if (!said++) fprintf(stderr, "  [MTL] frame arena full (%u chunks)\n", a->nchunk);
            return NULL;
        }
        {
            uint32_t sz = need > MTL_ARENA_CHUNK ? need : MTL_ARENA_CHUNK;
            a->chunk[a->nchunk] = [g_dev newBufferWithLength:sz
                                                      options:MTLResourceStorageModeShared];
            if (!a->chunk[a->nchunk]) return NULL;
            a->nchunk++;
        }
    }
}

/* ---- frames and passes ------------------------------------------------------------ */

static void enc_end(void)
{
    if (g_enc) { [g_enc endEncoding]; g_enc = nil; }
    E.valid = 0;
}

/* A command buffer to record into. The first one of a frame waits until the
 * frame that last used this arena slot has finished on the GPU. */
static double g_ms_gpu_wait;              /* the render thread's, in frame_begin */
static atomic_llong g_gpu_busy_us;        /* added up by the completion handlers */
static void frame_begin(void)
{
    if (g_cb) return;
    if (!g_frame_open) {
        double t0 = now_ms();
        dispatch_semaphore_wait(g_inflight, DISPATCH_TIME_FOREVER);
        g_ms_gpu_wait += now_ms() - t0;
        g_frame_slot = (g_frame_slot + 1) % MTL_FRAMES_IN_FLIGHT;
        g_arena[g_frame_slot].cur = 0;
        g_arena[g_frame_slot].pos = 0;
        g_arena[g_frame_slot].used = 0;
        g_frame_open = 1;
        g_consts_dirty = 1;          /* last frame's copy is in another slot */
        g_vbuf = nil;
    }
    g_cb = [g_queue commandBuffer];
    /* How long the GPU took over it, for the log's slow frames. */
    [g_cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
        CFTimeInterval s = b.GPUStartTime, e = b.GPUEndTime;
        if (e > s) atomic_fetch_add(&g_gpu_busy_us, (long long)((e - s) * 1e6));
    }];
}

/* Submit what is recorded. At the end of a frame, the arena slot is released
 * when the GPU is done with it; in the middle of one (a read-back), the frame
 * carries on into a new command buffer and keeps its slot. */
static void submit(int end_of_frame, int wait)
{
    enc_end();
    if (!g_cb) {
        if (end_of_frame && g_frame_open) {
            g_frame_open = 0;
            dispatch_semaphore_signal(g_inflight);
        }
        return;
    }
    if (end_of_frame) {
        dispatch_semaphore_t sem = g_inflight;
        [g_cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
            (void)b; dispatch_semaphore_signal(sem);
        }];
        g_frame_open = 0;
    }
    [g_cb commit];
    if (wait) [g_cb waitUntilCompleted];
    if ([g_cb status] == MTLCommandBufferStatusError) {
        static int said;
        if (said++ < 5)
            fprintf(stderr, "  [MTL] command buffer failed: %s\n",
                    [[[g_cb error] localizedDescription] UTF8String]);
    }
    g_cb = nil;
}

static int rt_ok(uint32_t h) { return h && h <= MTL_MAX_RT && g_rt[h - 1].used; }
static int tex_ok(uint32_t h) { return h && h <= MTL_MAX_TEX && g_tex[h - 1].used; }

/* Open a pass on the bound target, loading what it holds -- or clearing,
 * for a target never drawn into or a full clear that arrived first. */
static int enc_begin(void)
{
    MtlRT *rt;
    MTLRenderPassDescriptor *rp;
    unsigned clear;

    if (g_enc) return 1;
    if (!rt_ok(g_cur_rt)) return 0;
    rt = &g_rt[g_cur_rt - 1];
    if (rt->flat) return 0;                 /* nothing the title draws goes there */
    frame_begin();

    clear = g_pending_clear.mask;
    rp = [MTLRenderPassDescriptor renderPassDescriptor];
    if (rt->mscol) {
        /* Multisampled: drawn into its samples, which are kept for the next
         * pass on it, and resolved into its picture as each pass ends -- so
         * whatever samples, copies or shows the picture has it as drawn. */
        rp.colorAttachments[0].texture = rt->mscol;
        rp.colorAttachments[0].resolveTexture = g_tex[rt->color - 1].tex;
        rp.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
    } else {
        rp.colorAttachments[0].texture = g_tex[rt->color - 1].tex;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    rp.colorAttachments[0].loadAction = (clear & 1) ? MTLLoadActionClear : MTLLoadActionLoad;
    rp.colorAttachments[0].clearColor =
        MTLClearColorMake(g_pending_clear.rgba[0], g_pending_clear.rgba[1],
                          g_pending_clear.rgba[2], g_pending_clear.rgba[3]);
    rp.depthAttachment.texture = rt->mscol ? rt->msds : rt->ds;
    rp.depthAttachment.loadAction = (clear & 2) ? MTLLoadActionClear : MTLLoadActionLoad;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    rp.depthAttachment.clearDepth = g_pending_clear.depth;
    rp.stencilAttachment.texture = rt->mscol ? rt->msds : rt->ds;
    rp.stencilAttachment.loadAction = (clear & 4) ? MTLLoadActionClear : MTLLoadActionLoad;
    rp.stencilAttachment.storeAction = MTLStoreActionStore;
    rp.stencilAttachment.clearStencil = g_pending_clear.stencil & 0xFFu;
    g_pending_clear.mask = 0;

    g_enc = [g_cb renderCommandEncoderWithDescriptor:rp];
    if (!g_enc) return 0;
    [g_enc setViewport:(MTLViewport){ 0.0, 0.0, (double)rt->pw, (double)rt->ph, 0.0, 1.0 }];
    E.valid = 0;
    g_n_passes++;
    return 1;
}

static void flush_pending_clear(void);

/* ---- textures ------------------------------------------------------------------------- */

static uint32_t tex_slot(void)
{
    uint32_t i;
    for (i = 0; i < MTL_MAX_TEX; i++) {
        uint32_t k = (g_tex_hint + i) % MTL_MAX_TEX;
        if (!g_tex[k].used) { g_tex_hint = k + 1; return k + 1; }
    }
    fprintf(stderr, "  [MTL] out of texture handles\n");
    return 0;
}

static id<MTLSamplerState> sampler_for(const Nv2aSampler *s)
{
    static const MTLSamplerAddressMode wrap[3] = {
        MTLSamplerAddressModeClampToEdge, MTLSamplerAddressModeRepeat,
        MTLSamplerAddressModeMirrorRepeat };
    uint32_t key = (uint32_t)s->min_linear | (uint32_t)s->mag_linear << 1
                 | (uint32_t)(s->mip & 3) << 2 | (uint32_t)(s->wrap_s & 3) << 4
                 | (uint32_t)(s->wrap_t & 3) << 6 | (uint32_t)s->aniso << 8;
    MTLSamplerDescriptor *d;
    int i;
    for (i = 0; i < g_nsmp; i++) if (g_smp[i].key == key) return g_smp[i].smp;
    d = [MTLSamplerDescriptor new];
    d.minFilter = s->min_linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.magFilter = s->mag_linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.mipFilter = s->mip == 1 ? MTLSamplerMipFilterNearest
                : s->mip == 2 ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNotMipmapped;
    d.sAddressMode = wrap[s->wrap_s < 3 ? s->wrap_s : 0];
    d.tAddressMode = wrap[s->wrap_t < 3 ? s->wrap_t : 0];
    if (s->mip && s->aniso > 1) d.maxAnisotropy = s->aniso > 16 ? 16 : s->aniso;
    if (g_nsmp < MTL_SMP_CACHE) {
        g_smp[g_nsmp].key = key;
        g_smp[g_nsmp].smp = [g_dev newSamplerStateWithDescriptor:d];
        return g_smp[g_nsmp++].smp;
    }
    return [g_dev newSamplerStateWithDescriptor:d];
}

static uint32_t mtl_tex_new(uint32_t w, uint32_t h, const void *rgba, const Nv2aSampler *s)
{
    uint32_t hnd;
    MTLTextureDescriptor *td;
    id<MTLTexture> t;
    if (!w || !h) return 0;
    hnd = tex_slot();
    if (!hnd) return 0;
    @autoreleasepool {
        int given = s->mip && s->levels > 1 && rgba;    /* the title's own chain */
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                width:w height:h
                                                            mipmapped:s->mip ? YES : NO];
        if (given && s->levels < td.mipmapLevelCount) td.mipmapLevelCount = s->levels;
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        t = [g_dev newTextureWithDescriptor:td];
        if (!t) return 0;
        if (rgba)
            [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0
                   withBytes:rgba bytesPerRow:w * 4];
        if (given) {
            /* Level after level, as rgba holds them. */
            const uint8_t *p = (const uint8_t *)rgba + (size_t)w * h * 4;
            NSUInteger l, wl = w, hl = h;
            for (l = 1; l < [t mipmapLevelCount]; l++) {
                wl = wl > 1 ? wl / 2 : 1;
                hl = hl > 1 ? hl / 2 : 1;
                [t replaceRegion:MTLRegionMake2D(0, 0, wl, hl) mipmapLevel:l
                       withBytes:p bytesPerRow:wl * 4];
                p += wl * hl * 4;
            }
        } else if (s->mip && [t mipmapLevelCount] > 1) {
            /* A command buffer of its own, committed now: queued ahead of
             * the frame being recorded, so the chain exists before any draw
             * of this frame samples it. */
            id<MTLCommandBuffer> cb = [g_queue commandBuffer];
            id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
            [bl generateMipmapsForTexture:t];
            [bl endEncoding];
            [cb commit];
        }
        g_tex[hnd - 1].tex = t;
        g_tex[hnd - 1].smp = sampler_for(s);
        g_tex[hnd - 1].w = w; g_tex[hnd - 1].h = h;
        g_tex[hnd - 1].used = 1;
    }
    g_n_tex++;
    return hnd;
}

/* DXT as the title stored it (tex_new_dxt), where the GPU samples BC1-3
 * itself: no decoding on the CPU. Every Apple silicon Mac does; an iPhone or
 * iPad only where supportsBCTextureCompression says so (iOS 16.4 on), and
 * elsewhere the front half decodes them as before. A size not a whole number
 * of blocks, and a chain the device would have to make itself, are left to
 * the front half too. RECOMP_MTL_DXT=0: always decoded. */
static int mtl_bc_ok(void)
{
    static int ok = -1;
    if (ok < 0) {
        const char *v = getenv("RECOMP_MTL_DXT");
        ok = 0;
        if (!(v && v[0] == '0')) {
            if (@available(macOS 11.0, iOS 16.4, *)) ok = g_dev && g_dev.supportsBCTextureCompression ? 1 : 0;
        }
        fprintf(stderr, "  [MTL] DXT textures: %s\n", ok ? "sampled as they are (BC)"
                : (v && v[0] == '0') ? "decoded (RECOMP_MTL_DXT=0)" : "decoded (this GPU has no BC formats)");
    }
    return ok;
}
static uint32_t mtl_tex_new_dxt(uint32_t w, uint32_t h, int dxt, const void *blocks,
                                const Nv2aSampler *s)
{
    uint32_t hnd;
    MTLPixelFormat pf;
    NSUInteger bsz, levels, l, wl, hl;
    const uint8_t *p;
    id<MTLTexture> t;
    if (!blocks || !w || !h || (w & 3) || (h & 3) || (s->mip && s->levels < 2) || !mtl_bc_ok()) return 0;
    if (dxt == 1)      { pf = MTLPixelFormatBC1_RGBA; bsz = 8; }
    else if (dxt == 3) { pf = MTLPixelFormatBC2_RGBA; bsz = 16; }
    else if (dxt == 5) { pf = MTLPixelFormatBC3_RGBA; bsz = 16; }
    else return 0;
    hnd = tex_slot();
    if (!hnd) return 0;
    @autoreleasepool {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf
                                                                                      width:w height:h
                                                                                  mipmapped:s->mip ? YES : NO];
        if (s->mip && s->levels < td.mipmapLevelCount) td.mipmapLevelCount = s->levels;
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        t = [g_dev newTextureWithDescriptor:td];
        if (!t) return 0;
        levels = s->mip ? [t mipmapLevelCount] : 1;
        p = (const uint8_t *)blocks;
        for (l = 0, wl = w, hl = h; l < levels; l++) {
            NSUInteger bw = (wl + 3) / 4, bh = (hl + 3) / 4;
            [t replaceRegion:MTLRegionMake2D(0, 0, wl, hl) mipmapLevel:l
                   withBytes:p bytesPerRow:bw * bsz];
            p += bw * bh * bsz;
            wl = wl > 1 ? wl / 2 : 1;
            hl = hl > 1 ? hl / 2 : 1;
        }
        g_tex[hnd - 1].tex = t;
        g_tex[hnd - 1].smp = sampler_for(s);
        g_tex[hnd - 1].w = w; g_tex[hnd - 1].h = h;
        g_tex[hnd - 1].used = 1;
    }
    g_n_tex++;
    return hnd;
}

static void mtl_tex_free(uint32_t hnd)
{
    int u;
    if (!tex_ok(hnd)) return;
    /* A command buffer in flight keeps its own reference; this is ours. */
    g_tex[hnd - 1].tex = nil;
    g_tex[hnd - 1].smp = nil;
    g_tex[hnd - 1].used = 0;
    for (u = 0; u < 4; u++) if (g_bound[u] == hnd) g_bound[u] = 0;
    for (u = 0; u < 4; u++) if (E.tex[u] == hnd) E.tex[u] = 0xFFFFFFFFu;
}

/* Texels back as BGRA, row 0 first (a texture is not a target: no flip). */
static int read_texture(id<MTLTexture> t, MTLRegion r, int bgra, int flip, uint8_t *out)
{
    uint32_t w = (uint32_t)r.size.width, h = (uint32_t)r.size.height, y, x;
    id<MTLBuffer> rb;
    id<MTLBlitCommandEncoder> bl;
    const uint8_t *src;
    if (!w || !h) return 0;
    rb = [g_dev newBufferWithLength:(NSUInteger)w * h * 4 options:MTLResourceStorageModeShared];
    if (!rb) return 0;
    enc_end();
    frame_begin();
    bl = [g_cb blitCommandEncoder];
    [bl copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:r.origin sourceSize:r.size
               toBuffer:rb destinationOffset:0 destinationBytesPerRow:w * 4
destinationBytesPerImage:(NSUInteger)w * h * 4];
    [bl endEncoding];
    submit(0, 1);
    g_n_readbacks++;
    src = (const uint8_t *)[rb contents];
    for (y = 0; y < h; y++) {
        const uint8_t *s = src + (size_t)(flip ? h - 1 - y : y) * w * 4;
        uint8_t *d = out + (size_t)y * w * 4;
        if (!bgra) { memcpy(d, s, (size_t)w * 4); continue; }
        for (x = 0; x < w; x++) {
            d[x*4+0] = s[x*4+2]; d[x*4+1] = s[x*4+1];
            d[x*4+2] = s[x*4+0]; d[x*4+3] = s[x*4+3];
        }
    }
    return 1;
}

static int mtl_tex_read(uint32_t hnd, uint32_t w, uint32_t h, void *bgra)
{
    if (!tex_ok(hnd) || !bgra) return 0;
    { MTLPixelFormat pf = g_tex[hnd - 1].tex.pixelFormat;   /* BC blocks are not texels */
      if (pf == MTLPixelFormatBC1_RGBA || pf == MTLPixelFormatBC2_RGBA || pf == MTLPixelFormatBC3_RGBA) return 0; }
    return read_texture(g_tex[hnd - 1].tex, MTLRegionMake2D(0, 0, w, h), 1, 0, (uint8_t *)bgra);
}

/* ---- render targets -------------------------------------------------------------------- */

/* A texture for a target: `fmt`, pw x ph, `samples` a pixel. */
static id<MTLTexture> rt_texture(MTLPixelFormat fmt, uint32_t pw, uint32_t ph, int samples,
                                 MTLTextureUsage usage)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:pw height:ph mipmapped:NO];
    if (samples > 1) {
        d.textureType = MTLTextureType2DMultisample;
        d.sampleCount = (NSUInteger)samples;
    }
    d.usage = usage;
    d.storageMode = MTLStorageModePrivate;
    return [g_dev newTextureWithDescriptor:d];
}

/* A target: its colour as a texture handle, and its depth/stencil -- both
 * multisampled as well while samples() asks for it, unless `flat` (colour
 * only, and never more than one sample: SMAA's own target). */
static uint32_t target_new(uint32_t pw, uint32_t ph, int flat)
{
    int i, ms;
    uint32_t c;
    Nv2aSampler s;
    for (i = 0; i < MTL_MAX_RT; i++) if (!g_rt[i].used) break;
    if (i == MTL_MAX_RT) { fprintf(stderr, "  [MTL] out of render targets\n"); return 0; }
    c = tex_slot();
    if (!c) return 0;
    ms = !flat && g_samples > 1;
    memset(&s, 0, sizeof s);
    s.min_linear = s.mag_linear = 1;
    @autoreleasepool {
        g_tex[c - 1].tex = rt_texture(MTLPixelFormatRGBA8Unorm, pw, ph, 1,
                                      MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
        g_rt[i].ds = nil;
        g_rt[i].mscol = g_rt[i].msds = nil;
        if (ms) {
            g_rt[i].mscol = rt_texture(MTLPixelFormatRGBA8Unorm, pw, ph, g_samples,
                                       MTLTextureUsageRenderTarget);
            g_rt[i].msds = rt_texture(MTLPixelFormatDepth32Float_Stencil8, pw, ph, g_samples,
                                      MTLTextureUsageRenderTarget);
        } else if (!flat) {
            g_rt[i].ds = rt_texture(MTLPixelFormatDepth32Float_Stencil8, pw, ph, 1,
                                    MTLTextureUsageRenderTarget);
        }
        if (!g_tex[c - 1].tex || (ms && (!g_rt[i].mscol || !g_rt[i].msds)) || (!ms && !flat && !g_rt[i].ds)) {
            g_tex[c - 1].tex = nil;
            g_rt[i].ds = g_rt[i].mscol = g_rt[i].msds = nil;
            fprintf(stderr, "  [MTL] could not make a %ux%u target%s\n", pw, ph,
                    ms ? " (multisampled)" : "");
            return 0;
        }
        g_tex[c - 1].smp = sampler_for(&s);
        g_tex[c - 1].w = pw; g_tex[c - 1].h = ph;
        g_tex[c - 1].used = 1;
    }
    g_rt[i].color = c;
    g_rt[i].pw = pw; g_rt[i].ph = ph;
    g_rt[i].flat = flat;
    g_rt[i].used = 1;
    /* A new texture holds whatever the memory held. Zero it now, the way a
     * driver's fresh renderbuffer usually comes, so that a surface sampled
     * before anything is drawn into it reads as nothing rather than noise:
     * an empty pass that clears and stores. The pass open on the bound
     * target (if any) ends; its next draw loads what it had. */
    enc_end();
    frame_begin();
    @autoreleasepool {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        id<MTLRenderCommandEncoder> e;
        id<MTLTexture> dst = ms ? g_rt[i].msds : g_rt[i].ds;
        if (ms) {
            rp.colorAttachments[0].texture = g_rt[i].mscol;
            rp.colorAttachments[0].resolveTexture = g_tex[c - 1].tex;
            rp.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
        } else {
            rp.colorAttachments[0].texture = g_tex[c - 1].tex;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        }
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        if (dst) {
            rp.depthAttachment.texture = dst;
            rp.depthAttachment.loadAction = MTLLoadActionClear;
            rp.depthAttachment.clearDepth = g_zrev ? 0.0 : 1.0;    /* the far plane */
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            rp.stencilAttachment.texture = dst;
            rp.stencilAttachment.loadAction = MTLLoadActionClear;
            rp.stencilAttachment.clearStencil = 0;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        e = [g_cb renderCommandEncoderWithDescriptor:rp];
        [e endEncoding];
    }
    return (uint32_t)i + 1;
}

static uint32_t mtl_target_new(uint32_t pw, uint32_t ph)
{
    return target_new(pw, ph, 0);
}

static void mtl_target_free(uint32_t h)
{
    if (!rt_ok(h)) return;
    if (g_cur_rt == h) { flush_pending_clear(); enc_end(); g_cur_rt = 0; g_pending_clear.mask = 0; }
    mtl_tex_free(g_rt[h - 1].color);
    g_rt[h - 1].ds = nil;
    g_rt[h - 1].mscol = g_rt[h - 1].msds = nil;
    g_rt[h - 1].flat = 0;
    g_rt[h - 1].used = 0;
    g_rt[h - 1].color = 0;
}

/* A clear still waiting for the bound target's next pass has to happen
 * before anything else does: open the pass, which performs it. */
static void flush_pending_clear(void)
{
    if (g_pending_clear.mask && rt_ok(g_cur_rt)) enc_begin();
}

static void mtl_target_bind(uint32_t h)
{
    if (!rt_ok(h) || h == g_cur_rt) return;
    flush_pending_clear();
    enc_end();
    g_cur_rt = h;
    g_pending_clear.mask = 0;
}

static uint32_t mtl_target_texture(uint32_t h)
{
    return rt_ok(h) ? g_rt[h - 1].color : 0;
}

/* Sampling the target being drawn into: the pass ends, the picture as it
 * stands is copied out, and the next draw resumes the pass. */
static uint32_t mtl_target_snapshot(uint32_t h)
{
    MtlRT *rt;
    id<MTLBlitCommandEncoder> bl;
    if (!rt_ok(h)) return 0;
    rt = &g_rt[h - 1];
    /* A clear still waiting: the copy has to see what the title believes
     * is there. */
    if (h == g_cur_rt) flush_pending_clear();
    if (!tex_ok(g_snap_tex) || g_tex[g_snap_tex - 1].w != rt->pw
                            || g_tex[g_snap_tex - 1].h != rt->ph) {
        MTLTextureDescriptor *td;
        Nv2aSampler s;
        if (tex_ok(g_snap_tex)) mtl_tex_free(g_snap_tex);
        g_snap_tex = tex_slot();
        if (!g_snap_tex) return 0;
        memset(&s, 0, sizeof s);
        s.min_linear = s.mag_linear = 1;
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                width:rt->pw height:rt->ph
                                                            mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        g_tex[g_snap_tex - 1].tex = [g_dev newTextureWithDescriptor:td];
        g_tex[g_snap_tex - 1].smp = sampler_for(&s);
        g_tex[g_snap_tex - 1].w = rt->pw; g_tex[g_snap_tex - 1].h = rt->ph;
        g_tex[g_snap_tex - 1].used = 1;
    }
    enc_end();
    frame_begin();
    bl = [g_cb blitCommandEncoder];
    [bl copyFromTexture:g_tex[rt->color - 1].tex sourceSlice:0 sourceLevel:0
           sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(rt->pw, rt->ph, 1)
              toTexture:g_tex[g_snap_tex - 1].tex destinationSlice:0 destinationLevel:0
      destinationOrigin:MTLOriginMake(0, 0, 0)];
    [bl endEncoding];
    g_n_snapshots++;
    return g_snap_tex;
}

/* Colour back, rows bottom-up as glReadPixels gives them; (x, y) from the
 * bottom-left. A target holds row 0 at the top, so the region is turned over
 * on the way in and the rows on the way out. */
static int mtl_target_read(uint32_t h, uint32_t x, uint32_t y, uint32_t w,
                           uint32_t ht, int bgra, void *out)
{
    MtlRT *rt;
    if (!rt_ok(h) || !out || !w || !ht) return 0;
    rt = &g_rt[h - 1];
    if (x >= rt->pw || y >= rt->ph) return 0;
    if (x + w > rt->pw) w = rt->pw - x;
    if (y + ht > rt->ph) ht = rt->ph - y;
    if (h == g_cur_rt) flush_pending_clear();
    return read_texture(g_tex[rt->color - 1].tex,
                        MTLRegionMake2D(x, rt->ph - y - ht, w, ht), bgra, 1, (uint8_t *)out);
}

static int mtl_target_read_depth(uint32_t h, uint32_t pw, uint32_t ph, float *out)
{
    (void)h; (void)pw; (void)ph; (void)out;
    return 0;
}

/* ---- programs ------------------------------------------------------------------------------ */

static int word_in(const char *hay, const char *word)
{
    size_t n = strlen(word);
    const char *p = hay;
    while ((p = strstr(p, word)) != NULL) {
        char a = p == hay ? ' ' : p[-1], b = p[n];
        int ida = (a == '_' || (a >= '0' && a <= '9') || (a >= 'A' && a <= 'Z') || (a >= 'a' && a <= 'z'));
        int idb = (b == '_' || (b >= '0' && b <= '9') || (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z'));
        if (!ida && !idb && a != '.') return 1;
        p += n;
    }
    return 0;
}

/* Which uniforms a program reads: its bodies (everything after the marker
 * the emitters put there), searched for each name. The structs and #defines
 * above the marker name every uniform, so they are not looked at. */
static uint64_t uniforms_used(const char *vsrc, const char *fsrc)
{
    const char *vb = strstr(vsrc, "// nv2a body"), *fb = strstr(fsrc, "// nv2a body");
    uint64_t m = 0;
    int id;
    if (!vb) vb = vsrc;
    if (!fb) fb = fsrc;
    for (id = 0; id < NV2A_U_COUNT; id++) {
        const Nv2aUniformDesc *d = &nv2a_uniform_desc[id];
        if (d->type == NV2A_UT_SAMPLER) continue;
        if (word_in(d->frag ? fb : vb, d->name)) m |= 1ull << id;
    }
    /* nv_fog() reads the fog uniforms through a macro the body only names. */
    if (word_in(vb, "nv_fog"))
        m |= (1ull << NV2A_U_FOGENABLE) | (1ull << NV2A_U_FOGMODE) | (1ull << NV2A_U_FOGPARAM);
    return m;
}

static id<MTLFunction> compile_fn(const char *src, NSString *name, const char *what, int stage)
{
    static int fast = -1;
    NSError *err = nil;
    MTLCompileOptions *o;
    id<MTLLibrary> lib;
    if (fast < 0) fast = env_on("RECOMP_MTL_FASTMATH");
    o = [MTLCompileOptions new];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    o.fastMathEnabled = fast ? YES : NO;
#pragma clang diagnostic pop
    /* The position computed the same way in every program (see vsh_invariant
     * in nv2a_vsh.c): what makes [[invariant]] mean anything. */
    if (nv2a_vsh_msl_invariant && [o respondsToSelector:@selector(setPreserveInvariance:)])
        o.preserveInvariance = YES;
    lib = [g_dev newLibraryWithSource:[NSString stringWithUTF8String:src] options:o error:&err];
    if (!lib) {
        fprintf(stderr, "  [MTL] %s (%s) failed to compile:\n%s\n", what,
                stage ? "fragment" : "vertex", [[err localizedDescription] UTF8String]);
        if (getenv("RECOMP_GL_DUMP_SHADER"))
            fprintf(stderr, "----- source -----\n%s\n------------------\n", src);
        return nil;
    }
    return [lib newFunctionWithName:name];
}

/* ---- the shader cache (nv2a_shader_cache.h) ----------------------------------
 *
 * Compiling a program's Metal source took 30 ms on average in a measured run
 * and up to 190, and making its pipeline state another 12 -- each one a
 * visible stall when the game first draws something new. So every program
 * built is remembered on disk with the pipeline states it was drawn with, and
 * at the next start two background threads build them all again, in the order
 * the game first needed them, while the title runs. A program the game asks
 * for that they have built is handed over; one they have not reached yet is
 * compiled as before, and they then only make its pipeline states.
 *
 * The tables below are shared with those threads, under g_warm_mu. */
#define MTL_MAX_WARM 4096
/* `ahead` says who made an entry: 1 those threads, 0 the render thread
 * (kept, so that a program the game drops and asks for again, or a pipeline
 * state for a slot used again, is not built twice). */
static struct { uint64_t key; id<MTLFunction> vf, ff; int ahead; } g_wfn[MTL_MAX_WARM];
static int g_nwfn;
static struct { uint64_t key; uint32_t recipe; id<MTLRenderPipelineState> pso; int ahead; } g_wpso[MTL_MAX_WARM * 2];
static int g_nwpso;
static pthread_mutex_t g_warm_mu = PTHREAD_MUTEX_INITIALIZER;
/* the render thread's: found built ahead / found kept from earlier this run */
static unsigned g_warm_hits, g_keep_hits, g_warm_pso_hits, g_keep_pso_hits;
static atomic_int g_warm_built, g_warm_psos, g_warm_running;

/* A program's functions: 0 none, 1 kept from this run, 2 built ahead. */
static int warm_fn_get(uint64_t key, id<MTLFunction> __strong *vf, id<MTLFunction> __strong *ff)
{
    int i, found = 0;
    pthread_mutex_lock(&g_warm_mu);
    for (i = 0; i < g_nwfn; i++)
        if (g_wfn[i].key == key) { *vf = g_wfn[i].vf; *ff = g_wfn[i].ff; found = g_wfn[i].ahead ? 2 : 1; break; }
    pthread_mutex_unlock(&g_warm_mu);
    return found;
}

/* Keep a program's functions (once per key): 1 if they went in. */
static int warm_fn_put(uint64_t key, id<MTLFunction> vf, id<MTLFunction> ff, int ahead)
{
    int i, put = 0;
    pthread_mutex_lock(&g_warm_mu);
    for (i = 0; i < g_nwfn; i++) if (g_wfn[i].key == key) break;
    if (i == g_nwfn && g_nwfn < MTL_MAX_WARM) {
        g_wfn[g_nwfn].key = key; g_wfn[g_nwfn].vf = vf; g_wfn[g_nwfn].ff = ff; g_wfn[g_nwfn].ahead = ahead;
        g_nwfn++;
        put = 1;
    }
    pthread_mutex_unlock(&g_warm_mu);
    return put;
}

/* A pipeline state for a program's key and a recipe, or nil; *ahead says
 * who made it. */
static id<MTLRenderPipelineState> warm_pso_get(uint64_t key, uint32_t recipe, int *ahead)
{
    id<MTLRenderPipelineState> pso = nil;
    int i;
    pthread_mutex_lock(&g_warm_mu);
    for (i = 0; i < g_nwpso; i++)
        if (g_wpso[i].key == key && g_wpso[i].recipe == recipe) {
            pso = g_wpso[i].pso;
            if (ahead) *ahead = g_wpso[i].ahead;
            break;
        }
    pthread_mutex_unlock(&g_warm_mu);
    return pso;
}

static void warm_pso_put(uint64_t key, uint32_t recipe, id<MTLRenderPipelineState> pso, int ahead)
{
    int i;
    pthread_mutex_lock(&g_warm_mu);
    for (i = 0; i < g_nwpso; i++) if (g_wpso[i].key == key && g_wpso[i].recipe == recipe) break;
    if (i == g_nwpso && g_nwpso < MTL_MAX_WARM * 2) {
        g_wpso[g_nwpso].key = key; g_wpso[g_nwpso].recipe = recipe; g_wpso[g_nwpso].pso = pso;
        g_wpso[g_nwpso].ahead = ahead;
        g_nwpso++;
    }
    pthread_mutex_unlock(&g_warm_mu);
}

static uint32_t mtl_prog_new(const char *vsrc, const char *fsrc, const char *what)
{
    uint32_t i;
    id<MTLFunction> vf = nil, ff = nil;
    uint64_t key;
    if (!vsrc || !fsrc) return 0;
    for (i = 0; i < MTL_MAX_PROG; i++) if (!g_prog[i].used) break;
    if (i == MTL_MAX_PROG) { fprintf(stderr, "  [MTL] out of program slots\n"); return 0; }
    key = nv2a_sc_is_open() ? nv2a_sc_key(vsrc, fsrc) : 0;
    @autoreleasepool {
        int have = key ? warm_fn_get(key, &vf, &ff) : 0;
        if (have) {
            if (have == 2) g_warm_hits++; else g_keep_hits++;
        } else {
            double t0 = now_ms();
            vf = compile_fn(vsrc, @"nv2a_vsh_main", what, 0);
            ff = vf ? compile_fn(fsrc, @"nv2a_psh_main", what, 1) : nil;
            g_ms_compile += now_ms() - t0;
            if (!vf || !ff) return 0;
            if (key) { nv2a_sc_add(key, vsrc, fsrc); warm_fn_put(key, vf, ff, 0); }
        }
        g_prog[i].vf = vf;
        g_prog[i].ff = ff;
    }
    g_prog[i].skey = key;
    {   /* the state it was translated from, to file its blend recipes under
         * (the shader seed: nv2a_gl.c) */
        extern uint32_t nv2a_seed_building_id(void);
        g_prog[i].sid = nv2a_seed_building_id();
    }
    g_prog[i].uses = uniforms_used(vsrc, fsrc);
    g_prog[i].gen = ++g_prog_gen;
    g_prog[i].used = 1;
    g_n_progs++;
    return i + 1;
}

static int prog_ok(uint32_t h) { return h && h <= MTL_MAX_PROG && g_prog[h - 1].used; }

static void mtl_prog_free(uint32_t h)
{
    if (!prog_ok(h)) return;
    /* Its pipelines stay in the cache under its old generation and are
     * never matched again; they go as their slots are reused. */
    g_prog[h - 1].vf = nil;
    g_prog[h - 1].ff = nil;
    g_prog[h - 1].used = 0;
    if (g_cur_prog == h) g_cur_prog = 0;
}

static int mtl_prog_uses(uint32_t h, int id)
{
    if (!prog_ok(h) || id < 0 || id >= NV2A_U_COUNT) return 0;
    return (g_prog[h - 1].uses >> id) & 1;
}

static void mtl_prog_use(uint32_t h) { if (prog_ok(h)) g_cur_prog = h; }

/* ---- uniforms -------------------------------------------------------------------------------- */

static void mtl_uniform_f(int id, const float *v, int n)
{
    const Nv2aUniformDesc *d;
    uint32_t off, stride;
    uint8_t *base;
    int k;
    if (id < 0 || id >= NV2A_U_COUNT || !v || n <= 0) return;
    d = &nv2a_uniform_desc[id];
    if (n > d->count) n = d->count;
    if (id == NV2A_U_C) {
        memcpy(g_consts, v, (size_t)n * 16);
        g_consts_dirty = 1;
        return;
    }
    if (!nv2a_uniform_msl_offset(id, &off, &stride)) return;
    base = d->frag ? g_fu : g_vu;
    switch (d->type) {
    case NV2A_UT_VEC3:     /* packed threes in, sixteen-byte slots out */
        for (k = 0; k < n; k++) memcpy(base + off + (uint32_t)k * stride, v + 3 * k, 12);
        break;
    case NV2A_UT_FLOAT: memcpy(base + off, v, (size_t)n * 4);  break;
    case NV2A_UT_VEC2:  memcpy(base + off, v, (size_t)n * 8);  break;
    case NV2A_UT_VEC4:  memcpy(base + off, v, (size_t)n * 16); break;
    case NV2A_UT_MAT2:  memcpy(base + off, v, (size_t)n * 16); break;  /* two columns of two */
    default: break;
    }
}

static void mtl_uniform_i(int id, int v)
{
    uint32_t off, stride;
    const Nv2aUniformDesc *d;
    int32_t i32 = v;
    if (id < 0 || id >= NV2A_U_COUNT) return;
    d = &nv2a_uniform_desc[id];
    if (d->type != NV2A_UT_INT) return;          /* sampler units: bound by index */
    if (!nv2a_uniform_msl_offset(id, &off, &stride)) return;
    memcpy((d->frag ? g_fu : g_vu) + off, &i32, 4);
}

static void mtl_texture(int unit, uint32_t tex)
{
    if (unit >= 0 && unit < 4) g_bound[unit] = tex;
}

/* ---- state ------------------------------------------------------------------------------------ */

static void mtl_state(const Nv2aRenderState *rs) { g_rs = *rs; }
static void mtl_state_invalidate(void) { E.valid = 0; }

static MTLCompareFunction cmp_of(uint32_t gl)
{
    switch (gl) {
    case 0x0200: return MTLCompareFunctionNever;
    case 0x0201: return MTLCompareFunctionLess;
    case 0x0202: return MTLCompareFunctionEqual;
    case 0x0203: return MTLCompareFunctionLessEqual;
    case 0x0204: return MTLCompareFunctionGreater;
    case 0x0205: return MTLCompareFunctionNotEqual;
    case 0x0206: return MTLCompareFunctionGreaterEqual;
    case 0x0207: return MTLCompareFunctionAlways;
    default:     return MTLCompareFunctionLessEqual;   /* as GL's mapping */
    }
}

static MTLBlendFactor blend_of(uint32_t f)
{
    switch (f) {
    case 0x0000: return MTLBlendFactorZero;
    case 0x0001: return MTLBlendFactorOne;
    case 0x0300: return MTLBlendFactorSourceColor;
    case 0x0301: return MTLBlendFactorOneMinusSourceColor;
    case 0x0302: return MTLBlendFactorSourceAlpha;
    case 0x0303: return MTLBlendFactorOneMinusSourceAlpha;
    case 0x0304: return MTLBlendFactorDestinationAlpha;
    case 0x0305: return MTLBlendFactorOneMinusDestinationAlpha;
    case 0x0306: return MTLBlendFactorDestinationColor;
    case 0x0307: return MTLBlendFactorOneMinusDestinationColor;
    case 0x0308: return MTLBlendFactorSourceAlphaSaturated;
    default:     return MTLBlendFactorOne;
    }
}

static MTLStencilOperation sop_of(uint32_t v)
{
    switch (v) {
    case 0x0000: return MTLStencilOperationZero;
    case 0x1E01: return MTLStencilOperationReplace;
    case 0x1E02: return MTLStencilOperationIncrementClamp;
    case 0x1E03: return MTLStencilOperationDecrementClamp;
    case 0x150A: return MTLStencilOperationInvert;
    case 0x8507: return MTLStencilOperationIncrementWrap;
    case 0x8508: return MTLStencilOperationDecrementWrap;
    default:     return MTLStencilOperationKeep;
    }
}

/* The colour write mask: the console's bits (blue 0, green 8, red 16, alpha
 * 24; 0 meaning never written, so everything) in Metal's. */
static MTLColorWriteMask cmask_of(uint32_t cm)
{
    MTLColorWriteMask m = MTLColorWriteMaskNone;
    if (!cm) cm = 0x01010101u;
    if ((cm >> 16) & 1) m |= MTLColorWriteMaskRed;
    if ((cm >>  8) & 1) m |= MTLColorWriteMaskGreen;
    if ( cm        & 1) m |= MTLColorWriteMaskBlue;
    if ((cm >> 24) & 1) m |= MTLColorWriteMaskAlpha;
    return m;
}

/* A pipeline state for a pair of functions and a recipe -- the 32 bits of
 * pso_for's key that describe the blend: on/off, write mask, the factors. */
static id<MTLRenderPipelineState> make_pso(id<MTLFunction> vf, id<MTLFunction> ff, uint32_t recipe,
                                           NSError *__autoreleasing *err)
{
    int blend = (int)(recipe >> 31);
    uint32_t cm = (recipe >> 27) & 0xF, src = (recipe >> 12) & 0xFFF, dst = recipe & 0xFFF;
    id<MTLRenderPipelineState> pso;
    @autoreleasepool {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = vf;
        pd.fragmentFunction = ff;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        pd.colorAttachments[0].writeMask = (MTLColorWriteMask)cm;
        if (blend) {
            pd.colorAttachments[0].blendingEnabled = YES;
            pd.colorAttachments[0].sourceRGBBlendFactor = blend_of(src);
            pd.colorAttachments[0].destinationRGBBlendFactor = blend_of(dst);
            pd.colorAttachments[0].sourceAlphaBlendFactor = blend_of(src);
            pd.colorAttachments[0].destinationAlphaBlendFactor = blend_of(dst);
            pd.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
            pd.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        }
        pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        pd.rasterSampleCount = (NSUInteger)g_samples;
        pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:err];
    }
    return pso;
}

/* The builders: worker w of n takes the cache's programs w, w+n, w+2n...
 * Each one's functions come from the table when the render thread (or the
 * other worker) already has them, and are compiled otherwise; then every
 * pipeline state an earlier run drew it with is made. */
static void warm_build(int w, int n, int count)
{
    int i, k;
    for (i = w; i < count; i += n) {
        uint64_t key; const char *v, *f;
        uint32_t rc[64];
        int nr;
        if (!nv2a_sc_entry(i, &key, &v, &f)) continue;
        @autoreleasepool {
            id<MTLFunction> vf = nil, ff = nil;
            if (!warm_fn_get(key, &vf, &ff)) {
                vf = compile_fn(v, @"nv2a_vsh_main", "a cached program", 0);
                ff = vf ? compile_fn(f, @"nv2a_psh_main", "a cached program", 1) : nil;
                if (!vf || !ff) continue;
                if (warm_fn_put(key, vf, ff, 1)) atomic_fetch_add(&g_warm_built, 1);
            }
            nr = nv2a_sc_recipes(key, rc, 64);
            for (k = 0; k < nr; k++) {
                NSError *err = nil;
                id<MTLRenderPipelineState> pso;
                if (warm_pso_get(key, rc[k], NULL)) continue;
                pso = make_pso(vf, ff, rc[k], &err);
                if (!pso) continue;
                warm_pso_put(key, rc[k], pso, 1);
                atomic_fetch_add(&g_warm_psos, 1);
            }
        }
    }
}

static void mtl_cache_open(const char *dir, const char *tag)
{
    enum { WORKERS = 2 };
    int n, w;
    double t0;
    if (!nv2a_sc_open(dir, tag)) return;
    n = nv2a_sc_count();
    if (!n) return;
    fprintf(stderr, "  [SHADERS] Metal: building %d programs from earlier runs in the background\n", n);
    t0 = now_ms();
    atomic_store(&g_warm_running, WORKERS);
    for (w = 0; w < WORKERS; w++) {
        int me = w;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            warm_build(me, WORKERS, n);
            if (atomic_fetch_sub(&g_warm_running, 1) == 1)
                fprintf(stderr, "  [SHADERS] Metal: %d programs and %d pipeline states built ahead in %.0f ms\n",
                        atomic_load(&g_warm_built), atomic_load(&g_warm_psos), now_ms() - t0);
        });
    }
}

static id<MTLRenderPipelineState> pso_for(uint32_t prog)
{
    static int nob = -1;
    MtlProg *p = &g_prog[prog - 1];
    int blend;
    uint32_t src, dst, cm, i, recipe;
    uint64_t key, h;
    NSError *err = nil;
    id<MTLRenderPipelineState> pso;

    if (nob < 0) nob = env_on("RECOMP_GL_NOBLEND");
    blend = g_rs.blend_enable && !nob;
    src = blend ? (uint32_t)g_rs.blend_src & 0xFFFF : 0;
    dst = blend ? (uint32_t)g_rs.blend_dst & 0xFFFF : 0;
    cm = (uint32_t)cmask_of(g_rs.color_mask);
    /* program generation | blending | write mask | source | destination;
     * the factors are GL enums, 0x0000-0x0001 and 0x0300-0x0308, which fit
     * in 12 bits each. Never 0: 0 marks an empty slot. */
    recipe = (uint32_t)((blend ? 1u : 0u) << 31 | cm << 27 | (src & 0xFFFu) << 12 | (dst & 0xFFFu));
    key = (uint64_t)p->gen << 32 | recipe;
    key = key * 2 + 1;
    h = key * 0x9E3779B97F4A7C15ull;
    for (i = 0; i < MTL_PSO_CACHE; i++) {
        MtlPso *e = &g_pso[(h + i) & (MTL_PSO_CACHE - 1)];
        if (e->key == key) return e->pso;
        if (!e->key) break;
    }
    /* The first use of this pair this run: kept with the state the program was
     * made from, so that a later start -- here, or anywhere the engine's list
     * of states goes -- can build the pipeline before it is needed. */
    if (p->sid) nv2a_sc_state_recipe(p->sid, recipe);
    {
        int ahead = 0;
        pso = p->skey ? warm_pso_get(p->skey, recipe, &ahead) : nil;
        if (pso) {
            if (ahead) g_warm_pso_hits++; else g_keep_pso_hits++;
        }
    }
    if (!pso) {
        double t0 = now_ms(), dt;
        pso = make_pso(p->vf, p->ff, recipe, &err);
        dt = now_ms() - t0;
        g_ms_pso += dt;
        if (dt > g_ms_pso_max) g_ms_pso_max = dt;
        if (pso && p->skey) { nv2a_sc_recipe_add(p->skey, recipe); warm_pso_put(p->skey, recipe, pso, 0); }
    }
    if (!pso) {
        static int said;
        if (said++ < 10)
            fprintf(stderr, "  [MTL] pipeline failed: %s\n", [[err localizedDescription] UTF8String]);
        return nil;
    }
    g_n_psos++;
    /* Into the first empty slot of the chain, or over the slot it hashes to
     * when the chain is full (the table is sized never to be). */
    for (i = 0; i < MTL_PSO_CACHE; i++) {
        MtlPso *e = &g_pso[(h + i) & (MTL_PSO_CACHE - 1)];
        if (!e->key) { e->key = key; e->pso = pso; return pso; }
    }
    g_pso[h & (MTL_PSO_CACHE - 1)].key = key;
    g_pso[h & (MTL_PSO_CACHE - 1)].pso = pso;
    return pso;
}

/* Depth and stencil as one object, keyed on all of it. A depth test that is
 * off neither tests nor writes -- OpenGL's rule, which Metal does not have:
 * "always" with writes on would write. Likewise a stencil test that is off
 * leaves the stencil alone. */
static id<MTLDepthStencilState> dss_for(void)
{
    uint64_t key = 0, h;
    uint32_t i, sf = 0, ops = 0;
    int dt = g_rs.depth_test, st = g_rs.stencil_test;
    MTLDepthStencilDescriptor *d;
    if (dt) key |= 1ull | (uint64_t)((uint32_t)g_rs.depth_func & 0xF) << 1
                        | (uint64_t)(g_rs.depth_write ? 1 : 0) << 5;
    if (st) {
        sf = g_rs.stencil_func;
        if (sf < 0x200 || sf > 0x207) sf = 0x207;
        ops = (uint32_t)sop_of(g_rs.stencil_op[0]) | (uint32_t)sop_of(g_rs.stencil_op[1]) << 3
            | (uint32_t)sop_of(g_rs.stencil_op[2]) << 6;
        key |= 1ull << 6 | (uint64_t)(sf & 0xF) << 7 | (uint64_t)(g_rs.stencil_rmask & 0xFF) << 11
             | (uint64_t)(g_rs.stencil_wmask & 0xFF) << 19 | (uint64_t)ops << 27;
    }
    key = key * 2 + 1;
    h = key * 0x9E3779B97F4A7C15ull;
    for (i = 0; i < MTL_DSS_CACHE; i++) {
        MtlDss *e = &g_dss[(h + i) & (MTL_DSS_CACHE - 1)];
        if (e->used && e->key == key) return e->dss;
        if (!e->used) break;
    }
    d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction = dt ? cmp_of((uint32_t)g_rs.depth_func) : MTLCompareFunctionAlways;
    d.depthWriteEnabled = (dt && g_rs.depth_write) ? YES : NO;
    if (st) {
        MTLStencilDescriptor *s = [MTLStencilDescriptor new];
        s.stencilCompareFunction = cmp_of(sf);
        s.stencilFailureOperation = sop_of(g_rs.stencil_op[0]);
        s.depthFailureOperation = sop_of(g_rs.stencil_op[1]);
        s.depthStencilPassOperation = sop_of(g_rs.stencil_op[2]);
        s.readMask = g_rs.stencil_rmask & 0xFFu;
        s.writeMask = g_rs.stencil_wmask & 0xFFu;
        d.frontFaceStencil = s;
        d.backFaceStencil = s;
    }
    for (i = 0; i < MTL_DSS_CACHE; i++) {
        MtlDss *e = &g_dss[(h + i) & (MTL_DSS_CACHE - 1)];
        if (!e->used) {
            e->used = 1; e->key = key;
            e->dss = [g_dev newDepthStencilStateWithDescriptor:d];
            return e->dss;
        }
    }
    return [g_dev newDepthStencilStateWithDescriptor:d];
}

/* ---- vertices and draws ----------------------------------------------------------------- */

static void mtl_vertices(const float *v, uint32_t nverts, uint32_t stride_floats,
                         const int attr_at[16], uint32_t const_mask,
                         const float const_attr[16][4])
{
    uint32_t i, bytes = nverts * stride_floats * (uint32_t)sizeof(float);
    void *dst;
    id<MTLBuffer> b = nil;
    uint32_t off = 0;

    frame_begin();
    dst = arena_alloc(bytes ? bytes : 16, &b, &off);
    if (!dst) { g_vbuf = nil; return; }
    if (bytes) memcpy(dst, v, bytes);
    g_vbuf = b; g_voff = off;

    g_attr.stride = stride_floats;
    g_attr.mask = 0;
    for (i = 0; i < 16; i++) {
        if (attr_at[i] >= 0) {
            g_attr.mask |= 1u << i;
            g_attr.off[i] = (uint32_t)attr_at[i];
        } else {
            g_attr.off[i] = 0;
            if (const_mask & (1u << i)) memcpy(g_generic[i], const_attr[i], 16);
        }
        memcpy(g_attr.value[i], g_generic[i], 16);
    }
}

static void fan_indices(uint32_t nverts)
{
    uint32_t n, i, *p;
    if (g_fan && g_fan_verts >= nverts) return;
    n = 1024;
    while (n < nverts) n *= 2;
    g_fan = [g_dev newBufferWithLength:(NSUInteger)(n - 2) * 3 * 4
                               options:MTLResourceStorageModeShared];
    p = (uint32_t *)[g_fan contents];
    for (i = 0; i + 2 < n; i++) { p[i*3] = 0; p[i*3+1] = i + 1; p[i*3+2] = i + 2; }
    g_fan_verts = n;
}

/* Everything the open encoder needs for a draw that it does not have yet. */
static int encode_state(void)
{
    static int cull_on = -1, flip = -1, force_clamp = -2;
    MtlRT *rt = &g_rt[g_cur_rt - 1];
    id<MTLRenderPipelineState> pso;
    id<MTLDepthStencilState> dss;
    int cull, wind, clip, u;
    MTLScissorRect sc;

    if (cull_on < 0) { cull_on = !env_on("RECOMP_GL_NOCULL"); flip = env_on("RECOMP_GL_CULLFLIP"); }
    if (force_clamp == -2) { const char *v = getenv("RECOMP_GL_DEPTHCLAMP");
                             force_clamp = v ? atoi(v) : -1; }

    pso = pso_for(g_cur_prog);
    if (!pso) return 0;
    if (!E.valid || E.pso != pso) { [g_enc setRenderPipelineState:pso]; E.pso = pso; }
    dss = dss_for();
    if (!E.valid || E.dss != dss) { [g_enc setDepthStencilState:dss]; E.dss = dss; }
    /* The reference whether or not this draw tests the stencil. A pass
     * starts with a reference of 0, and a draw without the test, the first
     * of a pass, marked the encoder state valid while E still held the last
     * pass's reference -- so the next draw that did test, with that same
     * reference, never sent it and tested against 0. The shadow pass tests
     * "1 <= stencil": against 0 it passes everywhere, and darkened the whole
     * frame. */
    if (!E.valid || E.stencil_ref != (g_rs.stencil_ref & 0xFFu)) {
        [g_enc setStencilReferenceValue:g_rs.stencil_ref & 0xFFu];
        E.stencil_ref = g_rs.stencil_ref & 0xFFu;
    }

    /* Culling: the registers hold GL's enums (0x404 front, 0x405 back, 0x408
     * both; 0x900 clockwise, 0x901 counter-clockwise), which pass straight
     * through for the reason nv2a_gfx_gl.c gives. Both is not a Metal cull
     * mode; draw() skips the triangles instead. */
    cull = (cull_on && g_rs.cull_enable)
         ? (g_rs.cull_face == 0x404 ? 1 : g_rs.cull_face == 0x408 ? 3 : 2) : 0;
    wind = (g_rs.front_face == 0x901) ^ flip;
    if (!E.valid || E.cull != cull) {
        [g_enc setCullMode:cull == 1 ? MTLCullModeFront : cull == 2 ? MTLCullModeBack : MTLCullModeNone];
        E.cull = cull;
    }
    if (!E.valid || E.wind != wind) {
        [g_enc setFrontFacingWinding:wind ? MTLWindingCounterClockwise : MTLWindingClockwise];
        E.wind = wind;
    }
    clip = force_clamp >= 0 ? force_clamp : ((g_rs.zclamp & 0xF0u) || g_rs.w_buffer) ? 1 : 0;
    if (!E.valid || E.clip != clip) {
        [g_enc setDepthClipMode:clip ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
        E.clip = clip;
    }
    /* Polygon offset, as Metal's depth bias: glPolygonOffset's units and
     * factor (the front half has turned them round for reversed depth). */
    {
        float bu = g_rs.poly_offset ? g_rs.poly_offset_units : 0.0f;
        float bf = g_rs.poly_offset ? g_rs.poly_offset_factor : 0.0f;
        if (!E.valid || E.bias_u != bu || E.bias_f != bf) {
            [g_enc setDepthBias:bu slopeScale:bf clamp:0.0f];
            E.bias_u = bu; E.bias_f = bf;
        }
    }

    /* The scissor, inside the target: Metal refuses one that is not. */
    if (g_rs.scissor_enable) {
        uint32_t x0 = g_rs.scissor[0], y0 = g_rs.scissor[1];
        uint32_t x1 = g_rs.scissor[2], y1 = g_rs.scissor[3];
        if (x1 > rt->pw) x1 = rt->pw;
        if (y1 > rt->ph) y1 = rt->ph;
        if (x0 > x1) x0 = x1;
        if (y0 > y1) y0 = y1;
        sc = (MTLScissorRect){ x0, y0, x1 - x0, y1 - y0 };
    } else {
        sc = (MTLScissorRect){ 0, 0, rt->pw, rt->ph };
    }
    if (!sc.width || !sc.height) return 0;          /* clipped to nothing */
    if (!E.valid || memcmp(&E.sc, &sc, sizeof sc)) { [g_enc setScissorRect:sc]; E.sc = sc; }

    /* Buffers: vertices, constants, the two uniform structs, the fetch
     * table. The constants only when they changed (3 KB a time). */
    if (!g_vbuf) return 0;
    if (!E.valid || E.vbuf != g_vbuf || E.voff != g_voff) {
        [g_enc setVertexBuffer:g_vbuf offset:g_voff atIndex:NV2A_MSL_BUF_VERTS];
        E.vbuf = g_vbuf; E.voff = g_voff;
    }
    if (g_consts_dirty || !g_consts_buf) {
        id<MTLBuffer> b = nil; uint32_t off = 0;
        void *dst = arena_alloc(sizeof g_consts, &b, &off);
        if (!dst) return 0;
        memcpy(dst, g_consts, sizeof g_consts);
        g_consts_buf = b; g_consts_off = off;
        g_consts_dirty = 0;
    }
    if (!E.valid || E.cbuf != g_consts_buf || E.coff != g_consts_off) {
        [g_enc setVertexBuffer:g_consts_buf offset:g_consts_off atIndex:NV2A_MSL_BUF_CONSTS];
        E.cbuf = g_consts_buf; E.coff = g_consts_off;
    }
    [g_enc setVertexBytes:g_vu length:g_vu_size atIndex:NV2A_MSL_BUF_VU];
    [g_enc setVertexBytes:&g_attr length:sizeof g_attr atIndex:NV2A_MSL_BUF_ATTRS];
    [g_enc setFragmentBytes:g_fu length:g_fu_size atIndex:NV2A_MSL_BUF_FU];

    for (u = 0; u < 4; u++) {
        uint32_t t = g_bound[u];
        if (!tex_ok(t)) t = 0;
        if (E.valid && E.tex[u] == t) continue;
        /* Nothing bound reads as (0, 0, 0, 1), which is what OpenGL gives a
         * shader sampling texture 0 -- and Metal gives no promise at all. */
        [g_enc setFragmentTexture:t ? g_tex[t - 1].tex : g_black atIndex:(NSUInteger)u];
        [g_enc setFragmentSamplerState:t ? g_tex[t - 1].smp : g_black_smp atIndex:(NSUInteger)u];
        E.tex[u] = t;
    }
    E.valid = 1;
    return 1;
}

static void mtl_draw(int prim, uint32_t count)
{
    MTLPrimitiveType pt;
    if (!count || !rt_ok(g_cur_rt) || !prog_ok(g_cur_prog)) return;
    if (prim >= NV2A_PRIM_TRIANGLES && g_rs.cull_enable && g_rs.cull_face == 0x408
        && !env_on("RECOMP_GL_NOCULL"))
        return;                                       /* front and back culled */
    if (!enc_begin()) return;
    if (!encode_state()) return;
    switch (prim) {
    case NV2A_PRIM_POINTS:     pt = MTLPrimitiveTypePoint; break;
    case NV2A_PRIM_LINES:      pt = MTLPrimitiveTypeLine; break;
    case NV2A_PRIM_LINE_STRIP: pt = MTLPrimitiveTypeLineStrip; break;
    case NV2A_PRIM_TRIANGLE_STRIP: pt = MTLPrimitiveTypeTriangleStrip; break;
    case NV2A_PRIM_LINE_LOOP: {
        /* A strip that comes back to the start. */
        id<MTLBuffer> b = nil; uint32_t off = 0, i;
        uint32_t *ix = (uint32_t *)arena_alloc((count + 1) * 4, &b, &off);
        if (!ix) return;
        for (i = 0; i < count; i++) ix[i] = i;
        ix[count] = 0;
        [g_enc drawIndexedPrimitives:MTLPrimitiveTypeLineStrip indexCount:count + 1
                           indexType:MTLIndexTypeUInt32 indexBuffer:b indexBufferOffset:off];
        g_n_draws++;
        return;
    }
    case NV2A_PRIM_TRIANGLE_FAN:
        if (count < 3) return;
        fan_indices(count);
        [g_enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:(count - 2) * 3
                           indexType:MTLIndexTypeUInt32 indexBuffer:g_fan indexBufferOffset:0];
        g_n_draws++;
        return;
    default:                   pt = MTLPrimitiveTypeTriangle; break;
    }
    [g_enc drawPrimitives:pt vertexStart:0 vertexCount:count];
    g_n_draws++;
}

/* ---- clears -------------------------------------------------------------------------------------- */

/* The utility shaders: a triangle that covers the target, one colour and one
 * depth (the clear), and a texture copied with filtering (the flip). */
static const char *UTIL_MSL =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct ClearIn  { float4 color; float depth; };\n"
"struct ClearOut { float4 color [[color(0)]]; float depth [[depth(any)]]; };\n"
"struct BlitV    { float4 pos [[position]]; float2 uv; };\n"
"vertex float4 nv_clear_v(uint vid [[vertex_id]]) {\n"
"    float2 p = float2((vid << 1) & 2, vid & 2);\n"
"    return float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
"}\n"
"fragment ClearOut nv_clear_f(constant ClearIn &c [[buffer(0)]]) {\n"
"    ClearOut o; o.color = c.color; o.depth = c.depth; return o;\n"
"}\n"
"vertex BlitV nv_blit_v(uint vid [[vertex_id]]) {\n"
"    float2 p = float2((vid << 1) & 2, vid & 2);\n"
"    BlitV o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
"    o.uv = float2(p.x, 1.0 - p.y);\n"
"    return o;\n"
"}\n"
"fragment float4 nv_blit_f(BlitV in [[stage_in]], texture2d<float> t [[texture(0)]],\n"
"                          sampler s [[sampler(0)]]) {\n"
"    return float4(t.sample(s, in.uv).rgb, 1.0);\n"
"}\n";

/* The clear's two pipelines, drawn into the title's targets: at their
 * number of samples. */
static int clear_psos(void)
{
    NSError *err = nil;
    MTLRenderPipelineDescriptor *pd;
    int i;
    for (i = 0; i < 2; i++) {
        pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [g_util_lib newFunctionWithName:@"nv_clear_v"];
        pd.fragmentFunction = [g_util_lib newFunctionWithName:@"nv_clear_f"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        pd.colorAttachments[0].writeMask = i ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
        pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        pd.rasterSampleCount = (NSUInteger)g_samples;
        g_clear_pso[i] = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!g_clear_pso[i]) {
            fprintf(stderr, "  [MTL] clear pipeline: %s\n", [[err localizedDescription] UTF8String]);
            return 0;
        }
    }
    return 1;
}

static int util_init(void)
{
    NSError *err = nil;
    MTLRenderPipelineDescriptor *pd;
    MTLSamplerDescriptor *sd;
    g_util_lib = [g_dev newLibraryWithSource:[NSString stringWithUTF8String:UTIL_MSL]
                                     options:[MTLCompileOptions new] error:&err];
    if (!g_util_lib) {
        fprintf(stderr, "  [MTL] utility shaders: %s\n", [[err localizedDescription] UTF8String]);
        return 0;
    }
    if (!clear_psos()) return 0;
    pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [g_util_lib newFunctionWithName:@"nv_blit_v"];
    pd.fragmentFunction = [g_util_lib newFunctionWithName:@"nv_blit_f"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_blit_pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!g_blit_pso) {
        fprintf(stderr, "  [MTL] present pipeline: %s\n", [[err localizedDescription] UTF8String]);
        return 0;
    }
    sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    g_linear_clamp = [g_dev newSamplerStateWithDescriptor:sd];
    return 1;
}

static void mtl_clear(unsigned mask, const float rgba[4], float depth,
                      uint32_t stencil, const uint32_t rect[4])
{
    MtlRT *rt;
    uint32_t x0 = 0, y0 = 0, x1, y1;
    int full;
    struct { float color[4]; float depth; float pad[3]; } cin;

    mask &= 7u;
    if (!mask || !rt_ok(g_cur_rt)) return;
    rt = &g_rt[g_cur_rt - 1];
    x1 = rt->pw; y1 = rt->ph;
    if (rect) {
        x0 = rect[0] < rt->pw ? rect[0] : rt->pw;
        y0 = rect[1] < rt->ph ? rect[1] : rt->ph;
        x1 = rect[2] < rt->pw ? rect[2] : rt->pw;
        y1 = rect[3] < rt->ph ? rect[3] : rt->ph;
        if (x1 <= x0 || y1 <= y0) return;
    }
    full = (x0 == 0 && y0 == 0 && x1 == rt->pw && y1 == rt->ph);

    /* At the start of a pass a whole-target clear is the pass's load action:
     * the hardware does it as the tiles are set up, for nothing. */
    if (full && !g_enc) {
        if (mask & 1) memcpy(g_pending_clear.rgba, rgba, sizeof g_pending_clear.rgba);
        if (mask & 2) g_pending_clear.depth = depth;
        if (mask & 4) g_pending_clear.stencil = stencil;
        g_pending_clear.mask |= mask;
        g_n_load_clears++;
        return;
    }

    /* Anywhere else, a triangle over the rectangle that writes the clear
     * values and passes every test. */
    if (!enc_begin()) return;
    [g_enc setRenderPipelineState:g_clear_pso[(mask & 1) ? 1 : 0]];
    {
        uint64_t key = (mask & 2 ? 1u : 0u) | (mask & 4 ? 2u : 0u);
        static id<MTLDepthStencilState> ds[4];
        if (!ds[key]) {
            MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
            d.depthCompareFunction = MTLCompareFunctionAlways;
            d.depthWriteEnabled = (key & 1) ? YES : NO;
            if (key & 2) {
                MTLStencilDescriptor *s = [MTLStencilDescriptor new];
                s.stencilCompareFunction = MTLCompareFunctionAlways;
                s.depthStencilPassOperation = MTLStencilOperationReplace;
                s.stencilFailureOperation = MTLStencilOperationReplace;
                s.depthFailureOperation = MTLStencilOperationReplace;
                s.readMask = 0xFF; s.writeMask = 0xFF;
                d.frontFaceStencil = s; d.backFaceStencil = s;
            }
            ds[key] = [g_dev newDepthStencilStateWithDescriptor:d];
        }
        [g_enc setDepthStencilState:ds[key]];
    }
    [g_enc setStencilReferenceValue:stencil & 0xFFu];
    [g_enc setCullMode:MTLCullModeNone];
    [g_enc setDepthClipMode:MTLDepthClipModeClamp];
    [g_enc setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];   /* a draw's polygon offset */
    [g_enc setScissorRect:(MTLScissorRect){ x0, y0, x1 - x0, y1 - y0 }];
    memset(&cin, 0, sizeof cin);
    memcpy(cin.color, rgba, sizeof cin.color);
    cin.depth = depth;
    [g_enc setFragmentBytes:&cin length:sizeof cin atIndex:0];
    [g_enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    E.valid = 0;                    /* everything above was the clear's */
    g_n_quad_clears++;
}

/* ---- the window ---------------------------------------------------------------------
 *
 * A finished frame reaches the window through a thread of its own.
 *
 * The first version presented from the render thread at the flip, and so
 * could never wait for the display: the boot sequence and the loading screens
 * run as fast as they can, hundreds of frames a second, while a drawable is
 * only handed out as fast as the display takes them -- waiting for one held
 * the title back thirty seconds. So it showed a frame only while fewer than
 * two were still on their way to the screen, and dropped it otherwise. Right
 * for the boot, wrong for everything else: in a window a frame takes two to
 * three refreshes to reach the glass (the GPU, the compositor, the display),
 * so at a steady sixty, one frame in three found two still in flight and was
 * dropped. A player's log showed exactly 400 of every 600 frames shown,
 * minute after minute -- forty a second, unevenly, which is a stutter.
 *
 * Now the flip only hands the frame over: a copy on the GPU into one of three
 * slots, and a note of which slot is the newest. The presenter waits for a
 * new frame, then for the display to give it a drawable, for as long as that
 * takes -- nothing else waits on it -- and shows the newest frame there is.
 * At sixty, every frame is shown. Faster than the display, the newest wins at
 * each refresh, as in the OpenGL window, which hands frames to the main
 * thread the same way. On a display faster than sixty (a ProMotion screen
 * runs at 120) each frame is also held for at least the refreshes a 60 Hz
 * frame should get, so that one arriving just after a refresh is not shown
 * for one and the next for three.
 */

typedef struct {
    id<MTLTexture> tex;      /* a copy of a finished frame */
    uint32_t       w, h;     /* its size, in pixels */
    int            wide;     /* to be shown wide (nv2a_wide_scene at its flip)... */
    double         aspect;   /* ...at this shape: 16:9, or the screen's own */
} MtlShown;
static MtlShown        g_shown[3];
static pthread_mutex_t g_shown_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_shown_cv = PTHREAD_COND_INITIALIZER;
static int             g_shown_new = -1;    /* the newest slot handed over */
static int             g_shown_busy = -1;   /* the slot being put on screen now */
static uint64_t        g_shown_serial;      /* frames handed over so far */
static int             g_presenter;         /* 0 not started, 1 running, -1 failed */

/* How long frames stayed on the screen, from the display's own timestamps:
 * the measure of smoothness, printed by the report. */
static pthread_mutex_t g_pace_mu = PTHREAD_MUTEX_INITIALIZER;
static double   g_pace_last;      /* when the previous frame appeared */
static uint64_t g_pace[6];        /* <12 ms, ~1/60 s, ~1.5/60, ~2/60, longer, pauses */
static uint64_t g_pace_lost;      /* drawables the system never showed */

static void pace_note(CFTimeInterval t)
{
    pthread_mutex_lock(&g_pace_mu);
    if (t <= 0.0) {
        g_pace_lost++;
    } else {
        if (g_pace_last > 0.0 && t > g_pace_last) {
            double ms = (t - g_pace_last) * 1000.0;
            g_pace[ms < 12.0 ? 0 : ms < 21.0 ? 1 : ms < 29.0 ? 2 : ms < 38.0 ? 3
                   : ms < 70.0 ? 4 : 5]++;
        }
        g_pace_last = t;
    }
    pthread_mutex_unlock(&g_pace_mu);
}

/* At least this long between two frames on the screen, in seconds (0: no
 * minimum). A display at sixty paces a 60 Hz game by itself. A faster one
 * shows each frame from the refresh after it arrives, so a frame that lands
 * just after a refresh is up for one refresh and the next for three; held to
 * just under the whole number of refreshes a sixtieth of a second takes, each
 * gets its two at 120 Hz. RECOMP_MTL_PACE=<ms> sets it, 0 turns it off. */
static double present_min_interval(void)
{
    static int env = -1;
    static double env_s;
    int hz, n;
    if (env < 0) {
        const char *v = getenv("RECOMP_MTL_PACE");
        env = (v && *v) ? 1 : 0;
        env_s = env ? atof(v) / 1000.0 : 0.0;
    }
    if (env) return env_s > 0.0 ? env_s : 0.0;
    hz = nv_window_refresh_hz ? nv_window_refresh_hz() : 0;
    if (hz < 100) return 0.0;
    n = (int)(hz / 60.0 + 0.05);
    return n >= 2 ? (n - 0.5) / hz : 0.0;
}

static void present_one(CAMetalLayer *layer, uint64_t *seen)
{
    id<CAMetalDrawable> drw;
    id<MTLCommandBuffer> cb;
    MtlShown *s;
    int slot;
    uint64_t serial;
    double gap;

    if (layer.device != g_dev) layer.device = g_dev;
    drw = [layer nextDrawable];              /* as long as the display takes */
    pthread_mutex_lock(&g_shown_mu);
    slot = g_shown_new;
    serial = g_shown_serial;
    if (drw && slot >= 0) g_shown_busy = slot;
    pthread_mutex_unlock(&g_shown_mu);
    if (!drw || slot < 0) {
        if (!drw) atomic_fetch_add(&g_n_no_drawable, 1);
        atomic_fetch_add(&g_n_skipped, serial - *seen);
        *seen = serial;
        return;
    }
    /* Frames handed over while this waited, and replaced before they could be
     * shown: the display had no room for them. */
    if (serial - *seen > 1) atomic_fetch_add(&g_n_skipped, serial - *seen - 1);
    *seen = serial;

    s = &g_shown[slot];
    cb = [g_queue commandBuffer];
    {
        id<MTLTexture> dt = drw.texture;
        double vw = (double)dt.width, vh = (double)dt.height, fw, fh, dw = vw, dh = vh;
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        id<MTLRenderCommandEncoder> e;
        /* The frame keeps its shape, between black bars; the wide scene the
         * widescreen mode draws (squeezed into 4:3) shown at its own shape --
         * 16:9 as the OpenGL view does it, or the iPhone's whole screen. */
        fw = s->w; fh = s->h;
        if (s->wide) { fw = s->aspect > 1.0 ? s->aspect : 16.0 / 9.0; fh = 1.0; }
        if (vw * fh > vh * fw) dw = vh * fw / fh; else dh = vw * fh / fw;
        rp.colorAttachments[0].texture = dt;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setViewport:(MTLViewport){ (vw - dw) / 2.0, (vh - dh) / 2.0, dw, dh, 0.0, 1.0 }];
        [e setRenderPipelineState:g_blit_pso];
        [e setFragmentTexture:s->tex atIndex:0];
        [e setFragmentSamplerState:g_linear_clamp atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
        if (atomic_fetch_add(&g_n_presented, 1) == 0)
            fprintf(stderr, "  [MTL] first frame into the window: %.0fx%.0f drawable, "
                    "picture %.0fx%.0f from a %ux%u frame; at least %.1f ms a frame on "
                    "screen (%d Hz display)\n", vw, vh, dw, dh, s->w, s->h,
                    present_min_interval() * 1000.0,
                    nv_window_refresh_hz ? nv_window_refresh_hz() : 0);
    }
#if TARGET_OS_SIMULATOR
    /* The iOS Simulator has neither the presented-time handler nor a minimum
     * duration: no pacing statistics there, and plain presents. */
    gap = present_min_interval();
    (void)gap;
    [cb presentDrawable:drw];
#else
    [drw addPresentedHandler:^(id<MTLDrawable> d) { pace_note(d.presentedTime); }];
    gap = present_min_interval();
    if (gap > 0.0) [cb presentDrawable:drw afterMinimumDuration:gap];
    else           [cb presentDrawable:drw];
#endif
    [cb commit];
    pthread_mutex_lock(&g_shown_mu);
    g_shown_busy = -1;
    pthread_mutex_unlock(&g_shown_mu);
}

static void *presenter_main(void *arg)
{
    uint64_t seen = 0;
    (void)arg;
    pthread_setname_np("nv2a-present");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    for (;;) {
        pthread_mutex_lock(&g_shown_mu);
        while (g_shown_serial == seen) pthread_cond_wait(&g_shown_cv, &g_shown_mu);
        pthread_mutex_unlock(&g_shown_mu);
        @autoreleasepool {
            CAMetalLayer *layer = nv_window_metal_layer
                                ? (__bridge CAMetalLayer *)nv_window_metal_layer() : nil;
            if (layer && (!nv_window_visible || nv_window_visible())) {
                present_one(layer, &seen);
            } else {
                /* Hidden, minimised or gone: nothing to show them on. */
                pthread_mutex_lock(&g_shown_mu);
                atomic_fetch_add(&g_n_hidden, g_shown_serial - seen);
                seen = g_shown_serial;
                pthread_mutex_unlock(&g_shown_mu);
            }
        }
    }
    return NULL;
}

static void presenter_start(void)
{
    pthread_t t;
    pthread_attr_t a;
    g_presenter = -1;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, presenter_main, NULL) == 0) g_presenter = 1;
    pthread_attr_destroy(&a);
    if (g_presenter != 1)
        fprintf(stderr, "  [MTL] could not start the presenter thread: nothing will be "
                "shown in the window\n");
}

/* A slot's texture, at the frame's size. Only ever called for a slot the
 * presenter is not using and will not pick (see mtl_present). */
static int shown_fit(int slot, uint32_t w, uint32_t h)
{
    MtlShown *s = &g_shown[slot];
    if (s->tex && s->tex.width == w && s->tex.height == h) return 1;
    @autoreleasepool {
        MTLTextureDescriptor *d =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:w height:h mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModePrivate;
        s->tex = [g_dev newTextureWithDescriptor:d];
    }
    return s->tex != nil;
}

static void mtl_present(uint32_t h, uint32_t pw, uint32_t ph)
{
    int give = -1;
    (void)pw; (void)ph;

    /* A frame that ends with a clear still waiting: do it, so what is shown
     * (and what the next frame loads) is what the title asked for. */
    flush_pending_clear();
    enc_end();

    /* Hand the frame to the presenter: copy it into a slot that is neither
     * the newest one (the presenter may be about to take that) nor the one
     * being shown -- of three, there is always one -- and publish it once
     * the copy is committed. The presenter's own command buffer, committed
     * later on the same queue, then reads a finished copy. */
    if (rt_ok(h) && nv_window_metal_layer && nv_window_metal_layer()
        && (!nv_window_visible || nv_window_visible())) {
        MtlRT *rt = &g_rt[h - 1];
        int slot;
        if (!g_presenter) presenter_start();
        pthread_mutex_lock(&g_shown_mu);
        for (slot = 0; slot < 3; slot++)
            if (slot != g_shown_new && slot != g_shown_busy) break;
        pthread_mutex_unlock(&g_shown_mu);
        if (g_presenter == 1 && slot < 3 && shown_fit(slot, rt->pw, rt->ph)) {
            id<MTLBlitCommandEncoder> b;
            frame_begin();
            b = [g_cb blitCommandEncoder];
            [b copyFromTexture:g_tex[rt->color - 1].tex sourceSlice:0 sourceLevel:0
                  sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(rt->pw, rt->ph, 1)
                     toTexture:g_shown[slot].tex destinationSlice:0 destinationLevel:0
             destinationOrigin:MTLOriginMake(0, 0, 0)];
            [b endEncoding];
            g_shown[slot].w = rt->pw;
            g_shown[slot].h = rt->ph;
            g_shown[slot].wide = nv2a_wide_scene();
            g_shown[slot].aspect = (double)nv2a_wide_aspect();
            give = slot;
        }
    } else if (nv_window_metal_layer && nv_window_metal_layer()) {
        atomic_fetch_add(&g_n_hidden, 1);
    }
    nv_window_wait_visible();
    submit(1, 0);
    if (give >= 0) {
        pthread_mutex_lock(&g_shown_mu);
        g_shown_new = give;
        g_shown_serial++;
        pthread_cond_signal(&g_shown_cv);
        pthread_mutex_unlock(&g_shown_mu);
    }
    g_n_frames++;
}

/* ---- the rest ------------------------------------------------------------------------------------ */

static int mtl_init(void)
{
    int i;
    g_dev = MTLCreateSystemDefaultDevice();
    if (!g_dev) return 0;
    { MTLCompileOptions *probe = [MTLCompileOptions new];
      const char *v = getenv("RECOMP_GL_INVARIANT");
      nv2a_vsh_msl_invariant = [probe respondsToSelector:@selector(setPreserveInvariance:)]
                               && !(v && !atoi(v)); }
    g_queue = [g_dev newCommandQueue];
    if (!g_queue) return 0;
    g_inflight = dispatch_semaphore_create(MTL_FRAMES_IN_FLIGHT);
    if (!util_init()) return 0;
    {
        static const uint8_t px[4] = { 0, 0, 0, 255 };
        MTLTextureDescriptor *td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:1 height:1 mipmapped:NO];
        Nv2aSampler s;
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        g_black = [g_dev newTextureWithDescriptor:td];
        [g_black replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:px bytesPerRow:4];
        memset(&s, 0, sizeof s);
        g_black_smp = sampler_for(&s);
    }
    g_vu_size = nv2a_uniform_msl_struct_size(0);
    g_fu_size = nv2a_uniform_msl_struct_size(1);
    g_vu = (uint8_t *)calloc(1, g_vu_size);
    g_fu = (uint8_t *)calloc(1, g_fu_size);
    if (!g_vu || !g_fu) return 0;
    for (i = 0; i < 16; i++) { g_generic[i][0] = g_generic[i][1] = g_generic[i][2] = 0.0f;
                               g_generic[i][3] = 1.0f; }
    fprintf(stderr, "  [MTL] %s; uniforms %u + %u bytes a draw, constants 3 KB when "
            "they change; %d frames in flight\n",
            [[g_dev name] UTF8String], g_vu_size, g_fu_size, MTL_FRAMES_IN_FLIGHT);
    return 1;
}

static uint64_t smaa_frames(void);     /* below, with SMAA */

static void mtl_report(void)
{
    if (nv2a_sc_is_open()) {
        int progs = 0, added = 0;
        nv2a_sc_stats(&progs, NULL, &added);
        fprintf(stderr, "  [SHADERS] programs asked for: %u built ahead, %u kept from earlier; pipeline states: "
                "%u built ahead, %u kept; background: %d programs, %d pipelines%s; on disk %d programs (%d new)\n",
                g_warm_hits, g_keep_hits, g_warm_pso_hits, g_keep_pso_hits, atomic_load(&g_warm_built),
                atomic_load(&g_warm_psos), atomic_load(&g_warm_running) ? " (still building)" : "", progs, added);
    }
    uint32_t live_tex = 0, live_rt = 0, live_prog = 0, i;
    for (i = 0; i < MTL_MAX_TEX; i++)  if (g_tex[i].used)  live_tex++;
    for (i = 0; i < MTL_MAX_RT; i++)   if (g_rt[i].used)   live_rt++;
    for (i = 0; i < MTL_MAX_PROG; i++) if (g_prog[i].used) live_prog++;
    fprintf(stderr, "  [MTL] %llu draws in %llu passes over %llu frames; %u programs, "
            "%u pipelines, %u textures made\n",
            (unsigned long long)g_n_draws, (unsigned long long)g_n_passes,
            (unsigned long long)g_n_frames, g_n_progs, g_n_psos, g_n_tex);
    /* What is alive now, as against made so far: the numbers a leak moves. */
    fprintf(stderr, "  [MTL] alive: %u textures (of them %u render targets), %u programs\n",
            live_tex, live_rt, live_prog);
    fprintf(stderr, "  [MTL] building: %.0f ms compiling %u programs, %.0f ms making %u "
            "pipelines (slowest %.1f ms)\n", g_ms_compile, g_n_progs, g_ms_pso, g_n_psos,
            g_ms_pso_max);
    if (g_samples > 1 || smaa_frames())
        fprintf(stderr, "  [MTL] anti-aliasing: %d samples a pixel; SMAA on %llu frames\n",
                g_samples, (unsigned long long)smaa_frames());
    fprintf(stderr, "  [MTL] clears: %llu as load actions, %llu drawn; %u surface copies, "
            "%u read-backs; arena peak %.1f MB a frame\n",
            (unsigned long long)g_n_load_clears, (unsigned long long)g_n_quad_clears,
            g_n_snapshots, g_n_readbacks, (double)g_arena_peak / (1024.0 * 1024.0));
    {
        uint64_t shown = atomic_load(&g_n_presented), skipped = atomic_load(&g_n_skipped);
        uint64_t hidden = atomic_load(&g_n_hidden), nodrw = atomic_load(&g_n_no_drawable);
        uint64_t p[6], lost, tot = 0;
        int i;
        if (shown || skipped || hidden || nodrw) {
            fprintf(stderr, "  [MTL] window: %llu frames shown, %llu replaced by a newer one "
                    "before the display had room, %llu while hidden, %llu without a drawable\n",
                    (unsigned long long)shown, (unsigned long long)skipped,
                    (unsigned long long)hidden, (unsigned long long)nodrw);
            pthread_mutex_lock(&g_pace_mu);
            memcpy(p, g_pace, sizeof p);
            lost = g_pace_lost;
            pthread_mutex_unlock(&g_pace_mu);
            for (i = 0; i < 5; i++) tot += p[i];
            if (tot)
                fprintf(stderr, "  [MTL] on screen, each frame stayed: under 12 ms %.1f%%, "
                        "a 60th of a second %.1f%%, 1.5 %.1f%%, 2 %.1f%%, longer %.1f%% "
                        "(and %llu pauses over 70 ms; %llu frames the system never showed)\n",
                        100.0 * p[0] / tot, 100.0 * p[1] / tot, 100.0 * p[2] / tot,
                        100.0 * p[3] / tot, 100.0 * p[4] / tot,
                        (unsigned long long)p[5], (unsigned long long)lost);
        }
    }
}

static double mtl_pipeline_ms(void) { return g_ms_pso; }
static void mtl_gpu_ms(double *busy, double *waited)
{
    *busy = (double)atomic_load(&g_gpu_busy_us) / 1000.0;
    *waited = g_ms_gpu_wait;
}

/* ---- multisampling ------------------------------------------------------------------------------ */

static int mtl_samples_max(void)
{
    int n;
    for (n = 8; n > 1; n /= 2)
        if ([g_dev supportsTextureSampleCount:(NSUInteger)n]) return n;
    return 1;
}

/* Every target drawn with n samples a pixel (1: none) -- taken only before
 * the first target is made and the first pipeline built: every pipeline is
 * compiled for one count, those built ahead from the shader cache included,
 * and Metal draws a pipeline into a target of that count only. So the front
 * half asks at start, and a change waits for the next start. The count is a
 * power of two the device supports, and no more than asked. */
static int mtl_samples(int n)
{
    int i, p = 1, max = mtl_samples_max();
    while (p * 2 <= n && p * 2 <= max) p *= 2;
    while (p > 1 && ![g_dev supportsTextureSampleCount:(NSUInteger)p]) p /= 2;
    if (p == g_samples) return p;
    for (i = 0; i < MTL_MAX_RT; i++)
        if (g_rt[i].used && !g_rt[i].flat) return g_samples;
    if (g_n_psos || atomic_load(&g_warm_running)) return g_samples;
    pthread_mutex_lock(&g_warm_mu);
    i = g_nwpso;
    pthread_mutex_unlock(&g_warm_mu);
    if (i) return g_samples;
    g_samples = p;
    if (!clear_psos()) { g_samples = 1; clear_psos(); }
    return g_samples;
}

/* ---- SMAA (nv2a_smaa.h) ------------------------------------------------------------------------
 *
 * Three passes over the frame being shown -- its edges into an RG8 image, the
 * blending weights into an RGBA8 one, and the frame blended by them into a
 * target of this backend's own (colour only, one sample), which the window
 * is then given -- recorded into the frame's command buffer ahead of the
 * flip. The title's surfaces are untouched; a multisampled frame is already
 * resolved, as every pass is when it ends.
 *
 * The library is compiled for the frame's size (nv2a_smaa_msl_source) in the
 * background, as Metal offers: until it is ready the frame is shown as it is,
 * so turning SMAA on never holds the game for the compiler.
 */
static pthread_mutex_t g_sm_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    /* under g_sm_mu: what the compiler has finished, and what it is doing */
    uint32_t ready_w, ready_h;
    id<MTLRenderPipelineState> ready[3];
    uint32_t building_w, building_h;
    /* the render thread's */
    id<MTLTexture> area, search, edges, weights;
    uint32_t out;                       /* the target pass 3 draws into */
    uint32_t w, h;                      /* of edges, weights and out */
    uint64_t frames;
} g_sm;
static atomic_int g_sm_bad;            /* would not build: frames as they are */

static uint64_t smaa_frames(void) { return g_sm.frames; }

static void smaa_build(uint32_t pw, uint32_t ph)
{
    char *src = nv2a_smaa_msl_source(pw, ph);
    NSString *ns;
    double t0 = now_ms();
    if (!src) { atomic_store(&g_sm_bad, 1); return; }
    ns = [NSString stringWithUTF8String:src];
    free(src);
    pthread_mutex_lock(&g_sm_mu);
    g_sm.building_w = pw; g_sm.building_h = ph;
    pthread_mutex_unlock(&g_sm_mu);
    [g_dev newLibraryWithSource:ns options:[MTLCompileOptions new]
              completionHandler:^(id<MTLLibrary> lib, NSError *err) {
        static NSString *const fn[3][2] = { { @"smaa_edge_v", @"smaa_edge_f" },
                                            { @"smaa_weight_v", @"smaa_weight_f" },
                                            { @"smaa_blend_v", @"smaa_blend_f" } };
        id<MTLRenderPipelineState> pso[3];
        int p;
        if (!lib) {
            fprintf(stderr, "  [MTL] SMAA failed to compile; frames are shown without it:\n%s\n",
                    [[err localizedDescription] UTF8String]);
            atomic_store(&g_sm_bad, 1);
            return;
        }
        for (p = 0; p < 3; p++) {
            NSError *e2 = nil;
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = [lib newFunctionWithName:fn[p][0]];
            pd.fragmentFunction = [lib newFunctionWithName:fn[p][1]];
            pd.colorAttachments[0].pixelFormat = p == 0 ? MTLPixelFormatRG8Unorm
                                                        : MTLPixelFormatRGBA8Unorm;
            pso[p] = pd.vertexFunction && pd.fragmentFunction
                   ? [g_dev newRenderPipelineStateWithDescriptor:pd error:&e2] : nil;
            if (!pso[p]) {
                fprintf(stderr, "  [MTL] SMAA pipeline %d failed; frames are shown without it: %s\n",
                        p, e2 ? [[e2 localizedDescription] UTF8String] : "no function");
                atomic_store(&g_sm_bad, 1);
                return;
            }
        }
        pthread_mutex_lock(&g_sm_mu);
        for (p = 0; p < 3; p++) g_sm.ready[p] = pso[p];
        g_sm.ready_w = pw; g_sm.ready_h = ph;
        g_sm.building_w = g_sm.building_h = 0;
        pthread_mutex_unlock(&g_sm_mu);
        fprintf(stderr, "  [MTL] SMAA: built for %ux%u in %.0f ms\n", pw, ph, now_ms() - t0);
    }];
}

static id<MTLTexture> smaa_texture(MTLPixelFormat fmt, uint32_t w, uint32_t h, const void *px,
                                   uint32_t bpr)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
    id<MTLTexture> t;
    if (px) {
        d.usage = MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModeShared;
    } else {
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModePrivate;
    }
    t = [g_dev newTextureWithDescriptor:d];
    if (t && px) [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:px bytesPerRow:bpr];
    return t;
}

/* One pass: a triangle over `dst`, which is cleared to 0 first if `cleared`
 * (a pass that discards where it has nothing to write). */
static void smaa_pass(id<MTLTexture> dst, int cleared, id<MTLRenderPipelineState> pso,
                      id<MTLTexture> t0, id<MTLTexture> t1, id<MTLTexture> t2)
{
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    id<MTLRenderCommandEncoder> e;
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = cleared ? MTLLoadActionClear : MTLLoadActionDontCare;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    e = [g_cb renderCommandEncoderWithDescriptor:rp];
    if (!e) return;
    [e setRenderPipelineState:pso];
    [e setFragmentTexture:t0 atIndex:0];
    if (t1) [e setFragmentTexture:t1 atIndex:1];
    if (t2) [e setFragmentTexture:t2 atIndex:2];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

static uint32_t mtl_target_smaa(uint32_t h)
{
    MtlRT *rt;
    id<MTLRenderPipelineState> pso[3];
    uint32_t pw, ph;
    int have, building, p;
    if (!rt_ok(h) || atomic_load(&g_sm_bad)) return h;
    rt = &g_rt[h - 1];
    pw = rt->pw; ph = rt->ph;
    pthread_mutex_lock(&g_sm_mu);
    have = g_sm.ready_w == pw && g_sm.ready_h == ph;
    for (p = 0; p < 3; p++) pso[p] = have ? g_sm.ready[p] : nil;
    building = g_sm.building_w != 0;
    pthread_mutex_unlock(&g_sm_mu);
    if (!have) {
        if (!building) smaa_build(pw, ph);
        return h;                       /* as it is, until the passes exist */
    }
    @autoreleasepool {
        if (!g_sm.area) {
            g_sm.area = smaa_texture(MTLPixelFormatRG8Unorm, NV2A_SMAA_AREA_W, NV2A_SMAA_AREA_H,
                                     nv2a_smaa_area(), NV2A_SMAA_AREA_W * 2);
            g_sm.search = smaa_texture(MTLPixelFormatR8Unorm, NV2A_SMAA_SEARCH_W, NV2A_SMAA_SEARCH_H,
                                       nv2a_smaa_search(), NV2A_SMAA_SEARCH_W);
        }
        if (g_sm.w != pw || g_sm.h != ph || !rt_ok(g_sm.out)) {
            mtl_target_free(g_sm.out);
            g_sm.edges = smaa_texture(MTLPixelFormatRG8Unorm, pw, ph, NULL, 0);
            g_sm.weights = smaa_texture(MTLPixelFormatRGBA8Unorm, pw, ph, NULL, 0);
            g_sm.out = target_new(pw, ph, 1);
            g_sm.w = pw; g_sm.h = ph;
            if (!g_sm.area || !g_sm.search || !g_sm.edges || !g_sm.weights || !g_sm.out) {
                fprintf(stderr, "  [MTL] SMAA: no images at %ux%u; frames are shown without it\n", pw, ph);
                atomic_store(&g_sm_bad, 1);
                return h;
            }
            fprintf(stderr, "  [MTL] SMAA: the frame shown at %ux%u\n", pw, ph);
        }
        /* A clear still waiting on the frame: the passes see it done. */
        if (h == g_cur_rt) flush_pending_clear();
        enc_end();
        frame_begin();
        smaa_pass(g_sm.edges, 1, pso[0], g_tex[rt->color - 1].tex, nil, nil);
        smaa_pass(g_sm.weights, 1, pso[1], g_sm.edges, g_sm.area, g_sm.search);
        smaa_pass(g_tex[g_rt[g_sm.out - 1].color - 1].tex, 0, pso[2],
                  g_tex[rt->color - 1].tex, g_sm.weights, nil);
    }
    E.valid = 0;
    g_sm.frames++;
    return g_sm.out;
}

/* Reversed depth (nv2a_vsh.c, depth_defines): the depth buffer is already
 * 32-bit floating point and the clip volume already runs z from 0 to w, so
 * there is nothing to switch -- only a new target's depth starts at 0, the
 * far plane, instead of 1. The comparisons and clear values are turned
 * around by the front half. */
static int mtl_zrev(int want)
{
    g_zrev = want ? 1 : 0;
    return g_zrev;
}

static const Nv2aGfx g_metal = {
    "metal", NV2A_LANG_MSL, 0,
    mtl_init,
    mtl_target_new, mtl_target_free, mtl_target_bind, mtl_target_texture,
    mtl_target_snapshot, mtl_target_read, mtl_target_read_depth,
    mtl_tex_new, mtl_tex_free, mtl_tex_read,
    mtl_prog_new, mtl_prog_free, mtl_prog_uses, mtl_prog_use,
    mtl_uniform_f, mtl_uniform_i,
    mtl_texture, mtl_state, mtl_state_invalidate,
    mtl_vertices, mtl_draw, mtl_clear, mtl_present, mtl_report,
    NULL, NULL, NULL, NULL, NULL,       /* kept vertex buffers: not here */
    NULL,                               /* uniform_f_part */
    mtl_pipeline_ms,
    mtl_cache_open, NULL,       /* builds on a thread of its own */
    mtl_target_smaa,
    mtl_samples_max, mtl_samples,
    mtl_zrev,
    mtl_gpu_ms,
    mtl_tex_new_dxt,            /* DXT as they are, where the GPU takes BC */
};

const Nv2aGfx *nv2a_gfx_metal(void) { return &g_metal; }
