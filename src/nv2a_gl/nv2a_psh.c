/*
 * nv2a_psh.c -- register combiners to GLSL.
 *
 * Each of the eight stages computes, for colour and for alpha separately:
 *
 *     AB  = map(A) . map(B)      (product, or a dot product)
 *     CD  = map(C) . map(D)
 *     SUM = AB + CD              (or a mux on R0's alpha)
 *
 * and writes each of the three results to a register, through an output
 * mapping that can scale and bias. The final combiner then computes
 *
 *     rgb = D + A*B + (1-A)*C
 *     a   = G
 *
 * with two extra inputs E and F whose product is available as a register, and
 * the sum of the specular colour and R0 available as another.
 *
 * The mappings are the part worth reading carefully. The hardware's registers
 * hold signed values in [-1,1] and the input mappings exist to move between
 * that and the [0,1] a texture gives you. Getting one of them wrong does not
 * produce a black screen -- it produces a picture that looks almost right,
 * which is much harder to notice.
 */
#include "nv2a_psh.h"
#include "nv2a_vsh.h"   /* the varying struct both Metal stages share */

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct { char *buf; int cap, pos, overflow; } SB;

static void sb(SB *s, const char *fmt, ...)
{
    va_list ap;
    int n, room = s->cap - s->pos;

    if (s->overflow || room <= 1) { s->overflow = 1; return; }
    va_start(ap, fmt);
    n = vsnprintf(s->buf + s->pos, (size_t)room, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= room) { s->overflow = 1; s->pos = s->cap - 1; return; }
    s->pos += n;
}

/* ---- input decoding ----------------------------------------------------- */

#define IN_REG(x)   ((x) & 0x0F)
#define IN_ALPHA(x) (((x) >> 4) & 1)
#define IN_MAP(x)   (((x) >> 5) & 7)

/* NV097_SET_COMBINER_CONTROL bits 12 and 16: whether each stage has its own
 * C0 / C1 or every stage reads stage 0's (xemu: PS_COMBINERCOUNT_UNIQUE_C0/
 * _C1). The final combiner -- stage 8 here -- has a pair of its own,
 * NV097_SET_SPECULAR_FOG_FACTOR. Set by psh_emit for the shader being built. */
static int s_c0_each = 1, s_c1_each = 1;
#define PSH_FINAL_STAGE 8

static const char *reg_name(unsigned reg, int stage)
{
    static char buf[32];
    switch (reg) {
    case NV2A_PSH_REG_ZERO:     return "vec4(0.0)";
    case NV2A_PSH_REG_C0:
        if (stage >= PSH_FINAL_STAGE) return "c0_f";
        snprintf(buf, sizeof buf, "c0_%d", s_c0_each ? stage : 0); return buf;
    case NV2A_PSH_REG_C1:
        if (stage >= PSH_FINAL_STAGE) return "c1_f";
        snprintf(buf, sizeof buf, "c1_%d", s_c1_each ? stage : 0); return buf;
    case NV2A_PSH_REG_FOG:      return "r_fog";
    case NV2A_PSH_REG_V0:       return "r_v0";
    case NV2A_PSH_REG_V1:       return "r_v1";
    case NV2A_PSH_REG_T0:       return "r_t0";
    case NV2A_PSH_REG_T1:       return "r_t1";
    case NV2A_PSH_REG_T2:       return "r_t2";
    case NV2A_PSH_REG_T3:       return "r_t3";
    case NV2A_PSH_REG_R0:       return "r_r0";
    case NV2A_PSH_REG_R1:       return "r_r1";
    case NV2A_PSH_REG_V1R0_SUM: return "r_sum";
    case NV2A_PSH_REG_EF_PROD:  return "r_ef";
    default:                    return "vec4(0.0)";
    }
}

/* One input, mapped. `alpha` selects the scalar path (a single component)
 * rather than the three-component colour path. */
static void emit_input(SB *s, uint8_t in, int stage, int alpha_path)
{
    const char *r = reg_name(IN_REG(in), stage);
    /* .a replicated when the input asks for the alpha channel; otherwise the
     * colour path takes rgb and the alpha path takes a. */
    char v[64];

    /* The alpha bit means different things in the two halves. In the colour
     * combiner it replicates the register's alpha across rgb; in the alpha
     * combiner it selects alpha rather than *blue*. Reading alpha in both --
     * the obvious guess -- silently changes what a stage computes wherever a
     * title used blue as a scalar, which is common enough that it is worth
     * the comment. */
    if (alpha_path)
        snprintf(v, sizeof v, "%s.%c", r, IN_ALPHA(in) ? 'a' : 'b');
    else
        snprintf(v, sizeof v, "%s.%s", r, IN_ALPHA(in) ? "aaa" : "rgb");

    switch (IN_MAP(in)) {
    case 0: sb(s, "max(%s, 0.0)", v); break;                    /* unsigned identity */
    case 1: sb(s, "(1.0 - clamp(%s, 0.0, 1.0))", v); break;      /* unsigned invert   */
    case 2: sb(s, "(2.0 * max(%s, 0.0) - 1.0)", v); break;       /* expand normal     */
    case 3: sb(s, "(1.0 - 2.0 * max(%s, 0.0))", v); break;       /* expand negate     */
    case 4: sb(s, "(max(%s, 0.0) - 0.5)", v); break;             /* half bias normal  */
    case 5: sb(s, "(0.5 - max(%s, 0.0))", v); break;             /* half bias negate  */
    case 6: sb(s, "%s", v); break;                               /* signed identity   */
    case 7: sb(s, "(-%s)", v); break;                            /* signed negate     */
    default: sb(s, "%s", v); break;
    }
}

/* ---- output decoding ----------------------------------------------------
 *
 * The output word is what the XDK's PS_COMBINEROUTPUTS builds,
 *
 *     (flags << 12) | (mux_sum << 8) | (ab << 4) | cd
 *
 * with the flags
 *
 *     0x01  CD is a dot product         0x02  AB is a dot product
 *     0x04  the third output is a mux on R0's alpha, not the sum
 *     0x08..0x38  the output mapping: 0x08 bias, 0x10 x2, 0x18 x2 with
 *           bias, 0x20 x4, 0x30 x0.5
 *     0x40  CD blue to alpha            0x80  AB blue to alpha  (colour only)
 *
 * which is how xemu's parse_combiner_output reads it too. This used to take
 * the mapping from bits 12-14 and the dot and mux flags from 15-17, three
 * bits off: a x2 stage came out as a dot product at x1, a dot product came
 * out doubled, a mux came out x4, and blue-to-alpha did not exist. Any
 * combiner that is not a plain multiply-add was being computed as something
 * else -- the graffiti's, which builds per-part weights with dot products
 * and blue-to-alpha, summed to an alpha of zero and drew nothing at all. */

#define OUT_CD(x)       ((x) & 0x0F)
#define OUT_AB(x)       (((x) >> 4) & 0x0F)
#define OUT_SUM(x)      (((x) >> 8) & 0x0F)
#define OUT_CD_DOT(x)   (((x) >> 12) & 1)
#define OUT_AB_DOT(x)   (((x) >> 13) & 1)
#define OUT_MUX(x)      (((x) >> 14) & 1)
#define OUT_MAP(x)      (((x) >> 15) & 0x07)
#define OUT_CD_BLUE(x)  (((x) >> 18) & 1)
#define OUT_AB_BLUE(x)  (((x) >> 19) & 1)

/* RECOMP_GL_OLD_OCW=1: the old reading of the output word, for A/B. */
static int old_ocw(void)
{
    static int v = -1;
    if (v < 0) v = getenv("RECOMP_GL_OLD_OCW") ? 1 : 0;
    return v;
}
#define OLD_OUT_MAP(x)    (((x) >> 12) & 0x07)
#define OLD_OUT_AB_DOT(x) (((x) >> 15) & 1)
#define OLD_OUT_CD_DOT(x) (((x) >> 16) & 1)
#define OLD_OUT_MUX(x)    (((x) >> 17) & 1)
#define O_MAP(x)    (old_ocw() ? OLD_OUT_MAP(x) : OUT_MAP(x))
#define O_AB_DOT(x) (old_ocw() ? OLD_OUT_AB_DOT(x) : OUT_AB_DOT(x))
#define O_CD_DOT(x) (old_ocw() ? OLD_OUT_CD_DOT(x) : OUT_CD_DOT(x))
#define O_MUX(x)    (old_ocw() ? OLD_OUT_MUX(x) : OUT_MUX(x))
#define O_AB_BLUE(x) (old_ocw() ? 0 : OUT_AB_BLUE(x))
#define O_CD_BLUE(x) (old_ocw() ? 0 : OUT_CD_BLUE(x))

/* The output mapping is a scale and bias applied to everything the stage
 * produces. shl1/shl2 are how a title gets more than unit range out of a
 * combiner, and dropping them costs it every highlight it was counting on. */
static void emit_out_map(SB *s, unsigned map, const char *expr)
{
    switch (map) {
    case 0: sb(s, "%s", expr); break;                        /* identity      */
    case 1: sb(s, "((%s) - 0.5)", expr); break;              /* bias          */
    case 2: sb(s, "((%s) * 2.0)", expr); break;              /* shift left 1  */
    case 3: sb(s, "(((%s) - 0.5) * 2.0)", expr); break;      /* shl1 + bias   */
    case 4: sb(s, "((%s) * 4.0)", expr); break;              /* shift left 2  */
    case 6: sb(s, "((%s) * 0.5)", expr); break;              /* shift right 1 */
    default: sb(s, "%s", expr); break;
    }
}

/* Writing a stage result back. Register 0 means "discard", which is how a
 * stage that only needs two of its three outputs says so. */
static void emit_store(SB *s, unsigned reg, int stage, const char *value,
                       int alpha_path)
{
    if (reg == NV2A_PSH_REG_ZERO) return;
    if (reg < NV2A_PSH_REG_T0 || reg > NV2A_PSH_REG_R1) {
        /* Only the texture and temporary registers are writable; a stage
         * naming anything else as a destination is asking for something the
         * hardware would not do either. */
        if (reg != NV2A_PSH_REG_R0 && reg != NV2A_PSH_REG_R1) return;
    }
    if (alpha_path)
        sb(s, "    %s.a = %s;\n", reg_name(reg, stage), value);
    else
        sb(s, "    %s.rgb = %s;\n", reg_name(reg, stage), value);
}

/* ---- shader ------------------------------------------------------------- */

static int psh_emit(const Nv2aPshState *ps, char *buf, int bufsize, int msl)
{
    SB s = { buf, bufsize, 0, 0 };
    int stages = (int)(ps->control & 0xF);
    int i;
    /* Bring-up switches. A frame that comes out black has three plausible
     * causes -- the colour is black, the alpha test throws it away, or the
     * blend adds nothing -- and each of these removes one of them. */
    static int probe = -1;
    if (probe < 0) {
        probe = 0;
        if (getenv("RECOMP_GL_NOALPHATEST")) probe |= 1;
        if (getenv("RECOMP_GL_OPAQUE"))      probe |= 2;
        if (getenv("RECOMP_GL_TEX0ONLY"))    probe |= 4;
        { const char *p = getenv("RECOMP_GL_PROBE");
          if (p) probe |= (atoi(p) & 7) << 3; }
    }

    if (stages < 1) stages = 1;
    if (stages > NV2A_PSH_STAGES) stages = NV2A_PSH_STAGES;
    s_c0_each = old_ocw() ? 1 : (int)((ps->control >> 12) & 1);
    s_c1_each = old_ocw() ? 1 : (int)((ps->control >> 16) & 1);

    if (msl) {
        sb(&s,
           "#include <metal_stdlib>\n"
           "using namespace metal;\n"
           "// NV2A register combiners, translated (%d stage%s).\n"
           "typedef float2 vec2;\n"
           "typedef float3 vec3;\n"
           "typedef float4 vec4;\n"
           "#define NV2A_DISCARD discard_fragment()\n"
           "\n"
           "struct Nv2aPshUniforms {\n"
           "    float4 fogColor;\n"
           "    float2 texScale[4];\n"
           "    int    alphaFunc;\n"
           "    float  alphaRef;\n"
           "};\n"
           "\n%s\n",
           stages, stages == 1 ? "" : "s", nv2a_msl_varying_struct());
    } else {
        sb(&s,
           "#version 330 core\n"
           /* Set by the backend when RECOMP_GL_ZVIEW is on; see the epilogue
            * at the foot of this emitter for why depth has to travel out
            * through the colour buffer on this platform. */
           "#ifdef NV2A_ZVIEW_ON\n#define NV2A_ZVIEW 1\n#endif\n"
           "// NV2A register combiners, translated (%d stage%s).\n"
           "#define NV2A_DISCARD discard\n"
           "in vec4 oD0;\n"
           "in vec4 oD1;\n"
           "in vec4 oT0;\n"
           "in vec4 oT1;\n"
           "in vec4 oT2;\n"
           "in vec4 oT3;\n"
           "in float oFogC;\n"
           "uniform vec4 fogColor;\n"
           "uniform int  alphaFunc;\n"
           "uniform float alphaRef;\n"
           "out vec4 frag;\n",
           stages, stages == 1 ? "" : "s");

        for (i = 0; i < 4; i++)
            if (ps->tex_bound[i])
                sb(&s, "uniform sampler2D tex%d;\nuniform vec2 texScale%d;\n"
                       "uniform vec2 texOff%d;\n", i, i, i);
        /* BUMPENVMAP stages: the stage's 2x2 matrix and luminance scale and
         * offset, as uniforms because titles animate them. nv2a_s8 reads a
         * channel the title marked signed as the two's complement byte it is. */
        { int any = 0;
          for (i = 1; i < 4; i++)
              if (ps->tex_bound[i] && (ps->tex_mode[i] == 6 || ps->tex_mode[i] == 7)) {
                  sb(&s, "uniform mat2 bumpMat%d;\nuniform float bumpScale%d;\n"
                         "uniform float bumpOffset%d;\n", i, i, i);
                  any = 1;
              }
          if (any)
              sb(&s, "float nv2a_s8(float c) { float b = floor(c * 255.0 + 0.5);"
                     " return (b > 127.5 ? b - 256.0 : b) / 127.0; }\n"
                     "float nv2a_sb(float c) { return (c * 255.0 - 128.0) / 127.0; }\n"); }
    }

    /* Constants are per stage on this hardware; a title that shares them
     * across stages simply writes the same value to each. */
    /* Metal has address spaces: a variable at program scope must be declared
     * `constant`, and `const` alone is rejected outright. GLSL has no such
     * keyword. Same declaration, one word apart. */
    { const char *cq = msl ? "constant" : "const";
    for (i = 0; i < stages; i++) {
        sb(&s, "%s vec4 c0_%d = vec4(%.6f, %.6f, %.6f, %.6f);\n", cq, i,
           ((ps->c0[i] >> 16) & 0xFF) / 255.0, ((ps->c0[i] >> 8) & 0xFF) / 255.0,
           (ps->c0[i] & 0xFF) / 255.0, ((ps->c0[i] >> 24) & 0xFF) / 255.0);
        sb(&s, "%s vec4 c1_%d = vec4(%.6f, %.6f, %.6f, %.6f);\n", cq, i,
           ((ps->c1[i] >> 16) & 0xFF) / 255.0, ((ps->c1[i] >> 8) & 0xFF) / 255.0,
           (ps->c1[i] & 0xFF) / 255.0, ((ps->c1[i] >> 24) & 0xFF) / 255.0);
    }
    /* The final combiner reads C0/C1 too: its own pair, from
     * NV097_SET_SPECULAR_FOG_FACTOR (xemu's c0_8 / c1_8). It used to borrow
     * the last stage's. */
    if (old_ocw())
        sb(&s, "#define c0_f c0_%d\n#define c1_f c1_%d\n", stages - 1, stages - 1);
    else {
        sb(&s, "%s vec4 c0_f = vec4(%.6f, %.6f, %.6f, %.6f);\n", cq,
           ((ps->final_c0 >> 16) & 0xFF) / 255.0, ((ps->final_c0 >> 8) & 0xFF) / 255.0,
           (ps->final_c0 & 0xFF) / 255.0, ((ps->final_c0 >> 24) & 0xFF) / 255.0);
        sb(&s, "%s vec4 c1_f = vec4(%.6f, %.6f, %.6f, %.6f);\n", cq,
           ((ps->final_c1 >> 16) & 0xFF) / 255.0, ((ps->final_c1 >> 8) & 0xFF) / 255.0,
           (ps->final_c1 & 0xFF) / 255.0, ((ps->final_c1 >> 24) & 0xFF) / 255.0);
    }
    }

    if (msl) {
        /* Textures and samplers are arguments in Metal, and only the bound
         * stages get one: an unused texture2d argument still consumes an
         * argument-table slot and still has to be bound to something. */
        sb(&s,
           "\nfragment float4 nv2a_psh_main(Nv2aVshOut in [[stage_in]],\n"
           "        constant Nv2aPshUniforms &U [[buffer(%d)]]",
           NV2A_MSL_PSH_UNIFORM_INDEX);
        for (i = 0; i < 4; i++)
            if (ps->tex_bound[i])
                sb(&s, ",\n        texture2d<float> tex%d [[texture(%d)]],"
                       "\n        sampler smp%d [[sampler(%d)]]", i, i, i, i);
        sb(&s, ")\n{\n"
           "    float4 oD0 = in.oD0, oD1 = in.oD1;\n"
           "    float4 oT0 = in.oT0, oT1 = in.oT1, oT2 = in.oT2, oT3 = in.oT3;\n"
           "    float  oFogC = in.oFogC;\n"
           "    float4 fogColor = U.fogColor;\n"
           "    int    alphaFunc = U.alphaFunc;\n"
           "    float  alphaRef = U.alphaRef;\n"
           "    float4 frag;\n");
        for (i = 0; i < 4; i++)
            if (ps->tex_bound[i])
                sb(&s, "    float2 texScale%d = U.texScale[%d];\n", i, i);
    } else {
        sb(&s, "\nvoid main() {\n");
    }
    for (i = 0; i < 4; i++) {
        /* The texture shader for this stage (xemu psh.c, PS_TEXTUREMODES_*):
         * a stage set to NONE is never sampled and reads as (0,0,0,1);
         * PASSTHRU hands the coordinates through as a colour; CLIPPLANE
         * discards the fragment by the sign of each coordinate and reads
         * as zero. Everything else is sampled as a 2D texture. */
        if (ps->tex_mode[i] == 0) {
            sb(&s, "    vec4 r_t%d = vec4(0.0, 0.0, 0.0, 1.0);  // stage NONE\n", i);
        } else if (ps->tex_mode[i] == 4) {
            sb(&s, "    vec4 r_t%d = oT%d;  // stage PASSTHRU\n", i, i);
        } else if (ps->tex_mode[i] == 5) {
            int j;
            sb(&s, "    vec4 r_t%d = vec4(0.0);  // stage CLIPPLANE\n", i);
            for (j = 0; j < 4; j++)
                sb(&s, "    if (oT%d.%c %s 0.0) NV2A_DISCARD;\n", i, "xyzw"[j],
                   (ps->clip_cmp[i] >> j) & 1 ? ">=" : "<");
        } else if (!msl && i >= 1 && ps->tex_bound[i]
                   && (ps->tex_mode[i] == 6 || ps->tex_mode[i] == 7)) {
            /* BUMPENVMAP: (du, dv) from the source stage's blue and green,
             * through this stage's matrix, offset this stage's coordinates
             * (xemu psh.c). In the coordinates' own units: texels for a
             * linear texture, which texScale then normalises. */
            int j = ps->bump_src[i] < i ? ps->bump_src[i] : 0;
            /* 0x10: uploaded offset-binary; 0x20: a surface, raw bytes */
            const char *cv = (ps->tex_signed[j] & 0x20) ? "nv2a_s8" : "nv2a_sb";
            sb(&s, "    // stage %d: BUMPENVMAP%s from stage %d\n", i,
               ps->tex_mode[i] == 7 ? "_LUM" : "", j);
            sb(&s, "    vec2 dsdt%d = vec2(%s(r_t%d.b), %s(r_t%d.g));\n", i,
               cv, j, cv, j);
            sb(&s, "    dsdt%d = bumpMat%d * dsdt%d;\n", i, i, i);
            sb(&s, "    vec4 r_t%d = texture(tex%d, (oT%d.xy + dsdt%d) * texScale%d"
                   " + texOff%d);\n", i, i, i, i, i, i);
            if (ps->tex_mode[i] == 7)
                sb(&s, "    r_t%d *= clamp(bumpScale%d * r_t%d.r + bumpOffset%d, 0.0, 1.0);\n",
                   i, i, j, i);
            if (ps->tex_alpha_only[i])
                sb(&s, "    r_t%d.rgb = vec3(1.0);\n", i);
        } else if (ps->tex_bound[i]) {
            if (msl)
                sb(&s, "    vec4 r_t%d = tex%d.sample(smp%d, oT%d.xy * texScale%d);\n",
                   i, i, i, i, i);
            else
                sb(&s, "    vec4 r_t%d = texture(tex%d, oT%d.xy * texScale%d + texOff%d);\n",
                   i, i, i, i, i);
            /* An alpha-only texture has no colour of its own; the hardware
             * reads its rgb as one, so a title using it as a mask multiplies
             * by white rather than by black. */
            if (ps->tex_alpha_only[i])
                sb(&s, "    r_t%d.rgb = vec3(1.0);\n", i);
        } else {
            sb(&s, "    vec4 r_t%d = vec4(0.0);\n", i);
        }
    }
    /* A bump source uploaded offset-binary (0x10) goes back to its own
     * bytes before the combiners read it: to them it is an ordinary
     * texture. Only after every bump stage has taken its (du, dv). */
    if (!msl)
        for (i = 0; i < 4; i++)
            if (ps->tex_signed[i] & 0x10)
                sb(&s, "    r_t%d.gb = mod(floor(r_t%d.gb * 255.0 + 0.5) + 128.0, 256.0) / 255.0;"
                       "  // undo the upload bias\n", i, i);
    sb(&s,
       "    vec4 r_v0 = oD0;\n"
       "    vec4 r_v1 = oD1;\n"
       "    vec4 r_fog = vec4(fogColor.rgb, oFogC);\n");
    /* R0 starts black with texture 0's alpha (xemu psh.c: r0 = 0, r0.a =
     * t0.a, or 1 when stage 0 is NONE). It used to start as the whole of
     * texture 0, which fed t0's colour to any combiner that read R0 before
     * writing it. */
    sb(&s, ps->tex_mode[0] == 0 ? "    vec4 r_r0 = vec4(0.0, 0.0, 0.0, 1.0);\n"
                                : "    vec4 r_r0 = vec4(0.0, 0.0, 0.0, r_t0.a);\n");
    sb(&s,
       "    vec4 r_r1 = vec4(0.0);\n"
       "    vec4 r_sum = vec4(0.0);\n"
       "    vec4 r_ef = vec4(0.0);\n"
       "    vec3 ab, cd, sum, ab_raw, cd_raw;\n"
       "    float ab_a, cd_a, sum_a, ab_a_raw, cd_a_raw;\n\n");

    /* Each stage reads its inputs, computes all six results (AB, CD and the
     * sum or mux, for colour and for alpha), and only then writes them: a
     * stage that reads a register it also writes sees the value from before
     * the stage, in both halves. Results are clamped to the registers'
     * [-1, 1], as xemu does. The mux picks on R0's alpha: its top bit, or
     * with NV097_SET_COMBINER_CONTROL bit 8 clear, its bottom bit. */
    {
    const char *mux_sel = (old_ocw() || ((ps->control >> 8) & 1))
                        ? "(r_r0.a >= 0.5)"
                        : "((uint(r_r0.a * 255.0) & 1u) == 1u)";
    const char *lo = old_ocw() ? "" : "clamp(";
    const char *hi3 = old_ocw() ? "" : ", -1.0, 1.0)";
    for (i = 0; i < stages; i++) {
        uint32_t ric = ps->rgb_icw[i], aic = ps->alpha_icw[i];
        uint32_t roc = ps->rgb_ocw[i], aoc = ps->alpha_ocw[i];
        char expr[512], mux[640];
        SB e;

        sb(&s, "    // stage %d\n", i);

        /* --- colour --- */
        e.buf = expr; e.cap = sizeof expr; e.pos = 0; e.overflow = 0;
        if (O_AB_DOT(roc)) {
            sb(&e, "vec3(dot(");
            emit_input(&e, (uint8_t)(ric >> 24), i, 0);
            sb(&e, ", ");
            emit_input(&e, (uint8_t)(ric >> 16), i, 0);
            sb(&e, "))");
        } else {
            sb(&e, "(");
            emit_input(&e, (uint8_t)(ric >> 24), i, 0);
            sb(&e, " * ");
            emit_input(&e, (uint8_t)(ric >> 16), i, 0);
            sb(&e, ")");
        }
        sb(&s, "    ab_raw = %s;\n", expr);

        e.pos = 0; e.overflow = 0;
        if (O_CD_DOT(roc)) {
            sb(&e, "vec3(dot(");
            emit_input(&e, (uint8_t)(ric >> 8), i, 0);
            sb(&e, ", ");
            emit_input(&e, (uint8_t)ric, i, 0);
            sb(&e, "))");
        } else {
            sb(&e, "(");
            emit_input(&e, (uint8_t)(ric >> 8), i, 0);
            sb(&e, " * ");
            emit_input(&e, (uint8_t)ric, i, 0);
            sb(&e, ")");
        }
        sb(&s, "    cd_raw = %s;\n", expr);
        sb(&s, "    ab = %s", lo); emit_out_map(&s, O_MAP(roc), "ab_raw"); sb(&s, "%s;\n", hi3);
        sb(&s, "    cd = %s", lo); emit_out_map(&s, O_MAP(roc), "cd_raw"); sb(&s, "%s;\n", hi3);
        /* The sum is of the *unmapped* products; the mapping is applied once
         * to each of the three results, not twice to the sum. */
        snprintf(mux, sizeof mux, "%s ? cd_raw : ab_raw", mux_sel);
        sb(&s, "    sum = %s", lo);
        emit_out_map(&s, O_MAP(roc), O_MUX(roc) ? mux : "ab_raw + cd_raw");
        sb(&s, "%s;\n", hi3);

        /* --- alpha --- */
        e.pos = 0; e.overflow = 0;
        sb(&e, "(");
        emit_input(&e, (uint8_t)(aic >> 24), i, 1);
        sb(&e, " * ");
        emit_input(&e, (uint8_t)(aic >> 16), i, 1);
        sb(&e, ")");
        sb(&s, "    ab_a_raw = %s;\n", expr);

        e.pos = 0; e.overflow = 0;
        sb(&e, "(");
        emit_input(&e, (uint8_t)(aic >> 8), i, 1);
        sb(&e, " * ");
        emit_input(&e, (uint8_t)aic, i, 1);
        sb(&e, ")");
        sb(&s, "    cd_a_raw = %s;\n", expr);
        sb(&s, "    ab_a = %s", lo); emit_out_map(&s, O_MAP(aoc), "ab_a_raw"); sb(&s, "%s;\n", hi3);
        sb(&s, "    cd_a = %s", lo); emit_out_map(&s, O_MAP(aoc), "cd_a_raw"); sb(&s, "%s;\n", hi3);
        snprintf(mux, sizeof mux, "%s ? cd_a_raw : ab_a_raw", mux_sel);
        sb(&s, "    sum_a = %s", lo);
        emit_out_map(&s, O_MAP(aoc), O_MUX(aoc) ? mux : "ab_a_raw + cd_a_raw");
        sb(&s, "%s;\n", hi3);

        /* --- the writes: colour, its blue-to-alpha, then alpha --- */
        emit_store(&s, OUT_AB(roc), i, "ab", 0);
        if (O_AB_BLUE(roc) && OUT_AB(roc) != NV2A_PSH_REG_ZERO)
            emit_store(&s, OUT_AB(roc), i, "ab.b", 1);
        emit_store(&s, OUT_CD(roc), i, "cd", 0);
        if (O_CD_BLUE(roc) && OUT_CD(roc) != NV2A_PSH_REG_ZERO)
            emit_store(&s, OUT_CD(roc), i, "cd.b", 1);
        emit_store(&s, OUT_SUM(roc), i, "sum", 0);
        emit_store(&s, OUT_AB(aoc), i, "ab_a", 1);
        emit_store(&s, OUT_CD(aoc), i, "cd_a", 1);
        emit_store(&s, OUT_SUM(aoc), i, "sum_a", 1);
        sb(&s, "\n");
    }
    }

    /* --- final combiner --- */
    sb(&s, "    // final combiner\n");
    { int fst = old_ocw() ? stages - 1 : PSH_FINAL_STAGE;
    /* V1R0_SUM: specular plus R0, each optionally complemented, clamped only
     * when the title asks (SPECULAR_FOG_CW1 bits 5, 6, 7 -- xemu's inv_r0,
     * inv_v1, clamp_sum). It used to be clamped always and never inverted. */
    if (old_ocw())
        sb(&s, "    r_sum = clamp(r_v1 + r_r0, 0.0, 1.0);\n");
    else {
        uint32_t fl = ps->final_efg & 0xFF;
        sb(&s, "    r_sum.rgb = %s%s + %s%s%s;\n",
           (fl & 0x80) ? "clamp(" : "",
           (fl & 0x40) ? "(1.0 - r_v1.rgb)" : "r_v1.rgb",
           (fl & 0x20) ? "(1.0 - r_r0.rgb)" : "r_r0.rgb",
           (fl & 0x80) ? ", 0.0, 1.0)" : "", "");
        sb(&s, "    r_sum.a = clamp(r_v1.a + r_r0.a, 0.0, 1.0);\n");
    }
    {
        char expr[512];
        SB e; e.buf = expr; e.cap = sizeof expr; e.pos = 0; e.overflow = 0;
        sb(&e, "vec3(");
        emit_input(&e, (uint8_t)(ps->final_efg >> 24), fst, 0);
        sb(&e, " * ");
        emit_input(&e, (uint8_t)(ps->final_efg >> 16), fst, 0);
        sb(&e, ")");
        sb(&s, "    r_ef.rgb = %s;\n", expr);
    }
    sb(&s, "    vec3 fa, fb, fc, fd;\n");
    sb(&s, "    fa = "); emit_input(&s, (uint8_t)(ps->final_abcd >> 24), fst, 0); sb(&s, ";\n");
    sb(&s, "    fb = "); emit_input(&s, (uint8_t)(ps->final_abcd >> 16), fst, 0); sb(&s, ";\n");
    sb(&s, "    fc = "); emit_input(&s, (uint8_t)(ps->final_abcd >> 8), fst, 0); sb(&s, ";\n");
    sb(&s, "    fd = "); emit_input(&s, (uint8_t)ps->final_abcd, fst, 0); sb(&s, ";\n");
    sb(&s, "    float fg = ");
    emit_input(&s, (uint8_t)(ps->final_efg >> 8), fst, 1);
    sb(&s, ";\n");
    }
    sb(&s,
       "    vec4 result;\n"
       "    result.rgb = clamp(fd + fa * fb + (1.0 - fa) * fc, 0.0, 1.0);\n"
       "    result.a = clamp(fg, 0.0, 1.0);\n");
    if (probe & 4) sb(&s, "    result.rgb = r_t0.rgb;\n");
    if (probe & 2) sb(&s, "    result.a = 1.0;\n");
    /* RECOMP_GL_PROBE=<n>: replace the combiner result with one of its
     * ingredients. "The surface is black" has half a dozen causes -- no
     * texture bound, a texture that decoded black, a diffuse the vertex
     * program left at zero, coordinates that sample outside the image -- and
     * each of these shows a different one, in one run each. */
    switch (probe >> 3) {
    case 1: sb(&s, "    result.rgb = r_v0.rgb;\n"); break;
    case 2: sb(&s, "    result.rgb = r_v1.rgb;\n"); break;
    case 3: sb(&s, "    result.rgb = r_t1.rgb;\n"); break;
    case 4: sb(&s, "    result.rgb = r_t2.rgb;\n"); break;
    case 5: sb(&s, "    result.rgb = vec3(fract(oT0.xy), 0.0);\n"); break;
    case 6: sb(&s, "    result.rgb = vec3(r_v0.a, r_t0.a, 0.0);\n"); break;
    case 7: sb(&s, "    result.rgb = vec3(%d.0/8.0, %d.0/4.0, 0.5);\n",
                  stages, ps->tex_bound[0] + ps->tex_bound[1]
                          + ps->tex_bound[2] + ps->tex_bound[3]); break;
    default: break;
    }
    /* Deliberately leaves alpha alone. Forcing it to one turns the title's
     * last blended overlay into an opaque sheet over the whole frame, and
     * every probe then reports on that quad instead of the scene -- which
     * reads as "the whole screen is one flat value" and is entirely an
     * artefact of the probe. */

    /* Alpha test: fixed function on the console, absent from GL core. */
    if (probe & 1) sb(&s, "    // alpha test suppressed\n    frag = result;\n");
    else sb(&s,
       "    if (alphaFunc != 0) {\n"
       "        bool pass = true;\n"
       "        if      (alphaFunc == 1) pass = false;\n"
       "        else if (alphaFunc == 2) pass = (result.a <  alphaRef);\n"
       "        else if (alphaFunc == 3) pass = (result.a == alphaRef);\n"
       "        else if (alphaFunc == 4) pass = (result.a <= alphaRef);\n"
       "        else if (alphaFunc == 5) pass = (result.a >  alphaRef);\n"
       "        else if (alphaFunc == 6) pass = (result.a != alphaRef);\n"
       "        else if (alphaFunc == 7) pass = (result.a >= alphaRef);\n"
       "        if (!pass) NV2A_DISCARD;\n"
       "    }\n"
       "    frag = result;\n");

    /* RECOMP_GL_ZVIEW: the depth this fragment landed at, as a picture.
     *
     * Apple's OpenGL will not read a depth attachment back out of an FBO --
     * every 3D surface comes back as a buffer of exact zeros -- so the
     * ordinary way of asking "is there any geometry under that floating bus"
     * is closed on this platform. Writing gl_FragCoord.z into the COLOUR
     * buffer instead goes out through the readback path that demonstrably
     * works, and answers the same question.
     *
     * What it settles: a shape under the bus means the road deck IS being
     * drawn and something downstream is making it invisible, which is a
     * renderer bug in a small room. Flat background means it was never
     * submitted, and the decision not to draw it was made by the game's own
     * visibility code on the recompiled CPU -- the other half of the
     * project. Seven theories have died without anyone establishing which
     * of those two worlds this bug lives in.
     *
     * Depth is stretched hard because a perspective buffer spends almost all
     * its range in the first fraction: a linear view is a white rectangle.
     * The alpha test above still runs, so a surface discarded per fragment
     * stays absent here -- which is the point, since that is one of the two
     * things being told apart. */
    /* Stretched for the range this title actually uses, which is not the
     * one a perspective buffer usually has. Both instruments agree: the
     * depth readback reported every pixel inside the first tenth, and a
     * gamma curve tuned for values bunched near 1.0 came back uniformly
     * white -- two different measurements of the same fact, which is what
     * makes it trustworthy rather than a third broken probe. So the scene
     * lives in roughly z < 0.1 and the useful view is a linear stretch of
     * that. Anything beyond it clamps to background. */
    sb(&s, "#ifdef NV2A_ZVIEW\n"
           "    { float zz = clamp(gl_FragCoord.z * 10.0, 0.0, 1.0);\n"
           "      frag = vec4(1.0 - zz, 1.0 - zz, 1.0 - zz, 1.0); }\n"
           "#endif\n");

    /*
     * W-buffering, the way xemu does it.
     *
     * With Z_PERSPECTIVE_ENABLE set the NV2A's depth is not the program's z
     * at all: it is the clip w, interpolated perspective-correctly across the
     * triangle. xemu computes that with reciprocal-space barycentrics in the
     * fragment shader and writes gl_FragDepth from it; here gl_FragCoord.w,
     * which GL defines as the perspective-correct 1/w, gives the same number
     * for free. Divided by 2^24 to match a 24-bit fixed buffer, as xemu's
     * DEPTH_FORMAT_D24 path does.
     *
     * What this changes for the picture: the depth test compares w against
     * CLIP_MIN..CLIP_MAX, and this title sets those to 0..16777215, so no
     * fragment is ever rejected for being past the far plane. The vertex
     * stage still has to be stopped from clipping on the program's z -- the
     * backend enables GL_DEPTH_CLAMP for these draws -- and after that the
     * whole world reaches the screen, the far half included.
     *
     * A separate shader variant, keyed by the w_buffer field in the hash, so
     * that draws which do not use it keep early depth testing.
     */
    if (ps->w_buffer && !msl)
        sb(&s, "    gl_FragDepth = (1.0 / gl_FragCoord.w) / 16777216.0;\n");
    sb(&s, msl ? "    return frag;\n}\n" : "}\n");

    return s.overflow ? 0 : s.pos;
}

int nv2a_psh_emit_glsl(const Nv2aPshState *ps, char *buf, int bufsize)
{
    return psh_emit(ps, buf, bufsize, 0);
}

int nv2a_psh_emit_msl(const Nv2aPshState *ps, char *buf, int bufsize)
{
    return psh_emit(ps, buf, bufsize, 1);
}
