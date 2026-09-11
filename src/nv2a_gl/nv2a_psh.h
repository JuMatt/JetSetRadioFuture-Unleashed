/*
 * nv2a_psh.h -- NV2A register combiners: state and GLSL generation.
 *
 * The NV2A has no pixel shader in the modern sense. It has eight configurable
 * combiner stages and a final combiner, each wired up by a handful of register
 * words that name inputs, how to map them, and where the results go. That is
 * what a title's "pixel shader" is: a description of the wiring.
 *
 * Ignoring it and hardcoding "texture times diffuse" gets a surprising
 * distance -- logos and menus look right -- and then fails completely on
 * anything that uses the hardware as intended. JSRF sets its diffuse to black
 * and builds the colour in the combiners, so a modulate produces a black
 * frame with no error anywhere.
 *
 * Kept separate from the OpenGL backend so the same translation can serve a
 * Metal one: nothing here calls GL.
 */
#ifndef NV2A_PSH_H
#define NV2A_PSH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NV2A_PSH_STAGES 8

/* Input registers a combiner stage can read. */
enum {
    NV2A_PSH_REG_ZERO      = 0x0,
    NV2A_PSH_REG_C0        = 0x1,
    NV2A_PSH_REG_C1        = 0x2,
    NV2A_PSH_REG_FOG       = 0x3,
    NV2A_PSH_REG_V0        = 0x4,   /* diffuse  */
    NV2A_PSH_REG_V1        = 0x5,   /* specular */
    NV2A_PSH_REG_T0        = 0x8,
    NV2A_PSH_REG_T1        = 0x9,
    NV2A_PSH_REG_T2        = 0xA,
    NV2A_PSH_REG_T3        = 0xB,
    NV2A_PSH_REG_R0        = 0xC,
    NV2A_PSH_REG_R1        = 0xD,
    NV2A_PSH_REG_V1R0_SUM  = 0xE,
    NV2A_PSH_REG_EF_PROD   = 0xF
};

/* The raw register words a title writes, kept as they arrive. Translating at
 * capture time would mean re-deriving the state every method; keeping the
 * words means the whole thing hashes to one cache key. */
typedef struct {
    uint32_t rgb_icw[NV2A_PSH_STAGES];   /* NV097_SET_COMBINER_COLOR_ICW  */
    uint32_t alpha_icw[NV2A_PSH_STAGES]; /* NV097_SET_COMBINER_ALPHA_ICW  */
    uint32_t rgb_ocw[NV2A_PSH_STAGES];   /* NV097_SET_COMBINER_COLOR_OCW  */
    uint32_t alpha_ocw[NV2A_PSH_STAGES]; /* NV097_SET_COMBINER_ALPHA_OCW  */
    uint32_t c0[NV2A_PSH_STAGES];        /* NV097_SET_COMBINER_FACTOR0    */
    uint32_t c1[NV2A_PSH_STAGES];        /* NV097_SET_COMBINER_FACTOR1    */
    uint32_t control;                    /* NV097_SET_COMBINER_CONTROL    */
    uint32_t final_abcd;                 /* SPECULAR_FOG_CW0              */
    uint32_t final_efg;                  /* SPECULAR_FOG_CW1              */

    /* Which texture stages have something bound, and whether each texture's
     * alpha is meaningful. A one-channel texture reads as (0,0,0,a) in GL and
     * as (1,1,1,a) on the console, which is the difference between a font
     * drawing white and drawing nothing. */
    uint8_t  tex_bound[4];
    uint8_t  tex_alpha_only[4];

    /* Alpha test, which the NV2A does in fixed function. */
    uint8_t  alpha_test;
    uint32_t alpha_func;
    float    alpha_ref;
} Nv2aPshState;

/*
 * Emit a GLSL 330 fragment shader for this combiner configuration.
 *
 * Expects the varyings the translated vertex program produces: oD0, oD1,
 * oT0..oT3, oFogC. Samplers are `tex0`..`tex3` on units 0..3, with `texScale0`
 * ..`texScale3` converting the title's coordinates to GL's [0,1].
 *
 * Returns characters written, or 0 if the buffer was too small.
 */
int nv2a_psh_emit_glsl(const Nv2aPshState *ps, char *buf, int bufsize);

/*
 * The same combiner configuration as a Metal fragment shader.
 *
 * Entry point `nv2a_psh_main`. It takes the vertex stage's Nv2aVshOut as
 * stage_in, `Nv2aPshUniforms` at buffer NV2A_MSL_PSH_UNIFORM_INDEX, and a
 * texture2d + sampler pair at index N for each bound stage -- only the bound
 * ones, so a backend binds exactly what the shader declares.
 */
int nv2a_psh_emit_msl(const Nv2aPshState *ps, char *buf, int bufsize);

#define NV2A_MSL_PSH_UNIFORM_INDEX 0

/* Host-side twin of the generated `Nv2aPshUniforms`. */
typedef struct {
    float   fog_color[4];
    float   tex_scale[4][2];
    int32_t alpha_func;
    float   alpha_ref;
    float   _pad[2];
} Nv2aPshUniformsHost;

#ifdef __cplusplus
}
#endif

#endif /* NV2A_PSH_H */
