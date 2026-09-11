/**
 * Function tracing for recompiled code.
 *
 * recomp_types.h declares these and tools.recomp emits calls to them under
 * --trace-functions, but until now nothing defined them: the definitions lived
 * in one game project, so enabling tracing anywhere else failed at link time
 * with three unresolved symbols and no hint that the fix was to go and copy a
 * file. They belong with the runtime that declares them.
 *
 * Output goes to stderr, unbuffered, because the question tracing answers is
 * usually "what was the last thing that happened before it died".
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "xbox_memory_layout.h"

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;

/* A run that recurses produces trace lines without limit, and the useful
 * window is rarely the first few thousand. The budget stops a diagnostic from
 * filling a disk, and is deliberately generous: a budget that runs out before
 * the interesting part turns "no trace here" into a false negative, which is
 * worse than a large file. Override with RECOMP_TRACE_BUDGET. */
static long trace_budget(void)
{
    static long budget = -1;
    if (budget < 0) {
        const char *env = getenv("RECOMP_TRACE_BUDGET");
        budget = env ? strtol(env, NULL, 0) : 400000;
        if (budget < 0) budget = 0;
    }
    return budget > 0 ? budget-- : 0;
}

/* Where a title spends its calls.
 *
 * A recompiled title that is CPU-bound gives no clue which guest code is
 * responsible: the native profile is a wall of sub_XXXX, and the guest has no
 * program counter to sample. Counting entries does answer it, and the trace
 * hook is already on every function the run was generated to trace -- so
 * tracing everything and tallying instead of printing turns the existing
 * mechanism into a profile for the cost of an array increment.
 *
 * Counts, not time: a function called once that loops for a minute does not
 * appear here, and one called ten million times cheaply does. It says where
 * the calls go, which is the first question, not the last.
 *
 * Enable with RECOMP_TRACE_PROFILE=1. The report goes to stderr at exit,
 * hottest first.
 */
#define PROF_SLOTS 8192                 /* open addressing, power of two */

static struct { uint32_t va; unsigned long long hits; } g_prof[PROF_SLOTS];
static const char *g_prof_name[PROF_SLOTS];
static int g_prof_used, g_prof_full;
static unsigned long long g_prof_calls;

static void prof_report(void)
{
    int taken[40], ntaken = 0;
    int i, j, shown;

    if (!g_prof_used)
        return;
    fprintf(stderr, "\n[PROFILE] %d functions entered%s, hottest first:\n",
            g_prof_used, g_prof_full ? " (table full, some dropped)" : "");
    for (shown = 0; shown < 40; shown++) {
        int best = -1;
        for (i = 0; i < PROF_SLOTS; i++) {
            if (!g_prof[i].hits)
                continue;
            for (j = 0; j < ntaken; j++)
                if (taken[j] == i)
                    break;
            if (j < ntaken)
                continue;
            if (best < 0 || g_prof[i].hits > g_prof[best].hits)
                best = i;
        }
        if (best < 0)
            break;
        taken[ntaken++] = best;
        fprintf(stderr, "  %14llu  %s (0x%08X)\n",
                g_prof[best].hits, g_prof_name[best] ? g_prof_name[best] : "?",
                g_prof[best].va);
    }
    fflush(stderr);
}

/* RECOMP_TRACE_PROFILE=1 profiles; a larger number is also how often to
 * report, in calls. The default suits a title burning a core in a spin loop;
 * a title that no longer has one may never reach it, and then the only report
 * is the one at exit -- which a killed run never gets. */
static unsigned long long prof_interval(void)
{
    static unsigned long long every;
    if (!every) {
        const char *v = getenv("RECOMP_TRACE_PROFILE");
        unsigned long long n = v ? strtoull(v, NULL, 0) : 0;
        every = n > 1 ? n : 20000000ull;
    }
    return every;
}

static int prof_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("RECOMP_TRACE_PROFILE") ? 1 : 0;
        if (on)
            atexit(prof_report);
    }
    return on;
}

static void prof_count(const char *name, uint32_t va)
{
    unsigned i = (va * 2654435761u) & (PROF_SLOTS - 1);
    unsigned n;

    /* A title being profiled for a hang or a slowdown is a title that gets
     * killed rather than exited, and a kill does not reach atexit. Report as
     * it goes, so there is always a recent one. */
    if (++g_prof_calls % prof_interval() == 0)
        prof_report();

    for (n = 0; n < PROF_SLOTS; n++) {
        unsigned k = (i + n) & (PROF_SLOTS - 1);
        if (g_prof[k].va == va && g_prof[k].hits) { g_prof[k].hits++; return; }
        if (!g_prof[k].hits) {
            g_prof[k].va = va;
            g_prof[k].hits = 1;
            g_prof_name[k] = name;
            g_prof_used++;
            return;
        }
    }
    g_prof_full = 1;
}

/* RECOMP_TRACE_AFTER_VA=0x...: stay silent until that function is entered,
 * so the budget is spent on the interesting window rather than on boot. */
static int trace_armed(uint32_t va)
{
    static int state = -1;           /* -1 unread, 0 waiting, 1 armed */
    static uint32_t trigger;
    if (state < 0) {
        const char *env = getenv("RECOMP_TRACE_AFTER_VA");
        trigger = env ? (uint32_t)strtoul(env, NULL, 0) : 0;
        state = trigger ? 0 : 1;
    }
    if (state == 0 && va == trigger) state = 1;
    return state == 1;
}

/* Optional per-entry hook a title's bring-up code can supply (weak). */
void recomp_trace_user_hook(const char *name, uint32_t va) __attribute__((weak));
void recomp_trace_user_hook(const char *name, uint32_t va) { (void)name; (void)va; }

/*
 * The switch the generated code's hooks are behind, and the time-slice check
 * they used to carry.
 *
 * g_recomp_hooks stays zero unless something actually wants a callback on
 * every function entry: a trace, the entry profile, or a project's own hook
 * (which enables it through recomp_trace_enable_hooks). Nothing else pays for
 * them being there.
 *
 * recomp_slice_check is what remains of the old yield. The counter that
 * decides when to call it moved into the macro, so a thread that is not due a
 * slice boundary never leaves its own translation unit.
 */
unsigned g_recomp_tick;
int      g_recomp_hooks;

void recomp_slice_check(void)
{
    extern void xbox_guest_lock_yield_now(void);
    xbox_guest_lock_yield_now();
}

void recomp_trace_enable_hooks(void)
{
    g_recomp_hooks = 1;
}

/* ── the x87 stack, checked the way the ABI is ────────────────────────────
 *
 * The hottest guest functions in this title are full of fild/fadd/fstp, and
 * the lifter turns each of those into an index into an eight-entry rotating
 * array of doubles. A push that is never popped does not fault: it moves the
 * top by one, and from then on every register in that function -- and in
 * every function it calls -- is read one slot away from where it was
 * written. Numbers come out plausible and wrong, which is what a vehicle in
 * the wrong place looks like, and nothing anywhere reports it.
 *
 * The same trick that found the callee-saved register bug works here. Record
 * the depth on entry, compare it on exit, rank the offenders. A function that
 * leaves the stack deeper or shallower than it found it has been lifted
 * wrongly, whatever the picture looks like.
 *
 * Robust against the exits that never run: a tail jump leaves a frame behind,
 * so the exit is matched by address rather than assumed to be the top of the
 * shadow stack, and a stack that cannot be matched is abandoned rather than
 * reported. A diagnostic that cries wolf is worse than none -- three of them
 * were built last night and two were wrong.
 */
extern RECOMP_TLS int g_fp_top;
int g_recomp_fpcheck;

#define FPS_MAX 512
static RECOMP_TLS struct { uint32_t va; int top; } t_fps[FPS_MAX];
static RECOMP_TLS int t_fps_n;

/* Reading the deltas -- and which way is "deeper".
 *
 * The lifter models the x87 stack as an eight-entry array with a rotating
 * top, and a push DECREMENTS that top: fp_push does top = (top + 7) & 7.
 * So a function that ends with a smaller g_fp_top than it started with has
 * left a value on the stack, and the raw difference (now - was) for a push
 * is minus one, not plus one. The first version of this report had that
 * backwards and duly called four perfectly correct float-returning
 * functions bugs. Everything below is in DEPTH: positive means deeper,
 * which is what the words in the report say.
 *
 * A non-zero depth change is not by itself a bug, and a version of this
 * that said so would have been the fourth cries-wolf diagnostic on this
 * project. x86 returns a float in st(0), which the callee pushes and the
 * caller pops: every float-returning function in the title legitimately
 * ends exactly one deeper. Those are summarised away.
 *
 * What is actually worth reading:
 *   one shallower      the callee popped a register it never pushed, so it
 *                      has eaten one of the caller's -- every read the
 *                      caller makes afterwards is off by one. Legitimate
 *                      for an _ftol-style helper that takes its argument
 *                      in st(0), so check the disassembly before believing
 *                      it; a bug anywhere else.
 *   two or more either way
 *                      registers leaked or eaten in bulk. The stack is
 *                      eight deep and wraps silently, so four of these in
 *                      a chain and the whole file is garbage.
 *   two different depths for one function
 *                      its paths disagree. A compiler does not emit that,
 *                      so one of them was lifted wrongly. Strongest signal
 *                      of the three; read it first.
 *
 * The coverage line matters as much as the findings. Exits are matched by
 * address because a tail jump leaves a frame behind, and a frame that
 * cannot be matched within sixty-four makes the whole shadow stack
 * untrustworthy, so it is abandoned. Abandoning loses any violation that
 * was in flight -- so the report says how often that happened. A clean
 * report over poor coverage means nothing, and there is no way to tell
 * the two apart without printing the number.
 */
enum { FPV_SLOTS = 64 };
static uint32_t g_fpv_seen[FPV_SLOTS];
static uint64_t g_fpv_hits[FPV_SLOTS][8];   /* by (now - was) & 7 */
static int      g_fpv_count;
static uint64_t g_fpv_events;
static uint64_t g_fpv_lost;                 /* violations past the last slot */
static uint64_t g_fpv_calls;                /* entries seen */
static uint64_t g_fpv_resets;               /* shadow stacks abandoned */

/* bucket index -> depth change, positive = deeper. A push lands in 7. */
#define FPV_DEPTH(i) (-((int)(((i) + 4) & 7) - 4))

static uint64_t fpv_total(int s)
{
    int b; uint64_t n = 0;
    for (b = 0; b < 8; b++) n += g_fpv_hits[s][b];
    return n;
}

/* Everything except "always exactly one deeper", which is a float return. */
static uint64_t fpv_suspicious(int s)
{
    int b, buckets = 0; uint64_t n = 0;
    for (b = 0; b < 8; b++) if (g_fpv_hits[s][b]) buckets++;
    for (b = 0; b < 8; b++) {
        if (!g_fpv_hits[s][b]) continue;
        if (FPV_DEPTH(b) == 1 && buckets == 1) continue;  /* plain float return */
        n += g_fpv_hits[s][b];
    }
    return n;
}

static void recomp_fp_summary(void)
{
    int i, j, b, shown = 0;
    uint64_t benign_fns = 0, benign_hits = 0;
    char done[FPV_SLOTS];
    memset(done, 0, sizeof(done));

    fprintf(stderr, "[FPU] %llu returns out of %llu calls changed the x87 "
                    "stack depth, over %d function(s)%s\n",
            (unsigned long long)g_fpv_events, (unsigned long long)g_fpv_calls,
            g_fpv_count, g_fpv_lost ? " (table full, some untracked)" : "");
    fprintf(stderr, "[FPU]   coverage: %llu shadow stack(s) abandoned "
                    "(%s)\n", (unsigned long long)g_fpv_resets,
            g_fpv_resets * 100 < g_fpv_calls
                ? "rare, so a clean result below means something"
                : "OFTEN -- a clean result below means little");
    if (!g_fpv_count) { fprintf(stderr, "[FPU]   nothing at all.\n");
                        fflush(stderr); return; }

    for (i = 0; i < g_fpv_count; i++)
        if (!fpv_suspicious(i)) { benign_fns++; benign_hits += fpv_total(i); }
    if (benign_fns)
        fprintf(stderr, "[FPU]   %llu of them are plain float returns from "
                        "%llu function(s) -- expected, not shown\n",
                (unsigned long long)benign_hits,
                (unsigned long long)benign_fns);

    for (j = 0; j < g_fpv_count && shown < 12; j++) {
        int best = -1, buckets = 0;
        uint64_t bestn = 0, n;
        for (i = 0; i < g_fpv_count; i++) {
            if (done[i]) continue;
            n = fpv_suspicious(i);
            if (n > bestn) { bestn = n; best = i; }
        }
        if (best < 0) break;
        done[best] = 1; shown++;
        fprintf(stderr, "[FPU]   sub_%08X ", g_fpv_seen[best]);
        for (b = 0; b < 8; b++) {
            if (!g_fpv_hits[best][b]) continue;
            buckets++;
            fprintf(stderr, " %+d deeper x%llu", FPV_DEPTH(b),
                    (unsigned long long)g_fpv_hits[best][b]);
        }
        fprintf(stderr, "%s\n", buckets > 1 ? "   <-- its paths disagree" : "");
    }
    if (!shown) fprintf(stderr, "[FPU]   nothing suspicious.\n");
    fflush(stderr);
}

/* Called from the USB tick, which is the only loop in the process that runs
 * steadily and wants no lock. The run is normally killed rather than exited,
 * so a summary that only ran at exit would never be seen. */
void recomp_fp_report(void)
{
    static unsigned ticks;
    if (!g_recomp_fpcheck) return;
    if (++ticks % 500u) return;        /* the tick is 20 ms, so every 10 s */
    recomp_fp_summary();
}

static void recomp_fp_violation(uint32_t va, int was, int now)
{
    int i;
    g_fpv_events++;
    for (i = 0; i < g_fpv_count; i++)
        if (g_fpv_seen[i] == va) break;
    if (i == g_fpv_count) {
        if (g_fpv_count == FPV_SLOTS) { g_fpv_lost++; return; }
        g_fpv_seen[i] = va;
        memset(g_fpv_hits[i], 0, sizeof(g_fpv_hits[i]));
        g_fpv_count++;
    }
    g_fpv_hits[i][(unsigned)(now - was) & 7u]++;
}

void recomp_fp_enter(uint32_t va)
{
    g_fpv_calls++;
    if (t_fps_n >= 0 && t_fps_n < FPS_MAX) {
        t_fps[t_fps_n].va = va;
        t_fps[t_fps_n].top = g_fp_top;
    }
    t_fps_n++;
}

void recomp_fp_exit(uint32_t va)
{
    int i, floor_i;
    if (t_fps_n <= 0) { t_fps_n = 0; return; }
    if (t_fps_n > FPS_MAX) { t_fps_n--; return; }
    floor_i = t_fps_n - 64; if (floor_i < 0) floor_i = 0;
    for (i = t_fps_n - 1; i >= floor_i; i--)
        if (t_fps[i].va == va) break;
    if (i < floor_i) { g_fpv_resets++; t_fps_n = 0; return; }  /* lost it */
    if (g_fp_top != t_fps[i].top)
        recomp_fp_violation(va, t_fps[i].top, g_fp_top);
    t_fps_n = i;
}

/* Decided once, before main, so that the first guest function already sees
 * the right answer -- a lazy check would be the very cost being removed. */
__attribute__((constructor))
static void recomp_trace_switches(void)
{
    if (getenv("RECOMP_TRACE_PROFILE") || getenv("RECOMP_TRACE_AFTER_VA")
     || getenv("RECOMP_TRACE_ARGS")    || getenv("RECOMP_TRACE_DEREF")
     || getenv("RECOMP_TRACE_FUNCS"))
        g_recomp_hooks = 1;
    if (getenv("RECOMP_FP_CHECK")) {
        g_recomp_fpcheck = 1;
        atexit(recomp_fp_summary);
    }
}

void recomp_trace_enter(const char *name, uint32_t va)
{
    recomp_trace_user_hook(name, va);
    if (prof_enabled()) { prof_count(name, va); return; }
    if (!trace_armed(va)) return;
    if (!trace_budget()) return;
    /* The return address as well as the registers: at entry it is still at
     * [esp], and it names the call site, which is the thing a trace of "who
     * reached this" actually needs. Reading a guest stack dump for it works
     * only when the frames above are still live. */
    fprintf(stderr, "[TRACE] -> %s (0x%08X)  from=%08X esp=%08X eax=%08X "
            "ecx=%08X esi=%08X edi=%08X ebx=%08X\n",
            name, va,
            *(const uint32_t *)((uintptr_t)g_esp + xbox_GetMemoryOffset()),
            g_esp, g_eax, g_ecx, g_esi, g_edi, g_ebx);

    /* The stack arguments too, when asked. Registers alone do not say which
     * argument arrived null, and for a function with a long argument list,
     * counting pushes back from the call site is guesswork. */
    if (getenv("RECOMP_TRACE_ARGS")) {
        const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
        int n = atoi(getenv("RECOMP_TRACE_ARGS"));
        int i;

        if (n <= 0 || n > 32)
            n = 8;
        fprintf(stderr, "         args:");
        for (i = 1; i <= n; i++)
            fprintf(stderr, " %d=%08X", i,
                    *(const uint32_t *)(mem + g_esp + i * 4));
        fprintf(stderr, "\n");
        /* And the object eax points at. A matrix of NaNs says the maths went
         * wrong; whether its inputs were already zero says whether the maths
         * is at fault or the data behind it was never built. */
        /* Follow the pointer arguments one level. A matrix that arrives as
         * NaN was copied from somewhere, and the object it came from is what
         * needs looking at -- the value alone says only that it is wrong. */
        if (getenv("RECOMP_TRACE_DEREF")) {
            for (i = 1; i <= n; i++) {
                uint32_t a = *(const uint32_t *)(mem + g_esp + i * 4);
                int k;
                if (a < 0x00010000u || a >= 0x04000000u)
                    continue;
                fprintf(stderr, "         arg%d -> [%08X]:", i, a);
                for (k = 0; k < 12; k++)
                    fprintf(stderr, " %08X",
                            *(const uint32_t *)(mem + a + k * 4));
                fprintf(stderr, "\n");
            }
        }
        if (g_eax > 0x00010000u && g_eax < 0x04000000u) {
            fprintf(stderr, "         [eax=%08X]:", g_eax);
            for (i = 0; i < 24; i++)
                fprintf(stderr, " %08X",
                        *(const uint32_t *)(mem + g_eax + i * 4));
            fprintf(stderr, "\n");
        }
    }
    fflush(stderr);
}

/* Entry values answer "what was it called with"; only exit values answer "what
 * did the caller get back", which is the question when a callee-saved register
 * comes back wrong. */
void recomp_trace_exit(const char *name, uint32_t va)
{
    if (prof_enabled()) return;     /* entries alone carry the count */
    if (!trace_armed(0)) return;
    if (!trace_budget()) return;
    fprintf(stderr, "[TRACE] <- %s (0x%08X)  esp=%08X eax=%08X ecx=%08X "
            "esi=%08X edi=%08X ebx=%08X\n",
            name, va, g_esp, g_eax, g_ecx, g_esi, g_edi, g_ebx);
    fflush(stderr);
}

/* esp at a specific point inside a traced function. The epilogue's
 * `mov esp, ebp` hides drift from any return-time sample, so a leak of a few
 * bytes per call is only visible from a sample taken before it. */
void recomp_trace_esp(const char *name, const char *tag)
{
    if (prof_enabled()) return;
    if (!trace_armed(0)) return;
    if (!trace_budget()) return;
    fprintf(stderr, "[ESP] %s @%s  esp=%08X esi=%08X edi=%08X\n",
            name, tag, g_esp, g_esi, g_edi);
    fflush(stderr);
}

/* ---------------------------------------------------------------------------
 * Guest debug output (INT 2D / DebugService).
 *
 * The Xbox kernel debug trap. eax selects the service and ecx carries its
 * argument; service 1 is "print this ANSI_STRING", which is what
 * OutputDebugStringA and the XDK's DbgPrint compile down to. On hardware the
 * kernel consumes the trap and resumes at the int3 that follows, skipping it.
 *
 * Printing it is the whole point: this is the title telling us what it thinks
 * is happening, and during bring-up that is the most valuable output there is.
 * ------------------------------------------------------------------------- */

/* Defined in xbox_memory_layout.c; declared extern per consumer, as
 * kernel_bridge.c and nv2a_pb_replay.c already do. */
extern ptrdiff_t g_xbox_mem_offset;

void recomp_debug_service(uint32_t service, uint32_t arg_va)
{
    const uint8_t *mem = (const uint8_t *)g_xbox_mem_offset;
    uint16_t length;
    uint32_t buffer_va;

    if (service != 1) {
        fprintf(stderr, "[GUEST] DebugService %u (arg 0x%08X), ignored\n",
                (unsigned)service, arg_va);
        fflush(stderr);
        return;
    }

    /* ANSI_STRING { USHORT Length; USHORT MaximumLength; PCHAR Buffer; } */
    if (!arg_va)
        return;
    length    = *(const uint16_t *)(mem + arg_va);
    buffer_va = *(const uint32_t *)(mem + arg_va + 4);
    if (!buffer_va || !length)
        return;

    fprintf(stderr, "[GUEST] %.*s", (int)length, (const char *)(mem + buffer_va));
    if (length && ((const char *)(mem + buffer_va))[length - 1] != '\n')
        fputc('\n', stderr);
    fflush(stderr);
}
