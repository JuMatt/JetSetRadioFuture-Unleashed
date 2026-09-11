/*
 * Metal, proved rather than assumed.
 *
 * The shader translators already emit Metal Shading Language; what does not
 * exist yet is the backend that feeds them. Before writing one blind -- and it
 * has to be written blind, because Metal cannot be compiled or run on the
 * machine this is being developed on -- this establishes the things the whole
 * design rests on, on the machine that will run it:
 *
 *   1. A device and a queue exist, and the GPU is the one we think it is.
 *   2. MSL compiles AT RUN TIME from a string. The offline `metal` tool is not
 *      installed on this Mac, and it does not need to be: every shader this
 *      port will ever run is generated from microcode the title uploads, so
 *      they can only be compiled at run time anyway.
 *   3. A render pipeline can take its vertex function from one library and its
 *      fragment function from another. The two translators emit separate
 *      translation units that both declare the shared varying struct, and
 *      merging them would mean rewriting one of them.
 *   4. Something actually rasterises, and the pixels can be read back and
 *      checked -- not "no error was returned", but "the triangle is there".
 *   5. Metal 4's command allocators, if this SDK has them.
 *
 * Each of those is a thing that would otherwise be discovered a thousand lines
 * later, in code that cannot be bisected because none of it has ever run.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>

/* The same split the real backend will use: the vertex stage and the fragment
 * stage come from separate libraries and meet at a struct declared once. */
static const char *kVaryings =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct Nv2aVshOut {\n"
    "    float4 position [[position]];\n"
    "    float4 oD0;\n"
    "    float4 oT0;\n"
    "};\n";

static const char *kVertexSrc =
    "struct Nv2aVshIn { float4 v0 [[attribute(0)]]; float4 v3 [[attribute(3)]]; };\n"
    "struct Nv2aVshUniforms { float4 c[192]; float4 vpScale; float4 vpOff; };\n"
    "vertex Nv2aVshOut nv2a_vs(Nv2aVshIn in [[stage_in]],\n"
    "                          constant Nv2aVshUniforms &U [[buffer(16)]])\n"
    "{\n"
    "    Nv2aVshOut o;\n"
    "    float4 p = float4(dot(in.v0, U.c[0]), dot(in.v0, U.c[1]),\n"
    "                      dot(in.v0, U.c[2]), dot(in.v0, U.c[3]));\n"
    "    o.position = p;\n"
    "    o.oD0 = in.v3;\n"
    "    o.oT0 = float4(0.0);\n"
    "    return o;\n"
    "}\n";

static const char *kFragmentSrc =
    "struct Nv2aPshUniforms { float4 fogColor; float alphaRef; };\n"
    "fragment float4 nv2a_fs(Nv2aVshOut in [[stage_in]],\n"
    "                        constant Nv2aPshUniforms &U [[buffer(0)]])\n"
    "{\n"
    "    return in.oD0;\n"
    "}\n";

typedef struct { float x, y, z, w; float r, g, b, a; } Vtx;

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("FAIL: no Metal device\n"); return 1; }
        printf("device: %s\n", [[dev name] UTF8String]);
        printf("  unified memory: %s\n", [dev hasUnifiedMemory] ? "yes" : "no");
        if (@available(macOS 13.0, *))
            printf("  argument buffers tier: %ld\n",
                   (long)[dev argumentBuffersSupport]);
        printf("  MTL4CommandQueue in this SDK: %s\n",
               NSClassFromString(@"MTL4CommandQueueDescriptor") ? "yes" : "no");
        printf("  MTL4Compiler in this SDK: %s\n",
               NSProtocolFromString(@"MTL4Compiler") ? "yes" : "no");

        NSError *err = nil;
        MTLCompileOptions *opts = [MTLCompileOptions new];

        /* 2 and 3: two libraries, compiled from strings at run time. */
        NSString *vsrc = [NSString stringWithFormat:@"%s%s", kVaryings, kVertexSrc];
        NSString *fsrc = [NSString stringWithFormat:@"%s%s", kVaryings, kFragmentSrc];
        id<MTLLibrary> vlib = [dev newLibraryWithSource:vsrc options:opts error:&err];
        if (!vlib) { printf("FAIL: vertex library: %s\n",
                            [[err localizedDescription] UTF8String]); return 1; }
        id<MTLLibrary> flib = [dev newLibraryWithSource:fsrc options:opts error:&err];
        if (!flib) { printf("FAIL: fragment library: %s\n",
                            [[err localizedDescription] UTF8String]); return 1; }
        printf("runtime MSL compilation: ok (two separate libraries)\n");

        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        vd.attributes[0].format = MTLVertexFormatFloat4;
        vd.attributes[0].offset = 0;
        vd.attributes[0].bufferIndex = 0;
        vd.attributes[3].format = MTLVertexFormatFloat4;
        vd.attributes[3].offset = 16;
        vd.attributes[3].bufferIndex = 0;
        vd.layouts[0].stride = sizeof(Vtx);

        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction   = [vlib newFunctionWithName:@"nv2a_vs"];
        pd.fragmentFunction = [flib newFunctionWithName:@"nv2a_fs"];
        pd.vertexDescriptor = vd;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        id<MTLRenderPipelineState> pso =
            [dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!pso) { printf("FAIL: pipeline: %s\n",
                           [[err localizedDescription] UTF8String]); return 1; }
        printf("pipeline from two libraries: ok\n");

        /* 4: rasterise something and look at it. */
        const int W = 64, H = 64;
        MTLTextureDescriptor *td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeManaged;
        id<MTLTexture> color = [dev newTextureWithDescriptor:td];

        MTLTextureDescriptor *dd =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                               width:W height:H mipmapped:NO];
        dd.usage = MTLTextureUsageRenderTarget;
        dd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> depth = [dev newTextureWithDescriptor:dd];

        /* An identity transform in c[0..3], so the vertices below are already
         * in clip space -- the real backend's constants come from the title. */
        float uniforms[192 * 4 + 8];
        memset(uniforms, 0, sizeof uniforms);
        uniforms[0] = 1.0f; uniforms[5] = 1.0f;
        uniforms[10] = 1.0f; uniforms[15] = 1.0f;

        Vtx verts[3] = {
            { -0.8f, -0.8f, 0.5f, 1.0f,  0.0f, 0.0f, 1.0f, 1.0f },
            {  0.8f, -0.8f, 0.5f, 1.0f,  0.0f, 0.0f, 1.0f, 1.0f },
            {  0.0f,  0.8f, 0.5f, 1.0f,  0.0f, 0.0f, 1.0f, 1.0f },
        };
        float pshu[8] = { 0 };

        id<MTLCommandQueue> q = [dev newCommandQueue];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = color;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.0, 0.0, 1.0);
        rp.depthAttachment.texture = depth;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = 1.0;

        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> enc =
            [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:pso];
        [enc setVertexBytes:verts length:sizeof verts atIndex:0];
        [enc setVertexBytes:uniforms length:sizeof uniforms atIndex:16];
        [enc setFragmentBytes:pshu length:sizeof pshu atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit synchronizeResource:color];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if ([cb error]) { printf("FAIL: command buffer: %s\n",
                                 [[[cb error] localizedDescription] UTF8String]);
                          return 1; }

        uint8_t px[64 * 64 * 4];
        [color getBytes:px bytesPerRow:W * 4
             fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];

        /* The centre should be the triangle's blue, a corner the red clear.
         * Anything else and something rasterised that is not what was asked
         * for, which is worth knowing now rather than in ten thousand draws. */
        int ci = (H / 2) * W * 4 + (W / 2) * 4;
        int blue = px[ci] > 200 && px[ci + 1] < 60 && px[ci + 2] < 60;
        int red  = px[3 * 4 + 2] > 200 && px[3 * 4] < 60;
        printf("centre pixel BGRA %3u %3u %3u %3u  (want blue)\n",
               px[ci], px[ci+1], px[ci+2], px[ci+3]);
        printf("corner pixel BGRA %3u %3u %3u %3u  (want red)\n",
               px[12], px[13], px[14], px[15]);
        printf("%s: a triangle was rasterised and read back correctly\n",
               (blue && red) ? "PASS" : "FAIL");
        return (blue && red) ? 0 : 1;
    }
}
