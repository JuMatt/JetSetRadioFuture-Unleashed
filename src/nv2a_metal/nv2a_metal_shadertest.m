/*
 * Every shader this title uses, handed to the real Metal compiler.
 *
 * The Metal backend has to be written blind, so the translator's output cannot
 * be checked on the machine it is written on. It can be checked on the target.
 * These files are the Metal translation of every distinct program JSRF
 * compiled during a run -- emitted by the same code that emits the GLSL the
 * game is actually being rendered with -- and this compiles each one and builds
 * a pipeline from the pair.
 *
 * The point is not that it passes. The point is that when it does not, the
 * failure names one program and one line, which is a debuggable thing; whereas
 * a Metal backend brought up all at once against a hundred and forty-six
 * untested shader translations is not.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>

static NSString *slurp(NSString *path)
{
    return [NSString stringWithContentsOfFile:path
                                     encoding:NSUTF8StringEncoding error:nil];
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 2) { printf("usage: shadertest <dir>\n"); return 2; }
        NSString *dir = [NSString stringWithUTF8String:argv[1]];
        NSFileManager *fm = [NSFileManager defaultManager];
        NSArray *all = [fm contentsOfDirectoryAtPath:dir error:nil];
        NSArray *verts = [[all filteredArrayUsingPredicate:
            [NSPredicate predicateWithFormat:@"SELF ENDSWITH '.vert.metal'"]]
            sortedArrayUsingSelector:@selector(compare:)];

        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("no Metal device\n"); return 1; }
        MTLCompileOptions *opts = [MTLCompileOptions new];

        int pairs = 0, vs_ok = 0, fs_ok = 0, pso_ok = 0, shown = 0;
        for (NSString *vn in verts) {
            NSString *fn = [vn stringByReplacingOccurrencesOfString:@".vert.metal"
                                                         withString:@".frag.metal"];
            NSString *vsrc = slurp([dir stringByAppendingPathComponent:vn]);
            NSString *fsrc = slurp([dir stringByAppendingPathComponent:fn]);
            if (!vsrc || !fsrc) continue;
            pairs++;

            NSError *err = nil;
            id<MTLLibrary> vlib = [dev newLibraryWithSource:vsrc options:opts error:&err];
            if (!vlib) {
                if (shown++ < 6)
                    printf("VERTEX %s:\n%s\n", [vn UTF8String],
                           [[err localizedDescription] UTF8String]);
                continue;
            }
            vs_ok++;

            id<MTLLibrary> flib = [dev newLibraryWithSource:fsrc options:opts error:&err];
            if (!flib) {
                if (shown++ < 6)
                    printf("FRAGMENT %s:\n%s\n", [fn UTF8String],
                           [[err localizedDescription] UTF8String]);
                continue;
            }
            fs_ok++;

            /* A pipeline as well as two libraries: Metal matches the stages by
             * the shape of the varying struct, and two units that compile
             * separately can still refuse to be joined. That is the failure
             * this whole arrangement exists to catch early. */
            id<MTLFunction> vf = [vlib newFunctionWithName:@"nv2a_vsh_main"];
            id<MTLFunction> ff = [flib newFunctionWithName:@"nv2a_psh_main"];
            if (!vf || !ff) {
                if (shown++ < 6)
                    printf("ENTRY POINT missing in %s: vertex %s, fragment %s\n",
                           [vn UTF8String], vf ? "ok" : "NOT FOUND",
                           ff ? "ok" : "NOT FOUND");
                continue;
            }

            MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
            for (int i = 0; i < 16; i++) {
                vd.attributes[i].format = MTLVertexFormatFloat4;
                vd.attributes[i].offset = (NSUInteger)i * 16;
                vd.attributes[i].bufferIndex = 0;
            }
            vd.layouts[0].stride = 16 * 16;

            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = vf;
            pd.fragmentFunction = ff;
            pd.vertexDescriptor = vd;
            pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
            id<MTLRenderPipelineState> pso =
                [dev newRenderPipelineStateWithDescriptor:pd error:&err];
            if (!pso) {
                if (shown++ < 6)
                    printf("PIPELINE %s:\n%s\n", [vn UTF8String],
                           [[err localizedDescription] UTF8String]);
                continue;
            }
            pso_ok++;
        }
        printf("\n%d shader pairs from the running game\n", pairs);
        printf("  vertex compiled:   %d\n", vs_ok);
        printf("  fragment compiled: %d\n", fs_ok);
        printf("  pipeline linked:   %d\n", pso_ok);
        return pso_ok == pairs ? 0 : 1;
    }
}
