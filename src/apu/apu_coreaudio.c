/**
 * CoreAudio output backend, for macOS and iOS.
 *
 * Implements the same five entry points as the XAudio2 backend, because the
 * APU already asks its output through them and a second shape of question
 * would mean a second code path in the mixer. On this platform the answer is
 * an AudioQueue: it is plain C rather than Objective-C, it exists unchanged
 * on the phone, and it does not need an audio thread of our own.
 *
 * The queue pulls; the emulated APU pushes, on the timing of the guest's own
 * frame delivery. A ring between them absorbs the difference. Underrun plays
 * silence rather than stalling the guest -- an emulator that blocks its CPU
 * thread waiting for a speaker turns an audio glitch into a frame drop, and
 * the glitch is much the smaller of the two.
 */

#include "apu_xaudio2.h"

#if defined(__APPLE__)

#include <AudioToolbox/AudioToolbox.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CA_RATE        48000
#define CA_CHANNELS    2
#define CA_BUFFERS     3
#define CA_BUF_FRAMES  512            /* ~10.7 ms; three of them ~32 ms */
#define CA_RING_FRAMES 8192           /* power of two, ~170 ms of slack */

static AudioQueueRef       g_queue;
static AudioQueueBufferRef g_bufs[CA_BUFFERS];
static int                 g_active;

static int16_t          g_ring[CA_RING_FRAMES][CA_CHANNELS];
static _Atomic uint32_t g_wr, g_rd;
static _Atomic uint32_t g_underruns;
static _Atomic uint32_t g_drops;
static _Atomic uint64_t g_last_pull_ms;

static uint64_t ca_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* One device pull: CA_BUF_FRAMES frames out of the ring, silence for any
 * the ring does not have. */
static void ca_pull(int16_t (*out)[CA_CHANNELS])
{
    uint32_t want = CA_BUF_FRAMES;
    uint32_t rd = atomic_load_explicit(&g_rd, memory_order_relaxed);
    uint32_t wr = atomic_load_explicit(&g_wr, memory_order_acquire);
    uint32_t have = wr - rd;
    uint32_t i, n = have < want ? have : want;

    atomic_store_explicit(&g_last_pull_ms, ca_now_ms(), memory_order_relaxed);
    for (i = 0; i < n; i++)
        memcpy(out[i], g_ring[(rd + i) & (CA_RING_FRAMES - 1)], sizeof g_ring[0]);
    if (n < want) {
        memset(out[n], 0, (want - n) * sizeof g_ring[0]);
        atomic_fetch_add_explicit(&g_underruns, 1, memory_order_relaxed);
    }
    atomic_store_explicit(&g_rd, rd + n, memory_order_release);
}

static void ca_callback(void *user, AudioQueueRef q, AudioQueueBufferRef buf)
{
    (void)user;
    ca_pull((int16_t (*)[CA_CHANNELS])buf->mAudioData);
    buf->mAudioDataByteSize = CA_BUF_FRAMES * (UInt32)sizeof g_ring[0];
    AudioQueueEnqueueBuffer(q, buf, 0, NULL);
}

/* RECOMP_APU_FAKE_SINK=1 (test only): no audio device -- a thread pulls
 * CA_BUF_FRAMES from the ring every CA_BUF_FRAMES/48000 s and throws them
 * away, the way the queue would. The APU's ring pacing (apu_core.c,
 * throttle) runs against it exactly as against a real device, which is what
 * a machine whose audio output has gone away overnight cannot otherwise
 * test. */
#include <pthread.h>
static pthread_t g_fake_thread;
static void *ca_fake_sink(void *arg)
{
    static int16_t scratch[CA_BUF_FRAMES][CA_CHANNELS];
    struct timespec next;
    const long period_ns = (long)((uint64_t)CA_BUF_FRAMES * 1000000000ull / CA_RATE);
    /* RECOMP_APU_SINK_RAW=<path>: what the "device" received, raw s16
     * stereo 48 kHz -- underrun silence and dropped audio included, which
     * RECOMP_APU_WAV (the encode processor's output) cannot show. */
    FILE *raw = NULL;
    { const char *p = getenv("RECOMP_APU_SINK_RAW");
      if (p) raw = fopen(p, "wb"); }
    (void)arg;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        next.tv_nsec += period_ns;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        for (;;) {
            struct timespec now, d;
            clock_gettime(CLOCK_MONOTONIC, &now);
            d.tv_sec = next.tv_sec - now.tv_sec;
            d.tv_nsec = next.tv_nsec - now.tv_nsec;
            if (d.tv_nsec < 0) { d.tv_nsec += 1000000000L; d.tv_sec--; }
            if (d.tv_sec < 0) break;
            nanosleep(&d, NULL);
        }
        ca_pull(scratch);
        if (raw) fwrite(scratch, sizeof scratch, 1, raw);
    }
    return NULL;
}

int xa2_init(void)
{
    AudioStreamBasicDescription fmt;
    OSStatus st;
    int i;

    if (g_active) return 1;

    if (getenv("RECOMP_APU_FAKE_SINK")) {
        atomic_store(&g_wr, 0);
        atomic_store(&g_rd, 0);
        if (pthread_create(&g_fake_thread, NULL, ca_fake_sink, NULL) != 0) {
            fprintf(stderr, "[APU] fake sink: no thread\n");
            return 0;
        }
        g_active = 1;
        fprintf(stderr, "[APU] RECOMP_APU_FAKE_SINK: a thread pulls %d frames every %.2f ms, "
                "no device\n", CA_BUF_FRAMES, CA_BUF_FRAMES * 1000.0 / CA_RATE);
        return 1;
    }

    memset(&fmt, 0, sizeof fmt);
    fmt.mSampleRate       = CA_RATE;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsSignedInteger
                          | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = CA_CHANNELS;
    fmt.mBitsPerChannel   = 16;
    fmt.mBytesPerFrame    = CA_CHANNELS * 2;
    fmt.mFramesPerPacket  = 1;
    fmt.mBytesPerPacket   = fmt.mBytesPerFrame;

    st = AudioQueueNewOutput(&fmt, ca_callback, NULL, NULL, NULL, 0, &g_queue);
    if (st != noErr) {
        fprintf(stderr, "[APU] AudioQueueNewOutput failed (%d)\n", (int)st);
        return 0;
    }
    for (i = 0; i < CA_BUFFERS; i++) {
        UInt32 bytes = CA_BUF_FRAMES * (UInt32)sizeof g_ring[0];
        if (AudioQueueAllocateBuffer(g_queue, bytes, &g_bufs[i]) != noErr) {
            fprintf(stderr, "[APU] AudioQueueAllocateBuffer failed\n");
            AudioQueueDispose(g_queue, true);
            g_queue = NULL;
            return 0;
        }
        /* Prime with silence: the queue will not start until it has buffers,
         * and the guest has not produced a sample yet. */
        memset(g_bufs[i]->mAudioData, 0, bytes);
        g_bufs[i]->mAudioDataByteSize = bytes;
        AudioQueueEnqueueBuffer(g_queue, g_bufs[i], 0, NULL);
    }
    if (AudioQueueStart(g_queue, NULL) != noErr) {
        fprintf(stderr, "[APU] AudioQueueStart failed\n");
        AudioQueueDispose(g_queue, true);
        g_queue = NULL;
        return 0;
    }
    atomic_store(&g_wr, 0);
    atomic_store(&g_rd, 0);
    g_active = 1;
    fprintf(stderr, "[APU] CoreAudio output: %d Hz stereo, %d x %d frames\n",
            CA_RATE, CA_BUFFERS, CA_BUF_FRAMES);
    return 1;
}

void xa2_shutdown(void)
{
    if (!g_active) return;
    if (!g_queue) { g_active = 0; return; }   /* the fake sink */
    AudioQueueStop(g_queue, true);
    AudioQueueDispose(g_queue, true);
    g_queue = NULL;
    g_active = 0;
    fprintf(stderr, "[APU] CoreAudio output stopped (%u underruns)\n",
            (unsigned)atomic_load(&g_underruns));
}

int xa2_is_active(void) { return g_active; }

int xa2_get_buffer_size(void) { return CA_BUF_FRAMES; }

/* What the APU paces itself on (apu_core.c, throttle): how much is waiting,
 * and whether the device is still pulling at all. */
int xa2_ring_level(void)
{
    if (!g_active) return -1;
    return (int)(atomic_load_explicit(&g_wr, memory_order_acquire)
               - atomic_load_explicit(&g_rd, memory_order_acquire));
}

int xa2_ms_since_pull(void)
{
    uint64_t t;
    if (!g_active) return -1;
    t = atomic_load_explicit(&g_last_pull_ms, memory_order_relaxed);
    if (!t) return 1 << 30;
    return (int)(ca_now_ms() - t);
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    uint32_t wr, rd, space;
    int i;

    if (!g_active || num_samples <= 0) return 0;
    wr = atomic_load_explicit(&g_wr, memory_order_relaxed);
    rd = atomic_load_explicit(&g_rd, memory_order_acquire);
    space = CA_RING_FRAMES - (wr - rd);
    /* Full means the guest is ahead of real time, which happens whenever the
     * emulation runs faster than the console did. Dropping the oldest audio
     * rather than the newest keeps the delay from growing without bound. */
    if ((uint32_t)num_samples > space) {
        uint32_t drop = (uint32_t)num_samples - space;
        atomic_store_explicit(&g_rd, rd + drop, memory_order_release);
        atomic_fetch_add_explicit(&g_drops, drop, memory_order_relaxed);
    }

    /*
     * RECOMP_APU_STATS=1: how the ring is doing, once a second.
     *
     * Correct samples that arrive unevenly sound broken in a way that is
     * indistinguishable, by ear, from samples that are wrong: the queue plays
     * silence whenever it is starved and the guest's audio drops whenever it
     * is full, and both are heard as crackle. The underrun counter was only
     * printed at shutdown, and these runs are killed rather than shut down,
     * so it had never been read. Occupancy is the honest measure: it should
     * sit at a steady few hundred frames, not swing between empty and full.
     */
    { static int on = -1; static uint64_t last_ms; static uint32_t lu, ld;
      if (on < 0) on = getenv("RECOMP_APU_STATS") ? 1 : 0;
      if (on) {
          struct timespec ts; uint64_t now;
          clock_gettime(CLOCK_MONOTONIC, &ts);
          now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
          if (!last_ms) last_ms = now;
          if (now - last_ms >= 1000) {
              uint32_t u = atomic_load(&g_underruns), dr = atomic_load(&g_drops);
              fprintf(stderr, "  [APU] ring %u/%u frames (%.0f ms), "
                      "%u underruns/s, %u frames dropped/s\n",
                      wr - rd, (unsigned)CA_RING_FRAMES,
                      (double)(wr - rd) * 1000.0 / CA_RATE,
                      u - lu, dr - ld);
              fflush(stderr);
              lu = u; ld = dr; last_ms = now;
          }
      } }
    for (i = 0; i < num_samples; i++)
        memcpy(g_ring[(wr + (uint32_t)i) & (CA_RING_FRAMES - 1)],
               &samples[i * CA_CHANNELS], sizeof g_ring[0]);
    atomic_store_explicit(&g_wr, wr + (uint32_t)num_samples,
                          memory_order_release);
    return 1;
}

#endif /* __APPLE__ */
