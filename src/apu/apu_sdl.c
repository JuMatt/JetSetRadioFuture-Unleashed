/**
 * SDL2 audio output backend, for Linux (and any POSIX host that is not
 * Apple's).
 *
 * The same entry points as the CoreAudio backend (apu_coreaudio.c), and the
 * same shape: the device pulls, the emulated APU pushes on the timing of the
 * guest's own frame delivery, and a ring between them absorbs the
 * difference. Underrun plays silence rather than stalling the guest. The
 * ring level and the time since the device last pulled are what the APU
 * paces itself on (apu_core.c, throttle), so the game's audio runs on the
 * sound card's clock rather than drifting against it.
 */

#include "apu_xaudio2.h"

#if !defined(_WIN32) && !defined(__APPLE__)

#include <SDL.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>

#define SA_RATE        48000
#define SA_CHANNELS    2
#define SA_BUF_FRAMES  512            /* ~10.7 ms per device pull */
#define SA_RING_FRAMES 8192           /* power of two, ~170 ms of slack */

static SDL_AudioDeviceID g_dev;
static int               g_active;

static int16_t          g_ring[SA_RING_FRAMES][SA_CHANNELS];
static _Atomic uint32_t g_wr, g_rd;
static _Atomic uint32_t g_underruns;
static _Atomic uint32_t g_drops;
static _Atomic uint64_t g_last_pull_ms;

static uint64_t sa_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* One device pull: n frames out of the ring, silence for any it lacks. */
static void sa_pull(int16_t (*out)[SA_CHANNELS], uint32_t want)
{
    uint32_t rd = atomic_load_explicit(&g_rd, memory_order_relaxed);
    uint32_t wr = atomic_load_explicit(&g_wr, memory_order_acquire);
    uint32_t have = wr - rd;
    uint32_t i, n = have < want ? have : want;

    atomic_store_explicit(&g_last_pull_ms, sa_now_ms(), memory_order_relaxed);
    for (i = 0; i < n; i++)
        memcpy(out[i], g_ring[(rd + i) & (SA_RING_FRAMES - 1)], sizeof g_ring[0]);
    if (n < want) {
        memset(out[n], 0, (want - n) * sizeof g_ring[0]);
        atomic_fetch_add_explicit(&g_underruns, 1, memory_order_relaxed);
    }
    atomic_store_explicit(&g_rd, rd + n, memory_order_release);
}

static void sa_callback(void *user, Uint8 *stream, int len)
{
    (void)user;
    sa_pull((int16_t (*)[SA_CHANNELS])stream,
            (uint32_t)len / (uint32_t)sizeof g_ring[0]);
}

/* RECOMP_APU_FAKE_SINK=1 (test only): no audio device -- a thread pulls
 * SA_BUF_FRAMES from the ring every SA_BUF_FRAMES/48000 s and throws them
 * away, the way the device would. RECOMP_APU_SINK_RAW=<path> keeps what it
 * pulled (raw s16 stereo 48 kHz). */
static pthread_t g_fake_thread;
static void *sa_fake_sink(void *arg)
{
    static int16_t scratch[SA_BUF_FRAMES][SA_CHANNELS];
    struct timespec next;
    const long period_ns = (long)((uint64_t)SA_BUF_FRAMES * 1000000000ull / SA_RATE);
    FILE *raw = NULL;
    { const char *p = getenv("RECOMP_APU_SINK_RAW");
      if (p) raw = fopen(p, "wb"); }
    (void)arg;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        next.tv_nsec += period_ns;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        sa_pull(scratch, SA_BUF_FRAMES);
        if (raw) fwrite(scratch, sizeof scratch, 1, raw);
    }
    return NULL;
}

int xa2_init(void)
{
    SDL_AudioSpec want, have;

    if (g_active) return 1;
    atomic_store(&g_wr, 0);
    atomic_store(&g_rd, 0);

    if (getenv("RECOMP_APU_FAKE_SINK")) {
        if (pthread_create(&g_fake_thread, NULL, sa_fake_sink, NULL) != 0) {
            fprintf(stderr, "[APU] fake sink: no thread\n");
            return 0;
        }
        g_active = 1;
        fprintf(stderr, "[APU] RECOMP_APU_FAKE_SINK: a thread pulls %d frames every %.2f ms, "
                "no device\n", SA_BUF_FRAMES, SA_BUF_FRAMES * 1000.0 / SA_RATE);
        return 1;
    }

    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[APU] SDL audio unavailable: %s\n", SDL_GetError());
        return 0;
    }
    memset(&want, 0, sizeof want);
    want.freq     = SA_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = SA_CHANNELS;
    want.samples  = SA_BUF_FRAMES;
    want.callback = sa_callback;
    /* Whatever the device really runs at, SDL converts: the ring and the
     * APU's pacing stay in 48 kHz stereo frames. */
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_dev) {
        fprintf(stderr, "[APU] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return 0;
    }
    SDL_PauseAudioDevice(g_dev, 0);
    g_active = 1;
    fprintf(stderr, "[APU] SDL audio output (%s): %d Hz stereo, %d-frame pulls\n",
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
            SA_RATE, (int)have.samples);
    return 1;
}

void xa2_shutdown(void)
{
    if (!g_active) return;
    if (g_dev) {
        SDL_CloseAudioDevice(g_dev);
        g_dev = 0;
    }
    g_active = 0;
    fprintf(stderr, "[APU] SDL audio output stopped (%u underruns)\n",
            (unsigned)atomic_load(&g_underruns));
}

int xa2_is_active(void) { return g_active; }

int xa2_get_buffer_size(void) { return SA_BUF_FRAMES; }

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
    return (int)(sa_now_ms() - t);
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    uint32_t wr, rd, space;
    int i;

    if (!g_active || num_samples <= 0) return 0;
    wr = atomic_load_explicit(&g_wr, memory_order_relaxed);
    rd = atomic_load_explicit(&g_rd, memory_order_acquire);
    space = SA_RING_FRAMES - (wr - rd);
    /* Full: the guest is ahead of real time. Drop the oldest audio, not the
     * newest, so the delay cannot grow without bound. */
    if ((uint32_t)num_samples > space) {
        uint32_t drop = (uint32_t)num_samples - space;
        atomic_store_explicit(&g_rd, rd + drop, memory_order_release);
        atomic_fetch_add_explicit(&g_drops, drop, memory_order_relaxed);
    }
    /* RECOMP_APU_STATS=1: the ring once a second (see apu_coreaudio.c). */
    { static int on = -1; static uint64_t last_ms; static uint32_t lu, ld;
      if (on < 0) on = getenv("RECOMP_APU_STATS") ? 1 : 0;
      if (on) {
          uint64_t now = sa_now_ms();
          if (!last_ms) last_ms = now;
          if (now - last_ms >= 1000) {
              uint32_t u = atomic_load(&g_underruns), dr = atomic_load(&g_drops);
              fprintf(stderr, "  [APU] ring %u/%u frames (%.0f ms), "
                      "%u underruns/s, %u frames dropped/s\n",
                      wr - rd, (unsigned)SA_RING_FRAMES,
                      (double)(wr - rd) * 1000.0 / SA_RATE, u - lu, dr - ld);
              fflush(stderr);
              lu = u; ld = dr; last_ms = now;
          }
      } }
    for (i = 0; i < num_samples; i++)
        memcpy(g_ring[(wr + (uint32_t)i) & (SA_RING_FRAMES - 1)],
               &samples[i * SA_CHANNELS], sizeof g_ring[0]);
    atomic_store_explicit(&g_wr, wr + (uint32_t)num_samples, memory_order_release);
    return 1;
}

#endif /* !_WIN32 && !__APPLE__ */
