# The Metal backend

## Why there is a second backend at all

The OpenGL backend exists because it can be brought up and debugged on a
headless Linux box with llvmpipe. It is not the target. The target is Apple
silicon, where OpenGL is deprecated, capped at 4.1, and — on iOS — absent.

## What is already shared, and what is not

The translation from NV2A microcode to a shading language is the expensive,
error-prone part, and it is done once. `nv2a_vsh.c` and `nv2a_psh.c` each
generate one *body* and wrap it per dialect:

| | GLSL 330 | Metal |
|---|---|---|
| vector types | built in | `typedef float4 vec4;` and friends |
| `lessThan`, `greaterThanEqual`, `inversesqrt` | built in | four inline shims |
| inputs | `layout(location=N) in vec4 vN` | `float4 vN [[attribute(N)]]` in `Nv2aVshIn` |
| constants | `uniform vec4 c[192]` | `constant Nv2aVshUniforms&` at buffer 16 |
| varyings | `out`/`in` globals | one `Nv2aVshOut` struct, shared verbatim |
| texture fetch | `texture(texN, uv)` | `texN.sample(smpN, uv)` |
| stage constants | `const vec4` | `constant vec4` — Metal has address spaces |
| discard | `discard` | `discard_fragment()`, via `NV2A_DISCARD` |
| clip depth | z in [-w, w] | z in [0, w], like the console — `NV2A_CLIP_Z01` |

Everything else — every instruction, every combiner stage, the viewport
epilogue, the alpha test — is the same generated text in both. That is
deliberate: JSRF compiles over a thousand distinct programs and nobody reads
them, so two independent emitters would diverge on exactly the programs no
one looked at, and the symptom would be one wrong-looking object in one scene.

The two stages are compiled as separate Metal libraries. A render pipeline can
take its vertex function from one and its fragment function from another, and
doing so avoids having to merge two generated translation units that both
declare the shared varying struct. Metal matches the stages by that struct's
shape, which is why it has exactly one definition, handed out by
`nv2a_msl_varying_struct()`.

## The clip-depth trap

OpenGL's clip volume runs z from -w to w; Metal's and D3D's from 0 to w. The
console is D3D. The GLSL epilogue therefore *adds* a remap that the Metal one
must not have. Getting this backwards does not produce a black screen: it
produces a picture where everything renders, in the right places, with half
the depth range collapsed — which reads as z-fighting and bad sorting rather
than as a coordinate bug.

## What Metal 4 changes for this design

Metal 4 (macOS/iOS 26, Apple silicon M1+ / A14+) is not a rename of Metal 3.
The parts that matter here:

- **Command allocators.** Command buffers no longer own their memory; a pool
  of `MTL4CommandAllocator` does, and an allocator cannot be reset while its
  commands are in flight. So the backend needs one allocator per frame in
  flight, not one per command buffer.
- **Argument tables.** Bindings are an explicit object rather than a series of
  `setFragmentTexture:` calls: textures go in by `gpuResourceID`, buffers by
  `gpuAddress`. Table state is captured per draw, so a table can be rewritten
  between draws. This suits a pushbuffer translator well — the NV2A rebinds
  constantly, and an argument table is cheaper to rewrite than a sequence of
  setter calls.
- **Explicit compilation.** `MTL4Compiler` compiles pipelines, and function
  descriptors are mandatory. Since the shaders are generated at run time from
  whatever microcode the title uploads, compilation is on the hot path the
  first time each program appears — the same problem the GL backend solves
  with `g_pcache`, and the same solution: hash the microcode, cache the
  pipeline, and expect a few hundred distinct pipelines for a title.

None of this touches the translation. It is all in how the translated shaders
are fed.

## Order of work

1. Platform layer: Darwin already has its mapping, memory-status and
   process-query paths filled in. The remaining Linux-isms are in the crash
   reporter (`ucontext` register names) and the build files.
2. A window and a swapchain, and the GL backend's `nv2a_gl.c` split so that
   the API-agnostic half — pushbuffer method decode, texture format decode,
   vertex fetch, surface tracking, roughly two thirds of it — is shared.
3. The Metal backend proper: pipeline cache, argument tables, the draw path.
4. iOS: the same backend, a different window and input source.

## What has been proved, on the machine that will run it

Metal cannot be compiled or run on the machine this port is developed on, so
every piece of it is established on the target before anything is built on top
of it. As of the session that wrote the backend skeleton:

| | result |
|---|---|
| device | Apple M2 Pro, unified memory, argument buffers tier 1 |
| `MTL4CommandQueue`, `MTL4Compiler` in the SDK | present — Metal 4 proper, not Metal 3 renamed |
| offline `metal` tool installed | **no**, and not needed: every shader is generated at run time from uploaded microcode |
| MSL compiled from a string at run time | works |
| pipeline from two separate libraries | works |
| a triangle rasterised and read back | correct |
| **every shader JSRF uses** — 146 vertex/fragment pairs | 146 compiled, 146 linked |

That last row is the one that matters. The expensive, error-prone half of a
Metal backend is translating NV2A vertex microcode and register combiners into
a shading language, and it is now checked against the real title's shaders
rather than against a test case. What remains is plumbing, where a mistake is a
black screen that can be bisected rather than a silently wrong pixel in one
scene.

## The interface

`src/nv2a_gl/nv2a_backend.h` is the specification, not this document. It
carries, beside the operation each one applies to, every trap the OpenGL
backend cost real time to learn:

- a frame is a flip, not a render pass
- the fixed-function matrices must not go in the vertex constant file
- GL's clip volume is `[-w, w]` and Metal's is `[0, w]`
- the internal resolution is not the surface size
- a program cache sized for a menu is a performance bug in a level
- fetch only the attributes the program reads

## What is left

1. Route the existing state machine in `nv2a_gl.c` through `Nv2aBackend`
   instead of straight into OpenGL. This is the working renderer, so it is
   done where it can be tested end to end first.
2. A Metal presentation path for the window. The macOS window currently draws
   the finished frame with OpenGL; a Metal backend wants a `CAMetalLayer`.
3. iOS: the same backend, a different window and input source.
