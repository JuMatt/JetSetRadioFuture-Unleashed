/*
 * nv2a_vsh.h -- NV2A vertex program: decoder and GLSL generator.
 *
 * The NV2A runs a 128-bit VLIW vertex instruction: one MAC operation and one
 * ILU operation issue together, reading up to three shared source operands and
 * writing a temporary register, an output register, or both. A title uploads
 * the program through NV097_SET_TRANSFORM_PROGRAM as raw dwords, four per
 * instruction slot, and that is exactly what this decodes -- no D3D8 shader
 * blob, no declaration, no header to skip.
 *
 * Standalone by construction: it depends on nothing but the C library, so the
 * same decoder serves an OpenGL backend here and a Metal one later. The GLSL
 * it emits is #version 330 core with fixed attribute locations (v0..v15 at
 * locations 0..15) and one uniform array for the 192 program constants, which
 * are the only two conventions a backend has to agree with.
 */
#ifndef NV2A_VSH_H
#define NV2A_VSH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NV2A_VSH_MAX_INSNS      136
#define NV2A_VSH_NUM_CONSTS     192
#define NV2A_VSH_NUM_INPUTS     16
#define NV2A_VSH_NUM_TEMPS      13      /* R0..R11, plus R12 aliasing oPos */

/* MAC (multiply-accumulate) unit opcodes. */
typedef enum {
    NV2A_MAC_NOP = 0, NV2A_MAC_MOV, NV2A_MAC_MUL, NV2A_MAC_ADD,
    NV2A_MAC_MAD, NV2A_MAC_DP3, NV2A_MAC_DPH, NV2A_MAC_DP4,
    NV2A_MAC_DST, NV2A_MAC_MIN, NV2A_MAC_MAX, NV2A_MAC_SLT,
    NV2A_MAC_SGE, NV2A_MAC_ARL
} Nv2aMacOp;

/* ILU (inverse/logarithm unit) opcodes. Reads source C only. */
typedef enum {
    NV2A_ILU_NOP = 0, NV2A_ILU_MOV, NV2A_ILU_RCP, NV2A_ILU_RCC,
    NV2A_ILU_RSQ, NV2A_ILU_EXP, NV2A_ILU_LOG, NV2A_ILU_LIT
} Nv2aIluOp;

/* Source operand register banks, as the "mux" field encodes them.
 *
 * Numbered from one, with zero meaning nothing: a decoder that assumes
 * 0/1/2 reads every constant as an input and every input as a temporary,
 * which still compiles and still draws -- just not the right geometry. */
typedef enum {
    NV2A_PARAM_NONE = 0,
    NV2A_PARAM_R    = 1,   /* temporary  R0..R11        */
    NV2A_PARAM_V    = 2,   /* input      v0..v15        */
    NV2A_PARAM_C    = 3    /* constant   c0..c191       */
} Nv2aParamType;

/* Output registers, as encoded in the instruction's output address field. */
typedef enum {
    NV2A_OREG_POS = 0,
    NV2A_OREG_DIFFUSE = 3,
    NV2A_OREG_SPECULAR = 4,
    NV2A_OREG_FOG = 5,
    NV2A_OREG_POINT_SIZE = 6,
    NV2A_OREG_BACK_DIFFUSE = 7,
    NV2A_OREG_BACK_SPECULAR = 8,
    NV2A_OREG_TEX0 = 9,
    NV2A_OREG_TEX1 = 10,
    NV2A_OREG_TEX2 = 11,
    NV2A_OREG_TEX3 = 12,
    NV2A_OREG_A0X = 15
} Nv2aOutputReg;

typedef struct {
    uint8_t type;           /* Nv2aParamType                      */
    uint8_t index;          /* register number within the bank    */
    uint8_t negate;
    uint8_t swz[4];         /* per-component selector, 0=x .. 3=w */
} Nv2aSrc;

typedef struct {
    Nv2aMacOp mac;
    Nv2aIluOp ilu;

    Nv2aSrc   src[3];       /* A, B, C -- shared by both units    */
    uint8_t   rel_addr;     /* source C-style constant indexed by A0 */
    int16_t   const_index;  /* c[] index carried in the instruction  */
    uint8_t   input_index;  /* v[] index carried in the instruction  */

    uint8_t   mac_temp;     /* temp register written by the MAC unit */
    uint8_t   mac_mask;     /* bit3=x bit2=y bit1=z bit0=w           */
    uint8_t   ilu_temp;
    uint8_t   ilu_mask;

    uint8_t   out_is_oreg;  /* output write targets an output register */
    uint8_t   out_from_ilu; /* ... and its value comes from the ILU    */
    uint8_t   out_reg;      /* Nv2aOutputReg                            */
    uint8_t   out_mask;
    uint8_t   final;        /* last instruction of the program          */
} Nv2aVshInsn;

typedef struct {
    Nv2aVshInsn insns[NV2A_VSH_MAX_INSNS];
    int         length;
    uint16_t    inputs_read;    /* bit N set if vN is read anywhere */
    uint16_t    outputs_written;/* bit N set if output register N is written */
} Nv2aVshProgram;

/*
 * Decode `count` instruction slots (4 dwords each) into `out`.
 *
 * Stops early at the instruction whose final bit is set. Returns the number of
 * instructions decoded, which is also `out->length`.
 */
int nv2a_vsh_decode(const uint32_t *tokens, int count, Nv2aVshProgram *out);

/*
 * Emit GLSL 330 core for a decoded program.
 *
 * Conventions the backend must match:
 *   - inputs   `layout(location = N) in vec4 vN;` for every v register read
 *   - constants`uniform vec4 c[192];`
 *   - outputs  gl_Position, plus `out vec4 oD0, oD1, oT0..oT3;` and
 *              `out float oFogC;`
 *   - viewport The program produces NV2A screen-space coordinates; the shader
 *              converts them to clip space with `uniform vec4 vpScale, vpOff;`
 *              so the backend does not have to undo the title's viewport.
 *
 * Returns the number of characters written, or 0 if the buffer was too small.
 */
int nv2a_vsh_emit_glsl(const Nv2aVshProgram *p, char *buf, int bufsize);

/*
 * Emit Metal Shading Language for the same decoded program.
 *
 * Same translation, different wrapper: the instruction stream is generated by
 * the same code that produces the GLSL, so the two backends cannot disagree
 * about what a program means. What a Metal backend has to match:
 *
 *   - inputs    `float4 vN [[attribute(N)]]` in `Nv2aVshIn`, N as the NV2A
 *               input register number -- the same numbering the GLSL path
 *               uses for layout locations
 *   - constants `Nv2aVshUniforms` at buffer index NV2A_MSL_VSH_UNIFORM_INDEX,
 *               laid out to match Nv2aVshUniformsHost below
 *   - outputs   `Nv2aVshOut`, whose members the fragment stage receives
 *   - entry     `nv2a_vsh_main`
 *
 * Returns the number of characters written, or 0 if the buffer was too small.
 */
int nv2a_vsh_emit_msl(const Nv2aVshProgram *p, char *buf, int bufsize);

/* Buffer index the uniform block is bound at. Vertex attributes occupy the
 * low indices (one buffer per bound array), so the uniforms sit above all
 * sixteen of them. */
#define NV2A_MSL_VSH_UNIFORM_INDEX 16

/* The `struct Nv2aVshOut` declaration both Metal stages must share, so the
 * fragment emitter can repeat it verbatim rather than keeping its own copy. */
const char *nv2a_msl_varying_struct(void);

/*
 * The host-side twin of the generated `Nv2aVshUniforms`.
 *
 * Metal has no reflection worth relying on for a struct this simple, and the
 * layout rules that matter here are the ones C already follows: float4 is
 * 16-byte aligned, and the trailing float2 + int pack into one more 16-byte
 * slot. Declaring it once, beside the generator that emits its counterpart,
 * is what keeps the two from drifting.
 */
typedef struct {
    float c[NV2A_VSH_NUM_CONSTS][4];
    float vp_scale[4];
    float vp_off[4];
    float vp_surface[2];
    int32_t pos_mode;
    int32_t _pad;
} Nv2aVshUniformsHost;

/* Human-readable disassembly, for bring-up and for diffing against a
 * reference. Returns characters written. */
int nv2a_vsh_disasm(const Nv2aVshProgram *p, char *buf, int bufsize);

/*
 * Run a decoded program on the CPU for one vertex and return what it wrote to
 * oPos. Written from the instruction stream rather than from the generated
 * shader, so the two are independent: if they disagree, the translation is
 * wrong, and if they agree, the constants are.
 */
int nv2a_vsh_interp(const Nv2aVshProgram *p, const float v[16][4],
                    const float c[][4], float out_pos[4]);

/*
 * The same run, reporting every address the program's ARL instructions set.
 *
 * A title that skins its characters reads a bone matrix out of the constant
 * file at c[A0 + n], and whether A0 lands on a matrix is not something that
 * can be worked out from the vertex data alone: the index is scaled by a
 * constant the program picks, and different programs pick different ones.
 * Running the program is the only way to know what it meant.
 *
 * Returns how many were recorded, up to a0_max.
 */
int nv2a_vsh_interp_a0(const Nv2aVshProgram *p, const float v[16][4],
                       const float c[][4], int *a0_out, int a0_max);

#ifdef __cplusplus
}
#endif


/* The fixed-function transform, described by the state that changes its
 * shape. Everything else it needs -- the composite matrix, the texture
 * matrices, the viewport -- arrives through the same uniforms a translated
 * program uses, so this carries only what has to be baked into the code. */
typedef struct {
    uint16_t inputs_read;   /* v registers the emitted shader declares */
    uint8_t  tex_matrix;    /* bit n: transform oTn by the stage matrix */
    uint8_t  has_diffuse;   /* v3 is a real array or a set constant */
    uint8_t  has_specular;  /* v4 likewise */
    /* The title asked the hardware T&L unit to light these vertices. Nothing
     * here lights anything, so the flag exists to make the choice explicit:
     * pass the stream colour through, or treat it as an unlit material. */
    uint8_t  lit;
} Nv2aVshFixed;

int nv2a_vsh_emit_ff_glsl(const Nv2aVshFixed *f, char *buf, int bufsize);

/* The same fixed-function transform as a Metal vertex function. Its constants
 * are one buffer of its own (Nv2aFfUniforms) rather than the 192-entry file,
 * because the fixed-function matrices do not belong in that file -- see the
 * note in nv2a_vsh_emit_ff_glsl. */
int nv2a_vsh_emit_ff_msl(const Nv2aVshFixed *f, char *buf, int bufsize);

#endif /* NV2A_VSH_H */
