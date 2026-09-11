/*
 * The NV2A vertex program, interpreted on the CPU.
 *
 * Not for rendering -- far too slow -- but for answering one question that
 * nothing else can. This title's traffic and characters are drawn in the wrong
 * place, they come from the programmable pipeline, and every explanation that
 * could be tested cheaply has been eliminated: the depth range, the coordinate
 * space the program leaves positions in, the clip range, the window clip
 * rectangles, the vertex format. What is left is the program itself -- either
 * it is translated wrongly, or the constants it reads are wrong -- and those
 * two look identical from the picture.
 *
 * Running the microcode directly separates them. If this interpreter puts the
 * object where the screen shows it, the translation agrees with the hardware
 * and the constants are the problem. If it puts it somewhere else, the
 * translation is the problem and this says which instruction.
 *
 * Deliberately written from the decoded instruction stream rather than from
 * the generated GLSL, so that a mistake shared by both would have to be made
 * twice, independently.
 */

#include "nv2a_vsh.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    float r[12][4];
    float a0;
    float out[16][4];
    uint16_t written;
} VshMachine;

static void read_src(const Nv2aVshInsn *ins, const Nv2aSrc *s,
                     const float v[16][4], const float c[][4],
                     const VshMachine *m, float out[4])
{
    const float *base;
    float tmp[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    int i;

    switch (s->type) {
    /* R12 is not a temporary. On the NV2A it is the position output: the
     * same storage as oPos, readable and writable, and programs use that --
     * this title's transform writes oPos with four DP4s and then reads it
     * back as R12 to apply the viewport.
     *
     * There are only twelve temporaries, R0..R11, so `m->r[12]` ran off the
     * end of the array into the fields behind it -- a0, then the first three
     * floats of out[0] -- and handed back oPos shifted one component to the
     * right. Plausible numbers, wrong by one slot, which is the worst kind:
     * every program traced came out with its position pinned to the viewport
     * centre and nothing looked obviously broken.
     *
     * The GLSL translator has always had this right (it emits "oPos" for
     * index 12), so nothing rendered was ever affected. Only this
     * interpreter was wrong -- and this interpreter is what answered "does
     * the bone index land on a matrix" and "what space is oPos in". Both of
     * those answers have to be taken again now. */
    case NV2A_PARAM_R:
        base = (s->index == 12) ? m->out[NV2A_OREG_POS]
                                : m->r[s->index % 12];
        break;
    case NV2A_PARAM_V: base = v[ins->input_index & 15]; break;
    case NV2A_PARAM_C: {
        int idx = ins->const_index;
        if (ins->rel_addr) idx += (int)m->a0;
        if (idx < 0) idx = 0;
        if (idx >= NV2A_VSH_NUM_CONSTS) idx = NV2A_VSH_NUM_CONSTS - 1;
        base = c[idx];
        break;
    }
    default: base = tmp; break;
    }
    for (i = 0; i < 4; i++) out[i] = base[s->swz[i] & 3];
    if (s->negate) for (i = 0; i < 4; i++) out[i] = -out[i];
}

static void writemask(float dst[4], const float src[4], uint8_t mask)
{
    if (mask & 8) dst[0] = src[0];
    if (mask & 4) dst[1] = src[1];
    if (mask & 2) dst[2] = src[2];
    if (mask & 1) dst[3] = src[3];
}

/* The hardware's reciprocal and friends saturate rather than produce
 * infinities; a program that divides by a zero w and then multiplies the
 * result back is common enough that the difference shows. */
static float safe_rcp(float x) { return x != 0.0f ? 1.0f / x : 0.0f; }

/* Where the last run's ARL results go, if anyone asked. A file-scope pair
 * rather than two more parameters, because the interpreter's signature is
 * shared with the test that checks it against the generated shader and this
 * is a diagnostic hook rather than part of what it computes. Single-threaded
 * by construction: it is called from the draw path, which is one thread. */
static int *g_a0_out;
static int  g_a0_n, g_a0_max;

int nv2a_vsh_interp_a0(const Nv2aVshProgram *p,
                       const float v[16][4],
                       const float c[][4],
                       int *a0_out, int a0_max)
{
    int n;
    float pos[4];
    g_a0_out = a0_out; g_a0_n = 0; g_a0_max = a0_max;
    nv2a_vsh_interp(p, v, c, pos);
    n = g_a0_n;
    g_a0_out = NULL; g_a0_n = 0; g_a0_max = 0;
    return n;
}

int nv2a_vsh_interp(const Nv2aVshProgram *p,
                    const float v[16][4],
                    const float c[][4],
                    float out_pos[4])
{
    VshMachine m;
    int i, k;

    memset(&m, 0, sizeof m);
    for (i = 0; i < 16; i++) { m.out[i][3] = 1.0f; }

    for (i = 0; i < p->length; i++) {
        const Nv2aVshInsn *ins = &p->insns[i];
        float a[4], b[4], cc[4], macr[4] = {0,0,0,0}, ilur[4] = {0,0,0,0};
        int did_mac = 0, did_ilu = 0;

        read_src(ins, &ins->src[0], v, c, &m, a);
        read_src(ins, &ins->src[1], v, c, &m, b);
        read_src(ins, &ins->src[2], v, c, &m, cc);

        switch (ins->mac) {
        case NV2A_MAC_NOP: break;
        case NV2A_MAC_MOV: for (k=0;k<4;k++) macr[k]=a[k]; did_mac=1; break;
        case NV2A_MAC_MUL: for (k=0;k<4;k++) macr[k]=a[k]*b[k]; did_mac=1; break;
        case NV2A_MAC_ADD: for (k=0;k<4;k++) macr[k]=a[k]+cc[k]; did_mac=1; break;
        case NV2A_MAC_MAD: for (k=0;k<4;k++) macr[k]=a[k]*b[k]+cc[k]; did_mac=1; break;
        case NV2A_MAC_DP3: { float d=a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
                             for (k=0;k<4;k++) macr[k]=d; did_mac=1; } break;
        case NV2A_MAC_DPH: { float d=a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+b[3];
                             for (k=0;k<4;k++) macr[k]=d; did_mac=1; } break;
        case NV2A_MAC_DP4: { float d=a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3];
                             for (k=0;k<4;k++) macr[k]=d; did_mac=1; } break;
        case NV2A_MAC_DST: macr[0]=1.0f; macr[1]=a[1]*b[1];
                           macr[2]=a[2];  macr[3]=b[3]; did_mac=1; break;
        case NV2A_MAC_MIN: for (k=0;k<4;k++) macr[k]=a[k]<b[k]?a[k]:b[k]; did_mac=1; break;
        case NV2A_MAC_MAX: for (k=0;k<4;k++) macr[k]=a[k]>b[k]?a[k]:b[k]; did_mac=1; break;
        case NV2A_MAC_SLT: for (k=0;k<4;k++) macr[k]=a[k]<b[k]?1.0f:0.0f; did_mac=1; break;
        case NV2A_MAC_SGE: for (k=0;k<4;k++) macr[k]=a[k]>=b[k]?1.0f:0.0f; did_mac=1; break;
        case NV2A_MAC_ARL:
            m.a0 = floorf(a[0]);
            /* Every address the program sets, in order, for a caller that
             * wants to know where its constant reads are landing. */
            if (g_a0_out && g_a0_n < g_a0_max) g_a0_out[g_a0_n++] = (int)m.a0;
            break;
        default: break;
        }

        switch (ins->ilu) {
        case NV2A_ILU_NOP: break;
        case NV2A_ILU_MOV: for (k=0;k<4;k++) ilur[k]=cc[k]; did_ilu=1; break;
        case NV2A_ILU_RCP: { float r=safe_rcp(cc[0]);
                             for (k=0;k<4;k++) ilur[k]=r; did_ilu=1; } break;
        case NV2A_ILU_RCC: { float x=cc[0], r;
                             if (x > 0.0f) r = x < 5.42101e-20f ? 1.0f/5.42101e-20f : 1.0f/x;
                             else if (x < 0.0f) r = x > -5.42101e-20f ? 1.0f/-5.42101e-20f : 1.0f/x;
                             else r = 0.0f;
                             for (k=0;k<4;k++) ilur[k]=r; did_ilu=1; } break;
        case NV2A_ILU_RSQ: { float x=fabsf(cc[0]);
                             float r = x > 0.0f ? 1.0f/sqrtf(x) : 0.0f;
                             for (k=0;k<4;k++) ilur[k]=r; did_ilu=1; } break;
        case NV2A_ILU_EXP: { float r=exp2f(cc[0]);
                             for (k=0;k<4;k++) ilur[k]=r; did_ilu=1; } break;
        case NV2A_ILU_LOG: { float x=fabsf(cc[0]);
                             float r = x > 0.0f ? log2f(x) : -3.4e38f;
                             for (k=0;k<4;k++) ilur[k]=r; did_ilu=1; } break;
        case NV2A_ILU_LIT: { float d=cc[0], sp=cc[1];
                             float pw=cc[3] < -127.9961f ? -127.9961f
                                    : cc[3] >  127.9961f ?  127.9961f : cc[3];
                             ilur[0]=1.0f;
                             ilur[1]=d>0.0f?d:0.0f;
                             ilur[2]=(d>0.0f && sp>0.0f)
                                   ? exp2f(pw*log2f(sp>1e-30f?sp:1e-30f)) : 0.0f;
                             ilur[3]=1.0f; did_ilu=1; } break;
        default: break;
        }

        /* Writes to R12 land on oPos, for the same reason reads do. */
        if (did_mac && ins->mac_mask) {
            if (ins->mac_temp == 12) {
                writemask(m.out[NV2A_OREG_POS], macr, ins->mac_mask);
                m.written |= (uint16_t)(1u << NV2A_OREG_POS);
            } else {
                writemask(m.r[ins->mac_temp % 12], macr, ins->mac_mask);
            }
        }
        if (did_ilu && ins->ilu_mask) {
            if (ins->ilu_temp == 12) {
                writemask(m.out[NV2A_OREG_POS], ilur, ins->ilu_mask);
                m.written |= (uint16_t)(1u << NV2A_OREG_POS);
            } else {
                writemask(m.r[ins->ilu_temp % 12], ilur, ins->ilu_mask);
            }
        }

        if (ins->out_is_oreg && ins->out_mask) {
            const float *src = ins->out_from_ilu ? ilur : macr;
            if (ins->out_reg < 16) {
                writemask(m.out[ins->out_reg], src, ins->out_mask);
                m.written |= (uint16_t)(1u << ins->out_reg);
            }
        }
        /* The whole program, once, with all three sources and where each
         * one came from.
         *
         * The old trace stopped at eight instructions and printed A and C --
         * omitting B, which is the operand DP4 actually multiplies by, so
         * the one line that would show a matrix row arriving as zeros was
         * the line it did not print. And eight instructions is less than a
         * transform: the writes to oPos come later than that, which is
         * precisely the region in question, since every program measured
         * lands oPos.x on the viewport centre with no positional term at
         * all. Trace one program end to end instead. */
        { static int traced = -1, run;
          if (traced < 0) traced = getenv("RECOMP_GL_INTERP_TRACE") ? 0 : -2;
          if (traced >= 0 && run < 2) {
              static const char *ty[4] = { "-", "R", "v", "c" };
              fprintf(stderr, "  [I] %2d mac=%d ilu=%d  out=%s%u mask=%X "
                      "macmask=%X->R%u ilumask=%X->R%u"
                      "  A=%s%u(%.3f %.3f %.3f %.3f)"
                      "  B=%s%u(%.3f %.3f %.3f %.3f)"
                      "  C=%s%u(%.3f %.3f %.3f %.3f)"
                      "  => (%.3f %.3f %.3f %.3f)\n",
                      i, ins->mac, ins->ilu,
                      ins->out_is_oreg ? "o" : "R", ins->out_reg,
                      ins->out_mask, ins->mac_mask, ins->mac_temp,
                      ins->ilu_mask, ins->ilu_temp,
                      ty[ins->src[0].type & 3],
                      ins->src[0].type == NV2A_PARAM_C ? (unsigned)ins->const_index
                      : ins->src[0].type == NV2A_PARAM_V ? ins->input_index
                      : ins->src[0].index,
                      a[0],a[1],a[2],a[3],
                      ty[ins->src[1].type & 3],
                      ins->src[1].type == NV2A_PARAM_C ? (unsigned)ins->const_index
                      : ins->src[1].type == NV2A_PARAM_V ? ins->input_index
                      : ins->src[1].index,
                      b[0],b[1],b[2],b[3],
                      ty[ins->src[2].type & 3],
                      ins->src[2].type == NV2A_PARAM_C ? (unsigned)ins->const_index
                      : ins->src[2].type == NV2A_PARAM_V ? ins->input_index
                      : ins->src[2].index,
                      cc[0],cc[1],cc[2],cc[3],
                      ins->out_from_ilu ? ilur[0] : macr[0],
                      ins->out_from_ilu ? ilur[1] : macr[1],
                      ins->out_from_ilu ? ilur[2] : macr[2],
                      ins->out_from_ilu ? ilur[3] : macr[3]);
              if (ins->final) { run++; fprintf(stderr, "  [I] --- end of "
                      "program, oPos = (%.3f %.3f %.3f %.3f) ---\n",
                      m.out[NV2A_OREG_POS][0], m.out[NV2A_OREG_POS][1],
                      m.out[NV2A_OREG_POS][2], m.out[NV2A_OREG_POS][3]); }
          } }
        if (ins->final) break;
    }

    for (k = 0; k < 4; k++) out_pos[k] = m.out[NV2A_OREG_POS][k];
    return (m.written & (1u << NV2A_OREG_POS)) ? 1 : 0;
}
