/*
 * MCPX APU DSP (GP/EP) - Stub implementation
 *
 * The DSP Global Processor (GP) and Encode Processor (EP) handle effects
 * processing (reverb, chorus, etc.) and final output encoding. The full
 * DSP is ~3000 lines of DSP56300 emulation code.
 *
 * For initial audio, we bypass the DSP entirely:
 * - VP mixbins are passed directly to the EP output
 * - GP effects processing is skipped
 * - The EP just copies mixbin 0/1 (front L/R) to the monitor buffer
 *
 * This gives us basic voice playback without effects. The DSP can be
 * connected later for reverb, EQ, and other processing.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "apu_state.h"
#include "fpconv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── DSP command doorbell acknowledgement ────────────────────────────────
 *
 * DirectSound does not stop at creating the device. It hands the audio DSP a
 * command block in guest RAM, writes a command word, and spins until the DSP
 * writes zero back. On real hardware the GP runs a DSP56300 program that does
 * that. Here the DSP is a passthrough stub, so the word never changes and the
 * title hangs inside DirectSound initialisation -- which on Wreckless gates the
 * entire engine, not just audio.
 *
 * RECOMP_APU_DSP_ACK=<addr>[,<addr>...] clears those guest dwords once per APU
 * frame, which is what "the command completed" looks like to the title.
 *
 * ponytail: this is a handshake acknowledgement, not a DSP. It says every
 * command succeeded instantly and computes nothing, so anything whose *result*
 * the title reads back will still be wrong. The real fix is DSP56300 emulation
 * in the GP/EP; this exists so audio init stops blocking everything behind it.
 *
 * The address is not derivable from the APU registers: GPSADDR/GPFADDR/
 * EPSADDR/EPFADDR point at the DSP's own scratch and frame memory, while the
 * command block is a DirectSound heap allocation. On Wreckless the registers
 * read 0x01504000 / 0x014EC000 / 0x0151C000 / 0x014F0000 and the doorbell is at
 * 0x014F8810 -- inside none of them. So it has to be observed: run with
 * RECOMP_WATCHDOG_SECS and the spin shows up as ebx plus the poll offset.
 */
#define APU_DSP_ACK_MAX 8
static uint32_t s_dsp_ack[APU_DSP_ACK_MAX];
static int s_dsp_ack_count = -1;

static void dsp_ack_init(void)
{
    const char *spec = getenv("RECOMP_APU_DSP_ACK");
    char buf[256], *p, *end;

    s_dsp_ack_count = 0;
    if (!spec || !*spec)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (p = buf; *p && s_dsp_ack_count < APU_DSP_ACK_MAX; ) {
        unsigned long v = strtoul(p, &end, 0);
        if (end == p)
            break;
        if (v)
            s_dsp_ack[s_dsp_ack_count++] = (uint32_t)v;
        p = (*end == ',') ? end + 1 : end;
    }
    if (s_dsp_ack_count)
        fprintf(stderr, "[APU] DSP doorbell ack: %d address(es), first 0x%08X\n",
                s_dsp_ack_count, s_dsp_ack[0]);
}

static void dsp_ack_frame(MCPXAPUState *d)
{
    int i;

    if (s_dsp_ack_count < 0)
        dsp_ack_init();
    if (!d->ram_ptr)
        return;
    for (i = 0; i < s_dsp_ack_count; i++) {
        uint32_t *slot = (uint32_t *)(d->ram_ptr + s_dsp_ack[i]);
        if (*slot) {
            static int shown[APU_DSP_ACK_MAX];
            if (shown[i]++ < 3)
                fprintf(stderr, "[APU] DSP doorbell 0x%08X: command 0x%08X"
                                " acknowledged\n", s_dsp_ack[i], *slot);
            *slot = 0;
        }
    }
}

/* Register a doorbell address discovered at runtime.
 *
 * RECOMP_APU_DSP_ACK exists because the address cannot be derived from the APU
 * registers -- but it can be derived from the *title*, whose DirectSound code
 * computes it from its own object before spinning on it. A title that knows
 * where its doorbell is can say so here, which beats an environment variable
 * that has to be rediscovered every time the heap layout shifts underneath it.
 *
 * Idempotent: a call site inside a per-frame function may register the same
 * address thousands of times.
 */
void mcpx_apu_dsp_ack_add(uint32_t addr)
{
    int i;

    if (s_dsp_ack_count < 0)
        dsp_ack_init();
    addr &= 0x0FFFFFFFu;                  /* physical: strip the 0x80000000 VA */
    if (!addr)
        return;
    for (i = 0; i < s_dsp_ack_count; i++)
        if (s_dsp_ack[i] == addr)
            return;
    if (s_dsp_ack_count >= APU_DSP_ACK_MAX)
        return;
    s_dsp_ack[s_dsp_ack_count++] = addr;
    fprintf(stderr, "[APU] DSP doorbell registered by the title: 0x%08X\n", addr);
    fflush(stderr);
}

/* Doorbell ack independent of the APU register state: on hosts without MMIO
 * trapping the APU never sees the title's register writes, so it never looks
 * "active", but the DSP handshake the title spins on still has to be answered. */
void mcpx_apu_dsp_ack_tick(MCPXAPUState *d)
{
    dsp_ack_frame(d);
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    /* Allocate minimal DSP state for GP and EP.
     * We need these to exist so reset doesn't crash,
     * but they won't actually run DSP programs. */
    d->gp.dsp = (DSPState *)calloc(1, sizeof(DSPState));
    d->ep.dsp = (DSPState *)calloc(1, sizeof(DSPState));

    if (d->gp.dsp) d->gp.dsp->is_gp = true;
    if (d->ep.dsp) d->ep.dsp->is_gp = false;

    d->gp.realtime = false;
    d->ep.realtime = false;

    fprintf(stderr, "[APU] DSP GP/EP initialized (STUBBED - passthrough mode)\n");
}

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    /* In the real xemu, this reads settings to decide whether
     * GP/EP should run in realtime or cached mode. We ignore it. */
    (void)d;
}

void mcpx_apu_dsp_frame(MCPXAPUState *d,
                         float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    /* Bypass DSP: take mixbin 0 (front-left) and mixbin 1 (front-right)
     * and write them directly to the monitor frame buffer as the final
     * EP output.
     *
     * The Xbox DirectSound typically routes:
     *   Mixbin 0 = Front Left
     *   Mixbin 1 = Front Right
     *   Mixbin 2 = Center (often unused in stereo)
     *   Mixbin 3 = LFE
     *   Mixbin 4-5 = Rear L/R
     *
     * For stereo output, bins 0 and 1 are what we want.
     */

    int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;

    dsp_ack_frame(d);

    if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            /* Clamp to [-1, 1] range */
            float left = mixbins[0][i];
            float right = mixbins[1][i];
            if (left > 1.0f) left = 1.0f;
            if (left < -1.0f) left = -1.0f;
            if (right > 1.0f) right = 1.0f;
            if (right < -1.0f) right = -1.0f;

            /* Convert to 16-bit and write (not accumulate) into frame buffer.
             * Each of the 8 sub-frames writes its own 32-sample slice. */
            d->monitor.frame_buf[off + i][0] = (int16_t)(left * 32767.0f);
            d->monitor.frame_buf[off + i][1] = (int16_t)(right * 32767.0f);
        }
    }

    /* RECOMP_APU_WAV=<path>: the encode processor's output, as raw 16-bit
     * stereo at 48 kHz.
     *
     * "It makes a horrible noise" is not a diagnosis. A voice decoded in the
     * wrong format and a voice decoded correctly but played at the wrong rate
     * both sound wrong and need different fixes, and both are obvious the
     * moment the samples can be looked at rather than listened to. */
    {
        static FILE *wav = NULL;
        static int tried;
        if (!tried) {
            const char *path = getenv("RECOMP_APU_WAV");
            tried = 1;
            if (path) {
                wav = fopen(path, "wb");
                if (wav) fprintf(stderr, "  [APU] writing raw 48 kHz stereo "
                                         "s16 to %s\n", path);
            }
        }
        if (wav) {
            fwrite(&d->monitor.frame_buf[off][0], 2 * sizeof(int16_t),
                   NUM_SAMPLES_PER_FRAME, wav);
            fflush(wav);
        }
    }

    /* RECOMP_APU_STATS: is the encode processor producing anything?
     *
     * Separate from the output-side peak in apu_core.c on purpose. "The APU is
     * making sound" and "the sound is reaching the device" are different
     * claims, and when they disagree the difference names the bug -- which is
     * exactly how the memset that erased this buffer was found. */
    {
        static int stats = -1;
        static int64_t last_ms;
        static int peak;
        int i;
        if (stats < 0) stats = getenv("RECOMP_APU_STATS") ? 1 : 0;
        if (stats) {
            int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
            for (i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                int a = d->monitor.frame_buf[off + i][0];
                if (a < 0) a = -a;
                if (a > peak) peak = a;
            }
            if (!last_ms) last_ms = now;
            if (now - last_ms >= 1000) {
                fprintf(stderr, "[APU] encode processor peak %d/32767\n", peak);
                fflush(stderr);
                last_ms = now; peak = 0;
            }
        }
    }

    g_dbg.gp.cycles = 0;
    g_dbg.ep.cycles = 0;
}
