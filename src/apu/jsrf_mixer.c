/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* External JSRF mixer. Original game envelopes/volumes remain untouched.
 * Guest thread publishes routing and gains; audio thread only uses atomics,
 * fixed-size arrays and sample arithmetic. No file I/O or locks in audio. */
#include "jsrf_mixer.h"
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>
static atomic_int percentages[JM_COUNT] = {100, 100, 100, 100, 100, 100, 100};
static atomic_int muted_bus[JM_COUNT];
static atomic_uchar voice_bus[256];
static atomic_uint epoch, last_seen[256], pre_peak[256], post_peak[256], master_peak;
static float last_gain[256], last_master = 1.0f;
static unsigned char initialized[256];
static const char *keys[JM_COUNT] = {"master", "music",    "effects", "dialogue",
                                     "dj",     "ambience", "other"};
const char *xbox_JsrfMixer_Key(int bus)
{
    return bus >= 0 && bus < JM_COUNT ? keys[bus] : "other";
}
static unsigned bits(float v)
{
    unsigned n;
    memcpy(&n, &v, 4);
    return n;
}
static float number(unsigned v)
{
    float n;
    memcpy(&n, &v, 4);
    return n;
}
int xbox_JsrfMixer_Classify(const char *path)
{
    char p[512];
    size_t n, i;
    if (!path)
        return JM_OTHER;
    n = strlen(path);
    if (n < 4 || n >= sizeof p)
        return JM_OTHER;
    for (i = 0; i <= n; i++)
        p[i] = path[i] == '\\' ? '/' : (char)tolower((unsigned char)path[i]);
    if (strcmp(p + n - 4, ".adx"))
        return JM_OTHER;
    if (strstr(p, "/z_adx/bgm/"))
        return JM_MUSIC;
    if (strstr(p, "/z_adx/musen/") || strstr(p, "/z_adx/musenj/"))
        return JM_DIALOGUE;
    if (strstr(p, "/z_adx/dj/") || strstr(p, "/z_adx/dj_demo/"))
        return JM_DJ;
    if (strstr(p, "/z_adx/env/"))
        return JM_AMBIENCE;
    return JM_OTHER;
}
void xbox_JsrfMixer_SetBus(int bus, int percent, int muted)
{
    if (bus < 0 || bus >= JM_COUNT || percent < 0 || percent > 100)
        return;
    atomic_store_explicit(&percentages[bus], percent, memory_order_relaxed);
    atomic_store_explicit(&muted_bus[bus], muted != 0, memory_order_relaxed);
}
void xbox_JsrfMixer_Map(const unsigned char groups[256])
{
    for (unsigned i = 0; i < 256; i++)
        atomic_store_explicit(&voice_bus[i], groups[i], memory_order_relaxed);
}
static int group_of(unsigned v)
{
    int b = atomic_load_explicit(&voice_bus[v], memory_order_relaxed);
    return b > JM_MASTER && b < JM_COUNT ? b : JM_EFFECTS;
}
static float gain_of(int bus)
{
    if (atomic_load_explicit(&muted_bus[bus], memory_order_relaxed))
        return 0.0f;
    float gain = atomic_load_explicit(&percentages[bus], memory_order_relaxed) / 100.0f;
    return gain;
}
float xbox_JsrfMixer_SoftwareGain(void)
{
    return gain_of(JM_EFFECTS);
}
void xbox_JsrfMixer_Voice(unsigned v, float samples[][2], int frames, int multipass)
{
    if (v >= 256 || frames <= 0 || multipass)
        return; /* multipass already contains grouped sources */
    int bus = group_of(v);
    float target = gain_of(bus), from = initialized[v] ? last_gain[v] : 1.0f;
    float before = 0, after = 0;
    initialized[v] = 1;
    for (int i = 0; i < frames; i++)
    {
        float gain = from + (target - from) * (i + 1) / (float)frames;
        for (int c = 0; c < 2; c++)
        {
            float a = fabsf(samples[i][c]);
            if (a > before)
                before = a;
            samples[i][c] *= gain;
            a = fabsf(samples[i][c]);
            if (a > after)
                after = a;
        }
    }
    last_gain[v] = target;
    atomic_store_explicit(&pre_peak[v], bits(before), memory_order_relaxed);
    atomic_store_explicit(&post_peak[v], bits(after), memory_order_relaxed);
    atomic_store_explicit(&last_seen[v], atomic_load_explicit(&epoch, memory_order_relaxed) + 1,
                          memory_order_relaxed);
}
void xbox_JsrfMixer_Master(int16_t *samples, int frames)
{
    float target = gain_of(JM_MASTER), from = last_master, peak = 0;
    for (int i = 0; i < frames; i++)
    {
        float gain = from + (target - from) * (i + 1) / (float)frames;
        for (int c = 0; c < 2; c++)
        {
            int at = 2 * i + c;
            if (gain != 1.0f)
                samples[at] = (int16_t)(samples[at] * gain);
            float a = fabsf(samples[at] / 32768.0f);
            if (a > peak)
                peak = a;
        }
    }
    last_master = target;
    atomic_store_explicit(&master_peak, bits(peak), memory_order_relaxed);
    atomic_fetch_add_explicit(&epoch, 1, memory_order_relaxed);
}
void xbox_JsrfMixer_Stats(int bus, int *percent, int *muted, int *active, float *peak,
                          float *effective)
{
    *percent = atomic_load_explicit(&percentages[bus], memory_order_relaxed);
    *muted = atomic_load_explicit(&muted_bus[bus], memory_order_relaxed);
    *active = 0;
    *peak = 0;
    *effective = gain_of(bus);
    unsigned now = atomic_load_explicit(&epoch, memory_order_relaxed) + 1;
    for (unsigned v = 0; v < 256; v++)
    {
        unsigned seen = atomic_load_explicit(&last_seen[v], memory_order_relaxed);
        if (!seen || now - seen > 100 ||
            number(atomic_load_explicit(&pre_peak[v], memory_order_relaxed)) < 0.00001f)
            continue;
        if (bus != JM_MASTER && group_of(v) != bus)
            continue;
        (*active)++;
        float p = number(atomic_load_explicit(&post_peak[v], memory_order_relaxed));
        if (p > *peak)
            *peak = p;
    }
    if (bus == JM_MASTER)
        *peak = number(atomic_load_explicit(&master_peak, memory_order_relaxed));
}
