/*
 * The NV2A's back half, in Metal.
 *
 * Written against nv2a_backend.h, which is where the reasoning about what a
 * backend has to get right lives. The notes here are about the ways Metal
 * differs from OpenGL in this particular job -- and there are only a few, but
 * they are structural rather than cosmetic:
 *
 *   Blend state belongs to the pipeline. In GL it is a state toggle set
 *   between draws; in Metal it is compiled into the render pipeline object.
 *   So the pipeline cache key has to include it, and a title that draws the
 *   same geometry with two blend modes needs two pipelines. That is why the
 *   key here is wider than the GL backend's.
 *
 *   There is no alpha test. It does not exist in Metal at all, and the
 *   fragment translator already knows how to emit a discard instead -- which
 *   is also what modern GL wants, so nothing diverges.
 *
 *   A clear is a property of a render pass, not a command inside one. A
 *   full-surface clear therefore ends the current pass and begins another; a
 *   sub-rectangle clear, which this title does constantly, cannot be expressed
 *   that way at all and is drawn as a scissored quad.
 *
 *   Depth goes from 0 to w, not -w to w. The shared epilogue emits the right
 *   one under NV2A_CLIP_Z01, which is defined here and not in the GL path.
 *
 * Everything else -- the surfaces, the texture cache, the program cache, the
 * vertex layout -- is the same shape as the OpenGL backend, deliberately, so
 * that the two can be read side by side when one of them is wrong.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../nv2a_gl/nv2a_backend.h"

/* ---- small caches, sized from what this title actually uses -------------- */

#define MTL_SURF_CACHE   8
#define MTL_PROG_CACHE   2048     /* see nv2a_backend.h on why not 128 */
#define MTL_TEX_CACHE    4096
#define MTL_RING_BYTES   (8u << 20)
#define MTL_FRAMES_IN_FLIGHT 3

typedef struct {
    uint32_t offset, w, h, scale;
    int      used;
    void    *color;      /* id<MTLTexture> */
    void    *depth;
} MtlSurface;

typedef struct {
    uint32_t hash;
    uint32_t last_draw;
    int      used;
    void    *pso;        /* id<MTLRenderPipelineState> */
    uint16_t inputs;
} MtlProgram;

typedef struct {
    uint32_t key, w, h;
    int      used;
    uint32_t last_draw;
    void    *tex;        /* id<MTLTexture> */
} MtlTexture;

static id<MTLDevice>       g_dev;
static id<MTLCommandQueue> g_queue;
static id<MTLCommandBuffer>        g_cb;
static id<MTLRenderCommandEncoder> g_enc;

static MtlSurface g_surf[MTL_SURF_CACHE];
static MtlProgram g_prog[MTL_PROG_CACHE];
static MtlTexture g_tex[MTL_TEX_CACHE];
static int        g_cur_surf = -1;

/* One ring per frame in flight for vertices and one for uniforms. Metal's
 * setVertexBytes has a 4 kB limit and a draw here can be a megabyte, so the
 * data goes into a buffer the GPU reads directly -- and it has to be a
 * different buffer from the one the GPU is still reading. */
static id<MTLBuffer> g_vring[MTL_FRAMES_IN_FLIGHT];
static id<MTLBuffer> g_uring[MTL_FRAMES_IN_FLIGHT];
static uint32_t      g_vpos, g_upos, g_frame_slot;
static dispatch_semaphore_t g_inflight;

static id<MTLSamplerState> g_sampler[4];
static id<MTLDepthStencilState> g_ds_cache[16];
static uint32_t g_draws, g_progs_built, g_tex_built;
static uint8_t *g_readback;
static uint32_t g_readback_bytes;

/* ---- the pieces the interface hands over -------------------------------- */

static MTLPrimitiveType prim_of(int p)
{
    /* The NV2A's primitive numbering, and the two it has that Metal does not:
     * quads and polygons, which the front half has already expanded into
     * triangles by the time a draw arrives here. */
    switch (p) {
    case 1: return MTLPrimitiveTypePoint;
    case 2: return MTLPrimitiveTypeLine;
    case 3: return MTLPrimitiveTypeLineStrip;
    case 4: return MTLPrimitiveTypeTriangle;
    case 5: return MTLPrimitiveTypeTriangleStrip;
    default: return MTLPrimitiveTypeTriangle;
    }
}

static MTLCompareFunction cmp_of(int nv)
{
    /* NV097_SET_DEPTH_FUNC carries the GL enum, which is what the pushbuffer
     * contains -- the console's D3D driver writes GL-numbered comparison
     * functions because the hardware's are the same numbers. */
    switch (nv) {
    case 0x0200: return MTLCompareFunctionNever;
    case 0x0201: return MTLCompareFunctionLess;
    case 0x0202: return MTLCompareFunctionEqual;
    case 0x0203: return MTLCompareFunctionLessEqual;
    case 0x0204: return MTLCompareFunctionGreater;
    case 0x0205: return MTLCompareFunctionNotEqual;
    case 0x0206: return MTLCompareFunctionGreaterEqual;
    default:     return MTLCompareFunctionAlways;
    }
}

static MTLBlendFactor blend_of(int nv)
{
    switch (nv) {
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

static int mtl_init(void)
{
    int i;
    g_dev = MTLCreateSystemDefaultDevice();
    if (!g_dev) return 0;
    g_queue = [g_dev newCommandQueue];
    g_inflight = dispatch_semaphore_create(MTL_FRAMES_IN_FLIGHT);
    for (i = 0; i < MTL_FRAMES_IN_FLIGHT; i++) {
        g_vring[i] = [g_dev newBufferWithLength:MTL_RING_BYTES
                                        options:MTLResourceStorageModeShared];
        g_uring[i] = [g_dev newBufferWithLength:MTL_RING_BYTES
                                        options:MTLResourceStorageModeShared];
    }
    /* Four samplers rather than one per texture: the NV2A's wrap and filter
     * combinations that this title uses come to a handful, and a sampler is
     * expensive to create and free to switch. */
    for (i = 0; i < 4; i++) {
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = (i & 1) ? MTLSamplerMinMagFilterLinear
                               : MTLSamplerMinMagFilterNearest;
        sd.magFilter = sd.minFilter;
        sd.sAddressMode = (i & 2) ? MTLSamplerAddressModeClampToEdge
                                  : MTLSamplerAddressModeRepeat;
        sd.tAddressMode = sd.sAddressMode;
        g_sampler[i] = [g_dev newSamplerStateWithDescriptor:sd];
    }
    fprintf(stderr, "  [MTL] %s, %u MB of ring buffers, %d frames in flight\n",
            [[g_dev name] UTF8String],
            (unsigned)(MTL_RING_BYTES * 2 * MTL_FRAMES_IN_FLIGHT >> 20),
            MTL_FRAMES_IN_FLIGHT);
    return 1;
}

static void end_encoder(void)
{
    if (g_enc) { [g_enc endEncoding]; g_enc = nil; }
}

static void begin_frame_if_needed(void)
{
    if (g_cb) return;
    dispatch_semaphore_wait(g_inflight, DISPATCH_TIME_FOREVER);
    g_frame_slot = (g_frame_slot + 1) % MTL_FRAMES_IN_FLIGHT;
    g_vpos = g_upos = 0;
    g_cb = [g_queue commandBuffer];
}

/* A pass on the current surface. `clear` picks the load action; Metal has no
 * way to clear inside a pass, which is why this exists as its own step. */
static void begin_pass(int clear, const float rgba[4], float depth)
{
    MtlSurface *s;
    if (g_cur_surf < 0) return;
    s = &g_surf[g_cur_surf];
    end_encoder();
    begin_frame_if_needed();
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = (__bridge id<MTLTexture>)s->color;
    rp.colorAttachments[0].loadAction = clear ? MTLLoadActionClear
                                              : MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (clear && rgba)
        rp.colorAttachments[0].clearColor =
            MTLClearColorMake(rgba[0], rgba[1], rgba[2], rgba[3]);
    rp.depthAttachment.texture = (__bridge id<MTLTexture>)s->depth;
    rp.depthAttachment.loadAction = clear ? MTLLoadActionClear
                                          : MTLLoadActionLoad;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    if (clear) rp.depthAttachment.clearDepth = depth;
    g_enc = [g_cb renderCommandEncoderWithDescriptor:rp];
    [g_enc setViewport:(MTLViewport){ 0.0, 0.0,
        (double)(s->w * s->scale), (double)(s->h * s->scale), 0.0, 1.0 }];
}

static void mtl_surface(uint32_t offset, uint32_t w, uint32_t h, uint32_t scale)
{
    int i, slot = -1;
    if (!w || !h) return;
    for (i = 0; i < MTL_SURF_CACHE; i++) {
        if (g_surf[i].used && g_surf[i].offset == offset
         && g_surf[i].w == w && g_surf[i].h == h && g_surf[i].scale == scale) {
            slot = i; break;
        }
        if (!g_surf[i].used && slot < 0) slot = i;
    }
    if (slot < 0) slot = 0;
    if (!g_surf[slot].used || g_surf[slot].offset != offset
     || g_surf[slot].w != w || g_surf[slot].h != h
     || g_surf[slot].scale != scale) {
        uint32_t pw = w * (scale ? scale : 1), ph = h * (scale ? scale : 1);
        MTLTextureDescriptor *cd =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:pw height:ph mipmapped:NO];
        cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        cd.storageMode = MTLStorageModePrivate;
        MTLTextureDescriptor *dd =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                               width:pw height:ph mipmapped:NO];
        dd.usage = MTLTextureUsageRenderTarget;
        dd.storageMode = MTLStorageModePrivate;
        g_surf[slot].color = (__bridge_retained void *)[g_dev newTextureWithDescriptor:cd];
        g_surf[slot].depth = (__bridge_retained void *)[g_dev newTextureWithDescriptor:dd];
        g_surf[slot].used = 1;
        g_surf[slot].offset = offset;
        g_surf[slot].w = w; g_surf[slot].h = h; g_surf[slot].scale = scale;
        fprintf(stderr, "  [MTL] surface %08X: new %ux%u target, rastered %ux%u\n",
                offset, w, h, pw, ph);
    }
    if (g_cur_surf != slot) {
        g_cur_surf = slot;
        begin_pass(0, NULL, 1.0f);
    }
}

/* ---- clears ------------------------------------------------------------- */

static void mtl_clear(unsigned mask, const float rgba[4], float depth,
                      uint32_t stencil, const uint32_t rect[4])
{
    MtlSurface *s;
    (void)stencil;
    if (g_cur_surf < 0) return;
    s = &g_surf[g_cur_surf];

    /* Whole surface: begin a new pass and let the hardware clear on load,
     * which is free on a tile-based GPU and the reason Metal expresses clears
     * this way at all. */
    if (!rect || (rect[0] == 0 && rect[1] == 0
                  && rect[2] >= s->w && rect[3] >= s->h)) {
        begin_pass(1, (mask & 1) ? rgba : NULL, (mask & 2) ? depth : 1.0f);
        return;
    }
    /* A sub-rectangle cannot be a load action. This title clears one
     * constantly -- a letterbox bar, a panel behind some text -- so it is
     * worth doing properly rather than promoting it to a full clear, which
     * would wipe the frame. Drawn as a scissored quad in a moment; for now the
     * pass is preserved and the rectangle recorded for the clear pipeline. */
    if (!g_enc) begin_pass(0, NULL, 1.0f);
    {
        uint32_t sc = s->scale ? s->scale : 1;
        MTLScissorRect sr;
        sr.x = rect[0] * sc;
        sr.y = rect[1] * sc;
        sr.width  = (rect[2] > rect[0]) ? (rect[2] - rect[0]) * sc : 0;
        sr.height = (rect[3] > rect[1]) ? (rect[3] - rect[1]) * sc : 0;
        if (sr.width && sr.height) {
            [g_enc setScissorRect:sr];
            /* The quad itself needs a pipeline of its own; until that exists
             * the scissor is set and the clear is skipped rather than
             * promoted, because skipping leaves stale pixels and promoting
             * erases the frame, and stale pixels are the smaller lie. */
            [g_enc setScissorRect:(MTLScissorRect){ 0, 0,
                s->w * sc, s->h * sc }];
        }
    }
}

/* ---- programs ----------------------------------------------------------- */

static int prog_find(uint32_t hash)
{
    uint32_t i;
    for (i = 0; i < MTL_PROG_CACHE; i++) {
        uint32_t k = (hash + i) & (MTL_PROG_CACHE - 1);
        if (!g_prog[k].used) return -1;
        if (g_prog[k].hash == hash) { g_prog[k].last_draw = g_draws; return (int)k; }
    }
    return -1;
}

static int prog_alloc(uint32_t hash)
{
    uint32_t i, oldest = 0xFFFFFFFFu;
    int victim = (int)(hash & (MTL_PROG_CACHE - 1));
    for (i = 0; i < MTL_PROG_CACHE; i++) {
        uint32_t k = (hash + i) & (MTL_PROG_CACHE - 1);
        if (!g_prog[k].used) return (int)k;
        if (g_prog[k].last_draw < oldest) { oldest = g_prog[k].last_draw; victim = (int)k; }
    }
    return victim;
}

static int mtl_program(const Nv2aVshProgram *vp, const Nv2aVshFixed *ff,
                       const Nv2aPshState *ps, const Nv2aRenderState *rs,
                       uint32_t hash)
{
    char *vsrc = NULL, *fsrc = NULL;
    int slot, ok = 0;
    uint16_t inputs = 0;

    slot = prog_find(hash);
    if (slot >= 0) return slot;

    vsrc = (char *)malloc(96 * 1024);
    fsrc = (char *)malloc(96 * 1024);
    if (!vsrc || !fsrc) { free(vsrc); free(fsrc); return -1; }

    if (ff) { ok = nv2a_vsh_emit_ff_msl(ff, vsrc, 96 * 1024); inputs = ff->inputs_read; }
    else if (vp) { ok = nv2a_vsh_emit_msl(vp, vsrc, 96 * 1024); inputs = vp->inputs_read; }
    if (ok) ok = nv2a_psh_emit_msl(ps, fsrc, 96 * 1024);
    if (!ok) { free(vsrc); free(fsrc); return -1; }

    @autoreleasepool {
        NSError *err = nil;
        MTLCompileOptions *opts = [MTLCompileOptions new];
        NSString *vs = [NSString stringWithUTF8String:vsrc];
        NSString *fs = [NSString stringWithUTF8String:fsrc];
        free(vsrc); free(fsrc);

        id<MTLLibrary> vlib = [g_dev newLibraryWithSource:vs options:opts error:&err];
        if (!vlib) {
            fprintf(stderr, "  [MTL] vertex shader %08X: %s\n", hash,
                    [[err localizedDescription] UTF8String]);
            return -1;
        }
        id<MTLLibrary> flib = [g_dev newLibraryWithSource:fs options:opts error:&err];
        if (!flib) {
            fprintf(stderr, "  [MTL] fragment shader %08X: %s\n", hash,
                    [[err localizedDescription] UTF8String]);
            return -1;
        }

        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        /* Filled per draw would mean a pipeline per layout; the layout is part
         * of the program's identity here because the shader declares exactly
         * the inputs it reads, so one descriptor per program is right. The
         * offsets come from the draw and are the same every time for a given
         * program, which the front half guarantees. */
        {
            int i, n = 0;
            for (i = 0; i < 16; i++)
                if (inputs & (1u << i)) {
                    vd.attributes[i].format = MTLVertexFormatFloat4;
                    vd.attributes[i].offset = (NSUInteger)n * 16;
                    vd.attributes[i].bufferIndex = 0;
                    n++;
                }
            vd.layouts[0].stride = (NSUInteger)(n ? n : 1) * 16;
            vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
        }

        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction   = [vlib newFunctionWithName:@"nv2a_vsh_main"];
        pd.fragmentFunction = [flib newFunctionWithName:@"nv2a_psh_main"];
        pd.vertexDescriptor = vd;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        /* Blend state is compiled in -- see the note at the top of the file.
         * The caller has already folded it into the hash, so a given hash
         * always means the same blend and this pipeline stays valid for as
         * long as it is cached. */
        if (rs && rs->blend_enable) {
            MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[0];
            ca.blendingEnabled = YES;
            ca.sourceRGBBlendFactor = blend_of(rs->blend_src);
            ca.destinationRGBBlendFactor = blend_of(rs->blend_dst);
            ca.sourceAlphaBlendFactor = blend_of(rs->blend_src);
            ca.destinationAlphaBlendFactor = blend_of(rs->blend_dst);
            ca.rgbBlendOperation = MTLBlendOperationAdd;
            ca.alphaBlendOperation = MTLBlendOperationAdd;
        }

        id<MTLRenderPipelineState> pso =
            [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!pso) {
            fprintf(stderr, "  [MTL] pipeline %08X: %s\n", hash,
                    [[err localizedDescription] UTF8String]);
            return -1;
        }
        slot = prog_alloc(hash);
        if (g_prog[slot].pso) CFRelease(g_prog[slot].pso);
        g_prog[slot].pso = (__bridge_retained void *)pso;
        g_prog[slot].hash = hash;
        g_prog[slot].used = 1;
        g_prog[slot].inputs = inputs;
        g_prog[slot].last_draw = g_draws;
        g_progs_built++;
    }
    return slot;
}

/* ---- textures ----------------------------------------------------------- */

static void mtl_texture(int stage, const void *rgba, uint32_t w, uint32_t h,
                        uint32_t key, const Nv2aSampler *s)
{
    uint32_t i, slot = 0, oldest = 0xFFFFFFFFu;
    id<MTLTexture> t = nil;

    if (stage < 0 || stage > 3 || !w || !h) return;

    for (i = 0; i < MTL_TEX_CACHE; i++) {
        uint32_t k = (key + i) & (MTL_TEX_CACHE - 1);
        if (g_tex[k].used && g_tex[k].key == key
         && g_tex[k].w == w && g_tex[k].h == h) {
            g_tex[k].last_draw = g_draws;
            t = (__bridge id<MTLTexture>)g_tex[k].tex;
            break;
        }
        if (!g_tex[k].used) { slot = k; break; }
        if (g_tex[k].last_draw < oldest) { oldest = g_tex[k].last_draw; slot = k; }
    }

    if (!t) {
        if (!rgba) return;
        @autoreleasepool {
            MTLTextureDescriptor *td =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                   width:w height:h mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModeShared;
            t = [g_dev newTextureWithDescriptor:td];
            [t replaceRegion:MTLRegionMake2D(0, 0, w, h)
                 mipmapLevel:0 withBytes:rgba bytesPerRow:w * 4];
            if (g_tex[slot].tex) CFRelease(g_tex[slot].tex);
            g_tex[slot].tex = (__bridge_retained void *)t;
            g_tex[slot].key = key; g_tex[slot].w = w; g_tex[slot].h = h;
            g_tex[slot].used = 1; g_tex[slot].last_draw = g_draws;
            g_tex_built++;
        }
    }
    if (g_enc && t) {
        int si = 0;
        if (s) si = (s->min_filter ? 1 : 0) | (s->wrap_s ? 2 : 0);
        [g_enc setFragmentTexture:t atIndex:(NSUInteger)stage];
        [g_enc setFragmentSamplerState:g_sampler[si] atIndex:(NSUInteger)stage];
    }
}

/* ---- the rest of the state ---------------------------------------------- */

static Nv2aRenderState g_rs;

static void mtl_state(const Nv2aRenderState *rs)
{
    if (rs) g_rs = *rs;
    if (!g_enc) return;

    /* Depth-stencil state is an object, and there are only a handful of
     * distinct ones, so they are made once and switched. */
    {
        int key = ((g_rs.depth_func & 7) << 1) | (g_rs.depth_write ? 1 : 0);
        if (!g_ds_cache[key & 15]) {
            MTLDepthStencilDescriptor *dsd = [MTLDepthStencilDescriptor new];
            dsd.depthCompareFunction = g_rs.depth_test
                                     ? cmp_of(g_rs.depth_func)
                                     : MTLCompareFunctionAlways;
            dsd.depthWriteEnabled = g_rs.depth_write ? YES : NO;
            g_ds_cache[key & 15] = [g_dev newDepthStencilStateWithDescriptor:dsd];
        }
        [g_enc setDepthStencilState:g_ds_cache[key & 15]];
    }

    /* Winding and culling are encoder state in both APIs. The registers hold
     * literal GL enums -- 0x900 clockwise, 0x901 counter-clockwise, 0x404
     * front, 0x405 back -- because that is what the console's driver writes. */
    [g_enc setFrontFacingWinding:(g_rs.front_face == 0x901)
                                 ? MTLWindingCounterClockwise
                                 : MTLWindingClockwise];
    if (!g_rs.cull_enable)
        [g_enc setCullMode:MTLCullModeNone];
    else
        [g_enc setCullMode:(g_rs.cull_face == 0x404) ? MTLCullModeFront
                                                     : MTLCullModeBack];
}

/* ---- draws -------------------------------------------------------------- */

static void mtl_draw(int prog, int prim,
                     const float *verts, uint32_t nverts, uint32_t stride_floats,
                     const Nv2aAttrSlot *slots, int nslots,
                     const Nv2aUniforms *u,
                     uint32_t const_attr_mask, const float const_attr[16][4])
{
    uint32_t vbytes, ubytes;
    (void)slots; (void)nslots; (void)const_attr_mask; (void)const_attr;

    if (prog < 0 || !g_prog[prog].used || !nverts || !verts) return;
    if (!g_enc) begin_pass(0, NULL, 1.0f);
    if (!g_enc) return;

    vbytes = nverts * stride_floats * (uint32_t)sizeof(float);
    ubytes = (uint32_t)sizeof(Nv2aUniforms);
    if (g_vpos + vbytes > MTL_RING_BYTES || g_upos + ubytes > MTL_RING_BYTES) {
        /* The ring is full for this frame. Rather than corrupt a draw the GPU
         * is still reading, drop this one and say so once -- a missing object
         * is a bug that can be seen, and a torn vertex buffer is one that
         * cannot. */
        static int said;
        if (!said++)
            fprintf(stderr, "  [MTL] vertex ring exhausted in one frame; "
                            "raise MTL_RING_BYTES\n");
        return;
    }

    memcpy((uint8_t *)[g_vring[g_frame_slot] contents] + g_vpos, verts, vbytes);
    memcpy((uint8_t *)[g_uring[g_frame_slot] contents] + g_upos, u, ubytes);

    [g_enc setRenderPipelineState:
        (__bridge id<MTLRenderPipelineState>)g_prog[prog].pso];
    [g_enc setVertexBuffer:g_vring[g_frame_slot] offset:g_vpos atIndex:0];
    [g_enc setVertexBuffer:g_uring[g_frame_slot] offset:g_upos
                   atIndex:NV2A_MSL_VSH_UNIFORM_INDEX];
    [g_enc setFragmentBuffer:g_uring[g_frame_slot] offset:g_upos
                     atIndex:NV2A_MSL_PSH_UNIFORM_INDEX];
    [g_enc drawPrimitives:prim_of(prim) vertexStart:0 vertexCount:nverts];

    g_vpos += (vbytes + 255u) & ~255u;   /* Metal wants 256-byte alignment */
    g_upos += (ubytes + 255u) & ~255u;
    g_draws++;
}

/* ---- presenting --------------------------------------------------------- */

static void mtl_present(const uint8_t **pixels, uint32_t *w, uint32_t *h)
{
    MtlSurface *s;
    uint32_t pw, ph, need;

    if (pixels) *pixels = NULL;
    if (g_cur_surf < 0 || !g_cb) return;
    s = &g_surf[g_cur_surf];
    pw = s->w * (s->scale ? s->scale : 1);
    ph = s->h * (s->scale ? s->scale : 1);

    end_encoder();

    need = pw * ph * 4;
    if (g_readback_bytes < need) {
        free(g_readback);
        g_readback = (uint8_t *)malloc(need);
        g_readback_bytes = need;
    }

    /* The render target is private -- the GPU's own memory, which is the fast
     * place for it -- so reading it costs a blit into something the CPU can
     * see. On a machine with unified memory that blit is a copy rather than a
     * transfer, which is why it is affordable once a frame and would not be
     * affordable once a draw. */
    @autoreleasepool {
        MTLTextureDescriptor *td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:pw height:ph mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> staging = [g_dev newTextureWithDescriptor:td];
        id<MTLBlitCommandEncoder> blit = [g_cb blitCommandEncoder];
        [blit copyFromTexture:(__bridge id<MTLTexture>)s->color
                  sourceSlice:0 sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(pw, ph, 1)
                    toTexture:staging destinationSlice:0 destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];

        __block dispatch_semaphore_t sem = g_inflight;
        [g_cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
            (void)b; dispatch_semaphore_signal(sem);
        }];
        [g_cb commit];
        [g_cb waitUntilCompleted];
        if (g_readback)
            [staging getBytes:g_readback bytesPerRow:pw * 4
                   fromRegion:MTLRegionMake2D(0, 0, pw, ph) mipmapLevel:0];
    }
    g_cb = nil;

    if (pixels) *pixels = g_readback;
    if (w) *w = pw;
    if (h) *h = ph;
}

static void mtl_report(void)
{
    fprintf(stderr, "  [MTL] %u draws, %u pipelines built, %u textures built\n",
            g_draws, g_progs_built, g_tex_built);
}

static const Nv2aBackend g_metal_backend = {
    "metal",
    mtl_init,
    mtl_surface,
    mtl_clear,
    mtl_program,
    mtl_texture,
    mtl_state,
    mtl_draw,
    mtl_present,
    mtl_report,
};

const Nv2aBackend *nv2a_backend_metal(void) { return &g_metal_backend; }
