/*
 * nv2a_vsh.c -- NV2A vertex program decoder and GLSL generator.
 *
 * The bit layout below is the one nouveau and xemu both use, and it is worth
 * saying why it is written out as a table rather than as shifts inline: the
 * fields are scattered across three of the four dwords, several straddle a
 * dword boundary (source C's register index is split 2 bits / 2 bits across
 * words 2 and 3), and every reference numbers the bits differently. A table
 * can be checked against a reference line by line; a page of shift-and-mask
 * cannot, and a single wrong offset silently produces a program that compiles
 * and draws the wrong shape.
 *
 * Word 0 carries nothing. That is not a mistake: the NV2A instruction is 128
 * bits of which the first 32 are unused, and a decoder that assumes otherwise
 * reads every field one dword early -- which is the failure mode this file
 * exists to avoid, so it is stated here rather than discovered later.
 */
#include "nv2a_vsh.h"
#include <stdlib.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- field extraction --------------------------------------------------- */

typedef struct { uint8_t word, bit, len; } Field;

/*                             word  bit  len */
static const Field F_ILU        = { 1, 25, 3 };
static const Field F_MAC        = { 1, 21, 4 };
static const Field F_CONST      = { 1, 13, 8 };
static const Field F_V          = { 1,  9, 4 };

static const Field F_A_NEG      = { 1,  8, 1 };
static const Field F_A_SWZ_X    = { 1,  6, 2 };
static const Field F_A_SWZ_Y    = { 1,  4, 2 };
static const Field F_A_SWZ_Z    = { 1,  2, 2 };
static const Field F_A_SWZ_W    = { 1,  0, 2 };
static const Field F_A_R        = { 2, 28, 4 };
static const Field F_A_MUX      = { 2, 26, 2 };

static const Field F_B_NEG      = { 2, 25, 1 };
static const Field F_B_SWZ_X    = { 2, 23, 2 };
static const Field F_B_SWZ_Y    = { 2, 21, 2 };
static const Field F_B_SWZ_Z    = { 2, 19, 2 };
static const Field F_B_SWZ_W    = { 2, 17, 2 };
static const Field F_B_R        = { 2, 13, 4 };
static const Field F_B_MUX      = { 2, 11, 2 };

static const Field F_C_NEG      = { 2, 10, 1 };
static const Field F_C_SWZ_X    = { 2,  8, 2 };
static const Field F_C_SWZ_Y    = { 2,  6, 2 };
static const Field F_C_SWZ_Z    = { 2,  4, 2 };
static const Field F_C_SWZ_W    = { 2,  2, 2 };
static const Field F_C_R_HIGH   = { 2,  0, 2 };
static const Field F_C_R_LOW    = { 3, 30, 2 };
static const Field F_C_MUX      = { 3, 28, 2 };

static const Field F_OUT_MAC_MASK = { 3, 24, 4 };
static const Field F_OUT_R        = { 3, 20, 4 };
static const Field F_OUT_ILU_MASK = { 3, 16, 4 };
static const Field F_OUT_O_MASK   = { 3, 12, 4 };
static const Field F_OUT_ORB      = { 3, 11, 1 };   /* 1 = output register */
static const Field F_OUT_ADDRESS  = { 3,  3, 8 };
static const Field F_OUT_MUX      = { 3,  2, 1 };   /* 1 = value from ILU  */
static const Field F_A0X          = { 3,  1, 1 };
static const Field F_FINAL        = { 3,  0, 1 };

static uint32_t fld(const uint32_t *t, Field f)
{
    return (t[f.word] >> f.bit) & ((f.len == 32) ? 0xFFFFFFFFu
                                                 : ((1u << f.len) - 1u));
}

static void decode_src(const uint32_t *t, Nv2aSrc *s,
                       Field neg, Field x, Field y, Field z, Field w,
                       uint8_t mux, uint8_t reg)
{
    s->negate = (uint8_t)fld(t, neg);
    s->swz[0] = (uint8_t)fld(t, x);
    s->swz[1] = (uint8_t)fld(t, y);
    s->swz[2] = (uint8_t)fld(t, z);
    s->swz[3] = (uint8_t)fld(t, w);
    s->type   = mux;
    s->index  = reg;
}

int nv2a_vsh_decode(const uint32_t *tokens, int count, Nv2aVshProgram *out)
{
    int i, n = 0;

    memset(out, 0, sizeof *out);
    if (count > NV2A_VSH_MAX_INSNS)
        count = NV2A_VSH_MAX_INSNS;

    for (i = 0; i < count; i++) {
        const uint32_t *t = tokens + i * 4;
        Nv2aVshInsn *ins = &out->insns[n];
        uint32_t c_reg;

        memset(ins, 0, sizeof *ins);
        ins->mac = (Nv2aMacOp)fld(t, F_MAC);
        ins->ilu = (Nv2aIluOp)fld(t, F_ILU);

        ins->const_index = (int16_t)fld(t, F_CONST);
        ins->input_index = (uint8_t)fld(t, F_V);
        ins->rel_addr    = (uint8_t)fld(t, F_A0X);

        /* Source C's register index is split across two dwords, high part
         * first. Reassembling it the other way round yields a plausible but
         * wrong constant index, which is the kind of bug that survives a
         * review and shows up as one wrong matrix. */
        c_reg = (fld(t, F_C_R_HIGH) << 2) | fld(t, F_C_R_LOW);

        decode_src(t, &ins->src[0], F_A_NEG, F_A_SWZ_X, F_A_SWZ_Y, F_A_SWZ_Z,
                   F_A_SWZ_W, (uint8_t)fld(t, F_A_MUX), (uint8_t)fld(t, F_A_R));
        decode_src(t, &ins->src[1], F_B_NEG, F_B_SWZ_X, F_B_SWZ_Y, F_B_SWZ_Z,
                   F_B_SWZ_W, (uint8_t)fld(t, F_B_MUX), (uint8_t)fld(t, F_B_R));
        decode_src(t, &ins->src[2], F_C_NEG, F_C_SWZ_X, F_C_SWZ_Y, F_C_SWZ_Z,
                   F_C_SWZ_W, (uint8_t)fld(t, F_C_MUX), (uint8_t)c_reg);

        ins->mac_temp = (uint8_t)fld(t, F_OUT_R);
        ins->mac_mask = (uint8_t)fld(t, F_OUT_MAC_MASK);
        ins->ilu_mask = (uint8_t)fld(t, F_OUT_ILU_MASK);

        /* Paired issue: when both units are busy the ILU's temporary
         * destination is forced to R1 by the hardware, whatever the
         * instruction's destination field says. A decoder that honours the
         * field instead produces a program that is correct for every
         * instruction except the paired ones. */
        ins->ilu_temp = (ins->mac != NV2A_MAC_NOP && ins->ilu != NV2A_ILU_NOP)
                      ? 1u : (uint8_t)fld(t, F_OUT_R);

        ins->out_is_oreg  = (uint8_t)fld(t, F_OUT_ORB);
        ins->out_from_ilu = (uint8_t)fld(t, F_OUT_MUX);
        ins->out_reg      = (uint8_t)fld(t, F_OUT_ADDRESS);
        ins->out_mask     = (uint8_t)fld(t, F_OUT_O_MASK);
        ins->final        = (uint8_t)fld(t, F_FINAL);

        /* Which inputs the program actually reads decides the vertex layout
         * the backend has to bind. Count a v register only where a source
         * really selects the input bank. */
        {
            int k;
            for (k = 0; k < 3; k++)
                if (ins->src[k].type == NV2A_PARAM_V)
                    out->inputs_read |= (uint16_t)(1u << ins->input_index);
        }
        if (ins->out_is_oreg && ins->out_mask && ins->out_reg < 16)
            out->outputs_written |= (uint16_t)(1u << ins->out_reg);

        n++;
        if (ins->final)
            break;
    }
    out->length = n;
    return n;
}

/* ---- small string builder ----------------------------------------------- */

typedef struct { char *buf; int cap, pos; int overflow; } SB;

static void sb_add(SB *sb, const char *fmt, ...)
{
    va_list ap;
    int n, room = sb->cap - sb->pos;

    if (sb->overflow || room <= 1) { sb->overflow = 1; return; }
    va_start(ap, fmt);
    n = vsnprintf(sb->buf + sb->pos, (size_t)room, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= room) { sb->overflow = 1; sb->pos = sb->cap - 1; return; }
    sb->pos += n;
}

/* ---- GLSL generation ---------------------------------------------------- */

static const char SWZ[4] = { 'x', 'y', 'z', 'w' };

static void emit_swizzle(SB *sb, const uint8_t s[4])
{
    if (s[0] == 0 && s[1] == 1 && s[2] == 2 && s[3] == 3)
        return;                              /* identity: leave it off */
    sb_add(sb, ".%c%c%c%c", SWZ[s[0]], SWZ[s[1]], SWZ[s[2]], SWZ[s[3]]);
}

static void emit_src(SB *sb, const Nv2aVshInsn *ins, int which)
{
    const Nv2aSrc *s = &ins->src[which];

    if (s->negate) sb_add(sb, "(-");
    switch (s->type) {
    case NV2A_PARAM_R:
        sb_add(sb, "%s", s->index == 12 ? "oPos" : "R");
        if (s->index != 12) sb_add(sb, "%u", s->index);
        break;
    case NV2A_PARAM_V:
        sb_add(sb, "v%u", ins->input_index);
        break;
    case NV2A_PARAM_C:
        /* c[] is indexed from the instruction's constant field, biased by
         * the address register when relative addressing is on. Clamping is
         * not decoration: an out-of-range index is undefined in GLSL and in
         * practice reads whatever follows the array. */
        if (ins->rel_addr)
            sb_add(sb, "c[clamp(A0 + %d, 0, %d)]", ins->const_index,
                   NV2A_VSH_NUM_CONSTS - 1);
        else
            /* Clamped, not masked. There are 192 constants, which is not a
             * power of two, so "& (192 - 1)" is a bit pattern rather than a
             * range check: it silently turns c[107] into c[43]. Every matrix
             * a title keeps in the upper bank is then read from the wrong
             * place, every position comes out as zero, and the frame is
             * black with no error anywhere. */
            sb_add(sb, "c[%d]",
                   ins->const_index < NV2A_VSH_NUM_CONSTS
                       ? ins->const_index : NV2A_VSH_NUM_CONSTS - 1);
        break;
    default:
        /* A source whose bank is "none" reads as zero on the hardware. */
        sb_add(sb, "vec4(0.0)");
        break;
    }
    emit_swizzle(sb, s->swz);
    if (s->negate) sb_add(sb, ")");
}

static void emit_mask(SB *sb, uint8_t mask)
{
    if (mask == 0xF) return;
    sb_add(sb, ".");
    if (mask & 8) sb_add(sb, "x");
    if (mask & 4) sb_add(sb, "y");
    if (mask & 2) sb_add(sb, "z");
    if (mask & 1) sb_add(sb, "w");
}

static int mask_width(uint8_t mask)
{
    return ((mask >> 3) & 1) + ((mask >> 2) & 1) + ((mask >> 1) & 1) + (mask & 1);
}

/* The destination name for a temporary write. R12 is the position register:
 * on the NV2A it is the same storage as oPos, so a program may write R12 and
 * read it back, and both have to land in one variable. */
static void emit_temp_name(SB *sb, uint8_t idx)
{
    if (idx == 12) sb_add(sb, "oPos");
    else           sb_add(sb, "R%u", idx);
}

static const char *OREG_NAME[16] = {
    "oPos", 0, 0, "oD0", "oD1", "oFog", "oPts", "oB0", "oB1",
    "oT0", "oT1", "oT2", "oT3", 0, 0, 0
};

/* One MAC operation as a vec4-valued GLSL expression. */
static void emit_mac_expr(SB *sb, const Nv2aVshInsn *ins)
{
    switch (ins->mac) {
    case NV2A_MAC_MOV: emit_src(sb, ins, 0); break;
    case NV2A_MAC_MUL:
        sb_add(sb, "("); emit_src(sb, ins, 0); sb_add(sb, " * ");
        emit_src(sb, ins, 1); sb_add(sb, ")");
        break;
    case NV2A_MAC_ADD:
        sb_add(sb, "("); emit_src(sb, ins, 0); sb_add(sb, " + ");
        emit_src(sb, ins, 2); sb_add(sb, ")");
        break;
    case NV2A_MAC_MAD:
        sb_add(sb, "("); emit_src(sb, ins, 0); sb_add(sb, " * ");
        emit_src(sb, ins, 1); sb_add(sb, " + "); emit_src(sb, ins, 2);
        sb_add(sb, ")");
        break;
    case NV2A_MAC_DP3:
        sb_add(sb, "vec4(dot("); emit_src(sb, ins, 0); sb_add(sb, ".xyz, ");
        emit_src(sb, ins, 1); sb_add(sb, ".xyz))");
        break;
    case NV2A_MAC_DPH:
        sb_add(sb, "vec4(dot("); emit_src(sb, ins, 0); sb_add(sb, ".xyz, ");
        emit_src(sb, ins, 1); sb_add(sb, ".xyz) + "); emit_src(sb, ins, 1);
        sb_add(sb, ".w)");
        break;
    case NV2A_MAC_DP4:
        sb_add(sb, "vec4(dot("); emit_src(sb, ins, 0); sb_add(sb, ", ");
        emit_src(sb, ins, 1); sb_add(sb, "))");
        break;
    case NV2A_MAC_DST:
        /* Distance vector: (1, d.y*e.y, d.z, e.w) -- the classic attenuation
         * helper, spelled out because GLSL has no equivalent. */
        sb_add(sb, "vec4(1.0, ");
        emit_src(sb, ins, 0); sb_add(sb, ".y * "); emit_src(sb, ins, 1);
        sb_add(sb, ".y, "); emit_src(sb, ins, 0); sb_add(sb, ".z, ");
        emit_src(sb, ins, 1); sb_add(sb, ".w)");
        break;
    case NV2A_MAC_MIN:
        sb_add(sb, "min("); emit_src(sb, ins, 0); sb_add(sb, ", ");
        emit_src(sb, ins, 1); sb_add(sb, ")");
        break;
    case NV2A_MAC_MAX:
        sb_add(sb, "max("); emit_src(sb, ins, 0); sb_add(sb, ", ");
        emit_src(sb, ins, 1); sb_add(sb, ")");
        break;
    case NV2A_MAC_SLT:
        sb_add(sb, "vec4(lessThan("); emit_src(sb, ins, 0); sb_add(sb, ", ");
        emit_src(sb, ins, 1); sb_add(sb, "))");
        break;
    case NV2A_MAC_SGE:
        sb_add(sb, "vec4(greaterThanEqual("); emit_src(sb, ins, 0);
        sb_add(sb, ", "); emit_src(sb, ins, 1); sb_add(sb, "))");
        break;
    default:
        sb_add(sb, "vec4(0.0)");
        break;
    }
}

/* One ILU operation. All of them are scalar in C.x and replicate. */
static void emit_ilu_expr(SB *sb, const Nv2aVshInsn *ins)
{
    switch (ins->ilu) {
    case NV2A_ILU_MOV: emit_src(sb, ins, 2); break;
    case NV2A_ILU_RCP:
        sb_add(sb, "vec4(1.0 / "); emit_src(sb, ins, 2); sb_add(sb, ".x)");
        break;
    case NV2A_ILU_RCC:
        /* Reciprocal clamped to the hardware's range, which is what makes it
         * different from RCP: a program relying on RCC not returning
         * infinity gets a NaN through the rest of the shader without it. */
        sb_add(sb, "vec4(clamp(1.0 / "); emit_src(sb, ins, 2);
        sb_add(sb, ".x, 5.42101086e-20, 1.884467e+19))");
        break;
    case NV2A_ILU_RSQ:
        sb_add(sb, "vec4(inversesqrt(max(abs("); emit_src(sb, ins, 2);
        sb_add(sb, ".x), 1e-30)))");
        break;
    case NV2A_ILU_EXP:
        sb_add(sb, "vec4(exp2("); emit_src(sb, ins, 2); sb_add(sb, ".x))");
        break;
    case NV2A_ILU_LOG:
        sb_add(sb, "vec4(log2(max(abs("); emit_src(sb, ins, 2);
        sb_add(sb, ".x), 1.17549435e-38)))");
        break;
    case NV2A_ILU_LIT:
        sb_add(sb, "nv_lit("); emit_src(sb, ins, 2); sb_add(sb, ")");
        break;
    default:
        sb_add(sb, "vec4(0.0)");
        break;
    }
}

static const char *GLSL_PRELUDE =
"#version 330 core\n"
"// NV2A vertex program, translated.\n"
"// OpenGL's clip volume puts z in [-w, w]; the console's, like D3D's, in\n"
"// [0, w]. The epilogue converts.\n"
"#define NV2A_CLIP_Z01 0\n"
"uniform vec4 c[192];\n"
"uniform vec4 vpScale;   // NV097_SET_VIEWPORT_SCALE\n"
"uniform vec2 zClip;        // NV097_SET_CLIP_MIN / _MAX\n"
"uniform vec4 vpOff;     // NV097_SET_VIEWPORT_OFFSET\n"
"uniform vec2 vpSurface; // render target size, for the NDC conversion\n"
"uniform int  posMode;   // how to read what the program wrote to oPos\n"
"out vec4 oD0;\n"
"out vec4 oD1;\n"
"out vec4 oT0;\n"
"out vec4 oT1;\n"
"out vec4 oT2;\n"
"out vec4 oT3;\n"
"out float oFogC;\n"
"\n"
"// The NV2A's LIT, which differs from the GL fixed-function one in the\n"
"// clamping of the specular exponent -- a program that relies on the clamp\n"
"// produces a black highlight without it rather than a wrong one.\n"
"vec4 nv_lit(vec4 src) {\n"
"    float d = src.x, s = src.y, p = clamp(src.w, -127.9961, 127.9961);\n"
"    float spec = (d > 0.0 && s > 0.0) ? exp2(p * log2(max(s, 1e-30))) : 0.0;\n"
"    return vec4(1.0, max(d, 0.0), spec, 1.0);\n"
"}\n";

/*
 * Screen space back to clip space.
 *
 * Shared by the translated programs and by the fixed-function emitter: the
 * console applies the viewport transform in hardware for fixed-function
 * draws and inside the shader's own epilogue for programmable ones, so both
 * arrive here having written oPos in the same space, and one conversion
 * serves them both.
 */
static void emit_viewport_epilogue(SB *sb, int ff)
{
    sb_add(sb,
        "\n"
        "    vec4 ndc;\n");
    if (ff) {
        /*
         * The fixed-function half, done the way the hardware does it (xemu,
         * hw/xbox/nv2a/pgraph/glsl/vsh-ff.c):
         *
         *     oPos = position * composite;  oPos.xy /= oPos.w;
         *     oPos.xy += c[VPOFF].xy;
         *     oPos.xy = (2 * oPos.xy - surfaceSize) / surfaceSize;
         *
         * The composite matrix D3D uploads already carries the viewport
         * scale AND the screen centre, so the divide lands in 0..640 by
         * 0..480 pixels; the viewport OFFSET register, which the driver sets
         * to (0.53125, 0.53125) for the duration of its fixed-function draws
         * and to (320.53, 240.53) for its vertex-program draws, is added on
         * top by the hardware and is only the half-pixel nudge here. The
         * body has added it already. Then back to clip space by the surface
         * size, exactly as the programmable path in xemu does.
         *
         * Before this, the shared mode-0 conversion below subtracted that
         * (0.53, 0.53) offset and divided by the viewport scale -- which is
         * the inverse of the programmable path's (320.53, 240.53) viewport,
         * not of this one. Screen centre (320, 240) therefore landed at the
         * bottom-right CORNER of the frame: the whole fixed-function world --
         * every building, road and deck -- was drawn half a screen right and
         * half a screen down of where the title put it, while every vertex-
         * program draw -- every character, vehicle and pedestrian -- was
         * where it belonged. Pedestrians in the sky above the rooftops, a
         * bus in the air beside a deck that stops mid-span, and Beat
         * standing on nothing were all one displacement.
         */
        sb_add(sb,
            "    ndc.x = oPos.x * 2.0 / vpSurface.x - 1.0;\n"
            "    ndc.y = 1.0 - oPos.y * 2.0 / vpSurface.y;\n"
            "    ndc.z = (zClip.y > zClip.x)\n"
            "          ? (oPos.z - zClip.x) / (zClip.y - zClip.x)\n"
            "          : (oPos.z - vpOff.z) / (vpScale.z != 0.0 ? vpScale.z : 1.0);\n"
            "#if !NV2A_CLIP_Z01\n"
            "    ndc.z = ndc.z * 2.0 - 1.0;   // D3D depth [0,1] -> GL [-1,1]\n"
            "#endif\n"
            "    ndc.w = oPos.w;\n"
            "    gl_Position = vec4(ndc.xyz * ndc.w, ndc.w);\n"
            "    oFogC = oFog.x;\n"
            "    gl_PointSize = max(oPts.x, 1.0);\n");
        return;
    }
    sb_add(sb,
        "    if (posMode == 1) {\n"
        "        // The program left clip space alone: hand it over as is,\n"
        "        // remapping depth only where the target API wants [-w,w].\n"
        "#if NV2A_CLIP_Z01\n"
        "        gl_Position = vec4(oPos.xyz, oPos.w);\n"
        "#else\n"
        "        gl_Position = vec4(oPos.xy, 2.0 * oPos.z - oPos.w, oPos.w);\n"
        "#endif\n"
        "    } else if (posMode == 2) {\n"
        "        // Screen space in pixels, converted with the render target's\n"
        "        // own size rather than the viewport registers -- which a\n"
        "        // title may be using for its clip volume rather than for the\n"
        "        // transform its program already applied.\n"
        "        ndc.x = oPos.x * 2.0 / vpSurface.x - 1.0;\n"
        "        ndc.y = 1.0 - oPos.y * 2.0 / vpSurface.y;\n"
        "        ndc.z = oPos.z / 16777215.0;\n"
        "#if !NV2A_CLIP_Z01\n"
        "        ndc.z = ndc.z * 2.0 - 1.0;\n"
        "#endif\n"
        "        ndc.w = oPos.w;\n"
        "        gl_Position = vec4(ndc.xyz * ndc.w, ndc.w);\n"
        "    } else {\n"
        "    ndc.x = (oPos.x - vpOff.x) / max(vpScale.x, 1e-9);\n"
        "    ndc.y = (oPos.y - vpOff.y) / (vpScale.y != 0.0 ? vpScale.y : 1.0);\n"
        "    // Depth: the range the title actually asked for.\n"
        "    //\n"
        "    // NV097_SET_CLIP_MIN and _MAX give the screen-space z the\n"
        "    // viewport maps into, and this title sets them before nearly\n"
        "    // every batch. Assuming 0..16777215 instead was right only when\n"
        "    // the title happened to agree.\n"
        "    //\n"
        "    // Measured, though, this title does not agree either -- it\n"
        "    // leaves depth in D3D\'s 0..1 and never applies the viewport z\n"
        "    // scale, while declaring a clip range of 0..16777215. Dividing\n"
        "    // by that range then crushes every fragment in the scene to\n"
        "    // z = 0: the depth readback reports every pixel inside the\n"
        "    // first tenth, and a shader that paints gl_FragCoord.z comes\n"
        "    // back uniformly white at a linear stretch of that tenth. Two\n"
        "    // instruments, same answer. Its own projection matrix agrees:\n"
        "    // the z and w rows give z/w between 0 and 1 across the whole\n"
        "    // frustum, not 0 and sixteen million.\n"
        "    //\n"
        "    // With every surface at the same depth value, LESS stops\n"
        "    // meaning anything -- whichever draw reaches a pixel first\n"
        "    // keeps it and everything behind it in submission order is\n"
        "    // rejected, however far away it really is. NV2A_Z01 takes the\n"
        "    // program\'s depth as the 0..1 it already is.\n"
        "#ifdef NV2A_Z01\n"
        "    // The perspective divide, which oPos has not had yet.\n"
        "    //\n"
        "    // Taking oPos.z straight clipped the whole scene to black: the\n"
        "    // projection\'s z row is 0.113*z - 135.5 against a w row of\n"
        "    // 0.026*z, so oPos.z reaches forty at the far plane and only\n"
        "    // the ratio is the 0..1 depth. x and y are recovered from the\n"
        "    // viewport registers, which is the same divide wearing a\n"
        "    // different hat; z has no viewport scale to recover it with,\n"
        "    // because this title never applied one.\n"
        "    ndc.z = oPos.w != 0.0 ? oPos.z / oPos.w : oPos.z;\n"
        "#else\n"
        "    ndc.z = (zClip.y > zClip.x)\n"
        "          ? (oPos.z - zClip.x) / (zClip.y - zClip.x)\n"
        "          : (oPos.z - vpOff.z) / (vpScale.z != 0.0 ? vpScale.z : 1.0);\n"
        "#endif\n"
        "#if !NV2A_CLIP_Z01\n"
        "    ndc.z = ndc.z * 2.0 - 1.0;   // D3D depth [0,1] -> GL [-1,1]\n"
        "#endif\n"
        "    ndc.w = oPos.w;\n"
        "    gl_Position = vec4(ndc.xyz * ndc.w, ndc.w);\n"
        "    }\n"
        "    oFogC = oFog.x;\n"
        "    gl_PointSize = max(oPts.x, 1.0);\n");
}

/*
 * The translated program itself: local state, the instruction stream, and the
 * viewport epilogue.
 *
 * Shared verbatim by the GLSL and Metal emitters. The two languages differ in
 * how a shader declares its inputs, outputs and constants -- and in nothing
 * else that appears here, because both are C-like and agree on dot, clamp,
 * mix, floor, fract, exp2 and log2. Keeping one body generator is not tidiness:
 * a title with a thousand distinct programs cannot be checked by eye, so two
 * copies would differ silently and only on the programs nobody looked at.
 *
 * The caller supplies, in scope and by these names: v0..v15 for the inputs it
 * reads, `c` indexable to 191, vpScale, vpOff, vpSurface, posMode, the
 * varyings oD0, oD1, oT0..oT3, oFogC, and gl_Position / gl_PointSize.
 */
static void emit_program_body(SB *sb, const Nv2aVshProgram *p)
{
    int i;
    for (i = 0; i < NV2A_VSH_NUM_TEMPS - 1; i++)
        sb_add(sb, "    vec4 R%d = vec4(0.0);\n", i);
    sb_add(sb,
        "    vec4 oPos = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "    vec4 oFog = vec4(0.0);\n"
        "    vec4 oPts = vec4(1.0);\n"
        "    vec4 oB0 = vec4(0.0);\n"
        "    vec4 oB1 = vec4(0.0);\n"
        "    int  A0 = 0;\n"
        "    vec4 mac, ilu;\n");
    /* Outputs the program never writes still have to be defined: a varying
     * left undefined is not zero, it is whatever the last draw left in the
     * register, which shows up as colours flickering between frames. */
    sb_add(sb,
        "    oD0 = vec4(1.0); oD1 = vec4(0.0);\n"
        "    oT0 = vec4(0.0); oT1 = vec4(0.0);\n"
        "    oT2 = vec4(0.0); oT3 = vec4(0.0);\n"
        "    oFogC = 1.0;\n\n");

    for (i = 0; i < p->length; i++) {
        const Nv2aVshInsn *ins = &p->insns[i];
        int have_mac = (ins->mac != NV2A_MAC_NOP);
        int have_ilu = (ins->ilu != NV2A_ILU_NOP);

        if (!have_mac && !have_ilu)
            continue;
        sb_add(sb, "    // slot %d\n", i);

        /* Evaluate both units before either writes back. On the hardware they
         * issue together and read the same register file; doing it in two
         * steps is what makes "MUL R0, R0, c[0]" paired with an ILU reading
         * R0 come out the same here as on the console. */
        if (have_mac) {
            sb_add(sb, "    mac = "); emit_mac_expr(sb, ins);
            sb_add(sb, ";\n");
        }
        if (have_ilu) {
            sb_add(sb, "    ilu = "); emit_ilu_expr(sb, ins);
            sb_add(sb, ";\n");
        }

        if (have_mac && ins->mac == NV2A_MAC_ARL) {
            sb_add(sb, "    A0 = int(floor(");
            emit_src(sb, ins, 0);
            sb_add(sb, ".x));\n");
        } else if (have_mac && ins->mac_mask) {
            sb_add(sb, "    ");
            emit_temp_name(sb, ins->mac_temp);
            emit_mask(sb, ins->mac_mask);
            sb_add(sb, " = mac");
            if (ins->mac_mask != 0xF) {
                int w = mask_width(ins->mac_mask), k;
                sb_add(sb, ".");
                for (k = 0; k < 4; k++)
                    if (ins->mac_mask & (8 >> k)) sb_add(sb, "%c", SWZ[k]);
                (void)w;
            }
            sb_add(sb, ";\n");
        }

        if (have_ilu && ins->ilu_mask) {
            sb_add(sb, "    ");
            emit_temp_name(sb, ins->ilu_temp);
            emit_mask(sb, ins->ilu_mask);
            sb_add(sb, " = ilu");
            if (ins->ilu_mask != 0xF) {
                int k;
                sb_add(sb, ".");
                for (k = 0; k < 4; k++)
                    if (ins->ilu_mask & (8 >> k)) sb_add(sb, "%c", SWZ[k]);
            }
            sb_add(sb, ";\n");
        }

        if (ins->out_is_oreg && ins->out_mask && ins->out_reg < 16
         && OREG_NAME[ins->out_reg]) {
            const char *src = ins->out_from_ilu ? "ilu" : "mac";
            sb_add(sb, "    %s", OREG_NAME[ins->out_reg]);
            emit_mask(sb, ins->out_mask);
            sb_add(sb, " = %s", src);
            if (ins->out_mask != 0xF) {
                int k;
                sb_add(sb, ".");
                for (k = 0; k < 4; k++)
                    if (ins->out_mask & (8 >> k)) sb_add(sb, "%c", SWZ[k]);
            }
            sb_add(sb, ";\n");
        }
    }

    /*
     * The NV2A's viewport is applied inside the vertex program: a title
     * multiplies by its own projection and then by the viewport scale and
     * offset the driver gave it, so what the program produces is already in
     * screen coordinates -- pixels across, pixels down, and the depth range
     * the title asked for -- not clip space.
     *
     * OpenGL wants clip space, so undo it here rather than in the backend:
     * this keeps the whole convention in one place, and it means a draw with
     * a different viewport only has to change two uniforms.
     *
     * The .w is left alone deliberately. Position is still homogeneous at
     * this point and the perspective divide has not happened, so scaling xyz
     * against w is what puts the geometry back where the title meant it.
     */
    emit_viewport_epilogue(sb, 0);
}


/* ---- fixed-function transform ------------------------------------------ */

/*
 * The NV2A's hardware T&L unit, as a shader.
 *
 * D3D8's SetVertexShader takes either a compiled program or an FVF code, and
 * an FVF draw does not run a program at all -- it runs the fixed pipeline,
 * selected by NV097_SET_TRANSFORM_EXECUTION_MODE. JSRF submits more than half
 * its geometry that way: measured over one title screen, 157,700 fixed-
 * function batches against 139,340 programmable ones.
 *
 * Nothing here read that register before, so every one of those batches was
 * drawn with whichever program happened to be loaded last -- usually the
 * driver's own pre-transformed-vertex passthrough, which multiplies position
 * by a screen-space scale and passes the texture coordinates straight
 * through. Fed world-space vertices, it scattered the scenery a thousand
 * units off-screen; what stayed visible was one enormous triangle per
 * surface, sampling a few texels' worth of its texture across the whole
 * frame. That is the flat backdrop.
 *
 * The composite matrix arrives by its own method rather than through the
 * constant file, and the hardware mirrors it into constants 0..3 -- the same
 * arrangement as the viewport registers at 58 and 59. Reading it from there
 * keeps this shader's inputs identical in kind to a translated program's.
 *
 * That matrix already has the viewport folded into it. One vertex read out
 * of a real batch settles it: its transformed z over w comes to 16,777,215
 * exactly -- the full depth range, which is what NV097_SET_VIEWPORT_SCALE
 * holds -- so the driver multiplied the viewport in rather than leaving it
 * to the hardware, and x and y land in pixels for the same reason. Applying
 * the scale a second time here multiplied y by -240, which is a vertical
 * flip, and the first textured frame came out upside down.
 */
int nv2a_vsh_emit_ff_glsl(const Nv2aVshFixed *f, char *buf, int bufsize)
{
    SB sb = { buf, bufsize, 0, 0 };
    int i;

    sb_add(&sb, "%s", GLSL_PRELUDE);
    for (i = 0; i < NV2A_VSH_NUM_INPUTS; i++)
        if (f->inputs_read & (1u << i))
            sb_add(&sb, "layout(location = %d) in vec4 v%d;\n", i, i);

    sb_add(&sb,
        "\nuniform vec4 ffMat[4];      // NV097_SET_COMPOSITE_MATRIX\n"
        "uniform vec4 ffVpOff;       // NV097_SET_VIEWPORT_OFFSET, as written\n"
        "uniform float litAmbient;   // stand-in for the T&L unit's lighting\n"
        "uniform vec4 ffTexMat[16];  // NV097_SET_TEXTURE_MATRIX, 4 stages\n"
        "\nvoid main() {\n"
        "    vec4 oPos, oFog = vec4(0.0), oPts = vec4(1.0);\n"
        "    // Composite matrix: model x view x projection.\n"
        "    //\n"
        "    // Its own uniform, not the constant file. The hardware mirrors\n"
        "    // it into the low end of that file, and copying that faithfully\n"
        "    // destroyed the constants this title's own vertex programs read\n"
        "    // -- it writes them from c[0] upward. Every fixed-function draw\n"
        "    // was overwriting the next programmable draw's transform, which\n"
        "    // is why the traffic and the characters hung in mid-air while\n"
        "    // the city they belonged to was in the right place.\n"
        "    vec4 p = vec4(dot(v0, ffMat[0]), dot(v0, ffMat[1]),\n"
        "                  dot(v0, ffMat[2]), dot(v0, ffMat[3]));\n"
        "    // The viewport is already folded into the composite matrix --\n"
        "    // the driver multiplies it in rather than leaving it to the\n"
        "    // hardware -- so the divide by w lands straight in screen\n"
        "    // pixels, and the shared epilogue takes it from there.\n"
        "    float rhw = (p.w != 0.0) ? 1.0 / p.w : 0.0;\n"
        "    // The hardware adds the viewport offset register after the\n"
        "    // divide (xemu vsh-ff.c); the epilogue then un-screens by the\n"
        "    // surface size. w stays the true clip w so the rasteriser\n"
        "    // interpolates texture coordinates perspective-correctly --\n"
        "    // handing it 1/w inverted the correction on every world polygon.\n"
        "    oPos.xy = p.xy * rhw + ffVpOff.xy;\n"
        "    oPos.z = p.z * rhw;\n"
        "    oPos.w = (p.w != 0.0) ? p.w : 1.0;\n");

    /* Colour. Lighting is not emulated yet; a title that pre-bakes its
     * shading into the vertex colours -- which a cel-shaded one does -- is
     * correct either way, and one that does not comes out evenly lit rather
     * than black. */
    /*
     * Colour.
     *
     * Nothing here lights anything. A title that bakes its shading into the
     * vertex colours -- which a cel-shaded one does -- comes out right either
     * way; a title that asked the hardware to light the vertices does not,
     * because the stream colour is then only the material and the sun is
     * missing. This scene renders about two and a half times darker than the
     * reference and the title does enable lighting for some batches, so which
     * of the two this is has to be decided rather than assumed.
     */
    sb_add(&sb,
        !f->has_diffuse ? "    oD0 = vec4(1.0);\n"
        : f->lit        ? "    oD0 = vec4(v3.rgb + vec3(litAmbient), v3.a);\n"
                        : "    oD0 = v3;\n");
    sb_add(&sb,
        f->has_specular ? "    oD1 = v4;\n" : "    oD1 = vec4(0.0);\n");

    for (i = 0; i < 4; i++) {
        const char *o[4] = { "oT0", "oT1", "oT2", "oT3" };
        int v = 9 + i;
        if (!(f->inputs_read & (1u << v))) {
            sb_add(&sb, "    %s = vec4(0.0);\n", o[i]);
        } else if (f->tex_matrix & (1u << i)) {
            /* NV097_SET_TEXTURE_MATRIX, in its own uniform. */
            int b = 4 * i;
            sb_add(&sb,
                "    %s = vec4(dot(v%d, ffTexMat[%d]), dot(v%d, ffTexMat[%d]),\n"
                "              dot(v%d, ffTexMat[%d]), dot(v%d, ffTexMat[%d]));\n",
                o[i], v, b, v, b + 1, v, b + 2, v, b + 3);
        } else {
            sb_add(&sb, "    %s = v%d;\n", o[i], v);
        }
    }
    sb_add(&sb, "    oFogC = 1.0;\n");
    emit_viewport_epilogue(&sb, 1);
    sb_add(&sb, "}\n");
    return sb.overflow ? 0 : sb.pos;
}


int nv2a_vsh_emit_glsl(const Nv2aVshProgram *p, char *buf, int bufsize)
{
    SB sb = { buf, bufsize, 0, 0 };
    int i;

    sb_add(&sb, "%s", GLSL_PRELUDE);

    /* Only the inputs the program reads. Declaring all sixteen would compile
     * but would make every draw look like it needs sixteen bound arrays. */
    for (i = 0; i < NV2A_VSH_NUM_INPUTS; i++)
        if (p->inputs_read & (1u << i))
            sb_add(&sb, "layout(location = %d) in vec4 v%d;\n", i, i);

    sb_add(&sb, "\nvoid main() {\n");
    emit_program_body(&sb, p);
    sb_add(&sb, "}\n");

    return sb.overflow ? 0 : sb.pos;
}


/* ---- Metal ------------------------------------------------------------- */

/*
 * The Metal Shading Language is C++, and the parts of it this translation
 * touches agree with GLSL on almost everything: dot, min, max, clamp, floor,
 * abs, exp2, log2, mix, fract, and constructing a vector from a scalar all
 * spell and behave the same. What differs is the shape around the code --
 * how a shader receives its inputs and constants, and how it returns its
 * outputs -- plus three function names.
 *
 * So the prelude aliases the vector types, supplies the three missing names,
 * and the body generator is used unchanged. The alternative, a second copy of
 * the instruction emitter, would drift: JSRF alone compiles over a thousand
 * distinct programs, no one reads them, and a divergence would surface as one
 * wrong-looking object in one scene.
 */
/*
 * What crosses from the vertex stage to the fragment stage.
 *
 * Metal matches the two by the shape of this struct, so both shaders have to
 * declare it identically -- and the fragment emitter lives in another file.
 * One definition, handed out, is what stops a member added on one side from
 * silently shifting every varying on the other.
 */
static const char *MSL_VARYINGS =
"struct Nv2aVshOut {\n"
"    float4 gl_Position [[position]];\n"
"    float  gl_PointSize [[point_size]];\n"
"    float4 oD0;\n"
"    float4 oD1;\n"
"    float4 oT0;\n"
"    float4 oT1;\n"
"    float4 oT2;\n"
"    float4 oT3;\n"
"    float  oFogC;\n"
"};\n";

const char *nv2a_msl_varying_struct(void) { return MSL_VARYINGS; }

static const char *MSL_PRELUDE =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"// NV2A vertex program, translated.\n"
"// Metal's clip volume puts z in [0, w], the same as the console's, so the\n"
"// epilogue leaves depth alone -- a remap that is right for OpenGL halves\n"
"// the depth range here and buries everything in the near half.\n"
"#define NV2A_CLIP_Z01 1\n"
"typedef float2 vec2;\n"
"typedef float3 vec3;\n"
"typedef float4 vec4;\n"
"\n"
"// GLSL spellings the body uses that Metal names differently.\n"
"static inline float4 lessThan(float4 a, float4 b)         { return float4(a <  b); }\n"
"static inline float4 greaterThanEqual(float4 a, float4 b) { return float4(a >= b); }\n"
"static inline float  inversesqrt(float v)                 { return rsqrt(v); }\n"
"static inline float4 inversesqrt(float4 v)                { return rsqrt(v); }\n"
"\n"
"struct Nv2aVshUniforms {\n"
"    float4 c[192];\n"
"    float4 vpScale;   // NV097_SET_VIEWPORT_SCALE\n"
"    float4 vpOff;     // NV097_SET_VIEWPORT_OFFSET\n"
"    float2 zClip;     // NV097_SET_CLIP_MIN / _MAX\n"
"    float2 vpSurface; // render target size, for the NDC conversion\n"
"    int    posMode;   // how to read what the program wrote to oPos\n"
"};\n"
"\n"
"// The NV2A's LIT, which differs from the fixed-function one in the clamping\n"
"// of the specular exponent -- a program that relies on the clamp produces a\n"
"// black highlight without it rather than a wrong one.\n"
"static inline float4 nv_lit(float4 src) {\n"
"    float d = src.x, s = src.y, p = clamp(src.w, -127.9961f, 127.9961f);\n"
"    float spec = (d > 0.0f && s > 0.0f) ? exp2(p * log2(max(s, 1e-30f))) : 0.0f;\n"
"    return float4(1.0f, max(d, 0.0f), spec, 1.0f);\n"
"}\n";

int nv2a_vsh_emit_msl(const Nv2aVshProgram *p, char *buf, int bufsize)
{
    SB sb = { buf, bufsize, 0, 0 };
    int i;

    sb_add(&sb, "%s\n%s", MSL_PRELUDE, MSL_VARYINGS);

    /* Only the inputs the program reads, at the attribute index the NV2A
     * register number gives -- the same convention the GLSL path uses for
     * layout locations, so one vertex-descriptor builder serves both. */
    sb_add(&sb, "\nstruct Nv2aVshIn {\n");
    for (i = 0; i < NV2A_VSH_NUM_INPUTS; i++)
        if (p->inputs_read & (1u << i))
            sb_add(&sb, "    float4 v%d [[attribute(%d)]];\n", i, i);
    sb_add(&sb, "};\n\n");

    sb_add(&sb,
        "vertex Nv2aVshOut nv2a_vsh_main(Nv2aVshIn vin [[stage_in]],\n"
        "        constant Nv2aVshUniforms &U [[buffer(%d)]])\n"
        "{\n"
        "    constant float4 *c = &U.c[0];\n"
        "    float4 vpScale = U.vpScale;\n"
        "    float4 vpOff = U.vpOff;\n"
        "    float2 vpSurface = U.vpSurface;\n"
        "    float2 zClip = U.zClip;\n"
        "    int posMode = U.posMode;\n",
        NV2A_MSL_VSH_UNIFORM_INDEX);

    for (i = 0; i < NV2A_VSH_NUM_INPUTS; i++)
        if (p->inputs_read & (1u << i))
            sb_add(&sb, "    float4 v%d = vin.v%d;\n", i, i);

    /* The varyings are globals in GLSL and struct members in Metal; the body
     * writes them by bare name either way, so give it locals to write and
     * gather them at the end. */
    sb_add(&sb,
        "    float4 oD0, oD1, oT0, oT1, oT2, oT3;\n"
        "    float  oFogC;\n"
        "    float4 gl_Position = float4(0.0);\n"
        "    float  gl_PointSize = 1.0;\n");

    emit_program_body(&sb, p);

    sb_add(&sb,
        "    Nv2aVshOut o;\n"
        "    o.gl_Position = gl_Position;\n"
        "    o.gl_PointSize = gl_PointSize;\n"
        "    o.oD0 = oD0; o.oD1 = oD1;\n"
        "    o.oT0 = oT0; o.oT1 = oT1; o.oT2 = oT2; o.oT3 = oT3;\n"
        "    o.oFogC = oFogC;\n"
        "    return o;\n"
        "}\n");

    return sb.overflow ? 0 : sb.pos;
}

/* ---- disassembly -------------------------------------------------------- */

static const char *MAC_NAME[16] = {
    "NOP","MOV","MUL","ADD","MAD","DP3","DPH","DP4",
    "DST","MIN","MAX","SLT","SGE","ARL","?","?"
};
static const char *ILU_NAME[8] = {
    "NOP","MOV","RCP","RCC","RSQ","EXP","LOG","LIT"
};

static void disasm_src(SB *sb, const Nv2aVshInsn *ins, int which)
{
    const Nv2aSrc *s = &ins->src[which];

    sb_add(sb, "%s", s->negate ? "-" : "");
    switch (s->type) {
    case NV2A_PARAM_R: sb_add(sb, "R%u", s->index); break;
    case NV2A_PARAM_V: sb_add(sb, "v%u", ins->input_index); break;
    case NV2A_PARAM_C:
        if (ins->rel_addr) sb_add(sb, "c[A0+%d]", ins->const_index);
        else               sb_add(sb, "c[%d]", ins->const_index);
        break;
    default: sb_add(sb, "0"); break;
    }
    if (!(s->swz[0] == 0 && s->swz[1] == 1 && s->swz[2] == 2 && s->swz[3] == 3))
        sb_add(sb, ".%c%c%c%c", SWZ[s->swz[0]], SWZ[s->swz[1]],
               SWZ[s->swz[2]], SWZ[s->swz[3]]);
}

static void disasm_mask(SB *sb, uint8_t m)
{
    if (m == 0xF) return;
    sb_add(sb, ".");
    if (m & 8) sb_add(sb, "x");
    if (m & 4) sb_add(sb, "y");
    if (m & 2) sb_add(sb, "z");
    if (m & 1) sb_add(sb, "w");
}

int nv2a_vsh_disasm(const Nv2aVshProgram *p, char *buf, int bufsize)
{
    SB sb = { buf, bufsize, 0, 0 };
    int i;

    for (i = 0; i < p->length; i++) {
        const Nv2aVshInsn *ins = &p->insns[i];
        int n;

        sb_add(&sb, "%3d: ", i);
        if (ins->mac != NV2A_MAC_NOP) {
            sb_add(&sb, "%s ", MAC_NAME[ins->mac & 15]);
            if (ins->mac_mask) {
                sb_add(&sb, "R%u", ins->mac_temp);
                disasm_mask(&sb, ins->mac_mask);
                sb_add(&sb, ", ");
            }
            n = (ins->mac == NV2A_MAC_MOV || ins->mac == NV2A_MAC_ARL) ? 1
              : (ins->mac == NV2A_MAC_MAD) ? 3 : 2;
            {
                int k;
                for (k = 0; k < n; k++) {
                    int which = (n == 2 && ins->mac == NV2A_MAC_ADD && k == 1)
                              ? 2 : k;
                    if (k) sb_add(&sb, ", ");
                    disasm_src(&sb, ins, which);
                }
            }
        }
        if (ins->ilu != NV2A_ILU_NOP) {
            if (ins->mac != NV2A_MAC_NOP) sb_add(&sb, "  + ");
            sb_add(&sb, "%s ", ILU_NAME[ins->ilu & 7]);
            if (ins->ilu_mask) {
                sb_add(&sb, "R%u", ins->ilu_temp);
                disasm_mask(&sb, ins->ilu_mask);
                sb_add(&sb, ", ");
            }
            disasm_src(&sb, ins, 2);
        }
        if (ins->out_is_oreg && ins->out_mask) {
            const char *nm = (ins->out_reg < 16) ? OREG_NAME[ins->out_reg] : 0;
            sb_add(&sb, "   -> %s", nm ? nm : "o?");
            disasm_mask(&sb, ins->out_mask);
            sb_add(&sb, " (%s)", ins->out_from_ilu ? "ilu" : "mac");
        }
        if (ins->final) sb_add(&sb, "   [END]");
        sb_add(&sb, "\n");
    }
    return sb.overflow ? 0 : sb.pos;
}

/*
 * The fixed-function transform, as a Metal vertex function.
 *
 * The same shader as nv2a_vsh_emit_ff_glsl, in the other dialect, and
 * deliberately written beside it rather than generated from it: the two
 * languages differ in how a shader declares its inputs and its constants and
 * in nothing else that matters here, so the divergence is small enough to read
 * and the alternative -- a dialect-parameterised emitter -- makes the common
 * case harder to follow in order to avoid twenty lines of repetition.
 *
 * The matrices are in their own buffer, not the constant file, for the reason
 * the GLSL version explains at length: the hardware mirrors them into the low
 * end of that file, and this title reads its own constants from there.
 */
int nv2a_vsh_emit_ff_msl(const Nv2aVshFixed *f, char *buf, int bufsize)
{
    SB sb = { buf, bufsize, 0, 0 };
    int i;

    sb_add(&sb, "%s\n%s", MSL_PRELUDE, MSL_VARYINGS);

    sb_add(&sb,
        "\nstruct Nv2aFfUniforms {\n"
        "    float4 ffMat[4];      // NV097_SET_COMPOSITE_MATRIX\n"
        "    float4 ffTexMat[16];  // NV097_SET_TEXTURE_MATRIX, 4 stages\n"
        "    float4 vpScale;\n"
        "    float4 vpOff;\n"
        "    float2 zClip;\n"
        "    float2 vpSurface;\n"
        "    int    posMode;\n"
        "};\n\n");

    sb_add(&sb, "struct Nv2aVshIn {\n");
    for (i = 0; i < NV2A_VSH_NUM_INPUTS; i++)
        if (f->inputs_read & (1u << i))
            sb_add(&sb, "    float4 v%d [[attribute(%d)]];\n", i, i);
    sb_add(&sb, "};\n\n");

    sb_add(&sb,
        "vertex Nv2aVshOut nv2a_vsh_main(Nv2aVshIn vin [[stage_in]],\n"
        "        constant Nv2aFfUniforms &U [[buffer(%d)]])\n"
        "{\n"
        "    float4 vpScale = U.vpScale;\n"
        "    float4 vpOff = U.vpOff;\n"
        "    float2 zClip = U.zClip;\n"
        "    float2 vpSurface = U.vpSurface;\n"
        "    int posMode = U.posMode;\n"
        "    float4 oPos, oFog = float4(0.0), oPts = float4(1.0);\n"
        "    float4 oD0, oD1, oT0, oT1, oT2, oT3;\n"
        "    float  oFogC;\n"
        "    float4 gl_Position = float4(0.0);\n"
        "    float  gl_PointSize = 1.0;\n",
        NV2A_MSL_VSH_UNIFORM_INDEX);

    for (i = 0; i < NV2A_VSH_NUM_INPUTS; i++)
        if (f->inputs_read & (1u << i))
            sb_add(&sb, "    float4 v%d = vin.v%d;\n", i, i);

    sb_add(&sb,
        "    // Composite matrix: model x view x projection, with the viewport\n"
        "    // already multiplied in, so the divide lands in screen pixels.\n"
        "    float4 p = float4(dot(v0, U.ffMat[0]), dot(v0, U.ffMat[1]),\n"
        "                      dot(v0, U.ffMat[2]), dot(v0, U.ffMat[3]));\n"
        "    float rhw = (p.w != 0.0) ? 1.0 / p.w : 0.0;\n"
        "    // The viewport offset register, added after the divide as the\n"
        "    // hardware does (see the GLSL emitter). U.vpOff is that register.\n"
        "    oPos.xy = p.xy * rhw + U.vpOff.xy;\n"
        "    oPos.z = p.z * rhw;\n"
        "    oPos.w = (p.w != 0.0) ? p.w : 1.0;\n");

    sb_add(&sb,
        !f->has_diffuse ? "    oD0 = float4(1.0);\n" : "    oD0 = v3;\n");
    sb_add(&sb,
        f->has_specular ? "    oD1 = v4;\n" : "    oD1 = float4(0.0);\n");

    for (i = 0; i < 4; i++) {
        const char *o[4] = { "oT0", "oT1", "oT2", "oT3" };
        int v = 9 + i;
        if (!(f->inputs_read & (1u << v))) {
            sb_add(&sb, "    %s = float4(0.0);\n", o[i]);
        } else if (f->tex_matrix & (1u << i)) {
            int b = 4 * i;
            sb_add(&sb,
                "    %s = float4(dot(v%d, U.ffTexMat[%d]), dot(v%d, U.ffTexMat[%d]),\n"
                "                dot(v%d, U.ffTexMat[%d]), dot(v%d, U.ffTexMat[%d]));\n",
                o[i], v, b, v, b + 1, v, b + 2, v, b + 3);
        } else {
            sb_add(&sb, "    %s = v%d;\n", o[i], v);
        }
    }

    emit_viewport_epilogue(&sb, 1);

    sb_add(&sb,
        "    Nv2aVshOut o;\n"
        "    o.gl_Position = gl_Position;\n"
        "    o.gl_PointSize = gl_PointSize;\n"
        "    o.oD0 = oD0; o.oD1 = oD1;\n"
        "    o.oT0 = oT0; o.oT1 = oT1; o.oT2 = oT2; o.oT3 = oT3;\n"
        "    o.oFogC = oFogC;\n"
        "    return o;\n"
        "}\n");

    return sb.overflow ? 0 : sb.pos;
}
