/* External general mixer. Original game volume setters are untouched.
 * ADXT filename -> CRI output -> DirectSound buffer -> hardware voice IDs.
 * Unidentified hardware voices belong to Effects / samples. Unknown ADX
 * tracks belong to Other. No fixed hardware voice numbers are assumed. */
#include "../src/apu/jsrf_mixer.h"
#if defined(_WIN32)
#include <fcntl.h>
#endif
typedef struct
{
    uint32_t adxt;
    char path[512];
    int bus;
} JmRoute;
static JmRoute jm_routes[32];
static int jm_levels[JM_COUNT] = {100, 100, 100, 100, 100, 100, 100}, jm_mutes[JM_COUNT];
static double jm_next_config, jm_next_status;
static unsigned char jm_previous[256];
static int jm_first_map = 1;
static int jm_valid(uint32_t p, uint32_t n)
{
    uint64_t end = (uint64_t)p + n;
    return (p >= 0x10000u && end < (64u << 20)) || (p >= 0x80000000u && end < 0x84000000ull);
}
static uint32_t jm_u32(uint32_t p)
{
    return *(uint32_t *)((uint8_t *)g_xbox_mem_offset + p);
}
static void jm_refresh_routes(void)
{
    unsigned char groups[256], claimed[256] = {0};
    memset(groups, JM_EFFECTS, sizeof groups);
    for (int i = 0; i < 32; i++)
    {
        JmRoute *r = &jm_routes[i];
        if (!r->adxt || !jm_valid(r->adxt, 0x80))
            continue;
        uint8_t *m = (uint8_t *)g_xbox_mem_offset;
        int state = (int8_t)m[r->adxt + 1];
        if (state < 1 || state > 4)
            continue;
        uint32_t sd = jm_u32(r->adxt + 0xC);
        if (!jm_valid(sd, 0xA0))
            continue;
        uint32_t impl = jm_u32(sd + 0x38);
        if (!jm_valid(impl, 0x30))
            continue;
        uint32_t iface = jm_u32(impl + 8);
        if (iface < 0x1Cu || !jm_valid(iface - 0x1C, 0x20))
            continue;
        uint32_t owner = jm_u32(iface - 0x1C + 0xC);
        if (!jm_valid(owner, 0x68))
            continue;
        unsigned n = m[owner + 0x64];
        if (n < 1 || n > 8)
            continue;
        for (unsigned k = 0; k < n; k++)
        {
            unsigned hw = *(uint16_t *)(m + owner + 0xC + 2 * k);
            if (hw >= 256)
                continue;
            if (claimed[hw] && groups[hw] != r->bus)
                groups[hw] = JM_OTHER;
            else
                groups[hw] = (unsigned char)r->bus;
            claimed[hw] = 1;
        }
    }
    for (unsigned hw = 0; hw < 256; hw++)
        if (jm_first_map || groups[hw] != jm_previous[hw])
        {
            if (claimed[hw] || (!jm_first_map && jm_previous[hw] != JM_EFFECTS))
                fprintf(stderr, "[MIXER] hardware_voice=%u group=%s\n", hw,
                        xbox_JsrfMixer_Key(groups[hw]));
            jm_previous[hw] = groups[hw];
        }
    jm_first_map = 0;
    xbox_JsrfMixer_Map(groups);
}
static void jm_adxt_start(void)
{
    uint32_t h = GARG(1), p = GARG(2);
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    JmRoute *r = NULL;
    for (int i = 0; i < 32; i++)
        if (jm_routes[i].adxt == h)
        {
            r = &jm_routes[i];
            break;
        }
    if (!r)
        for (int i = 0; i < 32; i++)
            if (!jm_routes[i].adxt)
            {
                r = &jm_routes[i];
                break;
            }
    if (r && jm_valid(h, 0x80))
    {
        memset(r, 0, sizeof *r);
        r->adxt = h;
        if (jm_valid(p, 512))
        {
            size_t n = 0;
            while (n < 511 && m[p + n])
                n++;
            if (n < 511)
            {
                memcpy(r->path, m + p, n);
                r->path[n] = 0;
            }
        }
        r->bus = xbox_JsrfMixer_Classify(r->path);
        fprintf(stderr, "[MIXER] route adxt=%08X group=%s path=%s\n", h, xbox_JsrfMixer_Key(r->bus),
                r->path);
    }
    sub_0013AEB0_gen();
    jm_refresh_routes();
}
static FILE *jm_open_config_shared(const char *path)
{
#if defined(_WIN32)
    HANDLE h =
        CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    int fd = _open_osfhandle((intptr_t)h, _O_RDONLY | _O_TEXT);
    if (fd < 0)
    {
        CloseHandle(h);
        return NULL;
    }
    FILE *f = _fdopen(fd, "r");
    if (!f)
        _close(fd);
    return f;
#else
    return fopen(path, "r");
#endif
}
static void jm_read_config(void)
{
    const char *path = getenv("JSRF_MIXER_FILE");
    if (!path || !*path)
        return;
    FILE *f = jm_open_config_shared(path);
    if (!f)
        return;
    char line[128], key[64];
    int value, changed = 0;
    int next[JM_COUNT], mute[JM_COUNT];
    memcpy(next, jm_levels, sizeof next);
    memcpy(mute, jm_mutes, sizeof mute);
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "%63[^=]=%d", key, &value) == 2)
        {
            for (int b = 0; b < JM_COUNT; b++)
            {
                char mk[64];
                snprintf(mk, sizeof mk, "%s_mute", xbox_JsrfMixer_Key(b));
                if (!strcmp(key, xbox_JsrfMixer_Key(b)) && value >= 0 && value <= 100)
                    next[b] = value;
                if (!strcmp(key, mk) && (value == 0 || value == 1))
                    mute[b] = value;
            }
        }
    fclose(f);
    for (int b = 0; b < JM_COUNT; b++)
    {
        if (next[b] != jm_levels[b] || mute[b] != jm_mutes[b])
            changed = 1;
        jm_levels[b] = next[b];
        jm_mutes[b] = mute[b];
        xbox_JsrfMixer_SetBus(b, next[b], mute[b]);
    }
    if (changed)
    {
        fprintf(
            stderr,
            "[MIXER] config master=%d music=%d effects=%d dialogue=%d dj=%d ambience=%d other=%d\n",
            next[0], next[1], next[2], next[3], next[4], next[5], next[6]);
        fflush(stderr);
    }
}
static void jm_status(void)
{
    const char *path = getenv("JSRF_MIXER_STATUS_FILE");
    if (!path || !*path)
        return;
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "{\"revision\":5,\"groups\":[");
    for (int b = 0; b < JM_COUNT; b++)
    {
        int p, m, a;
        float peak, e;
        xbox_JsrfMixer_Stats(b, &p, &m, &a, &peak, &e);
        fprintf(f,
                "%s{\"key\":\"%s\",\"percent\":%d,\"muted\":%d,\"active\":%d,\"peak\":%.6f,"
                "\"effective\":%.6f}",
                b ? "," : "", xbox_JsrfMixer_Key(b), p, m, a, peak, e);
    }
    fprintf(f, "]}\n");
    fclose(f);
}
static void jsrf_mixer_tick(void)
{
    double now = jsrf_now();
    if (now >= jm_next_config)
    {
        jm_next_config = now + 0.10;
        jm_read_config();
    }
    jm_refresh_routes();
    if (now >= jm_next_status)
    {
        jm_next_status = now + 0.25;
        jm_status();
    }
}
