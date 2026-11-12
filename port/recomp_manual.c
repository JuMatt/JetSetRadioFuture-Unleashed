#include <time.h>
/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
static double jsrf_now(void);   /* defined further down */
static uint32_t jct_begin_call(void);
static void jct_sample(unsigned kind, unsigned phase, uint32_t self, uint32_t frame, uint32_t call);
static void jct_note_pad(int16_t lx, int16_t ly, int16_t rx, int16_t ry, uint8_t lt, uint8_t lb);
static void jct_delivered_pad(int16_t lx, int16_t ly, int16_t rx, int16_t ry, uint8_t lt, uint8_t lb);
static void jct_pad_failed(void);
static void jct_request_rail_capture(void);
static void jcx_pad_failed(void);
static void jcx_forget(void);

#if defined(_WIN32)
/* No pwrite on Windows: a seek and a write do the same for the one writer here. */
#include <io.h>
static long long jsrf_pwrite(int fd, const void *buf, size_t n, long long off)
{
    if (_lseeki64(fd, off, SEEK_SET) < 0) return -1;
    return _write(fd, buf, (unsigned)n);
}
#define pwrite jsrf_pwrite
#endif

/* The Win32 shim's thread id, so traces here and the kernel's [WAITLOG]
 * name the same thread. (On Windows it is the real one, from <windows.h>.) */
#if !defined(_WIN32)
extern uint32_t GetCurrentThreadId(void);
#else
#include "platform/xbox_winnt.h"     /* <windows.h>, as the runtime includes it */
#endif

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

#include "recomp/gen/recomp_types.h"
extern ptrdiff_t g_xbox_mem_offset;

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
int g_icall_verbose;
/* In-memory ring of indirect calls: what was called, from where, and what it
 * did to esp. Dumped by the crash handler; never printed live (printing from
 * inside guest code perturbs the run). */
#define ICALLV_RING 256
static struct { uint32_t va, esp0, esp1, eax, ra; } g_icallv[ICALLV_RING];
static unsigned g_icallv_n;
void recomp_icallv_record(uint32_t va, uint32_t esp0, uint32_t esp1, uint32_t eax, uint32_t ra)
{
    unsigned i = g_icallv_n++ & (ICALLV_RING - 1);
    g_icallv[i].va = va; g_icallv[i].esp0 = esp0; g_icallv[i].esp1 = esp1; g_icallv[i].eax = eax; g_icallv[i].ra = ra;
}
void recomp_icallv_dump(void)
{
    unsigned n = g_icallv_n < ICALLV_RING ? g_icallv_n : ICALLV_RING;
    fprintf(stderr, "[ICALLV] last %u indirect calls (oldest first):\n", n);
    for (unsigned k = 0; k < n; k++) {
        unsigned i = (g_icallv_n - n + k) & (ICALLV_RING - 1);
        fprintf(stderr, "   %08X from %08X esp %08X->%08X (%+d) eax=%08X\n",
                g_icallv[i].va, g_icallv[i].ra, g_icallv[i].esp0, g_icallv[i].esp1,
                (int)(g_icallv[i].esp1 - g_icallv[i].esp0), g_icallv[i].eax);
    }
}
/* ── Functions the recompiler never found ──────────────────────
 *
 * The release builds' generated code was made by an earlier discovery pass
 * and carries two tables of functions that pass missed, lifted one at a time
 * (recomp_seeded.c, recomp_holes.c). Code generated with today's tools/recomp
 * (README, "Building from source") finds them in the ordinary pass and has
 * neither file; the build then links empty ones (recomp_tables_none.c) and
 * these lookups find nothing.
 *
 * recomp/gen/recomp_seeded.c holds 40 functions that live in .text, are
 * referenced from vtables and state-handler tables in the data sections,
 * and were not lifted -- 0x0007DBD0 (the title menu's per-frame handler,
 * whose absence made Start do nothing), 0x00096F80 (hit next, 720 times,
 * once the menu worked), 0x000BE190 (a deleting destructor), and 37 more
 * found by the same scan. They are dispatched from here because the
 * ICALL path asks recomp_lookup_manual() before the generated table.
 * JSRF_NO_MANUAL_LIFT=1 switches them off for an A/B against the old
 * behaviour (the calls silently skipped). */
typedef void (*recomp_seeded_func_t)(void);
extern const struct { uint32_t va; recomp_seeded_func_t fn; } g_recomp_seeded[];
extern const unsigned g_recomp_seeded_count;

/* recomp_holes.c: functions reached by direct calls and tail jumps that
 * the discovery pass missed (each was a no-op stub before). Direct calls
 * link straight to them; this table is for an indirect call landing on one. */
typedef void (*recomp_holes_func_t)(void);
extern const struct { uint32_t va; recomp_holes_func_t fn; } g_recomp_holes[];
extern const unsigned g_recomp_holes_count;

static recomp_func_t holes_lookup(uint32_t va)
{
    unsigned lo = 0, hi = g_recomp_holes_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (g_recomp_holes[mid].va == va) return (recomp_func_t)g_recomp_holes[mid].fn;
        if (g_recomp_holes[mid].va < va) lo = mid + 1; else hi = mid;
    }
    return (recomp_func_t)0;
}

static recomp_func_t seeded_lookup(uint32_t va)
{
    unsigned lo = 0, hi = g_recomp_seeded_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (g_recomp_seeded[mid].va == va) return (recomp_func_t)g_recomp_seeded[mid].fn;
        if (g_recomp_seeded[mid].va < va) lo = mid + 1; else hi = mid;
    }
    return (recomp_func_t)0;
}

/* JSRF_NO_SEEDED=<va>[,<va>...] (test only): leave those seeded functions
 * out, as a build from before they were lifted would -- to reproduce what a
 * player on an older version saw. */
static int seeded_left_out(uint32_t va)
{
    static int n = -1;
    static uint32_t out[16];
    if (n < 0) {
        const char *s = getenv("JSRF_NO_SEEDED");
        char *e;
        n = 0;
        while (s && *s && n < 16) {
            uint32_t v = (uint32_t)strtoul(s, &e, 0);
            if (e == s) break;
            out[n++] = v;
            s = *e == ',' ? e + 1 : e;
        }
        if (n) fprintf(stderr, "[MANUAL] JSRF_NO_SEEDED: %d seeded function(s) left out\n", n);
    }
    for (int i = 0; i < n; i++)
        if (out[i] == va) return 1;
    return 0;
}

/* ── A DirectSound spin, made cooperative ──────────────────────
 *
 * 0x001A308E, in the XDK's DirectSound library:
 *
 *     test byte  [ecx+0x12], 1        ; voice has a stop pending?
 *     je   done
 *   w: test word  [ecx+0x12], 0x8000  ; ...wait for the ISR to retire it
 *     jne  w
 *   done: ret
 *
 * On the console the APU interrupt preempts this loop and clears the bit.
 * Here the loop holds the guest lock, makes no calls (so the entry-hook
 * hand-off never fires), and the ISR runs on the timer thread, which needs
 * that lock: a livelock, seen as "held the guest lock for 143982 ms" with
 * one thread at 98.8% CPU. Same test, same bit, but the lock is dropped
 * for a moment between polls so the interrupt can land. The interrupt
 * itself is delivered by kernel_apu_tick() in the runtime. */
extern void xbox_guest_lock_acquire(void);
extern void xbox_guest_lock_release(void);
int g_manual_trace;

static void manual_sub_001A308E(void)
{
    uint32_t obj = g_ecx;
    if (MEM8(obj + 0x12) & 1) {
        unsigned polls = 0;
        while (MEM16(obj + 0x12) & 0x8000) {
            xbox_guest_lock_release();
            usleep(100);
            xbox_guest_lock_acquire();
            if (++polls == 5000) {           /* half a second: say so, a few times */
                static int said;
                if (said++ < 3)
                    fprintf(stderr, "[MANUAL] sub_001A308E: voice %08X still "
                            "has its stop pending after 500 ms (flags %04X)\n",
                            obj, MEM16(obj + 0x12));
            }
        }
        if (g_manual_trace && polls) {
            static unsigned n;
            if (n++ < 5)
                fprintf(stderr, "[MANUAL] sub_001A308E: waited %u polls for "
                        "voice %08X\n", polls, obj);
        }
    }
    g_esp += 4; return;                      /* ret */
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    {
        static int manual_on = -1;
        if (manual_on < 0) {
            manual_on = (getenv("JSRF_NO_MANUAL_LIFT") == NULL);
            g_manual_trace = (getenv("JSRF_MANUAL_TRACE") != NULL);
            fprintf(stderr, "[MANUAL] hand-lifted overrides %s: %u seeded "
                    "functions + the DirectSound stop-pending wait; %u hole "
                    "functions (always on)\n",
                    manual_on ? "ON" : "OFF", g_recomp_seeded_count, g_recomp_holes_count);
            fflush(stderr);
        }
        if (manual_on) {
            recomp_func_t f;
            if (xbox_va == 0x001A308Eu) return manual_sub_001A308E;
            if (seeded_left_out(xbox_va)) return (recomp_func_t)0;
            f = seeded_lookup(xbox_va);
            if (f) return f;
        }
        {
            recomp_func_t f = holes_lookup(xbox_va);
            if (f) return f;
        }
    }

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 *
 * Said in full the first 4 times for an address, then once every 1000: a
 * missing callback runs every frame, and 1590 copies of the same 18 lines
 * made a player's log 1.4 MB without saying anything the first copy had not.
 */
void recomp_icall_fail_log(uint32_t va)
{
    enum { SLOTS = 64 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int nseen;
    uint64_t n = 0;
    int k;
    for (k = 0; k < nseen && seen[k] != va; k++)
        ;
    if (k == nseen && nseen < SLOTS) { seen[k] = va; hits[k] = 0; nseen++; }
    if (k < nseen) n = ++hits[k];
    if (n > 4 && n % 1000) return;

    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);
    if (n > 4) {
        fprintf(stderr, "  (%llu times for this address)\n", (unsigned long long)n);
        fflush(stderr);
        return;
    }

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Where from: the return address the lifted call pushed is at [esp]. */
    {
        static uint32_t sites[64]; static int nsites;
        uint32_t ra = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + g_esp);
        int k; for (k = 0; k < nsites; k++) if (sites[k] == ra) break;
        if (k == nsites && nsites < 64) {
            sites[nsites++] = ra;
            fprintf(stderr, "[ICALL] not-code target 0x%08X called from site 0x%08X\n", va, ra);
        }
    }
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);

    /*
     * The first one, in full.
     *
     * This line turned out to be the precursor to the crash that kills a run
     * now and then: a scene-graph walk calls a virtual on every node it
     * visits, one node's vtable slot reads as zero, the call is skipped, and
     * a few instructions later the walk dereferences a sibling pointer of
     * 0x8B28246C and dies. Waiting for the fault to report it is the wrong
     * end of the problem -- by then the object is three frames of registers
     * away. Here, the object is still in ecx and its vtable is still in eax.
     *
     * Once per address, and only the first time, so a wild pointer inside a
     * loop cannot turn this into the log.
     */
    if (hits[i] == 1) {
        uint32_t obj = g_ecx, vtbl = g_eax;
        fprintf(stderr, "  [ICALL] this=0x%08X vtable=0x%08X "
                "esi=0x%08X edi=0x%08X esp=0x%08X\n",
                obj, vtbl, g_esi, g_edi, g_esp);
        if (g_xbox_mem_offset && obj >= 0x10000u && obj < (64u << 20)) {
            const uint32_t *o = (const uint32_t *)
                                ((uintptr_t)g_xbox_mem_offset + obj);
            int k;
            fprintf(stderr, "  [ICALL] the object it was called on:\n");
            for (k = 0; k < 16; k += 4)
                fprintf(stderr, "    +%02X: %08X %08X %08X %08X\n",
                        k * 4, o[k], o[k+1], o[k+2], o[k+3]);
        }
        if (g_xbox_mem_offset && g_esp) {
            const uint32_t *sp = (const uint32_t *)
                                 ((uintptr_t)g_xbox_mem_offset + g_esp);
            int shown = 0, k;
            fprintf(stderr, "  [ICALL] guest stack (return addresses):\n");
            for (k = 0; k < 256 && shown < 16; k++) {
                uint32_t v = sp[k];
                if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                    fprintf(stderr, "    [esp+%-4d] 0x%08X\n", k * 4, v);
                    shown++;
                }
            }
        }
    }
    fflush(stderr);
}


/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * Code generated with today's tools/recomp has RECOMP_UNIMPL(text, va) at
 * every instruction the lifter has no translation for, in place of the bare
 * comment it used to leave. The instruction is still a no-op; this only stops
 * the omission being silent. RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at
 * the guest address of the cause rather than wherever the damage surfaces.
 * From spriteice's fork (f31560d). */
void recomp_unimpl(const char *text, uint32_t va);
void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/* ---- Heap tracing wrappers (bring-up) ---------------------------------- */
#define GARG(n) (*(uint32_t *)((uint8_t *)g_xbox_mem_offset + g_esp + 4 * (n)))
static int g_heap_trace = -1;
static uint32_t g_last_freed;
static int heap_trace_on(void) { if (g_heap_trace < 0) g_heap_trace = getenv("JSRF_HEAP_TRACE") ? 1 : 0; return g_heap_trace; }
extern void sub_00149407_gen(void);
extern void sub_001497DC_gen(void);
extern void sub_00149F5E_gen(void);
extern void sub_0014A12E_gen(void);
void sub_00149407(void) { /* RtlCreateHeap */
    uint32_t f=GARG(1), base=GARG(2), res=GARG(3), com=GARG(4);
    sub_00149407_gen();
    if (heap_trace_on()) fprintf(stderr, "[HEAPX] RtlCreateHeap(flags=%X base=%08X reserve=%X commit=%X) = %08X\n", f, base, res, com, g_eax);
}
void sub_001497DC(void) { /* RtlAllocateHeap(heap, flags, size) */
    uint32_t h=GARG(1), f=GARG(2), sz=GARG(3), ra=GARG(0);
    sub_001497DC_gen();
    if (g_eax == g_last_freed) g_last_freed = 0;
    if (heap_trace_on()) fprintf(stderr, "[HEAPX]   (alloc from %08X)\n", ra);
    if (heap_trace_on()) fprintf(stderr, "[HEAPX] alloc(h=%08X f=%X size=%X) = %08X\n", h, f, sz, g_eax);
}
void sub_00149F5E(void) { /* RtlFreeHeap(heap, flags, ptr) */
    uint32_t h=GARG(1), f=GARG(2), p=GARG(3);
    if (heap_trace_on()) fprintf(stderr, "[HEAPX] free(h=%08X f=%X ptr=%08X) from %08X\n", h, f, p, GARG(0));
    if (p && p == g_last_freed) {
        const uint32_t *sp = (const uint32_t *)((uint8_t *)g_xbox_mem_offset + g_esp);
        fprintf(stderr, "[HEAPX] DOUBLE FREE of %08X -- guest stack code refs:\n", p);
        for (int i = 0; i < 300; i++)
            if (sp[i] > 0x11000 && sp[i] < 0x18CB30) fprintf(stderr, "    [esp+%03X] %08X\n", i*4, sp[i]);
        fflush(stderr); _exit(3);
    }
    g_last_freed = p;
    sub_00149F5E_gen();
}
void sub_0014A12E(void) { /* RtlReAllocateHeap(heap, flags, ptr, size) */
    uint32_t h=GARG(1), f=GARG(2), p=GARG(3), sz=GARG(4);
    sub_0014A12E_gen();
    if (heap_trace_on()) fprintf(stderr, "[HEAPX] realloc(h=%08X f=%X ptr=%08X size=%X) = %08X\n", h, f, p, sz, g_eax);
}

/* ---- CRT memcpy (sub_0017CEC0) ------------------------------------------
 * MSVC's memcpy.asm dispatches its tail copies through jump tables indexed
 * from base+4 and, on the overlap path, with a negated index -- neither of
 * which the disassembler recovers, so the lifted body loses every tail copy.
 * The semantics are memmove's; implement it natively. cdecl: (dst, src, n),
 * returns dst. */
#include <string.h>
void sub_0017CEC0(void)
{
    uint32_t dst = GARG(1), src = GARG(2), n = GARG(3);
    uint8_t *mem = (uint8_t *)g_xbox_mem_offset;
    if (getenv("JSRF_MEMCPY_WATCH") && n >= 1024 &&
        ((dst < 0x27E074 && dst + n > 0x1EB760) || dst >= 0x04000000)) {
        fprintf(stderr, "[MEMCPY] dst=%08X src=%08X n=%u from %08X -- guest stack:\n", dst, src, n, GARG(0));
        const uint32_t *sp = (const uint32_t *)(mem + g_esp);
        for (int i = 0; i < 120; i++)
            if (sp[i] > 0x11000 && sp[i] < 0x1C3F60) fprintf(stderr, "    [esp+%03X] %08X\n", i*4, sp[i]);
        fflush(stderr);
    }
    if (n) memmove(mem + dst, mem + src, n);
    g_eax = dst;
    g_esp += 4;   /* ret: pop the return address */
}

/* ---- sub_0013B180: JSRF's spin thread --------------------------------------
 * `while (!g_exit) g_counter++;` at raised priority: it never blocks and calls
 * nothing, so under the guest scheduler lock it would hold the CPU forever.
 * It touches nothing shared, so run it outside the lock. */
extern void sub_0013B180_gen(void);
void sub_0013B180(void)
{
    extern void xbox_guest_lock_mark_idle(void);
    xbox_guest_lock_mark_idle();
    /* The loop itself, natively, with a sleep: the guest spins
     * `while (!exit) counter++` at raised priority, which on a two-core host
     * starved every other thread. Nothing reads the counter's rate. */
    {
        uint8_t *m = (uint8_t *)g_xbox_mem_offset;
        volatile uint32_t *exit_flag = (volatile uint32_t *)(m + 0x25EFC0);
        volatile uint32_t *counter = (volatile uint32_t *)(m + 0x25EFA8);
        while (!*exit_flag) {
            (*counter)++;
            struct timespec ts = { 0, 2000000L };
            nanosleep(&ts, NULL);
        }
    }
    sub_0013B180_gen();   /* sees the flag set: goes straight to ExitThread */
}

/* ---- sub_00012770: the "fatal error" handler (writes JSRF_FATAL.ERR) ------
 * Bring-up diagnostic: dump the guest call chain that reached it. */
extern void sub_00012770_gen(void);
void sub_00012770(void)
{
    const uint32_t *sp = (const uint32_t *)((uint8_t *)g_xbox_mem_offset + g_esp);
    fprintf(stderr, "[FATALX] sub_00012770 reached, ecx=%08X -- guest stack code refs:\n", g_ecx);
    for (int i = 0; i < 400; i++)
        if (sp[i] > 0x11000 && sp[i] < 0x1C3F60) fprintf(stderr, "    [esp+%03X] %08X\n", i*4, sp[i]);
    fflush(stderr);
    sub_00012770_gen();
}

/* ---- sub_00143240: CRI ADX error reporter (ADXERR) -- print what it says */
extern void sub_00143240_gen(void);
void sub_00143240(void)
{
    const char *fmt = (const char *)((uint8_t *)g_xbox_mem_offset + GARG(1));
    fprintf(stderr, "[ADXERR] fmt=\"%s\"\n", fmt);
    sub_00143240_gen();
    fprintf(stderr, "[ADXERR] msg=\"%s\"\n", (const char *)((uint8_t *)g_xbox_mem_offset + 0x26a280));
    fflush(stderr);
}

/* ---- sub_0006F730: raise the "disc error" screen; eax = ADXT error code */
extern void sub_0006F730_gen(void);
void sub_0006F730(void)
{
    fprintf(stderr, "[ADXTRAP] sub_0006F730: ADXT error code %d (eax=%08X), raised from %08X (255AD: save writer, 664C3: sub_000257B0 state, 116EA8: ADX handle status)\n",
            (int)(int16_t)g_eax, g_eax, MEM32(g_esp));
    fflush(stderr);
    sub_0006F730_gen();
}

/* ---- sub_001403B0: JSRF cvFs device ReqRd(handle, nsct, buf) diagnostic */
extern void sub_001403B0_gen(void);
void sub_001403B0(void)
{
    uint32_t h = GARG(1), nsct = GARG(2), buf = GARG(3);
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    if (h)
        fprintf(stderr, "[CVFS] ReqRd h=%08X state=%u nsct=%d buf=%08X sctsz=%u total=%u pos=%u\n",
                h, m[h + 1], (int)nsct, buf, *(uint32_t *)(m + h + 0xc),
                *(uint32_t *)(m + h + 0x14), *(uint32_t *)(m + h + 0x18));
    sub_001403B0_gen();
    if (h) fprintf(stderr, "[CVFS]   -> %d (state now %u)\n", (int)g_eax, m[h + 1]);
}
/* ---- the cvFs read pipeline, for the stalled tutorial music.
 * sub_001407E0(h): issues ReadFileEx for the pending request (+0x140 offset,
 * +0x144 bytes) with completion routine sub_001401B0, which clears the
 * in-flight flag +0x14C; sub_00140BA0(h), the per-handle server, SleepEx(0,
 * TRUE)s while +0x14C is set, then accounts the bytes and sets state 1. */
static void cvfs_line(const char *what, uint32_t h, int extra)
{
    static int lines;
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    if (!h || h + 0x150 >= (64u << 20) || lines >= 3000) return;
    lines++;
    fprintf(stderr, "[CVFS2] t=%.2f tid %lu %s h=%08X state=%u pend148=%u inflight14c=%u off=%u bytes=%u pos=%u total=%u %d\n",
            jsrf_now(), (unsigned long)GetCurrentThreadId(), what, h, m[h + 1],
            *(uint32_t *)(m + h + 0x148), *(uint32_t *)(m + h + 0x14c),
            *(uint32_t *)(m + h + 0x140), *(uint32_t *)(m + h + 0x144),
            *(uint32_t *)(m + h + 0x18), *(uint32_t *)(m + h + 0x14), extra);
}
extern void sub_001407E0_gen(void);
void sub_001407E0(void)
{
    uint32_t h = GARG(1);
    cvfs_line("issue>", h, 0);
    sub_001407E0_gen();
    cvfs_line("issue<", h, (int)g_eax);
}
extern void sub_001401B0_gen(void);
void sub_001401B0(void)
{
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    uint32_t err = GARG(1), bytes = GARG(2), ov = GARG(3);
    uint32_t h = (ov && ov + 0x14 < (64u << 20)) ? *(uint32_t *)(m + ov + 0x10) : 0;
    cvfs_line("done>", h, (int)bytes);
    (void)err;
    sub_001401B0_gen();
    cvfs_line("done<", h, (int)err);
}
extern void sub_00140BA0_gen(void);
void sub_00140BA0(void)
{
    /* Only the interesting calls: a request pending or in flight. */
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    uint32_t h = GARG(1);
    int busy = h && h + 0x150 < (64u << 20) && m[h + 1] == 2;
    static unsigned quiet;
    if (busy && (quiet++ % 64) == 0) cvfs_line("serve>", h, (int)quiet);
    sub_00140BA0_gen();
    if (busy && m[h + 1] != 2) cvfs_line("serve< completed", h, 0);
}

/* ---- sub_001402F0: cvFs device Seek(handle, pos) */
extern void sub_001402F0_gen(void);
void sub_001402F0(void)
{
    uint32_t h = GARG(1), pos = GARG(2);
    fprintf(stderr, "[CVFS] Seek h=%08X pos=%d\n", h, (int)pos);
    sub_001402F0_gen();
}

/* ---- sub_0013D300: ADXT trap/watchdog check -- print the handle's counters */
extern void sub_0013D300_gen(void);
void sub_0013D300(void)
{
    uint32_t h = GARG(1);
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    /* Print a handle's line when any of its fields change (and every 5 s),
     * for the whole run -- the old first-60 cap spent itself on the title
     * and said nothing about the tutorial's stream. On a new stream handle,
     * dump its first words once so the cvFs handle it reads through shows. */
    {
        static struct { uint32_t h, sig[6]; double t; uint32_t stm; } seen[8];
        static int lines;
        if (h && h + 0x80 < (64u << 20) && lines < 2000) {
            uint32_t sig[6];
            int i, slot = -1, changed;
            double now = jsrf_now();
            sig[0] = (uint8_t)m[h + 1]; sig[1] = *(uint16_t *)(m + h + 0x60);
            sig[2] = *(uint16_t *)(m + h + 0x68); sig[3] = *(uint16_t *)(m + h + 0x6a);
            sig[4] = m[h + 0x6d]; sig[5] = *(uint32_t *)(m + h + 0x14);
            for (i = 0; i < 8; i++) if (seen[i].h == h) { slot = i; break; }
            if (slot < 0) for (i = 0; i < 8; i++) if (!seen[i].h) { slot = i; seen[i].h = h; break; }
            if (slot >= 0) {
                changed = memcmp(seen[slot].sig, sig, sizeof sig) != 0;
                if (changed || now - seen[slot].t >= 5.0) {
                    lines++;
                    fprintf(stderr, "[ADXT] t=%.1f h=%08X state=%d thresh[0x38]=%d err[0x60]=%d cnt68=%d cnt6a=%d mode6d=%d stm=%08X%s\n",
                            now, h, (int8_t)m[h + 1], *(int32_t *)(m + h + 0x38), *(int16_t *)(m + h + 0x60),
                            *(int16_t *)(m + h + 0x68), *(int16_t *)(m + h + 0x6a), m[h + 0x6d], sig[5],
                            changed ? "  <- changed" : "");
                    if (sig[5] && sig[5] != seen[slot].stm && sig[5] + 0x40 < (64u << 20)) {
                        const uint32_t *w = (const uint32_t *)(m + sig[5]);
                        fprintf(stderr, "[ADXT]   stm %08X: %08X %08X %08X %08X %08X %08X %08X %08X  %08X %08X %08X %08X %08X %08X %08X %08X\n",
                                sig[5], w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7],
                                w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15]);
                        seen[slot].stm = sig[5];
                    }
                    memcpy(seen[slot].sig, sig, sizeof sig);
                    seen[slot].t = now;
                }
            }
        }
    }
    /*
     * JSRF_ADX_RATE=1: how often the ADXT server runs, against how often it
     * decides to decode.
     *
     * The voice consumes 44,100 samples a second and the title supplies
     * 1,152 to 4,608 of them, so the music plays as correct fragments in the
     * wrong order. Two quite different faults produce that. If this watchdog
     * runs 38 times a second and decodes 3, the library believes the buffer
     * is full and the play position it reads is wrong. If it runs 3 times a
     * second, nothing is wrong with the library and its thread is starved.
     */
    { static int on = -1; static int runs; static uint64_t last_ms;
      if (on < 0) on = getenv("JSRF_ADX_RATE") ? 1 : 0;
      if (on) {
          struct timespec ts; uint64_t now;
          clock_gettime(CLOCK_MONOTONIC, &ts);
          now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
          runs++;
          if (!last_ms) last_ms = now;
          if (now - last_ms >= 1000) {
              fprintf(stderr, "[ADXSERV] watchdog ran %d times/s on tid %lu\n",
                      runs, (unsigned long)GetCurrentThreadId());
              fflush(stderr); runs = 0; last_ms = now;
          }
      } }
    /* JSRF_ADX_STALL=1: while an ADXT handle sits in state 1 (preparing)
     * past a second, dump its stream (+0x14) and the cvFs handle the stream
     * reads through -- what the stream is waiting for. */
    { static int on = -1, dumps;
      if (on < 0) on = getenv("JSRF_ADX_STALL") ? 1 : 0;
      if (on && h && h + 0x80 < (64u << 20) && dumps < 12) {
          uint8_t *mm = (uint8_t *)g_xbox_mem_offset;
          int8_t st = (int8_t)mm[h + 1];
          int16_t c6a = *(int16_t *)(mm + h + 0x6a);
          uint32_t stm = *(uint32_t *)(mm + h + 0x14);
          if (st == 1 && c6a > 0 && (c6a % 90) == 60 && stm && stm + 0x100 < (64u << 20)) {
              const uint32_t *w = (const uint32_t *)(mm + stm);
              const uint32_t *hw = (const uint32_t *)(mm + h);
              int i;
              dumps++;
              fprintf(stderr, "[ADXSTALL] t=%.1f h=%08X cnt6a=%d\n[ADXSTALL]  adxt:", jsrf_now(), h, c6a);
              for (i = 0; i < 32; i++) fprintf(stderr, " %08X", hw[i]);
              fprintf(stderr, "\n[ADXSTALL]  sj %08X:", stm);
              for (i = 0; i < 16; i++) fprintf(stderr, " %08X", w[i]);
              fprintf(stderr, "\n");
              /* +8 is the ADXSTM (the stream), +4 the decoder, +0xC the
               * sound output (CRI ADX_TALK layout); dump each object. */
              { int f; static const int offs[3] = { 8, 4, 0xC };
                for (f = 0; f < 3; f++) {
                    uint32_t o = hw[offs[f] / 4];
                    if (o && o + 0x80 < (64u << 20)) {
                        const uint32_t *ow = (const uint32_t *)(mm + o);
                        fprintf(stderr, "[ADXSTALL]  +%02X -> %08X:", offs[f], o);
                        for (i = 0; i < 32; i++) fprintf(stderr, " %08X", ow[i]);
                        fprintf(stderr, "\n");
                    }
                } }
              { uint32_t fs = 0x00273780u; const uint8_t *f = mm + fs;
                fprintf(stderr, "[ADXSTALL]  cvfs %08X: inuse %u state %u off %u bytes %u pend %u inflight %u\n",
                        fs, f[0], f[1], *(const uint32_t *)(f + 0x140), *(const uint32_t *)(f + 0x144),
                        *(const uint32_t *)(f + 0x148), *(const uint32_t *)(f + 0x14c)); }
              /* CRI's Xbox file layer formats its last error here (wxCi*). */
              fprintf(stderr, "[ADXSTALL]  wxci last error: '%.160s'\n", (const char *)(mm + 0x00273640u));
              fflush(stderr);
          }
      } }
    sub_0013D300_gen();
}

/* ---- photo mode's freeze: the world's update skipped ------------------------
 * sub_000123E0 is the game's scene tick, once a frame from the main loop
 * (sub_00013A80). In play (none of its pause states at +0x40..+0x4C set) it
 * runs seven systems and then the update walk of the task tree (sub_00011070:
 * each node's vtable slot 1 -- the skaters, the people, the cars, the
 * camera...), then the publish walk (sub_00011D00, slot 2) and the draw.
 * While photo mode is on those eight calls are left out -- found by the
 * return address each pushed (they are called from there only, the walk also
 * from itself). The publish walk moves things too (the skaters' poses, the
 * cans turning are worked out there: a test with the updates left out still
 * had them going), so it is left out as well, all but the camera's publish,
 * which is made by itself (sub_00011D00, below): the scene as it stood, drawn
 * as ever, seen from the photo camera. JSRF_PHOTO_FREEZE=0 (test only) leaves
 * the world running, as in 0.6.4. */
static volatile int s_world_frozen;
static uint32_t s_frozen_cam;   /* the camera, whose publish is still made */
static int photo_freeze_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("JSRF_PHOTO_FREEZE"); on = !(e && e[0] == '0'); }
    return on;
}
static int frozen_skip(uint32_t ra)
{
    if (!s_world_frozen) return 0;
    if (*(const uint32_t *)((const uint8_t *)g_xbox_mem_offset + g_esp) != ra) return 0;
    g_esp += 4;                     /* the ret it would have made */
    return 1;
}
/* Each written out (the build finds the wrapped functions by their extern line). */
extern void sub_0006F8C0_gen(void);
void sub_0006F8C0(void) { if (frozen_skip(0x0001247Cu)) return; sub_0006F8C0_gen(); }
extern void sub_0001DF30_gen(void);
void sub_0001DF30(void) { if (frozen_skip(0x00012486u)) return; sub_0001DF30_gen(); }
extern void sub_00065800_gen(void);
void sub_00065800(void) { if (frozen_skip(0x00012490u)) return; sub_00065800_gen(); }
extern void sub_00066AD0_gen(void);
void sub_00066AD0(void) { if (frozen_skip(0x0001249Au)) return; sub_00066AD0_gen(); }
extern void sub_00024AE0_gen(void);
void sub_00024AE0(void) { if (frozen_skip(0x000124A4u)) return; sub_00024AE0_gen(); }
extern void sub_00066110_gen(void);
void sub_00066110(void) { if (frozen_skip(0x000124AEu)) return; sub_00066110_gen(); }
extern void sub_00015130_gen(void);
void sub_00015130(void) { if (frozen_skip(0x000124B8u)) return; sub_00015130_gen(); }

/* ---- sub_00011070: scene-graph walk -- validate node pointers first, so
 * the holder of a corrupted link is named before the walk dereferences it. */
extern void sub_00011070_gen(void);
static int jsrf_node_ok(uint32_t p) { return p >= 0x10000 && p < 0x04000000 && (p & 3) == 0; }
static void jsrf_check_tree(uint32_t node, int depth, uint32_t holder, const char *field)
{
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    int guard = 0;
    while (node) {
        if (!jsrf_node_ok(node)) {
            fprintf(stderr, "[TREE] bad node %08X held at %08X (%s) depth %d -- guest stack:\n", node, holder, field, depth);
            { const uint32_t *sp = (const uint32_t *)(m + g_esp);
              for (int i = 0; i < 200; i++)
                  if (sp[i] > 0x11000 && sp[i] < 0x1C3F60) fprintf(stderr, "    [esp+%03X] %08X\n", i*4, sp[i]); }
            fflush(stderr);
            abort();
        }
        if (depth < 64 && *(uint32_t *)(m + node + 0x28))
            jsrf_check_tree(*(uint32_t *)(m + node + 0x28), depth + 1, node + 0x28, "child");
        holder = node + 0x30; field = "sibling";
        node = *(uint32_t *)(m + node + 0x30);
        if (++guard > 100000) break;
    }
}
/* JSRF_TASK_DUMP="t1,t2,..." (test only): at each time, one [TASK] line per
 * node of the task tree the frame walks -- depth, address, vtable, its update
 * and draw entries, and the words at +4 (flags: bit 31 set = not updated),
 * +8 and +0xC -- so two runs can be set side by side to see which tasks one
 * of them has switched off. */
static double pad_now(void);
static void jsrf_dump_tree(uint32_t node, int depth)
{
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    int guard = 0;
    while (node && jsrf_node_ok(node) && guard++ < 4000) {
        const uint32_t *w = (const uint32_t *)(m + node);
        uint32_t vt = w[0], upd = 0, drw = 0;
        if (jsrf_node_ok(vt)) { upd = *(uint32_t *)(m + vt + 4); drw = *(uint32_t *)(m + vt + 0xC); }
        fprintf(stderr, "[TASK] %*s%08X vt %08X upd %08X drw %08X f4 %08X f8 %08X fC %08X f10 %08X f14 %08X f18 %08X f1C %08X f20 %08X f24 %08X\n",
                depth * 2, "", node, vt, upd, drw, w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9]);
        if (depth < 32 && w[0x28 / 4]) jsrf_dump_tree(w[0x28 / 4], depth + 1);
        node = w[0x30 / 4];
    }
}
void sub_00011070(void)
{
    static int check = -1, depth;
    static double dump_t[16]; static int ndump = -1, idump;
    if (check < 0) {
        const char *e = getenv("JSRF_TASK_DUMP");
        check = getenv("JSRF_TREE_CHECK") ? 1 : 0;
        ndump = 0;
        while (e && *e && ndump < 16) {
            char *end; double v = strtod(e, &end);
            if (end == e) break;
            dump_t[ndump++] = v;
            e = (*end == ',') ? end + 1 : end;
        }
    }
    if (depth == 0 && frozen_skip(0x000124C3u)) return;   /* photo mode: the world held */
    if (check && g_ecx) jsrf_check_tree(g_ecx, 0, 0, "root");
    if (depth == 0 && idump < ndump && g_ecx && pad_now() >= dump_t[idump]) {
        idump++;
        fprintf(stderr, "[TASK] ---- t=%.1f root %08X ----\n", pad_now(), g_ecx);
        jsrf_dump_tree(g_ecx, 0);
        fflush(stderr);
    }
    depth++;
    sub_00011070_gen();
    depth--;
}

/* ---- sub_000128C0: game-manager object table lookup -- flag garbage slots */
extern void sub_000128C0_gen(void);
void sub_000128C0(void)
{
    uint32_t idx = GARG(1), gm = g_ecx;
    sub_000128C0_gen();
    if (g_eax && !jsrf_node_ok(g_eax) && getenv("JSRF_TREE_CHECK")) {
        uint8_t *m = (uint8_t *)g_xbox_mem_offset;
        fprintf(stderr, "[OBJTAB] slot %d at %08X holds garbage %08X (gm=%08X) -- guest stack:\n",
                (int)idx, gm + 0x98 + idx * 4, g_eax, gm);
        const uint32_t *sp = (const uint32_t *)(m + g_esp);
        for (int i = 0; i < 200; i++)
            if (sp[i] > 0x11000 && sp[i] < 0x1C3F60) fprintf(stderr, "    [esp+%03X] %08X\n", i*4, sp[i]);
        fflush(stderr);
        abort();
    }
}

/* ---- corruption hunt: poll the sound manager's buffer-pointer array on every
 * traced function entry (JSRF_WATCH_SNDMGR=<va of manager>)
 *
 * The hook below is on the entry of every recompiled function, so the runtime
 * only calls it when something has asked for it -- see RECOMP_TRACE_ENTER.
 * These three switches are what "asked for it" means here, and the
 * constructor tells the runtime so before the first guest function runs. */
__attribute__((constructor))
static void jsrf_hook_switch(void)
{
    extern void recomp_trace_enable_hooks(void);
    if (getenv("JSRF_WATCH_SNDMGR") || getenv("JSRF_DEEP_ESP")
     || getenv("JSRF_BONE_CHECK") || getenv("JSRF_CRI_CALLS")
     || getenv("JSRF_ROBO_LOG"))
        recomp_trace_enable_hooks();
}

/* JSRF_CRI_CALLS=1 (test only): which functions of the CRI library
 * (0x138000-0x145000) the game calls, and from where. Every 10 s, the pairs
 * called since the last report, with counts. */
static double pad_now(void);
static void cri_census(uint32_t va)
{
    static int on = -1;
    static struct { uint32_t va, ra; unsigned n, shown; } tab[768];
    static int ntab; static double next;
    uint32_t ra; int i;
    if (on < 0) on = getenv("JSRF_CRI_CALLS") ? 1 : 0;
    if (!on || va < 0x00138000u || va >= 0x00145000u) return;
    ra = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + g_esp);
    if (ra >= 0x00138000u && ra < 0x00145000u) return;
    for (i = 0; i < ntab; i++) if (tab[i].va == va && tab[i].ra == ra) { tab[i].n++; break; }
    if (i == ntab && ntab < 768) { tab[i].va = va; tab[i].ra = ra; tab[i].n = 1; tab[i].shown = 0; ntab++; }
    if (pad_now() >= next) {
        next = pad_now() + 10.0;
        fprintf(stderr, "[CRI] t=%.1f calls since the last report:\n", pad_now());
        for (i = 0; i < ntab; i++)
            if (tab[i].n != tab[i].shown) {
                fprintf(stderr, "[CRI]   sub_%08X <- %08X x%u\n", tab[i].va, tab[i].ra, tab[i].n - tab[i].shown);
                tab[i].shown = tab[i].n;
            }
        fflush(stderr);
    }
}

/* JSRF_ROBO_LOG=<seconds> (test only): the Rokkaku robots built on
 * Media/Enemy/Robo. Enemy type 27 (made by sub_000ECF50; per-frame update
 * sub_000ECDB0, state at +0x128) is the Terror Drone of the Urban Battle at
 * the future site of the Expo Stadium; type 12 (made by sub_000EAC20; its
 * brain's update is sub_000EA6A0, state at +8) is the other robot. One line
 * per robot whenever its state changes, every that many seconds otherwise,
 * and every few frames while it attacks (states 2 and 3); with the player's
 * position, the robot's grind rails (its component at +0xBC keeps their
 * handles in the rail table at 0x0022FCE8, see sub_000179B0; each rail's
 * bounds at +0x10/+0x1C, its points at +4, how many at +0x28) and its four
 * graffiti spots (+0x174, 16 bytes each). One line too for each enemy a
 * mission creates (sub_000C0B30), with its type.
 * JSRF_ROBO_RIDE=<seconds>[:<rail>[:<point>]][,...] (with the log): at each
 * of those times (up to 8), put the player 40 units above a point of that
 * rail of the drone (rail 0 by default; the point by its index, or -1 for
 * the lowest, the default, -2 for the highest) -- to see whether landing on
 * it starts a grind -- and log the player's position every 0.2 s for 5 s
 * after. */
static float s_player_pos[3];
static uint32_t s_player_va;
static uint32_t robo_rail(const uint8_t *m, uint32_t h)
{
    uint32_t p;
    int guard;
    if (h == 0xFFFFFFFFu) return 0;
    p = *(const uint32_t *)(m + 0x0022FCE8u + 0x890u + (h & 0xFFu) * 4u);
    for (guard = 0; p >= 0x10000u && p < (64u << 20) && guard < 256; guard++) {
        if (*(const uint32_t *)(m + p + 0x38) == h) return p;
        p = *(const uint32_t *)(m + p + 0x94);
    }
    return 0;
}
static void jsrf_robo_log(uint32_t va)
{
    static double period = -1, ride_t[8], ride_until = -1, ride_next;
    static int ride_rail[8], ride_pt[8], ride_n, ride_cur;
    static struct { uint32_t obj, state, calls; double next; } r[16];
    const uint8_t *m;
    const uint32_t *w;
    const float *f;
    const uint32_t *cw;
    const float *cf;
    uint32_t obj, st, comp;
    int i, k, fine, t27, nrails;
    double now;
    if (va != 0x000ECDB0u && va != 0x000EA6A0u && va != 0x000C0B30u) return;
    if (period < 0) {
        const char *e = getenv("JSRF_ROBO_LOG");
        period = e ? atof(e) : 0;
        e = getenv("JSRF_ROBO_RIDE");
        while (e && *e && ride_n < 8) {
            char *x;
            ride_t[ride_n] = strtod(e, &x);
            if (x == e) break;
            ride_rail[ride_n] = *x == ':' ? (int)strtol(x + 1, &x, 10) : 0;
            ride_pt[ride_n] = *x == ':' ? (int)strtol(x + 1, &x, 10) : -1;
            ride_n++;
            e = *x == ',' ? x + 1 : x;
        }
    }
    if (period <= 0) return;
    m = (const uint8_t *)g_xbox_mem_offset;
    now = pad_now();
    if (va == 0x000C0B30u) {
        uint32_t desc = GARG(2);
        fprintf(stderr, "[ROBO] t=%.1f enemy created: type %d (args %08X %08X)\n", now,
                desc >= 0x10000u && desc < (64u << 20) ? (int)*(const uint32_t *)(m + desc) : -1,
                GARG(1), desc);
        fflush(stderr);
        return;
    }
    obj = g_ecx;
    if (obj < 0x10000u || obj >= (64u << 20)) return;
    w = (const uint32_t *)(m + obj);
    f = (const float *)(m + obj);
    st = va == 0x000ECDB0u ? w[0x128 / 4] : w[0x8 / 4];
    for (i = 0; i < 16 && r[i].obj && r[i].obj != obj; i++)
        ;
    if (i == 16) return;
    if (!r[i].obj) { r[i].obj = obj; r[i].state = 0xFFFFFFFFu; r[i].next = 0; r[i].calls = 0; }
    r[i].calls++;
    t27 = va == 0x000ECDB0u;
    comp = obj + (t27 ? 0xBCu : 0x30u);       /* the rails-and-spots component, sub_000EB830 */
    nrails = t27 ? 2 : 5;
    cw = (const uint32_t *)(m + comp);
    cf = (const float *)(m + comp);
    if (ride_cur < ride_n && now >= ride_t[ride_cur] && s_player_va) {
        int rr = ride_rail[ride_cur], rp = ride_pt[ride_cur];
        uint32_t rl = rr >= 0 && rr < nrails ? robo_rail(m, cw[1 + rr]) : 0;
        ride_cur++;
        if (rl) {
            uint32_t pts = *(const uint32_t *)(m + rl + 4), n = *(const uint32_t *)(m + rl + 0x28), lo = 0;
            if (pts >= 0x10000u && pts < (64u << 20) && n && n < 64) {
                const float *P = (const float *)(m + pts);
                float *pl = (float *)((uint8_t *)g_xbox_mem_offset + s_player_va + 0xCA4);
                if (rp >= 0) lo = (uint32_t)(rp < (int)n ? rp : (int)n - 1);
                else
                    for (k = 1; k < (int)n; k++)
                        if (rp == -2 ? P[k * 6 + 1] > P[lo * 6 + 1] : P[k * 6 + 1] < P[lo * 6 + 1]) lo = (uint32_t)k;
                fprintf(stderr, "[ROBO] t=%.1f ride: player (%.1f %.1f %.1f) -> 40 above point %u of rail %d"
                        " (%.1f %.1f %.1f) of the type-%d robot\n", now, pl[0], pl[1], pl[2], lo, rr,
                        P[lo * 6], P[lo * 6 + 1], P[lo * 6 + 2], t27 ? 27 : 12);
                pl[0] = P[lo * 6]; pl[1] = P[lo * 6 + 1] + 40.0f; pl[2] = P[lo * 6 + 2];
                ride_until = now + 5.0; ride_next = now;
            }
        } else
            fprintf(stderr, "[ROBO] t=%.1f ride: the robot has no rail %d\n", now, rr);
    }
    if (now < ride_until && now >= ride_next) {
        fprintf(stderr, "[ROBO] t=%.2f riding? player at (%.1f %.1f %.1f)\n", now,
                s_player_pos[0], s_player_pos[1], s_player_pos[2]);
        ride_next = now + 0.2;
    }
    fine = t27 && (st == 2 || st == 3) && r[i].calls % 6 == 0;
    if (st == r[i].state && now < r[i].next && !fine) return;
    if (t27)
        fprintf(stderr, "[ROBO] t=%.2f type27 %08X state %u%s count %u idx %u shot %u flags %08X"
                " motion %08X@%.2f %08X@%.2f at (%.1f %.1f %.1f) ground %.1f dist %.1f player (%.1f %.1f %.1f)\n",
                now, obj, st, st != r[i].state ? " (new)" : "", w[0x12C / 4], w[0x134 / 4],
                w[0x138 / 4], w[0x90 / 4], w[0x148 / 4], f[0x154 / 4], w[0x14C / 4], f[0x158 / 4],
                f[0x48 / 4], f[0x4C / 4], f[0x50 / 4], f[0x88 / 4], f[0x74 / 4],
                s_player_pos[0], s_player_pos[1], s_player_pos[2]);
    else {
        uint32_t ent = w[4 / 4];
        const float *ef = ent >= 0x10000u && ent < (64u << 20) ? (const float *)(m + ent + 0x44) : 0;
        fprintf(stderr, "[ROBO] t=%.1f type12 %08X state %u%s +10 %08X +144 %08X at (%.1f %.1f %.1f) ground %.1f"
                " entity %08X at (%.1f %.1f %.1f) flags %08X player (%.1f %.1f %.1f)\n", now, obj, st,
                st != r[i].state ? " (new)" : "", w[0x10 / 4], w[0x144 / 4], f[0x14 / 4], f[0x18 / 4],
                f[0x1C / 4], f[0x2C / 4], ent, ef ? ef[0] : 0.0f, ef ? ef[1] : 0.0f, ef ? ef[2] : 0.0f,
                ef ? *(const uint32_t *)(m + ent + 0xA4) : 0u, s_player_pos[0], s_player_pos[1], s_player_pos[2]);
    }
    if (now >= r[i].next) {
        for (k = 0; k < nrails; k++) {
            uint32_t h = cw[1 + k], rl = robo_rail(m, h);
            if (!rl) { fprintf(stderr, "[ROBO]   rail %d: handle %08X, none\n", k, h); continue; }
            {
                const float *R = (const float *)(m + rl);
                fprintf(stderr, "[ROBO]   rail %d: handle %08X, %u points, (%.1f %.1f %.1f) .. (%.1f %.1f %.1f)\n",
                        k, h, *(const uint32_t *)(m + rl + 0x28), R[0x10 / 4], R[0x14 / 4], R[0x18 / 4],
                        R[0x1C / 4], R[0x20 / 4], R[0x24 / 4]);
            }
        }
        fprintf(stderr, "[ROBO]   graffiti: handles %08X %08X %08X %08X progress %.2f %.2f %.2f %.2f\n",
                cw[0x3C / 4], cw[0x40 / 4], cw[0x44 / 4], cw[0x48 / 4], cf[0x4C / 4], cf[0x50 / 4],
                cf[0x54 / 4], cf[0x58 / 4]);
    }
    fflush(stderr);
    if (st != r[i].state || now >= r[i].next) r[i].next = now + period;
    r[i].state = st;
}

void recomp_trace_user_hook(const char *name, uint32_t va)
{
    cri_census(va);
    jsrf_robo_log(va);
    static int init = -1; static uint32_t mgr; static const char *last_name; static uint32_t last_va;
    if (init < 0) { const char *e = getenv("JSRF_WATCH_SNDMGR"); mgr = e ? (uint32_t)strtoul(e, 0, 0) : 0; init = 1; }
    if (mgr) {
        uint8_t *m = (uint8_t *)g_xbox_mem_offset;
        uint32_t n = *(uint32_t *)(m + mgr + 0x3f18);
        if (n < 64) {
            for (uint32_t i = 0; i < n; i++) {
                uint32_t p = *(uint32_t *)(m + mgr + 0x3f1c + i * 4);
                if (p && !jsrf_node_ok(p)) {
                    fprintf(stderr, "[SNDWATCH] entry %u = %08X (n=%u) seen entering %s; previous entry was %s (0x%08X)\n",
                            i, p, n, name, last_name ? last_name : "?", last_va);
                    { const uint32_t *sp = (const uint32_t *)(m + g_esp);
                      for (int k = 0; k < 120; k++)
                          if (sp[k] > 0x11000 && sp[k] < 0x1C3F60) fprintf(stderr, "    [esp+%03X] %08X\n", k*4, sp[k]); }
                    fflush(stderr); abort();
                }
            }
        }
    }
    last_name = name; last_va = va;
    /* Deep-stack snapshot: the first time the main thread's esp goes below
     * JSRF_DEEP_ESP, print who is on the stack. */
    {
        static int init3 = -1; static uint32_t limit; static int done;
        extern uint32_t xbox_current_thread_stack_top(void);
        if (init3 < 0) { const char *e = getenv("JSRF_DEEP_ESP"); limit = e ? (uint32_t)strtoul(e, 0, 0) : 0; init3 = 1; }
        if (limit && !done && g_esp < limit && xbox_current_thread_stack_top() == 0) {
            uint8_t *m = (uint8_t *)g_xbox_mem_offset;
            done = 1;
            fprintf(stderr, "[DEEP] esp=%08X entering %s -- guest stack code refs (sparse):\n", g_esp, name);
            const uint32_t *sp = (const uint32_t *)(m + g_esp);
            int shown = 0;
            for (uint32_t i = 0; i < (0x00F80000 - g_esp) / 4 && shown < 400; i++)
                if (sp[i] > 0x11000 && sp[i] < 0x1C3F60) { fprintf(stderr, "    [esp+%06X] %08X\n", i*4, sp[i]); shown++; }
            fflush(stderr);
        }
    }
    /* JSRF_BONE_CHECK: the node hierarchy sub_00048190 is about to measure.
     *
     * It counts the tree at arg3 by walking child (+0x60) and sibling (+0x64),
     * multiplies by 0x6c and allocates that. A corrupt link therefore shows up
     * as a 3 GB allocation and a wild pointer several frames later, with
     * nothing left to say which node was bad. Walk it here, where the tree is
     * still intact enough to name the offender.
     */
    if (va == 0x00048190) {
        static int on = -1; static int shown;
        if (on < 0) on = getenv("JSRF_BONE_CHECK") ? 1 : 0;
        if (on) {
            uint8_t *m = (uint8_t *)g_xbox_mem_offset;
            uint32_t root = GARG(3), stack[512]; int sp = 0; long n = 0; int bad = 0;
            uint32_t node = root;
            if (shown < 8) {
                fprintf(stderr, "[BONE] sub_00048190 this=%08X a1=%08X a2=%08X a3=%08X from=%08X\n"
                        "       D3D globals: 251d64=%08X 251d68=%08X 251d6c=%08X 251d70=%08X 251d74=%08X 251d78=%08X\n",
                        g_ecx, GARG(1), GARG(2), GARG(3), GARG(0),
                        *(uint32_t *)(m + 0x251d64), *(uint32_t *)(m + 0x251d68),
                        *(uint32_t *)(m + 0x251d6c), *(uint32_t *)(m + 0x251d70),
                        *(uint32_t *)(m + 0x251d74), *(uint32_t *)(m + 0x251d78));
                { uint32_t dev = *(uint32_t *)(m + 0x251d6c);
                  uint32_t vt = dev ? *(uint32_t *)(m + dev) : 0;
                  fprintf(stderr, "       device %08X vtable=%08X  [0x00]=%08X [0xf4]=%08X [0x148]=%08X\n",
                          dev, vt,
                          vt ? *(uint32_t *)(m + vt) : 0,
                          vt ? *(uint32_t *)(m + vt + 0xf4) : 0,
                          vt ? *(uint32_t *)(m + vt + 0x148) : 0); }
                shown++;
            }
            while (node || sp) {
                if (!node) { node = stack[--sp]; continue; }
                if (!jsrf_node_ok(node) || ++n > 200000) { bad = 1; break; }
                { uint32_t c = *(uint32_t *)(m + node + 0x60);
                  if (c && sp < 512) stack[sp++] = c; }
                node = *(uint32_t *)(m + node + 0x64);
            }
            if (bad) {
                fprintf(stderr, "[BONE] tree at %08X is corrupt after %ld nodes"
                        " (stopped at %08X, depth %d)\n", root, n, node, sp);
                for (uint32_t p = root; p && p < root + 0x300; p += 0x80) {
                    fprintf(stderr, "  node %08X:", p);
                    for (int k = 0; k < 8; k++)
                        fprintf(stderr, " %08X", *(uint32_t *)(m + p + 0x60 - 0x20 + k * 4));
                    fprintf(stderr, "  (that is +40..+5C, then +60 child=%08X sib=%08X)\n",
                            *(uint32_t *)(m + p + 0x60), *(uint32_t *)(m + p + 0x64));
                }
                fflush(stderr);
            }
        }
    }
    /* JSRF_SHADOW=<esp limit>: keep a shadow call stack of the main thread
     * (entry esp per traced function) and print the live chain, with the
     * stack each frame owns, the first time esp drops below the limit. */
    {
        static int init4 = -1; static uint32_t limit; static int done;
        static struct { const char *name; uint32_t va, esp; } sh[8192]; static int top;
        extern uint32_t xbox_current_thread_stack_top(void);
        if (init4 < 0) { const char *e = getenv("JSRF_SHADOW"); limit = e ? (uint32_t)strtoul(e, 0, 0) : 0; init4 = 1; }
        static struct { const char *name; uint32_t esp, ret; } ring[64]; static unsigned rn;
        if (limit && !done && xbox_current_thread_stack_top() == 0) {
            ring[rn & 63].name = name; ring[rn & 63].esp = g_esp; ring[rn & 63].ret = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + g_esp); rn++;
            { static uint32_t last_e, last_r; static int nn;
              if (va == 0x00080BD0) {
                  uint32_t r = ring[(rn - 1) & 63].ret;
                  if (last_e && last_e - g_esp == 64 && nn++ < 6)
                      fprintf(stderr, "[SH64] sub_00080BD0 esp=%08X ret=%08X (prev esp=%08X ret=%08X) prev-entry=%s ecx=%08X\n", g_esp, r, last_e, last_r, ring[(rn - 2) & 63].name, g_ecx);
                  last_e = g_esp; last_r = r;
              } }
            while (top > 0 && sh[top - 1].esp <= g_esp) top--;
            if (top < 8192) { sh[top].name = name; sh[top].va = va; sh[top].esp = g_esp; top++; }
            if (g_esp < limit) {
                done = 1;
                fprintf(stderr, "[SHADOW] esp=%08X depth=%d entering %s\n", g_esp, top, name);
                for (unsigned i = 0; i < 64 && i < rn; i++) { unsigned k = (rn - 64 + i) & 63; fprintf(stderr, "    ring %s esp=%08X ret=%08X\n", ring[k].name, ring[k].esp, ring[k].ret); }
                for (int i = top - 1; i >= 0 && i >= top - 400; i--)
                    fprintf(stderr, "    #%-4d %s (0x%08X) esp=%08X frame=%u\n", top - 1 - i, sh[i].name, sh[i].va, sh[i].esp,
                            i > 0 ? sh[i - 1].esp - sh[i].esp : 0);
                fflush(stderr);
            }
        }
    }
    /* JSRF_ESP_WATCH=<va>: print the guest esp at every entry to that function
     * (main thread only), to see whether the frame loop leaks stack. */
    {
        static int init2 = -1; static uint32_t wva; static uint32_t last_esp; static int n;
        extern uint32_t xbox_current_thread_stack_top(void);
        if (init2 < 0) { const char *e = getenv("JSRF_ESP_WATCH"); wva = e ? (uint32_t)strtoul(e, 0, 0) : 0; init2 = 1; }
        if (wva && va == wva && xbox_current_thread_stack_top() == 0) {
            if (g_esp != last_esp && n++ < 400) { fprintf(stderr, "[ESPW] %s esp=%08X (%+d)\n", name, g_esp, (int)(g_esp - last_esp)); fflush(stderr); }
            last_esp = g_esp;
        }
    }
}

/* ---- ADX decoders: report the output buffer they are about to write */
static void jsrf_adx_dec_report(const char *who)
{
    static int n;
    uint32_t h = GARG(1);
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    if (!h || n++ >= 12) return;
    fprintf(stderr, "[ADXDEC] %s h=%08X state=%u outbuf[0x64]=%08X [0x68]=%d [0x6c]=%d [0x54]=%d [0x50]=%08X [0x88]=%08X [0x8c]=%08X\n",
            who, h, *(uint32_t *)(m + h + 4), *(uint32_t *)(m + h + 0x64), *(int32_t *)(m + h + 0x68),
            *(int32_t *)(m + h + 0x6c), *(int32_t *)(m + h + 0x54), *(uint32_t *)(m + h + 0x50),
            *(uint32_t *)(m + h + 0x88), *(uint32_t *)(m + h + 0x8c));
    fflush(stderr);
}
extern void sub_00143EB0_gen(void);
void sub_00143EB0(void) { jsrf_adx_dec_report("stereo"); sub_00143EB0_gen(); }
extern void sub_00143FD0_gen(void);
void sub_00143FD0(void) { jsrf_adx_dec_report("mono"); sub_00143FD0_gen(); }

/* ---- sub_00144F60: ADX block decode core -- print args (out pointers) */
extern void sub_00144F60_gen(void);
void sub_00144F60(void)
{
    static int n;
    uint32_t a3 = GARG(3), a5 = GARG(5);
    int in_data = (a3 >= 0x1EB760 && a3 < 0x27E074) || (a5 >= 0x1EB760 && a5 < 0x27E074);
    if (n++ < 6 || in_data) {
        fprintf(stderr, "[ADXCORE] src=%08X a2=%08X outL=%08X a4=%08X outR=%08X a6=%08X a7=%08X a8=%08X esp=%08X%s\n",
                GARG(1), GARG(2), a3, GARG(4), a5, GARG(6), GARG(7), GARG(8), g_esp, in_data ? "  <-- .data!" : "");
        if (in_data) {
            uint8_t *m = (uint8_t *)g_xbox_mem_offset;
            const uint32_t *sp = (const uint32_t *)(m + g_esp);
            for (int i = 0; i < 120; i++)
                if (sp[i] > 0x11000 && sp[i] < 0x1C3F60) fprintf(stderr, "    [esp+%03X] %08X\n", i*4, sp[i]);
            fflush(stderr); abort();
        }
    }
    /*
     * JSRF_ADX_DUMP=<prefix>: the encoded blocks this call is given and the
     * PCM it produces from them, for the first few calls.
     *
     * The title's own ADX decoder is recompiled x86, and whether it decodes
     * correctly is the one question that separates "the emulated APU plays
     * the music wrongly" from "the music never was the music". Its arguments
     * already name the answer: a7 and a8 are 0x1CA6 and 0xF32D, which are
     * 7334 and -3283 -- exactly the predictor coefficients CRI's formula
     * gives for a 44.1 kHz stream with a 500 Hz highpass. So the same blocks
     * can be decoded here and compared sample for sample.
     *
     * src advances 0x510 = 1296 bytes a call: 72 blocks of 18 bytes, which
     * for a stereo stream is 36 per channel, 1152 samples each.
     */
    /*
     * JSRF_ADX_RATE=1: how fast the title is decoding, against how fast the
     * APU is consuming.
     *
     * The voice plays a 16384-sample ring that the title refills as it goes,
     * and the music comes out as correct fragments in the wrong order: 100 ms
     * of our output matches a reference decode at 0.998, 250 ms at 0.94, a
     * second at 0.35. Fragments that good cannot be a decode fault; an order
     * that bad is a producer and a consumer running at different speeds.
     *
     * Count the samples, not the calls. The first version of this multiplied
     * calls by 1152 because the first few calls carried 72 blocks, and read
     * 59 calls a second as 68,000 samples a second -- a 1.5x over-supply that
     * would have been a second bug to chase. The block count is an argument;
     * a caller topping up a ring passes whatever will fit, so it varies by
     * design. GARG(2) is that count, two blocks (one per channel) giving 32
     * samples.
     */
    { static int on = -1; static int calls; static long long samples;
      static long long blocks; static uint64_t last_ms;
      if (on < 0) on = getenv("JSRF_ADX_RATE") ? 1 : 0;
      if (on) {
          struct timespec ts; uint64_t now;
          uint32_t cnt = GARG(2);
          clock_gettime(CLOCK_MONOTONIC, &ts);
          now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
          calls++; blocks += cnt; samples += (long long)(cnt / 2u) * 32;
          if (!last_ms) last_ms = now;
          if (now - last_ms >= 1000) {
              fprintf(stderr, "[ADXRATE] %d decode calls/s, %lld blocks "
                      "(%.1f per call) = %lld samples/s (44100 wanted)\n",
                      calls, blocks, calls ? (double)blocks / calls : 0.0,
                      samples);
              fflush(stderr);
              calls = 0; blocks = 0; samples = 0; last_ms = now;
          }
      } }

    { static int init, n; static const char *pfx;
      if (!init) { init = 1; pfx = getenv("JSRF_ADX_DUMP"); }
      if (pfx && n < 4) {
          uint8_t *m = (uint8_t *)g_xbox_mem_offset;
          uint32_t src = GARG(1), cnt = GARG(2), outL = GARG(3), outR = GARG(5);
          char nm[256]; FILE *f;
          uint32_t src_bytes = cnt * 18u;
          uint32_t out_samples = (cnt / 2u) * 32u;
          sub_00144F60_gen();                      /* decode first */
          snprintf(nm, sizeof nm, "%s_%d_src.bin", pfx, n);
          f = fopen(nm, "wb");
          if (f) { fwrite(m + src, 1, src_bytes, f); fclose(f); }
          snprintf(nm, sizeof nm, "%s_%d_outL.bin", pfx, n);
          f = fopen(nm, "wb");
          if (f) { fwrite(m + outL, 2, out_samples, f); fclose(f); }
          snprintf(nm, sizeof nm, "%s_%d_outR.bin", pfx, n);
          f = fopen(nm, "wb");
          if (f) { fwrite(m + outR, 2, out_samples, f); fclose(f); }
          fprintf(stderr, "[ADXDUMP] call %d: %u blocks from %08X -> %u samples "
                  "at %08X/%08X (coefs %d %d)\n", n, cnt, src, out_samples,
                  outL, outR, (int)(int16_t)GARG(7), (int)(int16_t)GARG(8));
          fflush(stderr);
          n++;
          return;
      } }
    sub_00144F60_gen();
}

/* arm the .data write trap when the title music file is opened */
static double pad_now(void);
static unsigned s_game_frames;   /* sound-manager updates: one per game frame */
void xbox_file_open_hook(const char *path)
{
    extern void jsrf_protect_data_arm(void);
    const char *t = getenv("JSRF_PROTECT_AT");
    if (t && strstr(path, t)) jsrf_protect_data_arm();
    /* When each ADX stream is opened, on the wall clock and the game's own
     * frame count: a cutscene timed in frames drifts from its voice track
     * whenever the game runs below 60 fps, and this is how that shows. */
    if (strstr(path, "Z_ADX") && strstr(path, ".adx"))
        fprintf(stderr, "[ADXOPEN] t=%.2f frame %u %s\n", pad_now(), s_game_frames, path);
}

/* ---- sub_0017CAB0: _chkstk -- report absurd frame sizes */
extern void sub_0017CAB0_gen(void);
void sub_0017CAB0(void)
{
    static int n;
    if (g_eax > 0x10000 && n++ < 10) {
        uint8_t *m = (uint8_t *)g_xbox_mem_offset;
        fprintf(stderr, "[CHKSTK] size=0x%X esp=%08X ret=%08X\n", g_eax, g_esp, *(uint32_t *)(m + g_esp));
        const uint32_t *sp = (const uint32_t *)(m + g_esp);
        for (int i = 0; i < 60; i++)
            if (sp[i] > 0x11000 && sp[i] < 0x1C3F60) fprintf(stderr, "    [esp+%03X] %08X\n", i*4, sp[i]);
        fflush(stderr);
    }
    sub_0017CAB0_gen();
}

/* ======================================================================
 * XInput HLE (XPP section)
 *
 * The XDK's pad API sits on a USB stack that talks to the OHCI controllers
 * through MMIO; nothing answers those registers here, so XGetDevices never
 * reported a pad and the title sat on its attract loop for ever. Replace the
 * eight entry points the game calls with direct host input. On the host the
 * pad is whatever xbox_input has (SDL2 here, GameController on Apple);
 * JSRF_PAD_SCRIPT="20 START;26 A;30 A" presses buttons on a schedule for
 * unattended runs (seconds since start, 250 ms per press).
 *
 *   0x001BD5FF  XGetDevices(type)                        stdcall 4
 *   0x001BD621  XGetDeviceChanges(type, &ins, &rem)      stdcall 12
 *   0x001C3BA1  XInputOpen(type, port, slot, polling)    stdcall 16
 *   0x001C3C16  XInputClose(h)                           stdcall 4
 *   0x001C3C22  XInputGetCapabilities(h, caps)           stdcall 8
 *   0x001C3E14  XInputGetState(h, state)                 stdcall 8
 *   0x001C3E85  XInputSetState(h, feedback)              stdcall 8
 *   0x001C3EBD  XInputPoll(h)                            stdcall 4
 * ====================================================================== */
#include "xinput_xbox.h"
#define XPP_TYPE_GAMEPAD 0x001BC860u
#define XPP_HANDLE_BASE  0x58580000u

static struct { double t, dur; uint16_t buttons; uint8_t analog[8];
                int16_t axis[4]; int has_axis; char name[16]; int said; } s_pad_script[64];
static int s_pad_script_n = -1;
static double pad_now(void)
{
    static struct timespec t0; struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (!t0.tv_sec) t0 = t;
    return (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
}
static void pad_script_load(void)
{
    const char *s = getenv("JSRF_PAD_SCRIPT");
    s_pad_script_n = 0;
    if (!s) return;
    while (*s && s_pad_script_n < 64) {
        char name[16] = {0}; double t; int k = 0;
        while (*s == ' ' || *s == ';') s++;
        if (!*s) break;
        t = strtod(s, (char **)&s);
        while (*s == ' ') s++;
        while (*s && *s != ';' && *s != ' ' && k < 15) name[k++] = *s++;
        s_pad_script[s_pad_script_n].t = t;
        s_pad_script[s_pad_script_n].dur = 0.25;
        s_pad_script[s_pad_script_n].buttons = 0;
        s_pad_script[s_pad_script_n].said = 0;
        s_pad_script[s_pad_script_n].has_axis = 0;
        memset(s_pad_script[s_pad_script_n].axis, 0, sizeof s_pad_script[0].axis);
        /* "NAME@secs" holds for that long instead of a 250 ms tap. */
        { char *at = strchr(name, '@');
          if (at) { *at = 0; s_pad_script[s_pad_script_n].dur = atof(at + 1); } }
        memcpy(s_pad_script[s_pad_script_n].name, name, sizeof name);
        memset(s_pad_script[s_pad_script_n].analog, 0, 8);
        /* Sticks: LX+ LX- LY+ LY- RX+ RX- RY+ RY-, full deflection, or a
         * percentage after the sign ("LY+60"). Y is up, as on the pad. */
        if ((name[0] == 'L' || name[0] == 'R') && (name[1] == 'X' || name[1] == 'Y')
            && (name[2] == '+' || name[2] == '-')) {
            int ax = (name[0] == 'R' ? 2 : 0) + (name[1] == 'Y' ? 1 : 0);
            int pct = name[3] ? atoi(name + 3) : 100;
            long v;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            v = (long)pct * 32767 / 100;
            s_pad_script[s_pad_script_n].axis[ax] = (int16_t)(name[2] == '-' ? -v : v);
            s_pad_script[s_pad_script_n].has_axis = 1;
        }
        if (!strcmp(name, "START")) s_pad_script[s_pad_script_n].buttons = XBOX_GAMEPAD_START;
        else if (!strcmp(name, "BACK")) s_pad_script[s_pad_script_n].buttons = XBOX_GAMEPAD_BACK;
        else if (!strcmp(name, "UP")) s_pad_script[s_pad_script_n].buttons = XBOX_GAMEPAD_DPAD_UP;
        else if (!strcmp(name, "DOWN")) s_pad_script[s_pad_script_n].buttons = XBOX_GAMEPAD_DPAD_DOWN;
        else if (!strcmp(name, "LEFT")) s_pad_script[s_pad_script_n].buttons = XBOX_GAMEPAD_DPAD_LEFT;
        else if (!strcmp(name, "RIGHT")) s_pad_script[s_pad_script_n].buttons = XBOX_GAMEPAD_DPAD_RIGHT;
        else if (!strcmp(name, "A")) s_pad_script[s_pad_script_n].analog[XBOX_BUTTON_A] = 255;
        else if (!strcmp(name, "B")) s_pad_script[s_pad_script_n].analog[XBOX_BUTTON_B] = 255;
        else if (!strcmp(name, "X")) s_pad_script[s_pad_script_n].analog[XBOX_BUTTON_X] = 255;
        else if (!strcmp(name, "Y")) s_pad_script[s_pad_script_n].analog[XBOX_BUTTON_Y] = 255;
        else if (!strcmp(name, "LT")) s_pad_script[s_pad_script_n].analog[XBOX_BUTTON_LTRIGGER] = 255;
        else if (!strcmp(name, "RT")) s_pad_script[s_pad_script_n].analog[XBOX_BUTTON_RTRIGGER] = 255;
        s_pad_script_n++;
    }
    fprintf(stderr, "[PAD] script: %d scheduled presses\n", s_pad_script_n);
}
/* ---- autopilot for headless runs: JSRF_AUTOPILOT="t:x,z;t:x,z;...".
 * Waypoints are taken in order. Once a waypoint's time t has come (same
 * clock as JSRF_PAD_SCRIPT), the left stick steers the player toward world
 * (x, z), relative to where the follow camera looks, until the player is
 * within 12 units (or 40 s pass). The stick is recomputed every frame from
 * the camera update (sub_000A5070), so the loop closes through the game's
 * own camera-relative controls. Off unless the variable is set. */
static struct { double t, t_on, t_done; float x, z; int state; char btn[8]; } s_auto[16];  /* 0 waiting, 1 active, 2 done */
static int s_auto_n = -1;
static int16_t s_auto_lx, s_auto_ly;
/* JSRF_TALK_AUTO_A=1: while the follow camera is in talk mode (12), tap A
 * every 2 s to page through the conversation. */
static int s_talk_auto_a = -1;
static double s_talk_auto_a_until = 1e30;
static volatile uint32_t s_cam_mode_now;
static int s_auto_live;
static void auto_load(void)
{
    const char *s = getenv("JSRF_AUTOPILOT");
    s_auto_n = 0;
    if (!s) return;
    while (*s && s_auto_n < 16) {
        double t; float x, z; int used = 0;
        while (*s == ' ' || *s == ';') s++;
        if (!*s) break;
        if (sscanf(s, "%lf:%f,%f%n", &t, &x, &z, &used) != 3 || used <= 0) break;
        s_auto[s_auto_n].t = t; s_auto[s_auto_n].x = x; s_auto[s_auto_n].z = z;
        s_auto[s_auto_n].state = 0; s_auto[s_auto_n].t_on = 0; s_auto[s_auto_n].t_done = 0;
        memset(s_auto[s_auto_n].btn, 0, sizeof s_auto[0].btn);
        s += used;
        /* "t:x,z:RT" -- on arrival, tap that button three times, 1.5 s apart */
        if (*s == ':') {
            int k = 0; s++;
            while (*s && *s != ';' && *s != ' ' && k < 7) s_auto[s_auto_n].btn[k++] = *s++;
        }
        s_auto_n++;
    }
    fprintf(stderr, "[AUTO] %d waypoints\n", s_auto_n);
}
/* JSRF_TELEPORT="t:x,y,z" -- once, at time t, put the player at (x, y, z).
 * Test-only shortcut to a spot the autopilot cannot path to. */
static void teleport_step(float *chw)
{
    static int state = -1; static double t; static float x, y, z;
    if (state < 0) {
        const char *s = getenv("JSRF_TELEPORT");
        state = (s && sscanf(s, "%lf:%f,%f,%f", &t, &x, &y, &z) == 4) ? 1 : 0;
    }
    if (state != 1 || pad_now() < t) return;
    state = 2;
    fprintf(stderr, "[TELEPORT] t=%.1f (%.1f %.1f %.1f) -> (%.1f %.1f %.1f)\n", pad_now(),
            chw[0xCA4 / 4], chw[0xCA8 / 4], chw[0xCAC / 4], x, y, z);
    chw[0xCA4 / 4] = x; chw[0xCA8 / 4] = y; chw[0xCAC / 4] = z;
}
/* JSRF_CANS="t:n" -- once, at time t, give the player n spray cans
 * (char +0xFA0; sub_0007FC90 checks it against the boost cost, sub_0007FC60
 * takes the cost away). Test-only: the boost dash needs ten.
 * Every frame: log the boost timer (char +0xE60, set from the character's
 * parameters when B starts a boost, sub_0009C6A5) starting and ending, and
 * with JSRF_BOOST_DUMP="shots,every,frames" capture a burst of frames and a
 * per-pass log when the first boost starts. */
extern void nv2a_gl_dump_burst(int shots, int every);
extern void nv2a_gl_trace_frames(int n);
extern void nv2a_gl_test_skip(uint32_t tex, int k);
extern void nv2a_gl_test_skip_bit(int bit);
extern uint32_t nv2a_gl_flips(void);
static volatile int s_can_pickups;   /* cans the player picked up (test_hooks_step) */
static void test_hooks_step(uint8_t *ch)
{
    static int cans_state = -1; static double cans_t; static int cans_n;
    static int dump_state = -1; static int d_shots, d_every, d_frames;
    static uint32_t last_timer; static int boosts;
    uint32_t *cw = (uint32_t *)ch;
    uint32_t timer;
    if (cans_state < 0) {
        const char *s = getenv("JSRF_CANS");
        cans_state = (s && sscanf(s, "%lf:%d", &cans_t, &cans_n) == 2) ? 1 : 0;
        s = getenv("JSRF_BOOST_DUMP");
        dump_state = (s && sscanf(s, "%d,%d,%d", &d_shots, &d_every, &d_frames) == 3) ? 1 : 0;
    }
    if (cans_state == 1 && pad_now() >= cans_t) {
        cans_state = 2;
        fprintf(stderr, "[CANS] t=%.1f cans %d -> %d\n", pad_now(), (int)cw[0xFA0 / 4], cans_n);
        cw[0xFA0 / 4] = (uint32_t)cans_n;
    }
    { static int cans_was = -1; int now_cans = (int)cw[0xFA0 / 4];
      if (cans_was >= 0 && now_cans > cans_was) {
          s_can_pickups++;
          fprintf(stderr, "[CANS] t=%.2f picked up: %d -> %d\n", pad_now(), cans_was, now_cans);
      }
      cans_was = now_cans; }
    timer = cw[0xE60 / 4];
    if (timer != last_timer && (timer == 0 || last_timer == 0)) {
        if (timer) boosts++;
        fprintf(stderr, "[BOOST] t=%.2f flip %u boost %s: timer %u cans %d\n",
                pad_now(), nv2a_gl_flips(), timer ? "starts" : "ends", timer,
                (int)cw[0xFA0 / 4]);
        if (timer && dump_state == 1) {
            dump_state = 2;
            nv2a_gl_dump_burst(d_shots, d_every);
            nv2a_gl_trace_frames(d_frames);
        }
    }
    last_timer = timer;
}
/* ---- missions -------------------------------------------------------------
 * sub_00030490 / sub_00030690 queue the load of mission file .bin / .dat for
 * the id in their one argument: high 16 bits and low 16 bits, two decimal
 * digits each (0x00010001 is mssn0101, the tutorial; 0x00010060 is mssn0196,
 * what follows it). Every request is logged once per id.
 * JSRF_MISSION="101:196@20" (test only): from 20 s on, a request for 0101 is
 * turned into one for 0196 -- straight to the end of the tutorial. Up to four
 * pairs, comma-separated, share the time: "0x10001:0x200FA,0x10064:0x200FA@45"
 * sends New Game's tutorial and Load Game's garage alike to 99th Street. */
static uint32_t s_mission_mgr;   /* the object whose vtable requests missions */
static void mission_hook(const char *what)
{
    static int init; static uint32_t from[4], to[4]; static double after; static int npairs;
    static uint32_t said[32]; static int nsaid;
    uint32_t *arg = (uint32_t *)((uint8_t *)g_xbox_mem_offset + g_esp + 4);
    uint32_t id = *arg; int i, seen = 0;
    if (!init) {
        const char *e = getenv("JSRF_MISSION"); unsigned a, b; double t = 0;
        const char *at = e ? strchr(e, '@') : NULL;
        init = 1;
        /* An id is (chapter << 16) | number and the file is named after
         * the number modulo 100: 0x000100C4 (196) loads mssn0196, the scene
         * after the tutorial, and 0x00010064 (100) mssn0100, the garage.
         * "101:196" therefore meant 0x00010060, the right files under the
         * wrong number, and the game hung on "Now Loading". Values of
         * 0x10000 and up are taken as ids: JSRF_MISSION="0x10001:0x100C4". */
        if (at) t = atof(at + 1);
        while (e && npairs < 4 && sscanf(e, "%i:%i", (int *)&a, (int *)&b) == 2) {
            from[npairs] = a >= 0x10000u ? a : ((a / 100) << 16) | (a % 100);
            to[npairs]   = b >= 0x10000u ? b : ((b / 100) << 16) | (b % 100);
            npairs++;
            e = strchr(e, ',');
            if (!e || (at && e > at)) break;
            e++;
        }
        after = t;
    }
    for (i = 0; i < nsaid; i++) if (said[i] == (id ^ (what[0] << 24))) seen = 1;
    if (!seen && nsaid < 32) {
        const uint32_t *sp = (const uint32_t *)((uint8_t *)g_xbox_mem_offset + g_esp);
        int k, shown = 0;
        said[nsaid++] = id ^ (what[0] << 24);
        fprintf(stderr, "[MISSION] t=%.1f %s request mssn%02u%02u (id %08X) mgr %08X from",
                pad_now(), what, id >> 16, id & 0xFFFF, id, g_ecx);
        for (k = 0; k < 64 && shown < 8; k++)
            if (sp[k] > 0x11000u && sp[k] < 0x138000u) { fprintf(stderr, " %08X", sp[k]); shown++; }
        fprintf(stderr, "\n");
    }
    if (what[0] == '.' && what[1] == 'b') s_mission_mgr = g_ecx;
    for (i = 0; i < npairs; i++)
        if (id == from[i] && pad_now() >= after) {
            fprintf(stderr, "[MISSION] t=%.1f %s: redirecting mssn%02u%02u -> mssn%02u%02u\n",
                    pad_now(), what, from[i] >> 16, from[i] & 0xFFFF, to[i] >> 16, to[i] & 0xFFFF);
            *arg = to[i];
            break;
        }
}
/* ---- sub_00046FD0: fill in one storage device's record for the save menu
 * (slot 8 is the hard disk: +0 present, +4 ?, +8 room for a save, +0xC the
 * existing save's info, 0 = none). [SAVE] says what the menu is told. */
extern void sub_00046FD0_gen(void);
void sub_00046FD0(void)
{
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    uint32_t slot = *(uint32_t *)(m + g_esp + 4), rec = *(uint32_t *)(m + g_esp + 8);
    sub_00046FD0_gen();
    if (slot == 8 && rec) {
        static int n;
        if (n++ < 24)
            fprintf(stderr, "[SAVE] t=%.1f hard disk record %08X: present %u, %u, room %u, save %d, %04X\n",
                    pad_now(), rec, *(uint32_t *)(m + rec), *(uint32_t *)(m + rec + 4),
                    *(uint32_t *)(m + rec + 8), (int)*(uint32_t *)(m + rec + 0xC),
                    *(uint16_t *)(m + rec + 0x10));
    }
}
/* ---- the save menu (test only) ------------------------------------------
 *
 * sub_000681C0(mode, records) opens it: mode 0 saves, 1 loads; records is an
 * optional copy of the nine storage-device records (sub_00047080). The menu
 * object's state machine is sub_00067E90: +0x98 state, +0xA4 mode, +0xA8 the
 * active column (0 device, 1 slot, 2 confirm), +0x33C a table whose +8/+0x18/
 * +0x28 are each column's selection, +0x340 the device items (32 bytes, +8
 * enabled; item 0 is the hard disk), +0x5CC the hard disk's record, +0x678
 * the save buffer, +0x6B8 1 for a new save.
 *
 * JSRF_OPEN_SAVE=t opens it (save mode) at t seconds, from the camera update,
 * so a test can reach it without playing to the garage. [SAVEMENU] logs every
 * change of what the menu is doing. */
static uint32_t guest_call_cdecl(void (*fn)(void), uint32_t ra, int nargs, const uint32_t *args)
{
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    uint32_t sv[8], ret;
    int top = g_fp_top, i;
    sv[0] = g_eax; sv[1] = g_ecx; sv[2] = g_edx; sv[3] = g_ebx;
    sv[4] = g_esp; sv[5] = g_ebp; sv[6] = g_esi; sv[7] = g_edi;
    for (i = nargs - 1; i >= 0; i--) { g_esp -= 4; *(uint32_t *)(m + g_esp) = args[i]; }
    g_esp -= 4; *(uint32_t *)(m + g_esp) = ra;
    fn();
    ret = g_eax;
    g_eax = sv[0]; g_ecx = sv[1]; g_edx = sv[2]; g_ebx = sv[3];
    g_esp = sv[4]; g_ebp = sv[5]; g_esi = sv[6]; g_edi = sv[7];
    g_fp_top = top;
    return ret;
}
extern void sub_000681C0(void);
extern void sub_00030490(void);
static void mission_at_step(void)
{
    static int state = -1; static double t; static uint32_t id;
    if (state < 0) {
        const char *e = getenv("JSRF_MISSION_AT"); int v = 0;
        state = (e && sscanf(e, "%lf:%i", &t, &v) == 2) ? 1 : 0; id = (uint32_t)v;
    }
    if (state != 1 || pad_now() < t || !s_mission_mgr) return;
    state = 2;
    fprintf(stderr, "[MISSION] t=%.1f asking mgr %08X for mission %08X\n", pad_now(), s_mission_mgr, id);
    { uint32_t sv_ecx = g_ecx; uint32_t a[1]; a[0] = id;
      g_ecx = s_mission_mgr;
      { uint32_t r = guest_call_cdecl(sub_00030490, 0x00030490u, 1, a);
        g_ecx = sv_ecx;
        fprintf(stderr, "[MISSION] request returned %u\n", r); } }
}
/* JSRF_OPEN_SAVE="t[:mode]" (test only): open the save menu at t seconds;
 * mode 1 opens it to LOAD, which is how a test gets a player's saved
 * progress into a run that started with New Game. */
/* Video > Widescreen toggled in play: the title sets its camera once, at
 * startup, so the projection and the culling planes are recomputed here --
 * the same two calls SetCamera makes, on the device it made them on -- the
 * frame after the switch changes. */
static uint32_t s_cam_dev;      /* the device the camera was last set on */
static int s_wide_applied = -1; /* the mode it was set for */
extern int nv2a_widescreen(void);
extern int nv2a_wide_scene(void);
extern void nv2a_set_wide_scene(int on);
extern void sub_00153A90(void);
extern void sub_00153EC0(void);
static void wide_reapply_step(void)
{
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    uint32_t sv_eax, sv_ecx, sv_edx;
    /* JSRF_WIDE_AT=<t> (test only): flip the mode at time t, as the menu would. */
    { static int st = -1; static double t;
      if (st < 0) { const char *e = getenv("JSRF_WIDE_AT");
                    st = (e && sscanf(e, "%lf", &t) == 1) ? 1 : 0; }
      if (st == 1 && pad_now() >= t) {
          extern void nv2a_set_widescreen(int on);
          st = 2;
          nv2a_set_widescreen(!nv2a_widescreen());
          fprintf(stderr, "[WIDE] t=%.1f JSRF_WIDE_AT: widescreen %s\n", pad_now(),
                  nv2a_widescreen() ? "on" : "off");
      } }
    if (!s_cam_dev || s_wide_applied < 0 || s_wide_applied == nv2a_wide_scene()) return;
    /* Only on the device the camera was set on, and only while it is still
     * one: the graphics device class, vtable 0x1E0F00. */
    if (s_cam_dev < 0x10000u || s_cam_dev + 0x60u >= (64u << 20)
     || *(uint32_t *)(m + s_cam_dev) != 0x001E0F00u) { s_cam_dev = 0; return; }
    sv_eax = g_eax; sv_ecx = g_ecx; sv_edx = g_edx;
    g_esp -= 4; *(uint32_t *)(m + g_esp) = 0;   /* each callee's ret pops it */
    g_ecx = s_cam_dev;
    sub_00153A90();
    g_esp -= 4; *(uint32_t *)(m + g_esp) = 0;
    g_ecx = s_cam_dev;
    sub_00153EC0();
    g_eax = sv_eax; g_ecx = sv_ecx; g_edx = sv_edx;
    fprintf(stderr, "[WIDE] t=%.1f camera re-set for %s\n", pad_now(),
            nv2a_wide_scene() ? "16:9" : "4:3");
}

/* JSRF_FIND_PROJ=<t> (test only): at time t, scan guest RAM for projection
 * matrices -- [a 0 0 0; 0 b 0 0; 0 0 c d; 0 0 e f] with b/a = 4/3 -- in
 * both the row-major and the transposed layout, to find where the title
 * keeps the one it draws the world with. */
static void find_proj_step(void)
{
    static int state = -1; static double t;
    const uint8_t *m = (const uint8_t *)g_xbox_mem_offset;
    uint32_t a, n = 0;
    if (state < 0) {
        const char *e = getenv("JSRF_FIND_PROJ");
        state = (e && sscanf(e, "%lf", &t) == 1) ? 1 : 0;
    }
    if (state != 1 || pad_now() < t) return;
    state = 2;
    for (a = 0x10000; a + 64 <= (64u << 20) && n < 64; a += 4) {
        const float *f = (const float *)(m + a);
        float x = f[0], y;
        if (!(x > 0.2f && x < 8.0f)) continue;
        /* row-major: m00 at 0, m11 at 5 */
        y = f[5];
        if (y > 0.0f && fabsf(y / x - 4.0f / 3.0f) < 2e-3f
            && f[1] == 0 && f[2] == 0 && f[3] == 0 && f[4] == 0 && f[6] == 0 && f[7] == 0
            && f[8] == 0 && f[9] == 0 && f[12] == 0 && f[13] == 0) {
            fprintf(stderr, "[PROJ] t=%.1f %08X: %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f\n",
                    pad_now(), a, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
                    f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
            n++;
        }
    }
    fprintf(stderr, "[PROJ] t=%.1f scan done, %u found\n", pad_now(), n);
}

static void save_menu_open_step(void)
{
    static int state = -1; static double t; static int mode;
    if (state < 0) {
        const char *e = getenv("JSRF_OPEN_SAVE");
        state = e ? 1 : 0; t = e ? atof(e) : 0;
        mode = (e && strchr(e, ':')) ? atoi(strchr(e, ':') + 1) : 0;
    }
    if (state != 1 || pad_now() < t) return;
    state = 2;
    { uint32_t a[2]; a[0] = (uint32_t)mode; a[1] = 0;
      fprintf(stderr, "[SAVEMENU] t=%.1f opening the save menu (mode %d)\n", pad_now(), mode);
      guest_call_cdecl(sub_000681C0, 0x0007D25Au, 2, a); }
}
extern void sub_00067E90_gen(void);
void sub_00067E90(void)
{
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    uint32_t o = g_ecx;
    static uint32_t last[16]; static int n;
    sub_00067E90_gen();
    if (o >= 0x10000u && o + 0x6C0u < (64u << 20) && n < 400) {
        uint32_t tab = *(uint32_t *)(m + o + 0x33C), items = *(uint32_t *)(m + o + 0x340);
        uint32_t v[16] = {0};
        v[0] = *(uint32_t *)(m + o + 0x98); v[1] = *(uint32_t *)(m + o + 0xA4);
        v[2] = *(uint32_t *)(m + o + 0xA8);
        if (tab && tab < (64u << 20)) { v[3] = *(uint32_t *)(m + tab + 8); v[4] = *(uint32_t *)(m + tab + 0x18);
                                        v[5] = *(uint32_t *)(m + tab + 0x28); }
        if (items && items < (64u << 20)) v[6] = *(uint32_t *)(m + items + 8);
        v[7] = *(uint32_t *)(m + o + 0x5D4); v[8] = *(uint32_t *)(m + o + 0x5D8);
        v[9] = *(uint32_t *)(m + o + 0x678); v[10] = *(uint32_t *)(m + o + 0x6B8);
        v[11] = *(uint32_t *)(m + 0x251EE8); v[12] = *(uint32_t *)(m + 0x251F20);
        v[13] = *(uint32_t *)(m + 0x251E1C + (*(uint32_t *)(m + 0x251F20) & 3) * 0x40);
        v[14] = *(uint32_t *)(m + o + 0x334);
        if (memcmp(v, last, sizeof v)) {
            memcpy(last, v, sizeof v); n++;
            fprintf(stderr, "[SAVEMENU] t=%.1f obj %08X state %u mode %u col %u sel %d/%d/%d hdd-item-on %u "
                    "room %u save %d buf %08X new %u | pressed %08X port %u portflag %08X ready %u\n",
                    pad_now(), o, v[0], v[1], v[2], (int)v[3], (int)v[4], (int)v[5], v[6], v[7], (int)v[8],
                    v[9], v[10], v[11], v[12], v[13], v[14]);
        }
    }
}

/* ---- list menus and the keyboard -----------------------------------------
 *
 * The game's list menus move their cursor on either input: a new d-pad
 * press moves once (and the menu returns for that frame), and on a later
 * frame a stick that has just crossed its threshold moves once more. On a
 * pad those are two different hands. On the keyboard W/A/S/D drive the
 * d-pad and the left stick together -- the skater only reads the stick, the
 * menus mostly the d-pad -- so every tap moved the cursor twice. In a
 * two-item YES/NO that is no move at all: the save prompt sat on NO, A went
 * back a column, and the game could not be saved from the keyboard.
 *
 * While a d-pad direction is held, these menus now see the stick centred:
 * the d-pad has already said where to go. Input structs: 0x251DE0 + 0x40 *
 * port, and the active port's copy at 0x251EE0; +4 held buttons (d-pad
 * 0xF000), +0x14/+0x18 stick X/Y. The skater reads angle and magnitude
 * (+0x34/+0x38), which are left alone. Only the menus that read both inputs
 * are wrapped; RECOMP_MENU_STICK_BOTH=1 turns it off. */
static void menu_stick_mute(uint32_t saved[5][2], int *mask)
{
    static int off = -1;
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    int i;
    *mask = 0;
    if (off < 0) off = getenv("RECOMP_MENU_STICK_BOTH") ? 1 : 0;
    if (off) return;
    for (i = 0; i < 5; i++) {
        uint32_t b = 0x00251DE0u + 0x40u * (uint32_t)i;
        if (*(uint32_t *)(m + b + 4) & 0xF000u) {
            saved[i][0] = *(uint32_t *)(m + b + 0x14); saved[i][1] = *(uint32_t *)(m + b + 0x18);
            *(uint32_t *)(m + b + 0x14) = 0; *(uint32_t *)(m + b + 0x18) = 0;
            *mask |= 1 << i;
        }
    }
}
static void menu_stick_restore(uint32_t saved[5][2], int mask)
{
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    int i;
    for (i = 0; i < 5; i++)
        if (mask & (1 << i)) {
            uint32_t b = 0x00251DE0u + 0x40u * (uint32_t)i;
            *(uint32_t *)(m + b + 0x14) = saved[i][0]; *(uint32_t *)(m + b + 0x18) = saved[i][1];
        }
}
#define MENU_NAV_WRAP(va) \
    extern void sub_##va##_gen(void); \
    void sub_##va(void) { uint32_t sv[5][2]; int mk; menu_stick_mute(sv, &mk); \
                          sub_##va##_gen(); menu_stick_restore(sv, mk); }
/* One per line as well as inside the macro: the pipeline's manual_scan finds
 * wrapped functions by these declarations, and emits each body as sub_X_gen
 * only if it sees one. */
extern void sub_00069040_gen(void);
extern void sub_000689A0_gen(void);
extern void sub_00077000_gen(void);
extern void sub_00079740_gen(void);
extern void sub_0007A170_gen(void);
extern void sub_0011B680_gen(void);
extern void sub_000F9C40_gen(void);
extern void sub_000FA070_gen(void);
extern void sub_00074460_gen(void);
extern void sub_0007DAE0_gen(void);
MENU_NAV_WRAP(00069040)   /* save/load menu, active port */
MENU_NAV_WRAP(000689A0)   /* save/load menu, every port */
MENU_NAV_WRAP(00077000)
MENU_NAV_WRAP(00079740)
MENU_NAV_WRAP(0007A170)
MENU_NAV_WRAP(0011B680)
MENU_NAV_WRAP(000F9C40)
MENU_NAV_WRAP(000FA070)
MENU_NAV_WRAP(00074460)
MENU_NAV_WRAP(0007DAE0)

/* ---- sub_00051FC0: the game flow's per-frame step (test only) -------------
 *
 * The flow object runs the mission sequence: +0x58 the mission id, +0x5C a
 * state indexing the step table at 0x1F9888 (0 asks for the mission file,
 * 1 waits for it, and so on). [FLOW] logs every state change;
 * JSRF_FLOW_GOTO="t:id" sets the id and state 0 at t seconds -- how a test
 * gets to a later mission without playing the ones before it. */
/* JSRF_MSCRIPT_DUMP=<mission id> (test only): when that mission's flow first
 * reaches state 14 (its setup commands) and again at the first state 15
 * after a state 19 (an event ended and set its flags), list the setup commands ([mission+0x174], count
 * +0x178) and the per-frame triggers (+0x17C, count +0x180): 0x54 bytes each,
 * a condition list at +0 (count +4) of story-flag tests, flags to set at +8
 * (+0xC), the op at +0x10 and its parameters after. Each condition is shown
 * with whether it holds now (flags object 0x1EFFB0: bank = c & 7, bit =
 * (c >> 3) & 0xFFFF, wanted value = bit 19). */
static int mscript_cond(uint32_t c)
{
    static const uint32_t bank_off[4] = { 0x6A14, 0x18, 0x58, 0x98 };
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    uint32_t bank = c & 7, bit = (c >> 3) & 0xFFFF, want = (c >> 19) & 1, w;
    if (bank > 3 || bit >= 0x200) return -1;
    w = *(uint32_t *)(m + 0x001EFFB0u + bank_off[bank] + (bit >> 5) * 4);
    return ((w >> (bit & 31)) & 1) == want;
}
static void mscript_list(const char *what, uint32_t base, uint32_t n)
{
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    uint32_t i, k;
    if (base < 0x10000u || n > 400 || base + n * 0x54u >= (64u << 20)) return;
    for (i = 0; i < n; i++) {
        const uint32_t *c = (const uint32_t *)(m + base + i * 0x54u);
        int all = 1;
        fprintf(stderr, "[MSCRIPT] %s #%u op %3u |", what, i, c[4]);
        for (k = 5; k < 21; k++) fprintf(stderr, " %X", c[k]);
        fprintf(stderr, " | if");
        if (c[1] <= 16 && c[0] >= 0x10000u && c[0] < (64u << 20))
            for (k = 0; k < c[1]; k++) {
                uint32_t cv = *(uint32_t *)(m + c[0] + 4 * k);
                int r = mscript_cond(cv);
                if (r != 1) all = 0;
                fprintf(stderr, " %u:%03X=%u(%s)", cv & 7, (cv >> 3) & 0xFFFF, (cv >> 19) & 1, r == 1 ? "y" : r == 0 ? "n" : "?");
            }
        fprintf(stderr, " -> %s | set", all ? "TRUE" : "false");
        if (c[3] <= 16 && c[2] >= 0x10000u && c[2] < (64u << 20))
            for (k = 0; k < c[3]; k++) {
                uint32_t cv = *(uint32_t *)(m + c[2] + 4 * k);
                fprintf(stderr, " %u:%03X=%u", cv & 7, (cv >> 3) & 0xFFFF, (cv >> 19) & 1);
            }
        fprintf(stderr, "\n");
    }
}
static void mscript_dump(uint32_t flow, uint32_t state)
{
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    uint32_t ms = *(uint32_t *)(m + flow + 0x1040), b;
    if (ms < 0x10000u || ms + 0x200u >= (64u << 20)) return;
    fprintf(stderr, "[MSCRIPT] ---- t=%.1f mission %08X state %u: %u setup commands (pc %u), %u triggers ----\n",
            pad_now(), *(uint32_t *)(m + flow + 0x58), state, *(uint32_t *)(m + ms + 0x178),
            *(uint32_t *)(m + flow + 0x2B0), *(uint32_t *)(m + ms + 0x180));
    for (b = 0; b < 4; b++) {
        static const uint32_t bank_off[4] = { 0x6A14, 0x18, 0x58, 0x98 };
        const uint32_t *w = (const uint32_t *)(m + 0x001EFFB0u + bank_off[b]);
        int k;
        fprintf(stderr, "[MSCRIPT] flags bank %u:", b);
        for (k = 0; k < 16; k++) fprintf(stderr, " %08X", w[k]);
        fprintf(stderr, "\n");
    }
    mscript_list("setup", *(uint32_t *)(m + ms + 0x174), *(uint32_t *)(m + ms + 0x178));
    mscript_list("trig", *(uint32_t *)(m + ms + 0x17C), *(uint32_t *)(m + ms + 0x180));
    fflush(stderr);
}
extern void nv2a_set_wide_scene(int on);
/* What the flow last said about widescreen (-1 nothing yet: the boot logos,
 * widened, see jsrf_wide_boot). */
static int s_wide_scene_said = -1;
static int wide_log_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("JSRF_WIDE_LOG") ? 1 : 0;
    return on;
}
/* The boot logos (SEGA, Smilebit, Dolby) are widened like the game: each is
 * an overlay on a cleared screen, so the logo keeps its shape and its
 * background fills the 16:9 screen, where kept 4:3 it sat between black bars.
 * JSRF_WIDE_BOOT=0 keeps them 4:3. */
__attribute__((constructor)) static void jsrf_wide_boot(void)
{
    const char *v = getenv("JSRF_WIDE_BOOT");
    nv2a_set_wide_scene(v && v[0] == '0' ? 0 : 1);
}
/* ---- the map selector ------------------------------------------------------
 * Every platform: straight to a map, to skate it freely (the maps, each with
 * its chapter 9 mission, are src/nv2a_gl/jsrf_maps.h's). A map comes from the
 * window's menu (Mac, Windows) or the app's settings (iPhone, Android) through
 * nv_window_take_map_request(), or from the panel: -1 asks for it (F6 on
 * Windows and Linux, the Maps menu's Map Selector), a list drawn as photo
 * mode's help is, gone through with the pad (up/down or a stick, A or Start
 * to go, B or Back to close) while the game sees an idle pad.
 *
 * The map asked for is gone to the way a mission ends -- +0x4C/+0x54 the next
 * chapter and number, state 93 -- at the flow's next frame in play (state 15)
 * or once Start has been pressed at the title (mission 0x10001, state 31),
 * the state held for a second (0x10001 passes through 15 on its way to 31:
 * gone from there, the hand-over never finished). Asked for while a map
 * loads, or at the title itself (mission 0x5A: the opening and its demo,
 * whose flow takes its state back and never hands over -- the game hung), it
 * waits until then. Photo mode is left first.
 * JSRF_MAP_TOUR="t:id,t:id,..." (test only) asks for each id at its time;
 * JSRF_MAP_PANEL_TEST="t:downs" opens the panel at t and goes down that many. */
#include "../src/nv2a_gl/jsrf_maps.h"
static void photo_leave(const char *why);
int  nv_window_take_map_request(void);
void nv_window_photo_help(const char *text);
static struct {
    volatile int open, want_open;
    volatile int pick;                 /* a mission chosen in it, for the flow */
    int sel, fresh, dir_was;
    uint8_t go_was, close_was;
    double next_move;
} s_picker;
static void picker_show(void)
{
    char text[640];
    int k, n;
    n = snprintf(text, sizeof text, "GO TO A MAP\n");
    for (k = 0; k < JSRF_MAP_COUNT && n < (int)sizeof text - 48; k++)
        n += snprintf(text + n, sizeof text - n, "%s %s\n", k == s_picker.sel ? ">" : " ", k_jsrf_maps[k].name);
    snprintf(text + n, sizeof text - n, "UP/DOWN CHOOSE   A GO   B CLOSE");
    nv_window_photo_help(text);
}
static void picker_close(void)
{
    s_picker.open = 0;
    nv_window_photo_help(NULL);
}
/* The pad, before the game sees it. Returns 1 when the panel took it. */
static int picker_note_pad(XBOX_INPUT_STATE *s)
{
    double now = jsrf_now();
    int up, down, dir;
    uint8_t go, close;
    if (s_picker.want_open) {
        s_picker.want_open = 0;
        photo_leave("the map selector");
        s_picker.open = 1;
        s_picker.fresh = 1;
        picker_show();
    }
    if (!s_picker.open) return 0;
    up = (s->Gamepad.wButtons & XBOX_GAMEPAD_DPAD_UP) || s->Gamepad.sThumbLY > 16000 || s->Gamepad.sThumbRY > 16000;
    down = (s->Gamepad.wButtons & XBOX_GAMEPAD_DPAD_DOWN) || s->Gamepad.sThumbLY < -16000 || s->Gamepad.sThumbRY < -16000;
    dir = up ? -1 : down ? 1 : 0;
    go = s->Gamepad.bAnalogButtons[XBOX_BUTTON_A] > 64 || (s->Gamepad.wButtons & XBOX_GAMEPAD_START);
    close = s->Gamepad.bAnalogButtons[XBOX_BUTTON_B] > 64 || (s->Gamepad.wButtons & XBOX_GAMEPAD_BACK);
    if (s_picker.fresh) {              /* what was held as it opened does nothing */
        s_picker.fresh = 0;
        s_picker.go_was = go; s_picker.close_was = close; s_picker.dir_was = dir;
        s_picker.next_move = now + 0.4;
    }
    if (dir && (dir != s_picker.dir_was || now >= s_picker.next_move)) {
        s_picker.sel = (s_picker.sel + dir + JSRF_MAP_COUNT) % JSRF_MAP_COUNT;
        s_picker.next_move = now + (dir != s_picker.dir_was ? 0.4 : 0.12);
        picker_show();
    }
    s_picker.dir_was = dir;
    if (go && !s_picker.go_was) {
        s_picker.pick = k_jsrf_maps[s_picker.sel].mission;
        fprintf(stderr, "[MAP] the panel: %s\n", k_jsrf_maps[s_picker.sel].name);
        picker_close();
    } else if (close && !s_picker.close_was) {
        picker_close();
    }
    s_picker.go_was = go; s_picker.close_was = close;
    memset(&s->Gamepad, 0, sizeof s->Gamepad);   /* the game sees an idle pad */
    return 1;
}
static uint32_t map_request(void)
{
    static int n = -1, i, ptest = -1, pdowns; static double pt;
    static double at[32]; static uint32_t id[32];
    int r = nv_window_take_map_request();
    if (n < 0) {
        const char *e = getenv("JSRF_MAP_TOUR");
        n = 0;
        while (e && *e && n < 32) {
            char *end; double t = strtod(e, &end);
            if (end == e || *end != ':') break;
            at[n] = t; id[n] = (uint32_t)strtoul(end + 1, &end, 0); n++;
            e = (*end == ',') ? end + 1 : end;
        }
        e = getenv("JSRF_MAP_PANEL_TEST");
        ptest = (e && sscanf(e, "%lf:%d", &pt, &pdowns) == 2) ? 1 : 0;
    }
    if (ptest == 1 && pad_now() >= pt) {
        ptest = 0;
        s_picker.sel = pdowns % JSRF_MAP_COUNT;
        r = -1;
    }
    if (r == -1) { s_picker.want_open = 1; r = 0; }
    if (!r && s_picker.pick) { r = s_picker.pick; s_picker.pick = 0; }
    if (!r && i < n && pad_now() >= at[i]) r = (int)id[i++];
    return (uint32_t)r;
}
extern void sub_00051FC0_gen(void);
void sub_00051FC0(void)
{
    static int init = -1, log_on; static double t; static uint32_t id, last_state = 0xFFFFFFFFu;
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    uint32_t flow = g_ecx;
    if (init < 0) {
        const char *e = getenv("JSRF_FLOW_GOTO"); int v = 0;
        init = (e && sscanf(e, "%lf:%i", &t, &v) == 2) ? 1 : 0; id = (uint32_t)v;
        log_on = (init || getenv("JSRF_FLOW_LOG")) ? 1 : 0;
    }
    if (flow >= 0x10000u && flow + 0x1100u < (64u << 20)) {
        /* JSRF_GMVAR_SET="t:idx=val[,idx=val...]" (test only): write game
         * manager variables (+0x7868 + 4*idx) once, at t seconds. */
        { static int gs = -1; static double gt; static char spec[256];
          if (gs < 0) {
              const char *e = getenv("JSRF_GMVAR_SET"); const char *c;
              gs = 0;
              if (e && (c = strchr(e, ':')) != NULL) { gt = atof(e); snprintf(spec, sizeof spec, "%s", c + 1); gs = 1; }
          }
          if (gs == 1 && pad_now() >= gt) {
              uint32_t gm = *(uint32_t *)(m + 0x0022FCE0u);
              char *p = spec;
              gs = 2;
              while (gm >= 0x10000u && p && *p) {
                  char *end; uint32_t idx = (uint32_t)strtoul(p, &end, 0), val;
                  if (*end != '=') break;
                  val = (uint32_t)strtoul(end + 1, &end, 0);
                  if (idx < 0x800) {
                      fprintf(stderr, "[GMVAR] t=%.1f var %03X: %08X -> %08X\n", pad_now(), idx,
                              *(uint32_t *)(m + gm + 0x7868u + 4u * idx), val);
                      *(uint32_t *)(m + gm + 0x7868u + 4u * idx) = val;
                  }
                  p = (*end == ',') ? end + 1 : NULL;
              }
          } }
        { static uint32_t want, seen_flow, seen_st = 0xFFFFFFFFu; static double seen_t;
          uint32_t r = map_request(), st = *(uint32_t *)(m + flow + 0x5C);
          uint32_t mi = *(uint32_t *)(m + flow + 0x58);
          if (r) want = r;
          if (flow != seen_flow || st != seen_st) { seen_flow = flow; seen_st = st; seen_t = pad_now(); }
          /* in play (15, or 31 just past the title), for a second at least
           * (a flow can pass through 15 on its way), never the title's own */
          if (want && (st == 15 || st == 31) && mi != 0x5Au && pad_now() - seen_t >= 1.0) {
              photo_leave("a map was chosen");
              fprintf(stderr, "[MAP] t=%.1f flow %08X: state %u mission %08X -> mission %08X\n",
                      pad_now(), flow, st, *(uint32_t *)(m + flow + 0x58), want);
              *(uint32_t *)(m + flow + 0x4C) = want >> 16;
              *(uint32_t *)(m + flow + 0x54) = want & 0xFFFF;
              *(uint32_t *)(m + flow + 0x5C) = 93;
              want = 0;
          } }
        if (init == 1 && pad_now() >= t) {
            /* The way a mission ends: +0x4C/+0x54 the next chapter and
             * number, state 93 creates the next flow (task 0xA, 0x4E9B0)
             * and hands over to it once it has loaded. */
            init = 2;
            fprintf(stderr, "[FLOW] t=%.1f flow %08X: state %u mission %08X -> next %u/%u via state 93\n",
                    pad_now(), flow, *(uint32_t *)(m + flow + 0x5C), *(uint32_t *)(m + flow + 0x58),
                    id >> 16, id & 0xFFFF);
            *(uint32_t *)(m + flow + 0x4C) = id >> 16;
            *(uint32_t *)(m + flow + 0x54) = id & 0xFFFF;
            *(uint32_t *)(m + flow + 0x5C) = 93;
        }
        { static int ms_init = -1; static uint32_t ms_id; static uint32_t ms_done;
          uint32_t st = *(uint32_t *)(m + flow + 0x5C);
          if (ms_init < 0) { const char *e = getenv("JSRF_MSCRIPT_DUMP"); ms_init = e ? 1 : 0; ms_id = e ? (uint32_t)strtoul(e, NULL, 0) : 0; }
          /* at 14 before the setup runs, and at the first 15 after a 19 --
           * once the event's own flags have been set */
          if (ms_init && *(uint32_t *)(m + flow + 0x58) == ms_id) {
              if (st == 19) ms_done |= 2;
              if ((st == 14 && !(ms_done & 1)) || (st == 15 && (ms_done & 6) == 2)) {
                  ms_done |= st == 14 ? 1 : 4;
                  mscript_dump(flow, st);
              }
          } }
        /* Widescreen by scene: the title (mission 0x5A, the opening, the
         * attract demo and its menu) stays 4:3, pillarboxed -- its opening
         * is a composition drawn for a 4:3 screen: widened, its bars and its
         * giant logo come apart (some are overlays and are squeezed, some
         * are not, and black stripes open between them), and its overlays
         * park things just past the 4:3 edges. JSRF_WIDE_TITLE=1 widens it
         * anyway, to look. Every mission after it is widened. A flow
         * handing over to the next one (states 93/94) has no say. */
        { uint32_t mi = *(uint32_t *)(m + flow + 0x58), st = *(uint32_t *)(m + flow + 0x5C);
          if (st != 93 && st != 94) {
              static int title_wide = -1;
              int want;
              if (title_wide < 0) title_wide = getenv("JSRF_WIDE_TITLE") ? 1 : 0;
              want = (mi != 0x5Au) || title_wide;
              if (want != s_wide_scene_said) {
                  s_wide_scene_said = want;
                  nv2a_set_wide_scene(want);
                  if (wide_log_on())
                      fprintf(stderr, "[WIDE] t=%.1f mission %08X: %s\n", pad_now(), mi,
                              want ? "widened" : "kept 4:3 (pillarboxed)");
              }
          } }
        if (log_on && *(uint32_t *)(m + flow + 0x5C) != last_state) {
            last_state = *(uint32_t *)(m + flow + 0x5C);
            fprintf(stderr, "[FLOW] t=%.1f state %u mission %08X\n", pad_now(), last_state,
                    *(uint32_t *)(m + flow + 0x58));
        }
    }
    sub_00051FC0_gen();
}

/* ---- sub_0018E584: D3D's DAC gamma-ramp upload (CMiniport, runs in the
 * vblank DPC via sub_00193D10 when a SetGammaRamp is pending): writes the
 * 256-entry red, green and blue tables (768 bytes at the argument) to the
 * VGA DAC ports 0x6813C8/9 -- which on the console the display applies at
 * scan-out. JSRF_GAMMA_LOG=1: log each ramp (identity or not, samples). */
extern void sub_0018E584_gen(void);
void sub_0018E584(void)
{
    static int on = -1;
    if (on < 0) on = getenv("JSRF_GAMMA_LOG") ? 1 : 0;
    if (on) {
        uint8_t *m = (uint8_t *)g_xbox_mem_offset;
        uint32_t p = GARG(1);
        if (p >= 0x10000u && p + 0x300u < (64u << 20)) {
            const uint8_t *r = m + p;
            int i, c, ident = 1;
            for (c = 0; c < 3; c++) for (i = 0; i < 256; i++) if (r[c * 256 + i] != i) ident = 0;
            fprintf(stderr, "[GAMMA] t=%.2f ramp at %08X from %08X: %s |", pad_now(), p, GARG(0), ident ? "identity" : "NOT identity");
            for (c = 0; c < 3; c++) {
                static const int at[] = { 0, 16, 32, 64, 96, 128, 160, 192, 224, 255 };
                fprintf(stderr, " %c:", "RGB"[c]);
                for (i = 0; i < 10; i++) fprintf(stderr, " %u", r[c * 256 + at[i]]);
            }
            fprintf(stderr, "\n");
            fflush(stderr);
        }
    }
    sub_0018E584_gen();
}

/* ---- the player spawner (test only) --------------------------------------
 *
 * Task 0x1CD5C8's update (sub_000A8370) makes the players: for each of 32
 * slots it reads the game manager's variables (+0x7868 + 4*idx) -- 0x6D+i
 * the spawn request, 0x2D+i, 0x8D+i, 0xAD+i, 0xCD+i -- and the object table
 * (+0x98 + 4*idx) entry 0x0C+i; with a request and no object it builds a
 * player manager (sub_000A6980, vtable 0x1CD570, which builds the player,
 * vtable 0x1CCFF8), then clears 0x6D+i and 0x8D+i. JSRF_PLAYER_LOG=1 logs
 * those (and 0x0D+i, the slot's character: -1 and op 72 makes no player;
 * 0x14D+i) for the first four slots whenever one changes, and every player
 * manager made or destroyed -- to see whether a stage that comes up
 * without a player was never asked for one or lost it. */
static int player_log_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("JSRF_PLAYER_LOG") ? 1 : 0;
    return on;
}
/* The game manager's variable store, sub_000127C0(idx, value) -- with
 * JSRF_PLAYER_LOG, name whoever writes a spawn request (0x6D..0x70). */
extern void sub_000127C0_gen(void);
void sub_000127C0(void)
{
    if (player_log_on()) {
        uint32_t idx = GARG(1);
        if (idx >= 0x6D && idx <= 0x70) {
            uint8_t *m = (uint8_t *)g_xbox_mem_offset;
            const uint32_t *sp = (const uint32_t *)(m + g_esp);
            int i, n = 0;
            fprintf(stderr, "[PLAYER] t=%.2f set var %03X = %08X from %08X; stack:", pad_now(), idx, GARG(2), GARG(0));
            for (i = 3; i < 160 && n < 10; i++)
                if (sp[i] > 0x11000 && sp[i] < 0x18CB40) { fprintf(stderr, " %08X", sp[i]); n++; }
            fprintf(stderr, "\n");
        }
    }
    sub_000127C0_gen();
}
extern void sub_000A8370_gen(void);
void sub_000A8370(void)
{
    if (player_log_on()) {
        static uint32_t last[4][7]; static int init;
        uint8_t *m = (uint8_t *)g_xbox_mem_offset;
        uint32_t gm = *(uint32_t *)(m + 0x0022FCE0u), i, k;
        static const uint32_t var[6] = { 0x6D, 0x2D, 0x0D, 0xAD, 0x14D, 0x0B };
        if (!init) { init = 1; memset(last, 0xFF, sizeof last); }
        if (gm >= 0x10000u && gm + 0x8000u < (64u << 20))
            for (i = 0; i < 4; i++) {
                uint32_t now[7];
                for (k = 0; k < 6; k++) now[k] = *(uint32_t *)(m + gm + 0x7868u + 4u * (var[k] + (k < 5 ? i : 0)));
                now[6] = *(uint32_t *)(m + gm + 0x98u + 4u * (0x0Cu + i));
                if (memcmp(now, last[i], sizeof now)) {
                    fprintf(stderr, "[PLAYER] t=%.2f slot %u: req(6D) %08X 2D %08X char(0D) %08X AD %08X 14D %08X | obj(0C) %08X | var0B %08X\n",
                            pad_now(), i, now[0], now[1], now[2], now[3], now[4], now[6], now[5]);
                    memcpy(last[i], now, sizeof now);
                }
            }
    }
    sub_000A8370_gen();
}
extern void sub_000A6980_gen(void);
void sub_000A6980(void)
{
    if (player_log_on())
        fprintf(stderr, "[PLAYER] t=%.2f create manager %08X (args %08X %08X %08X %08X) from %08X\n",
                pad_now(), g_ecx, GARG(1), GARG(2), GARG(3), GARG(4), GARG(0));
    sub_000A6980_gen();
}
extern void sub_000A6110_gen(void);
void sub_000A6110(void)
{
    if (player_log_on())
        fprintf(stderr, "[PLAYER] t=%.2f destroy manager %08X from %08X\n", pad_now(), g_ecx, GARG(0));
    sub_000A6110_gen();
}

/* ---- sub_00116E30: the sound manager's per-frame update; counts frames.
 *
 * JSRF_VOICE_LOG=1 (test only): the speech slots, frame by frame. The
 * sound manager (task 5) keeps two ADXTs for speech: +0x6C with its state
 * at +0xC4 and +0x70 with +0xC0 (1 loading, 2 ready or playing, 0 done).
 * The talk boxes use +0x70 alone (sub_00117140). An event alternates the
 * two (sub_00117280): it loads the next line paused into one slot while
 * the other plays, and unpauses it when the playing line ends -- the DJ K
 * scene's seventeen lines run back to back that way. The ADXT's status is
 * the byte at +1 (3 playing, 4 decoded, 5 ended), its pause flag +0x72.
 * [VOICE] lines give the time, the game frame and the present count for
 * every change, so a line's real start and end can be set against the
 * frames captured while it plays. */
extern uint32_t nv2a_gl_flips(void);
extern void sub_0013A9A0(void);
static int voice_log_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("JSRF_VOICE_LOG") ? 1 : 0;
    return on;
}
static uint32_t s_sndmgr;   /* the sound manager (task 5), seen by its update */
extern void sub_00116E30_gen(void);
void sub_00116E30(void)
{
    /* two voice slots: +0x6C with its state at +0xC4, +0x70 with +0xC0 */
    static const uint32_t hoff[2] = { 0x6C, 0x70 }, soff[2] = { 0xC4, 0xC0 };
    static uint32_t last[2][4];
    static double next_level[2];
    static int init;
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    uint32_t o = g_ecx;
    int k;
    s_game_frames++;
    sub_00116E30_gen();
    if (!voice_log_on() || o < 0x10000u || o + 0x200u >= (64u << 20)) return;
    s_sndmgr = o;
    if (!init) { init = 1; memset(last, 0xFF, sizeof last); }
    for (k = 0; k < 2; k++) {
        uint32_t vs = *(uint32_t *)(m + o + soff[k]);
        uint32_t h = *(uint32_t *)(m + o + hoff[k]);
        uint32_t st, pz, f70;
        if (h < 0x10000u || h + 0x100u >= (64u << 20)) continue;
        st = (uint32_t)(int32_t)(int8_t)m[h + 1];
        pz = m[h + 0x72];
        f70 = m[h + 0x70] | (m[h + 0x6C] << 8);
        if (vs != last[k][0] || st != last[k][1] || pz != last[k][2] || f70 != last[k][3]) {
            fprintf(stderr, "[VOICE] t=%.3f frame %u flip %u slot %X: state %u adxt %08X status %d pause %u flags %04X\n",
                    pad_now(), s_game_frames, nv2a_gl_flips(), hoff[k], vs, h, (int)st, pz, f70);
            last[k][0] = vs; last[k][1] = st; last[k][2] = pz; last[k][3] = f70;
        }
        /* how full the decoded-sample buffer is, twice a second while the
         * line is live: rising means the output is not taking samples */
        if ((int)st >= 1 && (int)st <= 4 && pad_now() >= next_level[k]) {
            uint32_t a[2]; a[0] = h; a[1] = 0;
            next_level[k] = pad_now() + 0.5;
            fprintf(stderr, "[VOICE] t=%.3f slot %X obuf %u samples\n", pad_now(), hoff[k],
                    guest_call_cdecl(sub_0013A9A0, 0x00116C5Cu, 2, a));
        }
    }
}

/* ---- speech: sub_00117140 plays a line for the talk boxes, sub_00117280
 * one for an event (arg 1 the slot, arg 2 the line), sub_00116810 stops
 * the voice, sub_0013AA80 is ADXT_Pause. Logged with JSRF_VOICE_LOG=1. */
extern void sub_00117140_gen(void);
void sub_00117140(void)
{
    if (voice_log_on())
        fprintf(stderr, "[VOICE] t=%.3f frame %u flip %u play id %u from %08X\n",
                pad_now(), s_game_frames, nv2a_gl_flips(), GARG(1), GARG(0));
    sub_00117140_gen();
}
extern void sub_00117280_gen(void);
void sub_00117280(void)
{
    if (voice_log_on())
        fprintf(stderr, "[VOICE] t=%.3f frame %u flip %u event play slot %u line %u from %08X\n",
                pad_now(), s_game_frames, nv2a_gl_flips(), GARG(1), GARG(2), GARG(0));
    sub_00117280_gen();
}
extern void sub_00116810_gen(void);
void sub_00116810(void)
{
    if (voice_log_on())
        fprintf(stderr, "[VOICE] t=%.3f frame %u flip %u stop from %08X\n",
                pad_now(), s_game_frames, nv2a_gl_flips(), GARG(0));
    sub_00116810_gen();
}
extern void sub_0013AA80_gen(void);
void sub_0013AA80(void)
{
    if (voice_log_on() && s_sndmgr) {
        uint8_t *m = (uint8_t *)g_xbox_mem_offset;
        uint32_t h = GARG(1);
        if (h == *(uint32_t *)(m + s_sndmgr + 0x6C) || h == *(uint32_t *)(m + s_sndmgr + 0x70))
            fprintf(stderr, "[VOICE] t=%.3f frame %u pause %08X %u from %08X\n",
                    pad_now(), s_game_frames, h, GARG(2), GARG(0));
    }
    sub_0013AA80_gen();
}

/* ---- sub_001912A0: D3D's KickOff -------------------------------------------
 *
 * Hands the push buffer to the GPU: sets the write-combine flush bit
 * (+0x100410, 0x10000), spins until the GPU clears it, then writes the new
 * DMA_PUT. The GPU here is the ack thread in xbox_memory_layout.c, which
 * polled every 200 us, so each kickoff waited out a poll: in the DJ K scene
 * the game thread spent 84% of its time in this spin and the scene ran at
 * 42-56 fps. While KickOff runs the ack thread now polls without sleeping,
 * and it is woken once more for the new DMA_PUT. RECOMP_NV2A_KICK=0 = old. */
extern void xbox_nv2a_kick(void);
extern void xbox_nv2a_hurry(int on);
extern void sub_001912A0_gen(void);
void sub_001912A0(void)
{
    xbox_nv2a_hurry(1);
    sub_001912A0_gen();
    xbox_nv2a_hurry(0);
    xbox_nv2a_kick();
}

/* ---- sub_00194210: D3D's PGRAPH interrupt handler --------------------------
 *
 * CMiniport::ServiceGrInterrupt, called from the NV2A DPC (0x00194480) when
 * PMC_INTR_0 says PGRAPH. It reads PGRAPH_INTR (+0x400100) and the trap
 * registers (+0x400704 method, +0x400708 parameter, +0x400108 source), writes
 * PGRAPH_INTR back to acknowledge -- write-1-to-clear on the console -- and
 * for a NOP with a parameter calls the software-method dispatcher
 * (sub_00193F70; 5 = KeSetEvent on the push buffer's busy-block event). It
 * returns PGRAPH_INTR as it reads it afterwards, and the DPC loops for as
 * long as that is not zero.
 *
 * The runtime delivers those software methods now (kernel_pgraph_tick in
 * xboxrecomp's kernel_bridge.c), and the register here is memory: the write
 * that would have cleared the bits keeps them, so the DPC would call this
 * again and again. What it read, it acknowledged; that is cleared once it
 * returns, PMC_INTR_0's PGRAPH bit goes with it, the return value is the
 * register as the console would read it then, and the runtime may deliver the
 * next one. The hang this ends: the make-space wait at 0x0019151B, for good. */
int nv2a_title_acks_pgraph(void) { return 1; }
extern void sub_00194210_gen(void);
extern void xbox_nv2a_software_method_done(void);
void sub_00194210(void)
{
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    volatile uint32_t *intr = (volatile uint32_t *)(m + 0xFD400100u);
    volatile uint32_t *pmc  = (volatile uint32_t *)(m + 0xFD000100u);
    uint32_t seen = *intr;
    sub_00194210_gen();
    *intr &= ~seen;
    /* Atomic: the timer thread sets PMC_INTR_0's bits (the next software
     * method's, a vblank's) while this runs on the game's thread; a plain
     * read-modify-write could wipe a bit set in between, and a software
     * method whose bit is gone is never acknowledged: the make-space wait at
     * 0x0019151B sleeps for good. */
    if (!*intr) __atomic_fetch_and(pmc, ~0x1000u, __ATOMIC_ACQ_REL);
    g_eax = *intr;
    if (seen & 0x00100000u) xbox_nv2a_software_method_done();
}

/* ---- sub_001910E0: how far the GPU is behind the title (test only) ---------
 *
 * D3D's make-space path (0x00191440) inserts NOP(5), kicks, and asks this how
 * much of the push buffer the GPU still has to run before it reaches that
 * point: under 32 KB it spins; 32 KB or more it sleeps until the GPU raises
 * software method 5 -- the branch that froze the game for good while nothing
 * delivered software methods (see sub_00194210 above). Taken only when the
 * GPU thread happens to be far behind, so only now and then.
 *
 * JSRF_TEST_MAKESPACE_WAIT=1 takes it every time: the answer to the call from
 * 0x001914C6 is never below 32 KB. With the software methods delivered the
 * game plays on; with RECOMP_NV2A_SWM=0 it stops at the first make-space --
 * the 3 October freeze, on demand. */
extern void sub_001910E0_gen(void);
void sub_001910E0(void)
{
    static int force = -1;
    uint32_t from = GARG(0);
    if (force < 0) {
        const char *e = getenv("JSRF_TEST_MAKESPACE_WAIT");
        force = (e && e[0] == '1') ? 1 : 0;
    }
    sub_001910E0_gen();
    if (force && from == 0x001914C6u && g_eax < 0x8000u) {
        static unsigned long n;
        if (n++ < 3)
            fprintf(stderr, "  [TEST] make-space: sleeping branch forced (#%lu)\n", n);
        g_eax = 0x8000u;
    }
}

/* ---- sub_00013A80: one pass of the game's main loop (test only log) ------
 * JSRF_FRAME_LOG=1: once a second, how many passes ran, how many of them
 * presented (+0x74 set = no Present this pass) and how many waited a vblank
 * (+0x94), and the vblank counter the game keeps at 0x265174. */
extern void sub_00013A80_gen(void);
void sub_00013A80(void)
{
    static int on = -1; static unsigned n, nopres, waitv; static double next; static uint32_t v0;
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    uint32_t o = g_ecx;
    uint32_t w94 = (o > 0x10000u && o < (64u << 20)) ? *(uint32_t *)(m + o + 0x94) : 0;
    if (on < 0) on = getenv("JSRF_FRAME_LOG") ? 1 : 0;
    sub_00013A80_gen();
    if (!on || o < 0x10000u || o >= (64u << 20)) return;
    n++;
    if (*(uint32_t *)(m + o + 0x74)) nopres++;
    if (w94) waitv++;
    if (pad_now() >= next) {
        uint32_t v = *(uint32_t *)(m + 0x265174);
        fprintf(stderr, "[FRAMES] t=%.1f passes %u, no-present %u, vblank-wait %u, vblanks %u, sound updates %u\n",
                pad_now(), n, nopres, waitv, v - v0, s_game_frames);
        n = nopres = waitv = 0; v0 = v; next = pad_now() + 1.0;
    }
}

extern void sub_00030490_gen(void);
void sub_00030490(void) { mission_hook(".bin"); sub_00030490_gen(); }
extern void sub_00030690_gen(void);
void sub_00030690(void) { mission_hook(".dat"); sub_00030690_gen(); }

/* JSRF_TRACE_AT="t:frames[:shots,every][;t:frames[:shots,every]...]" (test
 * only): at each time t, log every render pass for that many frames and
 * capture a burst of frames -- up to eight of them, in time order, so one
 * run can photograph the title, the menus and the level. */
static void trace_at_step(void)
{
    static int n = -1, cur; static double t[32]; static int frames[32], shots[32], every[32];
    if (n < 0) {
        const char *e = getenv("JSRF_TRACE_AT");
        n = 0;
        while (e && *e && n < 32) {
            shots[n] = 0; every[n] = 1;
            if (sscanf(e, "%lf:%d:%d,%d", &t[n], &frames[n], &shots[n], &every[n]) >= 2) n++;
            e = strchr(e, ';');
            if (e) e++;
        }
    }
    if (cur >= n || pad_now() < t[cur]) return;
    fprintf(stderr, "[TRACE] t=%.1f tracing %d frames, %d shots every %d\n", pad_now(),
            frames[cur], shots[cur], every[cur]);
    nv2a_gl_trace_frames(frames[cur]);
    if (shots[cur] > 0) nv2a_gl_dump_burst(shots[cur], every[cur]);
    cur++;
}
static void auto_step(const float *camf, const float *chf)
{
    static double next_say;
    double now, dx, dz, dist, fx, fz, fl, rx, rz, sx, sy, sl, scale;
    int i, cur = -1;
    if (s_auto_n < 0) auto_load();
    if (s_auto_n <= 0) return;
    now = pad_now();
    for (i = 0; i < s_auto_n; i++)
        if (s_auto[i].state != 2) { if (now >= s_auto[i].t) cur = i; break; }
    if (cur < 0) { s_auto_live = 0; return; }
    dx = s_auto[cur].x - chf[0xCA4 / 4];
    dz = s_auto[cur].z - chf[0xCAC / 4];
    dist = sqrt(dx * dx + dz * dz);
    if (s_auto[cur].state == 0) {
        s_auto[cur].state = 1; s_auto[cur].t_on = now;
        fprintf(stderr, "[AUTO] t=%.1f waypoint %d: heading for (%.0f, %.0f) from (%.1f, %.1f), %.0f units\n",
                now, cur, s_auto[cur].x, s_auto[cur].z, chf[0xCA4 / 4], chf[0xCAC / 4], dist);
    }
    if (dist < 12.0 || now - s_auto[cur].t_on > 40.0) {
        s_auto[cur].state = 2; s_auto_live = 0; s_auto[cur].t_done = now;
        fprintf(stderr, "[AUTO] t=%.1f waypoint %d %s at (%.1f %.1f %.1f)\n", now, cur,
                dist < 12.0 ? "reached" : "GAVE UP", chf[0xCA4 / 4], chf[0xCA8 / 4], chf[0xCAC / 4]);
        return;
    }
    fx = camf[0x5C / 4] - camf[0x44 / 4];
    fz = camf[0x64 / 4] - camf[0x4C / 4];
    fl = sqrt(fx * fx + fz * fz);
    if (!(fl > 1e-3)) { s_auto_live = 0; return; }
    fx /= fl; fz /= fl;
    rx = fz; rz = -fx;                          /* left-handed: right of forward */
    sx = (dx * rx + dz * rz) / dist;
    sy = (dx * fx + dz * fz) / dist;
    sl = sqrt(sx * sx + sy * sy);
    if (!(sl > 1e-6)) { s_auto_live = 0; return; }
    scale = 32767.0 / sl;
    if (dist < 40.0) scale *= (dist < 16.0 ? 0.4 : dist / 40.0);
    s_auto_lx = (int16_t)(sx * scale);
    s_auto_ly = (int16_t)(sy * scale);
    s_auto_live = 1;
    if (now >= next_say) {
        next_say = now + 1.0;
        fprintf(stderr, "[AUTO] t=%.1f at (%.1f %.1f %.1f) -> (%.0f, %.0f) %.0f units, stick (%d, %d)\n",
                now, chf[0xCA4 / 4], chf[0xCA8 / 4], chf[0xCAC / 4], s_auto[cur].x, s_auto[cur].z,
                dist, s_auto_lx, s_auto_ly);
    }
}
/* Host pad state for a port: real pad if attached, else the script. */
static int pad_read(uint32_t port, XBOX_INPUT_STATE *st)
{
    static int inited; static uint32_t packet;
    if (!inited) { inited = 1; xbox_InputInit(); pad_script_load(); if (s_auto_n < 0) auto_load(); }
    { static int polls; if (++polls == 1 || polls == 600)
        fprintf(stderr, "[PAD] title polled the pad (%d times, t=%.1fs)\n",
                polls, pad_now()); }
    memset(st, 0, sizeof *st);
    trace_at_step();
    if (port == 0 && (s_pad_script_n > 0 || s_auto_n > 0)) {
        double now = pad_now();
        for (int i = 0; i < s_pad_script_n; i++)
            if (now >= s_pad_script[i].t && now < s_pad_script[i].t + s_pad_script[i].dur) {
                st->Gamepad.wButtons |= s_pad_script[i].buttons;
                if (s_pad_script[i].has_axis) {
                    if (s_pad_script[i].axis[0]) st->Gamepad.sThumbLX = s_pad_script[i].axis[0];
                    if (s_pad_script[i].axis[1]) st->Gamepad.sThumbLY = s_pad_script[i].axis[1];
                    if (s_pad_script[i].axis[2]) st->Gamepad.sThumbRX = s_pad_script[i].axis[2];
                    if (s_pad_script[i].axis[3]) st->Gamepad.sThumbRY = s_pad_script[i].axis[3];
                }
                for (int k = 0; k < 8; k++) if (s_pad_script[i].analog[k]) st->Gamepad.bAnalogButtons[k] = s_pad_script[i].analog[k];
                /* A scheduled press that is never polled looks exactly like
                 * a press the title ignored, and the two want completely
                 * different fixes. Say which one happened, once per entry. */
                if (!s_pad_script[i].said) {
                    s_pad_script[i].said = 1;
                    fprintf(stderr, "[PAD] %s delivered at %.1fs\n",
                            s_pad_script[i].name, now);
                }
            }
        if (s_auto_live) { st->Gamepad.sThumbLX = s_auto_lx; st->Gamepad.sThumbLY = s_auto_ly; }
        /* JSRF_TALK_AUTO_A=<t> (t > 1): stop at t seconds. The camera mode
         * it keys on is only updated while the follow camera runs, so after
         * a cut to an event it can stay 12 and skip the event's lines. */
        if (s_talk_auto_a < 0) {
            const char *v = getenv("JSRF_TALK_AUTO_A");
            s_talk_auto_a = v != NULL;
            s_talk_auto_a_until = (v && atof(v) > 1.0) ? atof(v) : 1e30;
        }
        if (s_talk_auto_a && now < s_talk_auto_a_until && s_cam_mode_now == 12 && fmod(now, 2.0) < 0.2)
            st->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
        for (int i = 0; i < s_auto_n && i < 16; i++) {
            double dt = now - s_auto[i].t_done;
            const char *b = s_auto[i].btn;
            if (s_auto[i].state != 2 || !b[0] || dt < 0) continue;
            if (!((dt < 0.3) || (dt >= 1.5 && dt < 1.8) || (dt >= 3.0 && dt < 3.3))) continue;
            if (!strcmp(b, "A")) st->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
            else if (!strcmp(b, "B")) st->Gamepad.bAnalogButtons[XBOX_BUTTON_B] = 255;
            else if (!strcmp(b, "X")) st->Gamepad.bAnalogButtons[XBOX_BUTTON_X] = 255;
            else if (!strcmp(b, "Y")) st->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 255;
            else if (!strcmp(b, "LT")) st->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = 255;
            else if (!strcmp(b, "RT")) st->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = 255;
            else if (!strcmp(b, "START")) st->Gamepad.wButtons |= XBOX_GAMEPAD_START;
            { static int said[16]; int n = dt < 0.3 ? 0 : dt < 1.8 ? 1 : 2;
              if (!(said[i] & (1 << n))) { said[i] |= 1 << n;
                fprintf(stderr, "[AUTO] t=%.1f arrival press %s (%d of 3) for waypoint %d\n", now, b, n + 1, i); } }
        }
        st->dwPacketNumber = ++packet;
        return 1;
    }
    if (xbox_InputGetState(port, st) == 0) return 1;
    return port == 0;   /* port 0 always "present", idle, so the title never waits for a pad */
}
static uint32_t pad_mask(uint32_t type) { return type == XPP_TYPE_GAMEPAD ? 1u : 0u; }
/* Seed a device-type descriptor the way the console has it by the time a
 * title looks: present mask and last-reported mask equal, no change pending.
 * Idempotent -- only the first sight of a descriptor seeds it. */
static uint32_t pad_descriptor_sync(uint32_t type)
{
    static uint32_t seeded[4]; static int nseeded;
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    uint32_t mask = pad_mask(type);
    int i;

    if (!type) return mask;
    for (i = 0; i < nseeded; i++)
        if (seeded[i] == type) return mask;
    if (nseeded < 4) seeded[nseeded++] = type;
    *(uint32_t *)(m + type)     = mask;   /* connected now                 */
    *(uint32_t *)(m + type + 4) = 0;      /* no change pending             */
    *(uint32_t *)(m + type + 8) = mask;   /* ... and already reported once */
    return mask;
}

/* XGetDevices(type) */
void sub_001BD5FF(void)
{
    uint32_t type = GARG(1); uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    uint32_t mask = pad_descriptor_sync(type);
    if (type) *(uint32_t *)(m + type + 8) = *(uint32_t *)(m + type);
    g_eax = mask; g_esp += 4 + 4;
}
/* XGetDeviceChanges(type, &ins, &rem)
 *
 * Reports what has been plugged in or unplugged since the last call, and
 * returns non-zero when anything has. JSRF's device enumeration
 * (sub_001669A0) treats that non-zero as E_FAIL and gives up -- it wants a
 * settled device list, and on a console it gets one, because the pad was
 * enumerated during XInitDevices long before the title asks.
 *
 * So the pad is modelled as already connected at boot rather than arriving
 * during the first poll: the descriptor's "currently present" and "last
 * reported" masks are seeded together, and this reports no change. Reporting
 * the insertion here instead cost the title its whole input, font and UI
 * init, and the first visible symptom was a 3 GB allocation four subsystems
 * away.
 */
void sub_001BD621(void)
{
    uint32_t type = GARG(1), pins = GARG(2), prem = GARG(3);
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    uint32_t now = 0, last = 0, ins, rem;

    pad_descriptor_sync(type);
    if (type) { now = *(uint32_t *)(m + type); last = *(uint32_t *)(m + type + 8); }
    ins = now & ~last;
    rem = ~now & last;
    if (pins) *(uint32_t *)(m + pins) = ins;
    if (prem) *(uint32_t *)(m + prem) = rem;
    if (type) { *(uint32_t *)(m + type + 4) = 0; *(uint32_t *)(m + type + 8) = now; }
    g_eax = (ins | rem) ? 1 : 0;
    g_esp += 4 + 12;
}
void sub_001C3BA1(void)
{
    uint32_t type = GARG(1), port = GARG(2);
    g_eax = (pad_mask(type) >> port) & 1 ? (XPP_HANDLE_BASE | port) : 0;
    if (getenv("JSRF_DBG")) fprintf(stderr, "[PAD] XInputOpen(type=%08X port=%u) = %08X\n", type, port, g_eax);
    g_esp += 4 + 16;
}
/* XInputClose(h) */
void sub_001C3C16(void)
{
    g_esp += 4 + 4;
}
/* XInputGetCapabilities(h, caps) */
void sub_001C3C22(void)
{
    uint32_t h = GARG(1), caps = GARG(2); uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    if ((h & 0xFFFF0000u) != XPP_HANDLE_BASE || !caps) { g_eax = 0x57; g_esp += 12; return; }
    memset(m + caps, 0, 28);
    m[caps + 0] = 1;                                  /* XINPUT_DEVSUBTYPE_GC_GAMEPAD */
    *(uint16_t *)(m + caps + 4) = 0x00FF;             /* In.wButtons: all digital */
    memset(m + caps + 6, 0xFF, 8);                    /* In.bAnalogButtons */
    for (int k = 0; k < 4; k++) *(int16_t *)(m + caps + 14 + 2 * k) = 0x7FFF;
    *(uint16_t *)(m + caps + 22) = 0xFFFF; *(uint16_t *)(m + caps + 24) = 0xFFFF; /* rumble */
    g_eax = 0; g_esp += 4 + 8;
}
/* XInputGetState(h, state) */
static void rcam_note_pad(XBOX_INPUT_STATE *s);   /* the camera on the right stick, below */
void sub_001C3E14(void)
{
    uint32_t h = GARG(1), st = GARG(2); uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    XBOX_INPUT_STATE s;
    static uint16_t last_btn; static uint8_t last_a;
    if ((h & 0xFFFF0000u) != XPP_HANDLE_BASE || !st) { g_eax = 0x57; g_esp += 12; return; }
    if (!pad_read(h & 3, &s)) { if ((h & 3) == 0) { jct_pad_failed(); jcx_pad_failed(); } g_eax = 0x48F; g_esp += 12; return; }   /* ERROR_DEVICE_NOT_CONNECTED */
    if ((h & 3) == 0) {
        jct_note_pad(s.Gamepad.sThumbLX, s.Gamepad.sThumbLY, s.Gamepad.sThumbRX, s.Gamepad.sThumbRY, s.Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER], s.Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK]);
        rcam_note_pad(&s);
        jct_delivered_pad(s.Gamepad.sThumbLX, s.Gamepad.sThumbLY, s.Gamepad.sThumbRX, s.Gamepad.sThumbRY, s.Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER], s.Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK]);
    }
    *(uint32_t *)(m + st) = s.dwPacketNumber;
    *(uint16_t *)(m + st + 4) = s.Gamepad.wButtons;
    memcpy(m + st + 6, s.Gamepad.bAnalogButtons, 8);
    *(int16_t *)(m + st + 14) = s.Gamepad.sThumbLX; *(int16_t *)(m + st + 16) = s.Gamepad.sThumbLY;
    *(int16_t *)(m + st + 18) = s.Gamepad.sThumbRX; *(int16_t *)(m + st + 20) = s.Gamepad.sThumbRY;
    if (getenv("JSRF_DBG") && (s.Gamepad.wButtons != last_btn || s.Gamepad.bAnalogButtons[0] != last_a)) {
        last_btn = s.Gamepad.wButtons; last_a = s.Gamepad.bAnalogButtons[0];
        fprintf(stderr, "[PAD] state buttons=%04X A=%u B=%u X=%u Y=%u\n", last_btn, last_a,
                s.Gamepad.bAnalogButtons[1], s.Gamepad.bAnalogButtons[2], s.Gamepad.bAnalogButtons[3]);
    }
    g_eax = 0; g_esp += 4 + 8;
}
/* XInputSetState(h, feedback) */
int nv_window_pref_vibration(void);   /* the window or the app: on by default */
/* The DirectInput layer's SetProperty (0x1E3828's slot 6): how the game asks
 * for rumble -- two magnitudes, 0..10000, into the device's +0x47C/+0x47E,
 * which its Poll (sub_00167180) hands to XInputSetState when they change.
 * JSRF_RUMBLE_LOG=1: every call that carries them, with the game's return
 * addresses on the stack, so a missing rumble can be told from one the game
 * never asked for. */
extern void sub_001678F0_gen(void);
void sub_001678F0(void)
{
    static int log = -1;
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    uint32_t dev = GARG(1), prop = GARG(2), ph = GARG(3);
    if (log < 0) log = getenv("JSRF_RUMBLE_LOG") ? 1 : 0;
    sub_001678F0_gen();
    /* The rumble property (0x1E1874, a 0x18-byte DIPROPDWORD pair): each
     * magnitude goes through imul by 0xFFFF and a divide by 10000 (multiply
     * by 0x68DB8BAD, sar 12). The game passes 0..65535, not 0..10000, so on
     * the console the 32-bit product wraps -- and that wrap is what its rumble
     * is made of. In C that imul is a signed overflow, which the compiler is
     * free to treat as impossible: it kept the full product instead, and a
     * full-strength 65535 came out as 36267 rather than 65523, a fade as
     * noise. Done again here as the processor does it. */
    if (g_eax == 0 && prop == 0x001E1874u && ph && *(uint32_t *)(m + ph) == 0x18) {
        uint32_t i;
        for (i = 0; i < 2; i++) {
            uint32_t v = *(uint32_t *)(m + ph + 0x10 + 4 * i);
            int32_t e = (int32_t)(v * 0xFFFFu);
            int32_t hi = (int32_t)(((int64_t)0x68DB8BAD * (int64_t)e) >> 32);
            hi >>= 12;
            hi += (int32_t)((uint32_t)hi >> 31);
            *(uint16_t *)(m + dev + 0x47C + 2 * i) = (uint16_t)hi;
        }
    }
    if (log && ph && *(uint32_t *)(m + ph) == 0x18) {
        char who[96]; int n = 0, k; uint32_t sp = g_esp - 16;   /* the caller's frame, after ret 12 */
        who[0] = 0;
        for (k = 0; k < 48 && n < 4; k++) {
            uint32_t v = *(uint32_t *)(m + sp + 4 * k);
            if (v >= 0x11000u && v < 0x150000u)
                { snprintf(who + strlen(who), sizeof who - strlen(who), " %06X", v); n++; }
        }
        fprintf(stderr, "[RUMBLE] t=%.2f SetProperty dev %08X prop %08X: %u %u -> device %u %u |%s\n",
                jsrf_now(), dev, prop, *(uint32_t *)(m + ph + 0x10), *(uint32_t *)(m + ph + 0x14),
                *(uint16_t *)(m + dev + 0x47C), *(uint16_t *)(m + dev + 0x47E), who);
    }
}
void sub_001C3E85(void)
{
    uint32_t fb = GARG(2); uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    if (fb) {
        uint32_t ev = *(uint32_t *)(m + fb + 4);
        *(uint32_t *)(m + fb) = 0;                    /* Header.dwStatus = ERROR_SUCCESS */
        /* XINPUT_FEEDBACK: a 0x42-byte header (dwStatus, hEvent, 58 reserved
         * bytes), then the two motors. The game's own input layer writes them
         * there as one dword (sub_00167180: [feedback+0x42] from the device's
         * +0x47C, the left motor in its low half). Until 0.6.8 they were read
         * at +0x40 and +0x42 -- a reserved word, always 0, as the left motor,
         * and the game's left motor as the right: whatever the game asked of
         * its right (small, fast) motor never reached the pad, and only the
         * big hits, which use the left, were felt. */
        XBOX_VIBRATION v = { *(uint16_t *)(m + fb + 0x42), *(uint16_t *)(m + fb + 0x44) };
        /* Vibration off in the settings or the menu: the motors are asked
         * to stop, whatever the game says. JSRF_RUMBLE_LOG=1: each change. */
        if (!nv_window_pref_vibration()) v.wLeftMotorSpeed = v.wRightMotorSpeed = 0;
        { static int log = -1; static uint16_t l0, h0;
          if (log < 0) log = getenv("JSRF_RUMBLE_LOG") ? 1 : 0;
          if (log && (v.wLeftMotorSpeed != l0 || v.wRightMotorSpeed != h0)) {
              l0 = v.wLeftMotorSpeed; h0 = v.wRightMotorSpeed;
              fprintf(stderr, "[RUMBLE] t=%.2f port %u: left %u right %u (from %08X; +0x40 %u)\n", jsrf_now(),
                      GARG(1) & 3, l0, h0, MEM32(g_esp), *(uint16_t *)(m + fb + 0x40));
          } }
        xbox_InputSetState(GARG(1) & 3, &v);
        if (ev) { static int once; if (!once++) fprintf(stderr, "[PAD] XInputSetState with hEvent=%08X (not signalled)\n", ev); }
    }
    g_eax = 0; g_esp += 4 + 8;
}
/* XInputPoll(h) */
void sub_001C3EBD(void)
{
    g_eax = 0; g_esp += 4 + 4;
}

/* ---- DirectSound DSP doorbell (sub_001A1769) ----------------------------
 * The XDK's DirectSound sends a command to the APU's GP DSP and then spins:
 *
 *     add  ebx, 0x810
 *     rep  movsb                  ; the command block
 *     mov  dword ptr [ebx], eax   ; ring the doorbell
 *     cmp  dword ptr [ebx], 0
 *     jne  0x1A18D0               ; ... until the DSP clears it
 *
 * Nothing here runs DSP56300 code, so the word never clears and the title's
 * audio thread spins for ever -- and in JSRF that thread is the one the whole
 * engine is created on, so the title never reaches its first frame.
 *
 * The APU can clear it (that is what RECOMP_APU_DSP_ACK does), but the address
 * is a DirectSound heap allocation: it moves whenever the heap layout changes,
 * and hunting for it again with a debugger after every build is not a fix. It
 * is derivable from the object this method is called on, so derive it, and
 * hand it to the APU the first time through.
 *
 *     doorbell = *(*(*(this + 8) + 0x10)) + 0x810
 */
extern void sub_001A1769_gen(void);
extern void mcpx_apu_dsp_ack_add(uint32_t addr);
void sub_001A1769(void)
{
    static uint32_t registered;
    const uint8_t *m = (const uint8_t *)g_xbox_mem_offset;
    uint32_t p = g_ecx;

    if (p) p = *(const uint32_t *)(m + p + 8);
    if (p) p = *(const uint32_t *)(m + p + 0x10);
    if (p) p = *(const uint32_t *)(m + p);
    if (p && p != registered) {
        registered = p;
        mcpx_apu_dsp_ack_add(p + 0x810);
    }
    sub_001A1769_gen();
}

/* ---- unaligned locked operations ---------------------------------------
 *
 * See RECOMP_ATOMIC_ADD32 in recomp_types.h. x86 allows a lock-prefixed
 * read-modify-write at any address; ARM's exclusive-load instructions do not,
 * and raise SIGBUS instead. The guest depends on these being atomic across
 * its threads, so the misaligned case is serialised under a lock rather than
 * quietly becoming non-atomic.
 *
 * Striped by address so that the rare unaligned refcount does not become a
 * contention point for every other one, and memcpy rather than a cast because
 * an unaligned uint32_t lvalue is undefined even where the hardware allows
 * the access.
 */
#include <pthread.h>

#define RECOMP_UNALIGNED_LOCKS 64
static pthread_mutex_t g_unaligned_locks[RECOMP_UNALIGNED_LOCKS] = {
#define M15 PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, \
            PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
    M15 M15 M15 M15 M15 M15 M15 M15
    M15 M15 M15 M15 M15 M15 M15 M15
#undef M15
};

static pthread_mutex_t *unaligned_lock_for(const void *p)
{
    uintptr_t a = (uintptr_t)p >> 2;
    a ^= a >> 11;
    return &g_unaligned_locks[a & (RECOMP_UNALIGNED_LOCKS - 1)];
}

uint32_t recomp_atomic_add32_unaligned(void *p, uint32_t v)
{
    pthread_mutex_t *m = unaligned_lock_for(p);
    uint32_t old, neu;
    pthread_mutex_lock(m);
    memcpy(&old, p, 4);
    neu = old + v;
    memcpy(p, &neu, 4);
    pthread_mutex_unlock(m);
    return old;
}

uint32_t recomp_atomic_cas32_unaligned(void *p, uint32_t cmp, uint32_t val)
{
    pthread_mutex_t *m = unaligned_lock_for(p);
    uint32_t old;
    pthread_mutex_lock(m);
    memcpy(&old, p, 4);
    if (old == cmp) memcpy(p, &val, 4);
    pthread_mutex_unlock(m);
    return old;
}

/* ==========================================================================
 * The skeleton poser, measured and replayed.
 *
 * sub_0005F0F0(obj, model, motion, frame[, mode]) poses a character: it
 * evaluates `motion` at `frame` over the node tree and stores one 4x4 matrix
 * per bone at obj+0x20 (the count is at obj+0x1c). Every rudie drawn in a
 * dialogue close-up goes through here -- sub_00048DB0 calls it via
 * sub_00047970 just before it builds the skinning palette.
 *
 * JSRF_POSE_CENSUS=<flip>  For every call during that frame and the next,
 *     print the arguments and how far each bone's up axis leans from bone
 *     0's. A spine folded backwards is a large number on a low bone.
 *
 * JSRF_SNAP=<flip>[:<k>]   On the k-th call (default 1) during the first frame
 * JSRF_SNAP_DIR=<dir>      at or after <flip>, write guest RAM and the CPU
 *     state to <dir>/snap_in.bin as the call begins and <dir>/snap_out.bin as
 *     it returns. tools/snap_replay.py runs the ORIGINAL x86 from snap_in in
 *     an emulator and compares with snap_out: any byte the two disagree on is
 *     something the recompiled code computed that the Xbox would not have.
 * ========================================================================== */
#include <math.h>
#include <string.h>
extern uint32_t nv2a_gl_flips(void);
extern size_t g_xbox_total_ram;

static void jsrf_snap_write(const char *dir, const char *name, uint32_t va,
                            uint32_t phase, uint32_t flip, uint32_t idx)
{
    char path[1024];
    uint8_t hdr[512];
    uint32_t w[16];
    size_t ram = g_xbox_total_ram ? g_xbox_total_ram : ((size_t)64 << 20);
    const RecompXmm *x[8] = { &g_xmm0, &g_xmm1, &g_xmm2, &g_xmm3,
                              &g_xmm4, &g_xmm5, &g_xmm6, &g_xmm7 };
    FILE *f;
    int i;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[SNAP] cannot write %s\n", path); return; }
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, "JSRFSNAP", 8);
    w[0] = 1; w[1] = va; w[2] = phase; w[3] = (uint32_t)ram;
    w[4] = g_eax; w[5] = g_ecx; w[6] = g_edx; w[7] = g_ebx;
    w[8] = g_esp; w[9] = g_ebp; w[10] = g_esi; w[11] = g_edi;
    w[12] = g_fs_base; w[13] = (uint32_t)g_fp_top; w[14] = flip; w[15] = idx;
    memcpy(hdr + 8, w, sizeof w);                 /* 8 .. 72   */
    memcpy(hdr + 72, (const void *)g_fp_stack, 64);  /* 72 .. 136 */
    for (i = 0; i < 8; i++) memcpy(hdr + 136 + 16 * i, x[i], 16);  /* .. 264 */
    fwrite(hdr, 1, sizeof hdr, f);
    fflush(f);
    /* Page by page with pwrite: a page the runtime has decommitted cannot be
     * read, and one fwrite of the whole span stops dead at the first such
     * page (the first rudie snapshot came out 37 MB of 64). Unreadable pages
     * are written as zeros and flagged in a bitmap after the RAM image, so
     * the replay can leave them unmapped instead of trusting zeros. */
    {
        int fd = fileno(f);
        static uint8_t zero[4096];
        static uint8_t bitmap[(256u << 20) / 4096 / 8];
        size_t npages = ram / 4096, pg, bad = 0;
        memset(bitmap, 0, sizeof bitmap);
        for (pg = 0; pg < npages; pg++) {
            const uint8_t *src = (const uint8_t *)g_xbox_mem_offset + pg * 4096;
            off_t off = (off_t)sizeof hdr + (off_t)pg * 4096;
            if (pwrite(fd, src, 4096, off) != 4096) {
                (void)pwrite(fd, zero, 4096, off);
                bad++;
            } else if (pg / 8 < sizeof bitmap) {
                bitmap[pg / 8] |= (uint8_t)(1u << (pg & 7));
            }
        }
        (void)pwrite(fd, bitmap, npages / 8, (off_t)sizeof hdr + (off_t)ram);
        if (bad) fprintf(stderr, "[SNAP] %s: %zu unreadable page(s) written as zeros\n", name, bad);
    }
    fclose(f);
}

static void jsrf_pose_census(uint32_t flip, uint32_t idx, uint32_t obj,
                             uint32_t model, uint32_t motion, uint32_t fbits,
                             uint32_t from)
{
    const uint8_t *m = (const uint8_t *)g_xbox_mem_offset;
    const uint32_t lim = 64u << 20;
    uint32_t nb, mats;
    float fr;
    char line[2048];
    int n = 0, i;
    memcpy(&fr, &fbits, 4);
    if (obj < 0x10000u || obj >= lim) return;
    nb = *(const uint32_t *)(m + obj + 0x1c);
    mats = *(const uint32_t *)(m + obj + 0x20);
    fprintf(stderr, "[POSE] flip %u #%u obj=%08X model=%08X motion=%08X frame=%.3f"
            " bones=%u mats=%08X from=%08X", flip, idx, obj, model, motion, fr,
            nb, mats, from);
    if (motion >= 0x10000u && motion < lim)
        fprintf(stderr, " mhdr=%08X %08X %08X", *(const uint32_t *)(m + motion),
                *(const uint32_t *)(m + motion + 4), *(const uint32_t *)(m + motion + 8));
    fprintf(stderr, "\n");
    if (nb == 0 || nb > 128 || mats < 0x10000u || mats + nb * 64u > lim) return;
    {
        const float *b0 = (const float *)(m + mats);
        float ux = b0[4], uy = b0[5], uz = b0[6];
        float ul = sqrtf(ux * ux + uy * uy + uz * uz);
        n += snprintf(line + n, sizeof line - n, "[POSE]   lean from bone0 (deg):");
        for (i = 1; i < (int)nb && n < (int)sizeof line - 16; i++) {
            const float *b = (const float *)(m + mats + 64u * (uint32_t)i);
            float vx = b[4], vy = b[5], vz = b[6];
            float vl = sqrtf(vx * vx + vy * vy + vz * vz);
            float c = (ul > 0 && vl > 0) ? (ux * vx + uy * vy + uz * vz) / (ul * vl) : 1.0f;
            if (c > 1) c = 1;
            if (c < -1) c = -1;
            n += snprintf(line + n, sizeof line - n, " %d", (int)lrintf(acosf(c) * 57.29578f));
        }
        fprintf(stderr, "%s\n", line);
        fprintf(stderr, "[POSE]   bone0 at (%.2f %.2f %.2f) up (%.3f %.3f %.3f)"
                " bone1 at (%.2f %.2f %.2f)\n", b0[12], b0[13], b0[14], ux, uy, uz,
                nb > 1 ? b0[16 + 12] : 0.f, nb > 1 ? b0[16 + 13] : 0.f,
                nb > 1 ? b0[16 + 14] : 0.f);
    }
    fflush(stderr);
}

static double jsrf_t0;
__attribute__((constructor)) static void jsrf_t0_init(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    jsrf_t0 = ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ---- the snapshot trigger, for any wrapped function ------------------------
 *
 * JSRF_SNAP_VA=<va>        which wrapped function to capture (default the
 *                          poser, 0x5F0F0)
 * JSRF_SNAP=<flip>[:<k>]   the k-th matching call at or after that flip, or
 * JSRF_SNAP=@<sec>[:<k>]   ... at or after that many seconds after launch
 * JSRF_SNAP_ARG=<n>=<val>  only calls whose n-th stack argument is <val>
 * JSRF_SNAP_DIR=<dir>      where snap_in.bin / snap_out.bin go            */
static struct {
    int init, have_arg, done;
    uint32_t va, flip, k, argi, argv, seen;
    double sec;
    const char *dir;
} SN;

static double jsrf_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9 - jsrf_t0;
}

static int jsrf_snap_begin(uint32_t va)
{
    uint32_t fl;
    if (!SN.init) {
        const char *v = getenv("JSRF_SNAP_VA"), *s = getenv("JSRF_SNAP");
        const char *a = getenv("JSRF_SNAP_ARG");
        SN.va = v ? (uint32_t)strtoul(v, 0, 0) : 0x0005F0F0u;
        SN.sec = -1; SN.k = 1;
        if (s) {
            char *e;
            if (*s == '@') { SN.sec = strtod(s + 1, &e); SN.flip = 0xFFFFFFFFu; }
            else SN.flip = (uint32_t)strtoul(s, &e, 0);
            if (*e == ':') SN.k = (uint32_t)strtoul(e + 1, 0, 0);
        }
        if (a) {
            char *e;
            SN.argi = (uint32_t)strtoul(a, &e, 0);
            if (*e == '=') { SN.argv = (uint32_t)strtoul(e + 1, 0, 0); SN.have_arg = 1; }
        }
        SN.dir = getenv("JSRF_SNAP_DIR");
        if (!SN.dir) SN.dir = ".";
        SN.init = 1;
    }
    if (SN.done || !SN.flip || va != SN.va) return 0;
    fl = nv2a_gl_flips();
    if (SN.sec >= 0) {
        double now = jsrf_now();
        if (now < SN.sec) return 0;
        SN.flip = fl; SN.sec = -1;
        fprintf(stderr, "[SNAP] armed at flip %u (%.1f s after launch) for sub_%08X call #%u\n",
                fl, now, va, SN.k);
    }
    if (fl < SN.flip) return 0;
    if (SN.have_arg && GARG(SN.argi) != SN.argv) return 0;
    if (++SN.seen != SN.k) return 0;
    fprintf(stderr, "[SNAP] capturing sub_%08X call #%u at flip %u: esp=%08X from=%08X"
            " args %08X %08X %08X %08X %08X\n", va, SN.seen, fl, g_esp, GARG(0),
            GARG(1), GARG(2), GARG(3), GARG(4), GARG(5));
    jsrf_snap_write(SN.dir, "snap_in.bin", va, 0, fl, SN.seen);
    return 1;
}

static void jsrf_snap_end(uint32_t va)
{
    jsrf_snap_write(SN.dir, "snap_out.bin", va, 1, nv2a_gl_flips(), SN.seen);
    SN.done = 1;
    fprintf(stderr, "[SNAP] written: %s/snap_in.bin, snap_out.bin\n", SN.dir);
    fflush(stderr);
}

static void jsrf_cam_counts(void);
/* ---- where the rudies are ---------------------------------------------------
 *
 * JSRF_WHERE=<seconds>: every that many seconds, one line per 20-bone skeleton
 * posed in world space (the pass whose root has an exactly vertical up axis --
 * the other pass is camera space and tilts with the camera), giving its world
 * position. Enough to steer a scripted run towards a character. */
static void jsrf_where(uint32_t fl, uint32_t obj)
{
    static double period = -1, next_at;
    static uint32_t where_flip;
    const uint8_t *m = (const uint8_t *)g_xbox_mem_offset;
    uint32_t nb, mats;
    const float *b0;
    if (period < 0) {
        const char *e = getenv("JSRF_WHERE");
        period = e ? atof(e) : 0;
        next_at = period;
    }
    if (period <= 0) return;
    if (jsrf_now() >= next_at) { where_flip = fl + 1; next_at = jsrf_now() + period; }
    if (fl != where_flip) return;
    if (obj < 0x10000u || obj >= (64u << 20)) return;
    nb = *(const uint32_t *)(m + obj + 0x1c);
    mats = *(const uint32_t *)(m + obj + 0x20);
    if (nb != 20 || mats < 0x10000u || mats + 64u * 20u > (64u << 20)) return;
    b0 = (const float *)(m + mats);
    if (fabsf(b0[5] - 1.0f) > 1e-3f || fabsf(b0[6]) > 1e-3f) return;
    fprintf(stderr, "[WHERE] t=%.1f flip %u obj=%08X at (%.1f %.1f %.1f)\n",
            jsrf_now(), fl, obj, b0[12], b0[13], b0[14]);
}

/* ---- where a skater is posed: JSRF_EVT_LOG=<seconds> ------------------------
 *
 * The character select's preview skaters were drawn into the floor and under
 * the rails they grind. Every that many seconds, for each skeleton posed
 * through sub_00047970 (the usual "pose this skeleton with this motion"):
 * who called sub_00047970 (the return address above its saved ebx), the
 * matrix the skeleton was posed under (the matrix stack's top: translation and
 * up axis -- up exactly vertical means a world-space pass), and the posed
 * bones: bone 0, bone 1, the lowest and the highest bone, in that space. Plus,
 * from sub_00080BD0, each character's own position (+0xCA4). Enough to tell a
 * skater posed low (bones under the matrix origin) from one placed low. */
static double evt_period = -1;
static int evt_on(void)
{
    if (evt_period < 0) {
        const char *e = getenv("JSRF_EVT_LOG");
        evt_period = e ? atof(e) : 0;
    }
    return evt_period > 0;
}
static void jsrf_evt_pose(uint32_t caller, uint32_t obj, uint32_t motion)
{
    static struct { uint32_t obj, caller; double next; } seen[256];
    const uint8_t *m = (const uint8_t *)g_xbox_mem_offset;
    const uint32_t lim = 64u << 20;
    uint32_t nb, mats, top, i, lo = 0, hi = 0;
    const float *mt, *b;
    float ylo = 1e30f, yhi = -1e30f;
    int k, slot = -1;
    double now = jsrf_now();
    if (obj < 0x10000u || obj + 0x24u > lim) return;
    top = *(const uint32_t *)(m + 0x264C04u);
    top = (top >= 0x10000u && top + 4 <= lim) ? *(const uint32_t *)(m + top) : 0;
    mt = (top >= 0x10000u && top + 64 <= lim) ? (const float *)(m + top) : NULL;
    /* One line per object and pass kind (world: up exactly vertical). */
    {
        uint32_t kind = (mt && fabsf(mt[5] - 1.0f) < 1e-3f && fabsf(mt[4]) < 1e-3f && fabsf(mt[6]) < 1e-3f) ? 1u : 0u;
        uint32_t key = caller ^ kind;
        for (k = 0; k < 256; k++) {
            if (seen[k].obj == obj && seen[k].caller == key) { slot = k; break; }
            if (!seen[k].obj) { seen[k].obj = obj; seen[k].caller = key; slot = k; break; }
        }
        if (slot < 0) {                    /* full: a new scene's objects -- start over */
            memset(seen, 0, sizeof seen);
            seen[0].obj = obj; seen[0].caller = key; slot = 0;
        }
        if (now < seen[slot].next) return;
        seen[slot].next = now + evt_period;
        nb = *(const uint32_t *)(m + obj + 0x1c);
        mats = *(const uint32_t *)(m + obj + 0x20);
        fprintf(stderr, "[EVT] t=%.2f flip %u obj %08X from %08X %s", now, nv2a_gl_flips(), obj, caller,
                kind ? "world" : "other");
        if (mt) fprintf(stderr, " | matrix at (%.2f %.2f %.2f) up (%.3f %.3f %.3f)",
                        mt[12], mt[13], mt[14], mt[4], mt[5], mt[6]);
        if (motion >= 0x10000u && motion + 0x10u <= lim)
            fprintf(stderr, " | motion %08X root flags %08X", *(const uint32_t *)(m + motion),
                    *(const uint32_t *)(m + motion + 8));
        if (nb == 20 && mats >= 0x10000u && mats + 64u * 20u <= lim) {
            for (i = 0; i < 20; i++) {
                b = (const float *)(m + mats + 64u * i);
                if (b[13] < ylo) { ylo = b[13]; lo = i; }
                if (b[13] > yhi) { yhi = b[13]; hi = i; }
            }
            b = (const float *)(m + mats);
            fprintf(stderr, " | bone0 (%.2f %.2f %.2f) bone1 (%.2f %.2f %.2f) lowest %u y %.2f highest %u y %.2f",
                    b[12], b[13], b[14], b[16 + 12], b[16 + 13], b[16 + 14], lo, ylo, hi, yhi);
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    }
}
static void jsrf_evt_char(uint32_t self)
{
    static struct { uint32_t obj; double next; } seen[64];
    const uint8_t *m = (const uint8_t *)g_xbox_mem_offset;
    const float *p;
    int k, slot = -1;
    double now = jsrf_now();
    if (self < 0x10000u || self + 0xCB0u > (64u << 20)) return;
    for (k = 0; k < 64; k++) {
        if (seen[k].obj == self) { slot = k; break; }
        if (!seen[k].obj) { seen[k].obj = self; slot = k; break; }
    }
    if (slot < 0) { memset(seen, 0, sizeof seen); seen[0].obj = self; slot = 0; }
    if (now < seen[slot].next) return;
    seen[slot].next = now + evt_period;
    p = (const float *)(m + self + 0xCA4u);
    fprintf(stderr, "[EVTCHAR] t=%.2f flip %u char %08X at (%.2f %.2f %.2f)\n", now, nv2a_gl_flips(),
            self, p[0], p[1], p[2]);
    fflush(stderr);
}

/* ---- the poser: census + snapshot ---------------------------------------- */
extern void sub_0005F0F0_gen(void);
void sub_0005F0F0(void)
{
    static int init;
    static uint32_t census_flip, cur_flip = 0xFFFFFFFFu, idx;
    static double census_sec = -1;
    uint32_t fl, from, obj, model, motion, fr;
    int snap;
    if (!init) {
        const char *c = getenv("JSRF_POSE_CENSUS");
        if (c && *c == '@') census_sec = atof(c + 1);
        else census_flip = c ? (uint32_t)strtoul(c, 0, 0) : 0;
        init = 1;
    }
    /* Called through sub_00047970: its caller's return address sits above
     * sub_00047970's saved ebx, at GARG(7). */
    if (evt_on() && GARG(0) == 0x0004798Du) {
        uint32_t caller = GARG(7), o = GARG(1), mo = GARG(3);
        sub_0005F0F0_gen();
        jsrf_evt_pose(caller, o, mo);
        return;
    }
    snap = jsrf_snap_begin(0x0005F0F0u);
    if (!census_flip && census_sec < 0) {
        uint32_t o = GARG(1);
        sub_0005F0F0_gen();
        if (snap) jsrf_snap_end(0x0005F0F0u);
        jsrf_where(nv2a_gl_flips(), o);
        jsrf_cam_counts();
        return;
    }
    fl = nv2a_gl_flips();
    if (census_sec >= 0 && jsrf_now() >= census_sec) {
        census_flip = fl + 1; census_sec = -1;
        fprintf(stderr, "[POSE] census armed for flip %u\n", census_flip);
    }
    if (fl != cur_flip) { cur_flip = fl; idx = 0; }
    idx++;
    from = GARG(0); obj = GARG(1); model = GARG(2); motion = GARG(3); fr = GARG(4);
    sub_0005F0F0_gen();
    if (snap) jsrf_snap_end(0x0005F0F0u);
    if (census_flip && (fl == census_flip || fl == census_flip + 1))
        jsrf_pose_census(fl, idx, obj, model, motion, fr, from);
    jsrf_where(fl, obj);
}

/* ---- the pose sampler: sub_0005E7B0 ----------------------------------------
 *
 * Samples a source motion at a (fractional) frame into a one-key motion --
 * the kind every rudie in a dialogue is posed from. JSRF_SAMPLE_LOG=1 prints
 * each call: the source motion and frame, the floor/ceil the original would
 * take, and the first joints' angles it produced.                         */
static void jsrf_motion_brief(const char *tag, uint32_t mo)
{
    const uint8_t *m = (const uint8_t *)g_xbox_mem_offset;
    uint32_t ch, n, i;
    if (mo < 0x10000u || mo >= (64u << 20)) { fprintf(stderr, " %s=%08X", tag, mo); return; }
    ch = *(const uint32_t *)(m + mo + 4);
    n = (*(const uint32_t *)(m + mo) >> 8) & 0xFF;
    fprintf(stderr, " %s=%08X[type %02X nodes %u frames %d]", tag, mo, m[mo],
            n, *(const int16_t *)(m + mo + 2));
    if (ch < 0x10000u || ch >= (64u << 20)) return;
    for (i = 1; i < n && i < 5; i++) {
        uint32_t c = ch + 16u * i, fl = *(const uint32_t *)(m + c);
        uint32_t ap = *(const uint32_t *)(m + c + 8);
        if ((fl & 0x10) && ap >= 0x10000u && ap < (64u << 20))
            fprintf(stderr, " n%u(%d %d %d)", i, *(const int16_t *)(m + ap),
                    *(const int16_t *)(m + ap + 2), *(const int16_t *)(m + ap + 4));
    }
}


/* ---- where the rudies' folded pose first appears ---------------------------
 * JSRF_FOLD_TRACE=1: in the sampler and the blender, note a destination pose
 * whose joint 1 and joint 3 pitch are both strongly negative (the folded
 * dialogue pose starts n1 x=-6792, n3 x=-10084) where it was not before the
 * call, and print every input with its own joint angles -- the first such
 * line names the computation that produced the fold. */
static int jsrf_node_ang(uint32_t mo, uint32_t node, uint32_t key, int16_t out[3])
{
    const uint8_t *m = (const uint8_t *)g_xbox_mem_offset;
    uint32_t ch, c, fl, ap;
    if (mo < 0x10000u || mo >= (64u << 20)) return 0;
    ch = *(const uint32_t *)(m + mo + 4);
    if (ch < 0x10000u || ch >= (64u << 20)) return 0;
    c = ch + 16u * node;
    fl = *(const uint32_t *)(m + c);
    ap = *(const uint32_t *)(m + c + 8);
    if (!(fl & 0x10) || ap < 0x10000u || ap >= (64u << 20)) return 0;
    ap += 6u * key;
    out[0] = *(const int16_t *)(m + ap); out[1] = *(const int16_t *)(m + ap + 2);
    out[2] = *(const int16_t *)(m + ap + 4);
    return 1;
}
static int jsrf_is_folded(uint32_t mo)
{
    int16_t a1[3], a3[3];
    if (!jsrf_node_ang(mo, 1, 0, a1) || !jsrf_node_ang(mo, 3, 0, a3)) return 0;
    return a1[0] < -4000 && a3[0] < -8000;
}
static int jsrf_fold_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("JSRF_FOLD_TRACE") ? 1 : 0;
    return on;
}
static void jsrf_fold_report(const char *who, uint32_t dst, const uint32_t *a, int na,
                             int was)
{
    static int shown;
    int i;
    if (shown >= 60) return;
    shown++;
    fprintf(stderr, "[FOLD] %s made dst %08X folded (was %s) at flip %u t=%.2f from=%08X:",
            who, dst, was ? "folded" : "normal", nv2a_gl_flips(), jsrf_now(), a[0]);
    for (i = 1; i < na; i++) fprintf(stderr, " %08X", a[i]);
    fprintf(stderr, "\n");
    for (i = 1; i < na; i++) {
        uint32_t mo = a[i];
        int16_t q[3];
        uint32_t n;
        if (mo < 0x10000u || mo >= (64u << 20)) continue;
        if ((*(const uint32_t *)((const uint8_t *)g_xbox_mem_offset + mo) & 0xFF) != 0x11
         && (*(const uint32_t *)((const uint8_t *)g_xbox_mem_offset + mo) & 0xFF) != 0x15) continue;
        fprintf(stderr, "[FOLD]   arg%d", i);
        jsrf_motion_brief("", mo);
        fprintf(stderr, " folded=%d", jsrf_is_folded(mo));
        for (n = 5; n < 8; n++)
            if (jsrf_node_ang(mo, n, 0, q)) fprintf(stderr, " n%u(%d %d %d)", n, q[0], q[1], q[2]);
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}
extern void sub_0005E7B0_gen(void);
void sub_0005E7B0(void)
{
    /* JSRF_SAMPLE_LOG=1 or =@<sec> (from that many seconds on);
     * JSRF_SAMPLE_DST=<lo>:<hi> keeps only samples into that address range. */
    static int on = -1, shown;
    static double from_sec;
    static uint32_t dlo, dhi = 0xFFFFFFFFu;
    uint32_t a[6], i;
    float fr;
    int snap = jsrf_snap_begin(0x0005E7B0u);
    if (on < 0) {
        const char *e = getenv("JSRF_SAMPLE_LOG"), *d = getenv("JSRF_SAMPLE_DST");
        on = e ? 1 : 0;
        from_sec = (e && *e == '@') ? atof(e + 1) : 0;
        if (d) { char *x; dlo = (uint32_t)strtoul(d, &x, 0); if (*x == ':') dhi = (uint32_t)strtoul(x + 1, 0, 0); }
    }
    for (i = 0; i < 6; i++) a[i] = GARG(i);
    { int was = jsrf_fold_on() ? jsrf_is_folded(a[2]) : 0;
      sub_0005E7B0_gen();
      if (jsrf_fold_on() && !was && jsrf_is_folded(a[2]))
          jsrf_fold_report("sample(sub_0005E7B0)", a[2], a, 6, was); }
    if (snap) jsrf_snap_end(0x0005E7B0u);
    if (on && shown < 600 && a[2] >= dlo && a[2] <= dhi
        && (from_sec <= 0 || jsrf_now() >= from_sec)) {
        shown++;
        memcpy(&fr, &a[5], 4);
        fprintf(stderr, "[SAMPLE] flip %u t=%.1f from=%08X a1=%08X a3=%08X frame=%.4f"
                " (floor %.0f ceil %.0f)", nv2a_gl_flips(), jsrf_now(), a[0], a[1],
                a[3], fr, floor(fr), ceil(fr));
        jsrf_motion_brief("src", a[4]);
        jsrf_motion_brief("dst", a[2]);
        fprintf(stderr, "\n");
        fflush(stderr);
    }
}

/* ---- the motion blender: sub_0005EC20(tree, dst, ?, motionA, frameA,
 * motionB, frameB, weight) -------------------------------------------------
 * Writes the pose a rudie is actually drawn with (the store watch on Corn's
 * pose buffer names sub_0005E480, called from here, as its only steady
 * writer). JSRF_BLEND_LOG=@<sec> with JSRF_SAMPLE_DST=<lo>:<hi> prints each
 * call into that range: both motions, both frames, the weight, and the
 * first joints it produced. */
extern void sub_0005EC20_gen(void);
void sub_0005EC20(void)
{
    static int on = -1, shown;
    static double from_sec;
    static uint32_t dlo, dhi = 0xFFFFFFFFu;
    uint32_t a[9], i;
    int snap = jsrf_snap_begin(0x0005EC20u);
    if (on < 0) {
        const char *e = getenv("JSRF_BLEND_LOG"), *d = getenv("JSRF_SAMPLE_DST");
        on = e ? 1 : 0;
        from_sec = (e && *e == '@') ? atof(e + 1) : 0;
        if (d) { char *x; dlo = (uint32_t)strtoul(d, &x, 0); if (*x == ':') dhi = (uint32_t)strtoul(x + 1, 0, 0); }
    }
    for (i = 0; i < 9; i++) a[i] = GARG(i);
    { int was = jsrf_fold_on() ? jsrf_is_folded(a[2]) : 0;
      sub_0005EC20_gen();
      if (jsrf_fold_on() && !was && jsrf_is_folded(a[2]))
          jsrf_fold_report("blend(sub_0005EC20)", a[2], a, 9, was); }
    if (snap) jsrf_snap_end(0x0005EC20u);
    if (on && shown < 400 && a[2] >= dlo && a[2] <= dhi
        && (from_sec <= 0 || jsrf_now() >= from_sec)) {
        float fa, fb, w;
        shown++;
        memcpy(&fa, &a[5], 4); memcpy(&fb, &a[7], 4); memcpy(&w, &a[8], 4);
        fprintf(stderr, "[BLEND] flip %u t=%.1f from=%08X tree=%08X a3=%08X frameA=%.4f"
                " frameB=%.4f w=%.4f", nv2a_gl_flips(), jsrf_now(), a[0], a[1], a[3],
                fa, fb, w);
        jsrf_motion_brief("A", a[4]);
        jsrf_motion_brief("B", a[6]);
        jsrf_motion_brief("dst", a[2]);
        fprintf(stderr, "\n");
        fflush(stderr);
    }
}

/* ---- DirectSound: a voice-done notification for a voice nobody owns ---------
 *
 * sub_001A200D is the APU interrupt handler's per-voice step: it looks the
 * hardware voice up in DirectSound's owner table (dsound+0x2C4, 256 entries)
 * and, if that voice is still on its list, retires it (sub_001A2E2E). It
 * never checks the table entry for NULL -- on the console a notification
 * only ever arrives for a voice that is playing, so the entry is always set.
 *
 * Here one arrives for hardware voice 0 while no DirectSound voice owns it.
 * With a NULL entry the handler reads the "voice" at address 0; the
 * console's page 0 is unmapped and that would fault, ours reads zeros, so
 * the checks pass and sub_001A2E2E retires a voice at address 0. Its list
 * link reads as NULL, CONTAINING_RECORD turns that into 0xFFFFFFB4, and the
 * next load lands at guest 0xFFFFFFBE: "[CRASH] signal 10 ... fault
 * addr=0x3FFFFFFBE in sub_001A2E2E" -- the black screen on New Game (23 Sep,
 * a player's run and 1 of 2 reproductions with those settings).
 *
 * There is nothing to retire when nobody owns the voice, so the notification
 * is dropped. Why the voice processor reports voice 0 at all is an APU
 * emulation question and is logged so it can be followed up. */
extern void sub_001A200D_gen(void);
void sub_001A200D(void)
{
    /* Only a NULL entry is caught. Owners live wherever DirectSound put them
     * -- in practice the contiguous window at 0x80000000 -- so anything that
     * is not page 0 is left to the original code, exactly as before. (The
     * first version of this guard also rejected pointers above 64 MB, which
     * threw away every legitimate notification and stalled the audio thread:
     * 23 Sep, run g2.) The address is computed exactly as the original does,
     * so this read is no riskier than the one it guards. */
    static unsigned drops;
    const uint8_t *m = (const uint8_t *)g_xbox_mem_offset;
    uint32_t hw = GARG(1);
    uint32_t owner = *(const uint32_t *)(m + (uint32_t)(g_ecx + 0x2C4u + hw * 4u));
    if (owner < 0x10000u) {
        unsigned n = ++drops;
        if (n <= 8 || (n & (n - 1)) == 0) {
            fprintf(stderr, "[DSOUND] voice-done notification for hardware voice %u,"
                    " which no DirectSound voice owns (entry %08X) -- dropped (%u so far)\n",
                    hw, owner, n);
            fflush(stderr);
        }
        g_esp += 8;          /* ret 4: return address and the one argument */
        return;
    }
    sub_001A200D_gen();
}


/* ---- talk events: which one starts, when, and from where --------------------
 *
 * sub_00038460 is the event manager's "start talk event N" (virtual, slot 8
 * of the table at 0x1EC068; thiscall, the event number as its one argument;
 * it records N at +0x1578 and the loader state machine at sub_00038890 then
 * reads TE<N>.bin). JSRF_TALK_LOG=1 prints every call with its return
 * address, so the code that decided to start a talk can be found. Added
 * 23 Sep: Gum's talk (TE002) starts the moment Corn's ends, with Beat still
 * 570 units away from her. */
extern void sub_00038460_gen(void);
void sub_00038460(void)
{
    static int on = -1;
    if (on < 0) on = getenv("JSRF_TALK_LOG") != NULL;
    if (on) {
        const uint8_t *m = (const uint8_t *)g_xbox_mem_offset;
        uint32_t cur = (g_ecx >= 0x10000u && g_ecx < (64u << 20))
                     ? *(const uint32_t *)(m + g_ecx + 0x1578u) : 0xFFFFFFFFu;
        fprintf(stderr, "[TALK] t=%.1f flip %u: start talk event %u (was %u) this=%08X"
                " from %08X  stack %08X %08X %08X %08X %08X %08X\n",
                jsrf_now(), nv2a_gl_flips(), GARG(1), cur, g_ecx, GARG(0),
                GARG(2), GARG(3), GARG(4), GARG(5), GARG(6), GARG(8));
        fflush(stderr);
    }
    sub_00038460_gen();
}


/* ---- the event manager's command and query API ------------------------------
 *
 * sub_000256A0(cmd, arg) runs command <cmd> (table 0x1EC068, 33 entries; 8 is
 * "start talk event <arg>") on the event manager, and sub_000256C0(cmd, arg)
 * asks it question <cmd> (table 0x1EC178). The tutorial is plain code calling
 * these -- 52 command and 84 query call sites -- so logging them gives the
 * tutorial's script as it actually runs. JSRF_TALK_LOG=1 prints each
 * (call site, cmd, arg, result) the first time and whenever the result
 * changes: queries are polled every frame. */
static int ev_log_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("JSRF_TALK_LOG") != NULL;
    return on;
}
static int ev_changed(uint32_t from, uint32_t cmd, uint32_t arg, uint32_t res)
{
    static struct { uint32_t from, cmd, arg, res; int used; } t[512];
    uint32_t h = (from * 2654435761u ^ cmd * 40503u ^ arg * 97u) & 511u;
    int i;
    for (i = 0; i < 512; i++) {
        uint32_t k = (h + (uint32_t)i) & 511u;
        if (!t[k].used) {
            t[k].used = 1; t[k].from = from; t[k].cmd = cmd; t[k].arg = arg; t[k].res = res;
            return 1;
        }
        if (t[k].from == from && t[k].cmd == cmd && t[k].arg == arg) {
            if (t[k].res == res) return 0;
            t[k].res = res;
            return 1;
        }
    }
    return 1;
}
extern void sub_000256A0_gen(void);
void sub_000256A0(void)
{
    uint32_t from = GARG(0), cmd = GARG(1), arg = GARG(2);
    sub_000256A0_gen();
    if (ev_log_on() && ev_changed(from, cmd | 0x80000000u, arg, g_eax)) {
        fprintf(stderr, "[EVCMD] t=%.2f flip %u cmd %u arg %u (0x%X) -> %u from %08X\n",
                jsrf_now(), nv2a_gl_flips(), cmd, arg, arg, g_eax, from);
        fflush(stderr);
    }
}
extern void sub_000256C0_gen(void);
void sub_000256C0(void)
{
    uint32_t from = GARG(0), cmd = GARG(1), arg = GARG(2);
    sub_000256C0_gen();
    if (ev_log_on() && ev_changed(from, cmd, arg, g_eax)) {
        fprintf(stderr, "[EVQRY] t=%.2f flip %u query %u arg %u (0x%X) -> %u from %08X\n",
                jsrf_now(), nv2a_gl_flips(), cmd, arg, arg, g_eax, from);
        fflush(stderr);
    }
}


/* ---- the stage script runner ------------------------------------------------
 *
 * sub_0005B3F0 (thiscall) walks the stage's script: header at this+0x1040,
 * entries at header+0x174 (0x54 bytes each), count at header+0x178, current
 * index at this+0x2B0. An entry runs only if its flag conditions hold
 * (entry[0] -> list, entry[1] = count; sub_00039B50 checks one flag), and
 * entry[4] is its opcode; 0x7E..0xEF are "blocking" and handed back to the
 * caller, the rest go to sub_000585E0 (which issues event commands).
 * JSRF_TALK_LOG=1 dumps the whole script once and logs every index change. */
extern void sub_0005B3F0_gen(void);
void sub_0005B3F0(void)
{
    static uint32_t dumped_hdr;
    const uint8_t *m = (const uint8_t *)g_xbox_mem_offset;
    const uint32_t lim = 64u << 20;
    uint32_t self = g_ecx, hdr = 0, before = 0, after;
    int on = ev_log_on() && self >= 0x10000u && self < lim;
    if (on) {
        hdr = *(const uint32_t *)(m + self + 0x1040u);
        before = *(const uint32_t *)(m + self + 0x2B0u);
        if (hdr >= 0x10000u && hdr < lim && hdr != dumped_hdr) {
            uint32_t ents = *(const uint32_t *)(m + hdr + 0x174u);
            uint32_t n = *(const uint32_t *)(m + hdr + 0x178u), i, k;
            dumped_hdr = hdr;
            fprintf(stderr, "[SCRIPT] t=%.2f runner %08X header %08X: %u entries at %08X\n",
                    jsrf_now(), self, hdr, n, ents);
            for (i = 0; i < n && i < 512 && ents >= 0x10000u && ents + (i + 1) * 0x54u < lim; i++) {
                const uint32_t *e = (const uint32_t *)(m + ents + i * 0x54u);
                char line[512]; int len = 0;
                len += snprintf(line + len, sizeof line - len, "[SCRIPT]   #%-3u op %3u (0x%02X)", i, e[4], e[4]);
                for (k = 0; k < 21; k++)
                    len += snprintf(line + len, sizeof line - len, " %08X", e[k]);
                if (e[1] && e[1] < 16 && e[0] >= 0x10000u && e[0] + e[1] * 4u < lim) {
                    len += snprintf(line + len, sizeof line - len, "  conds:");
                    for (k = 0; k < e[1]; k++)
                        len += snprintf(line + len, sizeof line - len, " %08X",
                                        *(const uint32_t *)(m + e[0] + k * 4u));
                }
                fprintf(stderr, "%s\n", line);
            }
            fflush(stderr);
        }
    }
    sub_0005B3F0_gen();
    if (on) {
        after = *(const uint32_t *)(m + self + 0x2B0u);
        if (after != before) {
            fprintf(stderr, "[SCRIPT] t=%.2f flip %u index %u -> %u, returned %d\n",
                    jsrf_now(), nv2a_gl_flips(), before, after, (int32_t)g_eax);
            fflush(stderr);
        }
    }
}


/* ---- story flags and the triggers that read them ------------------------------
 *
 * The flag manager (object at 0x1EFFB0) keeps four banks of 512 bits at
 * +0x6A14, +0x18, +0x58, +0x98. A condition word is bank | index << 3 |
 * value << 19; sub_00039B50 tests one, sub_00039BE0 sets one. The event
 * triggers are entries of 0x54 bytes run every frame by sub_000585E0(list,
 * index): if all of the entry's conditions hold and the event manager is
 * idle, its opcode becomes an event command. JSRF_TALK_LOG=1 logs every flag
 * that changes (with the code that changed it) and every trigger that fires. */
static uint32_t jsrf_flag_word(uint32_t fm, uint32_t cond, uint32_t *addr, uint32_t *bit)
{
    static const uint32_t bank_off[4] = { 0x6A14u, 0x18u, 0x58u, 0x98u };
    uint32_t bank = cond & 7u, idx = (cond >> 3) & 0xFFFFu;
    if (bank > 3 || idx >= 0x200u) return 0;
    *addr = fm + bank_off[bank] + (idx >> 5) * 4u;
    *bit = idx & 31u;
    return 1;
}
extern void sub_00039BE0_gen(void);
void sub_00039BE0(void)
{
    const uint8_t *m = (const uint8_t *)g_xbox_mem_offset;
    uint32_t fm = g_ecx, cond = GARG(1), from = GARG(0), a = 0, b = 0, was = 0, now;
    int ok = ev_log_on() && fm >= 0x10000u && fm < (64u << 20) && jsrf_flag_word(fm, cond, &a, &b);
    if (ok) was = (*(const uint32_t *)(m + a) >> b) & 1u;
    sub_00039BE0_gen();
    if (ok) {
        now = (*(const uint32_t *)(m + a) >> b) & 1u;
        if (now != was) {
            fprintf(stderr, "[FLAG] t=%.2f flip %u bank %u flag %u := %u (cond %08X) from %08X\n",
                    jsrf_now(), nv2a_gl_flips(), cond & 7u, (cond >> 3) & 0xFFFFu, now, cond, from);
            fflush(stderr);
        }
    }
}
extern void sub_000585E0_gen(void);
void sub_000585E0(void)
{
    const uint8_t *m = (const uint8_t *)g_xbox_mem_offset;
    const uint32_t lim = 64u << 20;
    uint32_t self = g_ecx, list = GARG(1), idx = GARG(2), busy0 = 0, busy1;
    int on = ev_log_on() && self >= 0x10000u && self < lim;
    if (on) busy0 = *(const uint32_t *)(m + self + 0x238u);
    /* Every entry whose conditions have just become true -- whatever its
     * opcode, not only the ones that issue an event command. */
    if (on && list >= 0x10000u && list + (idx + 1) * 0x54u < lim) {
        static struct { uint32_t list, idx; int held; } seen[1024];
        const uint32_t *e = (const uint32_t *)(m + list + idx * 0x54u);
        uint32_t k, a, b, h, ok = 1;
        for (k = 0; k < e[1] && k < 16; k++) {
            uint32_t c;
            if (e[0] < 0x10000u || e[0] + (k + 1) * 4u >= lim) { ok = 0; break; }
            c = *(const uint32_t *)(m + e[0] + k * 4u);
            if (!jsrf_flag_word(0x1EFFB0u, c, &a, &b)) { ok = 0; break; }
            if (((*(const uint32_t *)(m + a) >> b) & 1u) != ((c >> 19) & 1u)) { ok = 0; break; }
        }
        h = (list * 2654435761u ^ idx * 97u) & 1023u;
        for (k = 0; k < 1024; k++) {
            uint32_t j = (h + k) & 1023u;
            if (!seen[j].list) { seen[j].list = list; seen[j].idx = idx; seen[j].held = 0; h = j; break; }
            if (seen[j].list == list && seen[j].idx == idx) { h = j; break; }
        }
        if (ok && !seen[h].held) {
            char line[640]; int len = 0;
            len += snprintf(line + len, sizeof line - len,
                            "[ENTRY] t=%.2f flip %u list %08X #%u op %u (0x%X) args %08X %08X %08X %08X %08X %08X; conds",
                            jsrf_now(), nv2a_gl_flips(), list, idx, e[4], e[4], e[5], e[6], e[7], e[8], e[9], e[10]);
            for (k = 0; k < e[1] && k < 12; k++) {
                uint32_t c = *(const uint32_t *)(m + e[0] + k * 4u);
                len += snprintf(line + len, sizeof line - len, " [b%u f%u =%u]",
                                c & 7u, (c >> 3) & 0xFFFFu, (c >> 19) & 1u);
            }
            len += snprintf(line + len, sizeof line - len, "; sets");
            for (k = 0; k < e[3] && k < 12 && e[2] >= 0x10000u && e[2] + (k + 1) * 4u < lim; k++) {
                uint32_t c = *(const uint32_t *)(m + e[2] + k * 4u);
                len += snprintf(line + len, sizeof line - len, " [b%u f%u =%u]",
                                c & 7u, (c >> 3) & 0xFFFFu, (c >> 19) & 1u);
            }
            fprintf(stderr, "%s\n", line);
            fflush(stderr);
        }
        seen[h].held = (int)ok;
    }
    sub_000585E0_gen();
    if (!on) return;
    busy1 = *(const uint32_t *)(m + self + 0x238u);
    if (!busy0 && busy1 && list >= 0x10000u && list + (idx + 1) * 0x54u < lim) {
        const uint32_t *e = (const uint32_t *)(m + list + idx * 0x54u);
        char line[640]; int len = 0; uint32_t k;
        len += snprintf(line + len, sizeof line - len,
                        "[TRIGGER] t=%.2f flip %u list %08X entry #%u op %u (0x%X) arg %u (0x%X) fired; conds",
                        jsrf_now(), nv2a_gl_flips(), list, idx, e[4], e[4], e[5], e[5]);
        for (k = 0; k < e[1] && k < 12 && e[0] >= 0x10000u && e[0] + (k + 1) * 4u < lim; k++) {
            uint32_t c = *(const uint32_t *)(m + e[0] + k * 4u);
            len += snprintf(line + len, sizeof line - len, " [b%u f%u =%u]",
                            c & 7u, (c >> 3) & 0xFFFFu, (c >> 19) & 1u);
        }
        len += snprintf(line + len, sizeof line - len, "; sets");
        for (k = 0; k < e[3] && k < 12 && e[2] >= 0x10000u && e[2] + (k + 1) * 4u < lim; k++) {
            uint32_t c = *(const uint32_t *)(m + e[2] + k * 4u);
            len += snprintf(line + len, sizeof line - len, " [b%u f%u =%u]",
                            c & 7u, (c >> 3) & 0xFFFFu, (c >> 19) & 1u);
        }
        fprintf(stderr, "%s\n", line);
        fflush(stderr);
    }
}


/* ---- the player's talk trigger ----------------------------------------------
 *
 * sub_00080BD0 is a character's per-frame update (this = the character; its
 * position is the vec3 at +0xCA4). When +0x308 is set it looks up the talk
 * point +0x2F8 in the event-scene registry (0x20C750) and opens the talk --
 * the "get close to her and pull the Right Trigger" path. JSRF_TALK_LOG=1
 * logs every change of those fields, per character, with the position. */
extern void sub_00080BD0_gen(void);
void sub_00080BD0(void)
{
    static struct { uint32_t obj, f308, f2f8, f2dc, f2e0; } seen[16];
    const uint8_t *m = (const uint8_t *)g_xbox_mem_offset;
    uint32_t self = g_ecx;
    sub_00080BD0_gen();
    if (evt_on()) jsrf_evt_char(self);
    if (ev_log_on() && self >= 0x10000u && self < (64u << 20)) {
        uint32_t f308 = *(const uint32_t *)(m + self + 0x308u);
        uint32_t f2f8 = *(const uint32_t *)(m + self + 0x2F8u);
        uint32_t f2dc = *(const uint32_t *)(m + self + 0x2DCu);
        uint32_t f2e0 = *(const uint32_t *)(m + self + 0x2E0u);
        const float *pos = (const float *)(m + self + 0xCA4u);
        int i, slot = -1;
        for (i = 0; i < 16; i++) {
            if (seen[i].obj == self) { slot = i; break; }
            if (!seen[i].obj) { seen[i].obj = self; seen[i].f308 = ~f308; slot = i; break; }
        }
        if (slot >= 0 && (seen[slot].f308 != f308 || seen[slot].f2f8 != f2f8
                          || seen[slot].f2dc != f2dc || seen[slot].f2e0 != f2e0)) {
            fprintf(stderr, "[TALKPT] t=%.2f flip %u char %08X at (%.1f %.1f %.1f): +308=%08X +2F8=%08X +2DC=%08X +2E0=%08X\n",
                    jsrf_now(), nv2a_gl_flips(), self, pos[0], pos[1], pos[2], f308, f2f8, f2dc, f2e0);
            fflush(stderr);
            seen[slot].f308 = f308; seen[slot].f2f8 = f2f8;
            seen[slot].f2dc = f2dc; seen[slot].f2e0 = f2e0;
        }
    }
}


/* ---- who opens a talk ------------------------------------------------------
 * sub_0006D9A0 constructs the talk display (class 0x1CC5D0; args: owner,
 * 0x1DDE, 0x10, talk id, camera shot, scene position, ...). sub_00052780 is
 * the stage script's talk op, which also moves the player to the scene. */
extern void sub_0006D9A0_gen(void);
void sub_0006D9A0(void)
{
    if (ev_log_on()) {
        fprintf(stderr, "[TALKOPEN] t=%.2f flip %u display %08X from %08X args %08X %08X %08X talk=%d shot=%d %08X %08X\n",
                jsrf_now(), nv2a_gl_flips(), g_ecx, GARG(0), GARG(1), GARG(2), GARG(3),
                (int32_t)GARG(4), (int32_t)GARG(5), GARG(6), GARG(7));
        fflush(stderr);
    }
    sub_0006D9A0_gen();
}
extern void sub_00052780_gen(void);
void sub_00052780(void)
{
    if (ev_log_on()) {
        fprintf(stderr, "[TALKOP] t=%.2f flip %u script talk op, this=%08X from %08X\n",
                jsrf_now(), nv2a_gl_flips(), g_ecx, GARG(0));
        fflush(stderr);
    }
    sub_00052780_gen();
}


/* ---- the stage's blocking-entry list ------------------------------------------
 * sub_00057DE0 (thiscall, the event object) scans header+0x18C (count at
 * +0x190, 0x54-byte entries): the first entry whose flag conditions hold and
 * whose op is 0xE2..0xF9 is returned and becomes a mode (0xE7 = open a talk).
 * Only op 0xF6 also asks the player (message 0x1AF). JSRF_TALK_LOG=1 dumps
 * the list and the trigger list (header+0x17C / +0x180) once, and logs every
 * entry this returns. */
static void jsrf_dump_entries(const char *what, uint32_t ents, uint32_t n)
{
    const uint8_t *m = (const uint8_t *)g_xbox_mem_offset;
    const uint32_t lim = 64u << 20;
    uint32_t i, k;
    fprintf(stderr, "[LIST] %s: %u entries at %08X\n", what, n, ents);
    for (i = 0; i < n && i < 256 && ents >= 0x10000u && ents + (i + 1) * 0x54u < lim; i++) {
        const uint32_t *e = (const uint32_t *)(m + ents + i * 0x54u);
        char line[768]; int len = 0;
        len += snprintf(line + len, sizeof line - len, "[LIST]   %s #%-3u op 0x%02X args %08X %08X %08X %08X %08X %08X; conds",
                        what, i, e[4], e[5], e[6], e[7], e[8], e[9], e[10]);
        for (k = 0; k < e[1] && k < 12 && e[0] >= 0x10000u && e[0] + (k + 1) * 4u < lim; k++) {
            uint32_t c = *(const uint32_t *)(m + e[0] + k * 4u);
            len += snprintf(line + len, sizeof line - len, " [b%u f%u =%u]", c & 7u, (c >> 3) & 0xFFFFu, (c >> 19) & 1u);
        }
        len += snprintf(line + len, sizeof line - len, "; sets");
        for (k = 0; k < e[3] && k < 12 && e[2] >= 0x10000u && e[2] + (k + 1) * 4u < lim; k++) {
            uint32_t c = *(const uint32_t *)(m + e[2] + k * 4u);
            len += snprintf(line + len, sizeof line - len, " [b%u f%u =%u]", c & 7u, (c >> 3) & 0xFFFFu, (c >> 19) & 1u);
        }
        fprintf(stderr, "%s\n", line);
    }
    fflush(stderr);
}
extern void sub_00057DE0_gen(void);
void sub_00057DE0(void)
{
    static uint32_t dumped;
    const uint8_t *m = (const uint8_t *)g_xbox_mem_offset;
    const uint32_t lim = 64u << 20;
    uint32_t self = g_ecx, hdr = 0;
    int on = ev_log_on() && self >= 0x10000u && self < lim;
    if (on) {
        hdr = *(const uint32_t *)(m + self + 0x1040u);
        if (hdr >= 0x10000u && hdr < lim && hdr != dumped) {
            dumped = hdr;
            jsrf_dump_entries("blocking", *(const uint32_t *)(m + hdr + 0x18Cu), *(const uint32_t *)(m + hdr + 0x190u));
            jsrf_dump_entries("trigger", *(const uint32_t *)(m + hdr + 0x17Cu), *(const uint32_t *)(m + hdr + 0x180u));
        }
    }
    sub_00057DE0_gen();
    if (on && (int32_t)g_eax >= 0) {
        uint32_t ents = *(const uint32_t *)(m + hdr + 0x18Cu);
        uint32_t cur = *(const uint32_t *)(m + self + 0xFB8u);
        fprintf(stderr, "[BLOCK] t=%.2f flip %u returned op 0x%02X, entry #%d\n",
                jsrf_now(), nv2a_gl_flips(), g_eax,
                (cur >= ents && ents) ? (int)((cur - ents) / 0x54u) : -1);
        fflush(stderr);
    }
}


/* ---- the zone test ----------------------------------------------------------
 * sub_0004A6F0(point, zone) -> 1 if the point is on the inner side of all six
 * planes of the zone (6 plane points at zone+0x00, 6 normals at zone+0x48):
 * dot(plane_point - point, normal) must not be below 0 (a NaN counts as
 * inside, on the console too). The event-scene registry uses it to decide
 * that a character has walked into a talk zone. JSRF_TALK_LOG=1 logs the
 * first 24 "inside" answers and a few "outside" ones, with the numbers. */
extern void sub_0004A6F0_gen(void);
void sub_0004A6F0(void)
{
    static unsigned n_in, n_out;
    const uint8_t *m = (const uint8_t *)g_xbox_mem_offset;
    const uint32_t lim = 64u << 20;
    uint32_t pt = GARG(1), zone = GARG(2), from = GARG(0);
    sub_0004A6F0_gen();
    /* Only the event-scene registry's calls (sub_0006C6E0), and only once the
     * stage is running: the title's own zone checks would use up the budget. */
    if (ev_log_on() && from >= 0x0006C6E0u && from < 0x0006C840u && jsrf_now() > 60.0
        && pt >= 0x10000u && pt < lim && zone >= 0x10000u && zone + 0x90u < lim
        && ((g_eax && n_in < 24) || (!g_eax && n_out < 12))) {
        const float *p = (const float *)(m + pt), *z = (const float *)(m + zone);
        char line[640]; int len = 0, i;
        if (g_eax) n_in++; else n_out++;
        len += snprintf(line + len, sizeof line - len,
                        "[ZONE] t=%.2f %s from %08X point %08X (%.1f %.1f %.1f) zone %08X dots",
                        jsrf_now(), g_eax ? "INSIDE" : "outside", from, pt, p[0], p[1], p[2], zone);
        for (i = 0; i < 6; i++) {
            const float *q = z + 3 * i, *nv = z + 18 + 3 * i;
            float d = (q[0] - p[0]) * nv[0] + (q[1] - p[1]) * nv[1] + (q[2] - p[2]) * nv[2];
            len += snprintf(line + len, sizeof line - len, " %.1f", d);
        }
        len += snprintf(line + len, sizeof line - len, "  p0 (%.1f %.1f %.1f) n0 (%.2f %.2f %.2f)",
                        z[0], z[1], z[2], z[18], z[19], z[20]);
        {   /* which registration, which probe */
            uint32_t reg = (from == 0x0006C769u || from == 0x0006C7E2u) ? zone - 0xE0u : zone - 0x50u;
            const uint32_t *r = (const uint32_t *)(m + reg);
            const uint32_t *pr = (const uint32_t *)(m + pt);
            len += snprintf(line + len, sizeof line - len,
                            "  reg %08X id=%u task=%u param=%u type=%u shot=%d | probe idx=%d +24=%u +28=%u +2C=%u +30=%u",
                            reg, r[0x178 / 4], r[0x17C / 4], r[0x184 / 4], r[8 / 4], (int32_t)r[0xC / 4],
                            (int32_t)pr[0xC / 4], pr[0x24 / 4], pr[0x28 / 4], pr[0x2C / 4], pr[0x30 / 4]);
        }
        fprintf(stderr, "%s\n", line);
        fflush(stderr);
    }
}


/* ---- "has character <a> walked into the zone of scene <b>?" --------------------
 * sub_0006C680(a, b), thiscall on the event-scene registry (0x20C750): true
 * if a registration with +0x17C == b has +0x180 == a + 1 (the registry's
 * per-frame update, sub_0006C6E0, sets +0x180 to the index + 1 of a probe it
 * finds inside the scene's zone). Trigger op 0x04 uses it: Gum's lesson
 * waits on (0, 1) -- the player inside scene 1's zone. JSRF_TALK_LOG=1 logs
 * every true answer and the first false ones, with every registration. */
extern void sub_0006C680_gen(void);
void sub_0006C680(void)
{
    static unsigned n_false;
    const uint8_t *m = (const uint8_t *)g_xbox_mem_offset;
    const uint32_t lim = 64u << 20;
    uint32_t self = g_ecx, a = GARG(1), b = GARG(2), from = GARG(0);
    sub_0006C680_gen();
    if (ev_log_on() && self >= 0x10000u && self < lim && (g_eax || n_false < 4)) {
        uint32_t reg = *(const uint32_t *)(m + self + 4u);
        int k = 0;
        if (!g_eax) n_false++;
        fprintf(stderr, "[INZONE] t=%.2f flip %u (%u, %u) -> %u from %08X; regs:", jsrf_now(),
                nv2a_gl_flips(), a, b, g_eax, from);
        while (reg >= 0x10000u && reg + 0x190u < lim && k++ < 16) {
            const uint32_t *r = (const uint32_t *)(m + reg);
            fprintf(stderr, " [%08X id=%u task=%u in=%u]", reg, r[0x178 / 4], r[0x17C / 4], r[0x180 / 4]);
            reg = r[0x18C / 4];
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    }
}


/* ---- graffiti spots ---------------------------------------------------------
 * sub_0003FEC0 is a graffiti spot's per-frame update (ecx = the spot; a
 * switch on +0x60). JSRF_SPOT_LOG=1: each spot the first time it updates --
 * its id (+0x58), state, the player's position then, and the floats of its
 * first 0xC0 bytes, to find where spots are -- and every change of state. */
/* A spot's descriptor: the graffiti manager at 0x2314B0 keeps them in a hash
 * table at +8 (bucket id & 0xFF, chained through +0x104, matched on +0x9C).
 * Its +0xA4 holds 3 bits per part (+0x98 parts): the design sprayed there, 7
 * for none; +0xA8 the same for the design being replaced; +0xA0 is set while
 * some part is still 7. sub_0003F9E0 draws the spot's graffiti mesh (+0x90)
 * from these, the arrows while +0xA0 is set. */
static uint32_t spot_desc(const uint8_t *m, uint32_t id)
{
    uint32_t e = *(const uint32_t *)(m + 0x2314B8u + (id & 0xFFu) * 4u);
    int guard = 0;
    while (e >= 0x10000u && e + 0x110u < (64u << 20) && guard++ < 256) {
        if (*(const uint32_t *)(m + e + 0x9C) == id) return e;
        e = *(const uint32_t *)(m + e + 0x104);
    }
    return 0;
}
extern void sub_0003FEC0_gen(void);
void sub_0003FEC0(void)
{
    static int on = -1, nseen;
    static uint32_t seen[128], st[128], d4c[128], d50[128];
    static uint32_t da0[128], da4[128], da8[128], de4[128];
    const uint8_t *m = (const uint8_t *)g_xbox_mem_offset;
    uint32_t spot = g_ecx;
    int k = -1, i;
    if (on < 0) on = getenv("JSRF_SPOT_LOG") != NULL;
    if (on && spot >= 0x10000u && spot + 0x100u < (64u << 20)) {
        const uint32_t *w = (const uint32_t *)(m + spot);
        for (i = 0; i < nseen; i++) if (seen[i] == spot) { k = i; break; }
        if (k < 0 && nseen < 128) {
            k = nseen++; seen[k] = spot; st[k] = w[0x60 / 4];
            d4c[k] = w[0x4C / 4]; d50[k] = w[0x50 / 4];
            fprintf(stderr, "[SPOT] t=%.1f new %08X id %08X state %u player (%.1f %.1f %.1f) |",
                    pad_now(), spot, w[0x58 / 4], w[0x60 / 4],
                    s_player_pos[0], s_player_pos[1], s_player_pos[2]);
            for (i = 0; i < 0xC0 / 4; i++) {
                float f; memcpy(&f, &w[i], 4);
                if (w[i] && f == f && fabsf(f) > 1e-3f && fabsf(f) < 1e5f)
                    fprintf(stderr, " %02X=%.2f", i * 4, f);
                else if (w[i])
                    fprintf(stderr, " %02X:%08X", i * 4, w[i]);
            }
            fprintf(stderr, "\n");
            /* +0x54: its 0xB8-byte record (the stride the update walks). */
            { uint32_t rec = w[0x54 / 4];
              if (rec >= 0x10000u && rec + 0xB8u < (64u << 20)) {
                  const uint32_t *r = (const uint32_t *)(m + rec);
                  fprintf(stderr, "[SPOT]   record %08X:", rec);
                  for (i = 0; i < 0xB8 / 4; i++) {
                      float f; memcpy(&f, &r[i], 4);
                      if (r[i] && f == f && fabsf(f) > 1e-3f && fabsf(f) < 1e5f)
                          fprintf(stderr, " %02X=%.2f", i * 4, f);
                      else if (r[i])
                          fprintf(stderr, " %02X:%08X", i * 4, r[i]);
                  }
                  fprintf(stderr, "\n");
              } }
        }
    }
    sub_0003FEC0_gen();
    if (on && k >= 0) {
        const uint32_t *w = (const uint32_t *)(m + spot);
        if (w[0x60 / 4] != st[k] || w[0x4C / 4] != d4c[k] || w[0x50 / 4] != d50[k]) {
            fprintf(stderr, "[SPOT] t=%.1f %08X id %08X state %u -> %u, +4C %08X -> %08X, "
                    "+50 %08X -> %08X, player (%.1f %.1f %.1f)\n",
                    pad_now(), spot, w[0x58 / 4], st[k], w[0x60 / 4], d4c[k], w[0x4C / 4],
                    d50[k], w[0x50 / 4],
                    s_player_pos[0], s_player_pos[1], s_player_pos[2]);
            st[k] = w[0x60 / 4]; d4c[k] = w[0x4C / 4]; d50[k] = w[0x50 / 4];
        }
        { uint32_t de = spot_desc(m, w[0x58 / 4]);
          if (de) {
              const uint32_t *q = (const uint32_t *)(m + de);
              if (q[0xA0 / 4] != da0[k] || q[0xA4 / 4] != da4[k] || q[0xA8 / 4] != da8[k] ||
                  q[0xE4 / 4] != de4[k]) {
                  uint32_t np = q[0x98 / 4], pa = q[0x94 / 4], i2;
                  fprintf(stderr, "[SPOT] t=%.1f desc %08X id %08X parts %u: A0 %u A4 %08X A8 %08X "
                          "B8 %08X BC %08X E4 %u FC %08X | spot +8C %08X +90 %08X | timers",
                          pad_now(), de, w[0x58 / 4], np, q[0xA0 / 4], q[0xA4 / 4], q[0xA8 / 4],
                          q[0xB8 / 4], q[0xBC / 4], q[0xE4 / 4], q[0xFC / 4],
                          w[0x8C / 4], w[0x90 / 4]);
                  for (i2 = 0; i2 < np && i2 < 10 && pa >= 0x10000u && pa + i2 * 0xB8u + 0xB8u < (64u << 20); i2++) {
                      const float *pf = (const float *)(m + pa + i2 * 0xB8u);
                      fprintf(stderr, " %.2f@(%.0f %.0f %.0f)", pf[0x90 / 4], pf[0xA0 / 4], pf[0xA4 / 4], pf[0xA8 / 4]);
                  }
                  fprintf(stderr, "\n");
                  da0[k] = q[0xA0 / 4]; da4[k] = q[0xA4 / 4]; da8[k] = q[0xA8 / 4]; de4[k] = q[0xE4 / 4];
              }
              /* The player's spray state against this spot (sub_0008D280
               * commits a part when the player is inside its six planes --
               * sub_0004A6F0, dot(plane point - P, normal) >= 0 for all six --
               * and the part does not already hold the player's design +0xC9C). */
              if (s_player_va) {
                  const uint32_t *pl = (const uint32_t *)(m + s_player_va);
                  const float *P = (const float *)(m + s_player_va + 0xCA4);
                  uint32_t np = q[0x98 / 4], pa = q[0x94 / 4], i2, mask = 0;
                  static uint32_t lastkey[128]; static double lastt[128];
                  float best = 1e9f;
                  for (i2 = 0; i2 < np && i2 < 16 && pa >= 0x10000u && pa + (i2 + 1) * 0xB8u < (64u << 20); i2++) {
                      const float *b = (const float *)(m + pa + i2 * 0xB8u);
                      float worst = 1e9f; int j;
                      for (j = 0; j < 6; j++) {
                          float dx = b[j * 3] - P[0], dy = b[j * 3 + 1] - P[1], dz = b[j * 3 + 2] - P[2];
                          float dd = dx * b[18 + j * 3] + dy * b[18 + j * 3 + 1] + dz * b[18 + j * 3 + 2];
                          if (dd < worst) worst = dd;
                      }
                      if (worst >= 0.0f) mask |= 1u << i2;
                      if (-worst < best) best = -worst;
                  }
                  if (pl[0x2C0 / 4] == w[0x58 / 4] || mask) {
                      uint32_t key = mask ^ (pl[0x438 / 4] << 20) ^ (pl[0x2C0 / 4] << 8) ^ (q[0xA4 / 4] * 31u);
                      double now = pad_now();
                      if (key != lastkey[k] || now - lastt[k] > 2.0) {
                          lastkey[k] = key; lastt[k] = now;
                          fprintf(stderr, "[SPRAY] t=%.2f spot %08X: player at (%.1f %.1f %.1f) +2C0 %08X +438 %u +44C %u "
                                  "+C9C %u +CA0 %u +450 %08X +468 %08X | inside mask %X (nearest miss %.1f) A4 %08X B4 %08X C4 %u\n",
                                  now, w[0x58 / 4], P[0], P[1], P[2], pl[0x2C0 / 4], pl[0x438 / 4], pl[0x44C / 4],
                                  pl[0xC9C / 4], pl[0xCA0 / 4], pl[0x450 / 4], pl[0x468 / 4], mask, best,
                                  q[0xA4 / 4], q[0xB4 / 4], q[0xC4 / 4]);
                      }
                  }
              }
          } }
    }
}


/* ---- widescreen -------------------------------------------------------------
 *
 * The title draws 4:3 and never asks the console whether the TV is 16:9 (its
 * one XGetVideoFlags call, at 0x18AEC6, is about PAL-60). Its camera is the
 * graphics device class at vtable 0x1E0F00:
 *
 *   +0x7C SetCamera(angle, near, far), sub_00153FE0: +0x48 the horizontal
 *         field of view (0x10000 = 360 degrees), +0x54 tan(hfov/2), +0x50
 *         the projection distance in pixels; then
 *   sub_00153A90, the culling frustum: per side plane a slope (tan, at
 *         0x264E84 = -tan and 0x264E8C = +tan) and a sphere-radius factor
 *         (sec = sqrt(1 + tan^2), 0x264E80 / 0x264E88) -- the vertical
 *         planes at 0x264E78.. are separate;
 *   sub_00153EC0, the projection: the static matrix at 0x22E6B8, whose
 *         m00 is a constant 1.0 and whose field of view rides in w (m23 =
 *         tan), copied to 0x264EF8 for the shaders and passed to
 *         D3DDevice_SetTransform(D3DTS_PROJECTION = 1 on Xbox).
 *   +0x6C SetTransform(state, matrix) (sub_00154420): any other matrix the
 *         title sets explicitly.
 *
 * Widescreen (nv2a_widescreen(): RECOMP_WIDESCREEN, or Video > Widescreen)
 * keeps the vertical field of view and widens the horizontal one to 16:9 --
 * or to the screen's own shape where the platform asks for it (the iPhone's
 * "Fill the screen", nv2a_set_wide_aspect): m00 = s = (4/3) / that shape
 * (0.75 at 16:9) for every perspective projection, and the side planes'
 * slope / s so the title culls against what is now on screen. The 640x480
 * frame then holds the wide view squeezed, and the window shows it at its
 * shape; the renderer squeezes the 2D overlays (nv2a_gl.c) so they keep
 * their shape. JSRF_WIDE_LOG=1 logs the projections the title sets. */
extern int nv2a_widescreen(void);
extern float nv2a_wide_squeeze(void);
static int wide_log(void)
{
    static int on = -1;
    if (on < 0) on = getenv("JSRF_WIDE_LOG") ? 1 : 0;
    return on;
}
/* JSRF_WIDE_NOPROJ=1 (test only): leave the 3D projection and culling 4:3,
 * so a widescreen run differs from a 4:3 one only by the 2D squeeze. */
static int wide_proj(void)
{
    static int on = -1;
    if (on < 0) on = getenv("JSRF_WIDE_NOPROJ") ? 0 : 1;
    return on && nv2a_wide_scene();
}

extern void sub_00153EC0_gen(void);
void sub_00153EC0(void)
{
    float *m00 = (float *)((uint8_t *)g_xbox_mem_offset + 0x22E6B8u);
    if (wide_log()) {
        static uint32_t said;
        if (g_ecx != said && g_ecx >= 0x10000u && g_ecx < (64u << 20)) {
            said = g_ecx;
            fprintf(stderr, "[WIDE] t=%.1f camera device %08X, vtable %08X\n", pad_now(), g_ecx,
                    *(uint32_t *)((uint8_t *)g_xbox_mem_offset + g_ecx));
        }
    }
    s_cam_dev = g_ecx;
    s_wide_applied = nv2a_wide_scene();
    float want = wide_proj() ? nv2a_wide_squeeze() : 1.0f;
    /* Only over what was there: the title's 1.0, or what this last put. */
    static float put = 0.75f;
    if ((*m00 == 1.0f || *m00 == put) && *m00 != want) {
        if (wide_log())
            fprintf(stderr, "[WIDE] t=%.1f camera projection m00 %.2f -> %.2f\n",
                    pad_now(), *m00, want);
        *m00 = want;
    }
    if (want != 1.0f) put = want;
    sub_00153EC0_gen();
}

extern void sub_00153A90_gen(void);
void sub_00153A90(void)
{
    sub_00153A90_gen();
    if (wide_proj()) {
        float *f = (float *)((uint8_t *)g_xbox_mem_offset + 0x264E78u);
        /* f[2] = sec at 0x264E80, f[3] = -tan, f[4] = sec, f[5] = +tan */
        float t = f[5] * (1.0f / nv2a_wide_squeeze());   /* x 4/3 at 16:9 */
        float sec = sqrtf(1.0f + t * t);
        f[2] = sec; f[3] = -t; f[4] = sec; f[5] = t;
        { static float said;
          if (wide_log() && t != said) {
              said = t;
              fprintf(stderr, "[WIDE] t=%.1f culling: side slope %.4f (camera tan %.4f), radius factor %.4f\n",
                      pad_now(), t, t * nv2a_wide_squeeze(), sec);
          } }
    }
}

extern void sub_00154420_gen(void);
void sub_00154420(void)
{
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    uint32_t self = *(uint32_t *)(m + g_esp + 4);
    uint32_t state = *(uint32_t *)(m + g_esp + 8);
    uint32_t mat = *(uint32_t *)(m + g_esp + 12);
    float *mf = NULL, keep[4];
    int i;
    if (state == 1 && wide_proj()) {
        if (mat == 0xFFFFFFFFu && self >= 0x10000u && self + 0x68u < (64u << 20)) {
            uint32_t top = *(uint32_t *)(m + self + 0x64);
            if (top >= 0x10000u && top + 4u < (64u << 20))
                mat = *(uint32_t *)(m + top);
        }
        if (mat >= 0x10000u && mat != 0xFFFFFFFFu && mat + 64u < (64u << 20)) {
            float *p = (float *)(m + mat);
            /* Perspective: w comes from z (m23) and not from the constant
             * (m33). An orthographic projection is a 2D overlay -- left to
             * the renderer. */
            if (p[11] != 0.0f && p[15] == 0.0f) {
                mf = p;
                for (i = 0; i < 4; i++) { keep[i] = p[i * 4]; p[i * 4] *= nv2a_wide_squeeze(); }
            }
            if (wide_log()) {
                static int n;
                if (n++ < 40)
                    fprintf(stderr, "[WIDE] t=%.1f SetTransform(PROJECTION) %08X: %s m00 %.3f m11 %.3f "
                            "m22 %.3f m23 %.3f m32 %.3f m33 %.3f\n", pad_now(), mat,
                            mf ? "perspective, widened" : "not perspective, left",
                            mf ? keep[0] : p[0], p[5], p[10], p[11], p[14], p[15]);
            }
        }
    }
    sub_00154420_gen();
    if (mf)
        for (i = 0; i < 4; i++) mf[i * 4] = keep[i];
}


/* ---- a graffiti mesh's draw ---------------------------------------------------
 * sub_00042EE0 (ecx = the spot's mesh, spot +0x90) draws a sprayed spot: the
 * mesh's texture (+8: count, id) on stages 0-3, c0 = (256, 1, 0, 0.5), and per
 * part pair c12+ the fades (+0xC/+0x10 per part, 8 apart) and c22+ the design
 * offsets (+0x64/+0x68), then [0x1F90C4]->+0x58(dev, [0x1F90C8], +0, +4).
 * JSRF_GRF_LOG=1 prints what it drew with, at most once a second per mesh. */
extern void sub_00042EE0_gen(void);
void sub_00042EE0(void)
{
    static int on = -1;
    static uint32_t seen[32]; static double when[32]; static int nseen;
    const uint8_t *m = (const uint8_t *)g_xbox_mem_offset;
    const uint32_t lim = 64u << 20;
    uint32_t mesh = g_ecx;
    static int skip = -1;
    if (on < 0) on = getenv("JSRF_GRF_LOG") != NULL;
    if (skip < 0) skip = getenv("JSRF_GRF_SKIP") != NULL;
    if (skip) { g_esp += 4; return; }   /* A/B: which NV2A draw is the graffiti
                                         * (the callee's `ret` pops the return address) */
    sub_00042EE0_gen();
    if (!on || mesh < 0x10000u || mesh + 0xC0u >= lim) return;
    {
        const uint32_t *w = (const uint32_t *)(m + mesh);
        double now = pad_now();
        int k = -1, i;
        for (i = 0; i < nseen; i++) if (seen[i] == mesh) { k = i; break; }
        if (k < 0) { if (nseen >= 32) return; k = nseen++; seen[k] = mesh; when[k] = -9; }
        if (now - when[k] < 1.0) return;
        when[k] = now;
        fprintf(stderr, "[GRF] t=%.2f mesh %08X: +0 %08X +4 %08X +8 %08X", now, mesh, w[0], w[1], w[2]);
        if (w[2] >= 0x10000u && w[2] + 16u < lim) {
            const uint32_t *tl = (const uint32_t *)(m + w[2]);
            fprintf(stderr, " (texlist n=%u id %08X %08X)", tl[0], tl[1], tl[2]);
        }
        for (i = 0; i < 2; i++) {
            uint32_t o = w[i];
            if (o >= 0x10000u && o + 0x20u < lim) {
                const uint32_t *b = (const uint32_t *)(m + o);
                fprintf(stderr, " | +%d-> %08X %08X %08X %08X %08X", i * 4, b[0], b[1], b[2], b[3], b[4]);
            }
        }
        { uint32_t t = *(const uint32_t *)(m + 0x251D54u);
          /* A handle into the title's own table: its SetTexture (0x14FDE0)
           * passes [0x264F68][handle] to D3DDevice_SetTexture when the handle
           * is below [0x264F70]. */
          uint32_t tab = *(const uint32_t *)(m + 0x264F68u), cnt = *(const uint32_t *)(m + 0x264F70u);
          fprintf(stderr, " | stage0 handle %u (table %08X n %u)", t, tab, cnt);
          if (t < cnt && tab >= 0x10000u && tab + 4u * cnt < lim) {
              uint32_t tx = *(const uint32_t *)(m + tab + 4u * t);
              fprintf(stderr, " -> D3D tex %08X", tx);
              if (tx >= 0x10000u && tx + 0x20u < lim) {
                  const uint32_t *b = (const uint32_t *)(m + tx);
                  fprintf(stderr, " = common %08X data %08X lock %08X format %08X size %08X", b[0], b[1], b[2], b[3], b[4]);
              }
          } }
        { uint32_t r = *(const uint32_t *)(m + 0x251D7Cu);
          if (r >= 0x10000u && r + 0x580u < lim)
              fprintf(stderr, " | fx %08X/%08X", *(const uint32_t *)(m + r + 0x544), *(const uint32_t *)(m + r + 0x56C));
          fprintf(stderr, " dev %08X vb? %08X draws %u", *(const uint32_t *)(m + 0x1F90C4u),
                  *(const uint32_t *)(m + 0x1F90C8u), *(const uint32_t *)(m + 0x251D40u));
          { uint32_t d = *(const uint32_t *)(m + 0x251D6Cu), d2 = *(const uint32_t *)(m + 0x1F90C4u);
            if (d >= 0x10000u && d + 8u < lim) {
                uint32_t vt = *(const uint32_t *)(m + d);
                if (vt >= 0x10000u && vt + 0x150u < lim)
                    fprintf(stderr, " | d3d %08X vt %08X +144 %08X", d, vt, *(const uint32_t *)(m + vt + 0x144));
            }
            if (d2 >= 0x10000u && d2 + 8u < lim) {
                uint32_t vt = *(const uint32_t *)(m + d2);
                if (vt >= 0x10000u && vt + 0x60u < lim)
                    fprintf(stderr, " | drawer vt %08X +58 %08X", vt, *(const uint32_t *)(m + vt + 0x58));
            } } }
        fprintf(stderr, "\n[GRF]   parts (fadeA fadeB | offA offB):");
        for (i = 0; i < 10; i++) {
            float a, b2, c, d;
            memcpy(&a, m + mesh + 0x10 + i * 8, 4); memcpy(&b2, m + mesh + 0xC + i * 8, 4);
            memcpy(&c, m + mesh + 0x68 + i * 8, 4); memcpy(&d, m + mesh + 0x64 + i * 8, 4);
            fprintf(stderr, " %d:%.2f %.2f|%.2f %.2f", i, a, b2, c, d);
        }
        fprintf(stderr, "\n");
    }
}


/* ---- the camera on the right stick -------------------------------------------
 *
 * Not in the original game. Its camera follows the skater by itself, the L
 * trigger puts it back behind, and the right stick only does one thing: with
 * the skater standing still it switches to a first-person look (camera mode
 * 14). With this, the right stick turns the camera around the skater and
 * tilts it up and down, moving or not; let go, and after a moment it drifts
 * back behind; the L trigger still puts it back at once. While it does, the
 * game is not shown the right stick, so the first-person look is not there.
 *
 * The camera (object 0x4C, below) works out where it wants to be in its own
 * fields -- the eye at +0x20C, the point it looks at at +0x218, eased from
 * one frame to the next by its update, sub_000A5070 -- and sub_000A28B0
 * publishes them: the eye to +0x44 and the point to +0x5C, which the view is
 * drawn from, the heading from one to the other (+0x228, 0x10000 to a turn:
 * atan2 of x over z) and the distance (+0x230). So the turn is made to what
 * is published, right after it is: the eye, about the point looked at, and
 * the heading with it -- the skater's controls go by the heading, so "up" on
 * the left stick goes where the camera now looks (with the heading left
 * alone, a quarter turn of the view sent the skater off to the side of the
 * screen). The camera's own fields are never touched, and what was published
 * is put back before the camera's next update and publication, so that the
 * game's camera moves exactly as it always did and only the view is turned.
 * If something else moved the eye in between (an event placing the camera),
 * that is left as it is and the turn dropped.
 *
 * Tracking, the second way to have it (issue #44, players' feedback): the
 * view turns as above but the heading is left alone, so the skater keeps the
 * way he was going -- "up" on the left stick is still straight ahead of him,
 * as the game's own camera behind him has it -- and the right stick only
 * looks around. With the turn on the heading, panning the camera to look at
 * something steered the skater towards it.
 *
 * Not in a conversation (mode 12) or the first-person look (14).
 * nv_window_pref_rstick_camera says which: 0 off (the game's camera), 1 the
 * camera (the default), 2 tracking -- the Mac's Controls menu, the iPhone's
 * and the Android app's settings, and on Windows and Linux F7 (and Controls
 * in the Windows window's menu bar); RECOMP_RSTICK_CAMERA=0, 1 or 2
 * overrides.
 * The turn goes by the game's frames, a sixtieth of a second each, as its own
 * camera and its first-person look do: by the clock, it came out in uneven
 * steps from one picture to the next whenever the frames were not evenly
 * spaced -- a slow pan judders, a player's report: "like a really bad V-Sync".
 * JSRF_RCAM_CLOCK=1 goes by the clock, as 0.5.8 did.
 * Tests: JSRF_RCAM_LOG=1 logs it ten times a second (=2: at every frame);
 * JSRF_RCAM_FIXED=<degrees> holds a fixed turn; JSRF_RCAM_DUMPCAM=<s> prints
 * the camera object once.
 */
__attribute__((weak)) int nv_window_pref_rstick_camera(void);
__attribute__((weak)) int nv_window_pref_rstick_camera(void) { return 0; }   /* no switch: off */

#define RCAM_DEAD   0.20f      /* of full deflection: a resting stick drifts */
/* The speed: what is past the dead zone, to the power 1.5 -- fine near the
 * middle of the stick, and not next to nothing at a third of the way (the
 * square it was gave a thirtieth of a degree a frame at 30%). */
#define RCAM_CURVE(x) ((x) * sqrtf(fabsf(x)))
#define RCAM_YAW    2.6f       /* radians a second at full deflection, about 150 degrees */
#define RCAM_PITCH  1.4f
#define RCAM_PMAX   0.6f       /* the tilt either way, about 35 degrees */
/* The eye's elevation above the point looked at (the skater's head), kept
 * within: from a little below it, looking up, to well above. */
#define RCAM_EL_MIN -0.30f
#define RCAM_EL_MAX 1.2f
#define RCAM_HOLD   0.6f       /* seconds after the stick is let go before it drifts back */
#define RCAM_TAU    0.3f       /* and how quickly: e-fold time, seconds */

static struct {
    float    rx, ry;           /* the right stick, -1..1, dead zone taken out */
    int      lt;               /* the L trigger held */
    float    yaw, pitch;       /* the turn, radians: to the right, and up */
    float    idle;             /* seconds since the stick was let go */
    double   last;             /* when the turn was last worked out */
    uint32_t last_frame;       /* and at which of the game's frames (s_rcam_frames) */
    double   active_until;     /* the stick is the camera's (not the game's) until then */
    uint32_t cam;              /* the camera it was applied to */
    float    game_eye[3];      /* the eye as the game published it */
    float    our_eye[3];       /* and as this left it */
    int32_t  game_hd, our_hd;  /* the heading, likewise */
    int      applied;
    int      photo;            /* photo mode moved the point looked at too */
    float    game_tgt[3], our_tgt[3];
} s_rcam;
/* The camera's updates (sub_000A5070): one a game frame. */
static uint32_t s_rcam_frames;

/* ---- Photo mode -------------------------------------------------------------
 *
 * Hold Back (Esc on a keyboard) for most of a second while skating -- or pick
 * Photo Mode in a menu or the settings -- and the camera leaves the skater:
 * the left stick moves it, the right stick turns it, the triggers lower and
 * raise it; Y hides the HUD, A this help, and B or Back goes back to the
 * game. The game sees no buttons meanwhile, so the skater stands where he
 * was (the city goes on around him). The camera is the game's own, moved:
 * its eye (+0x44) and the point it looks at (+0x5C) are set where the photo
 * camera is after the game publishes them and put back before its next
 * update (rcam_restore), as the camera on the right stick does with the
 * eye. A help panel says all that on screen while it is on
 * (nv_window_photo_help: the window's own on the Mac and the iPhone, drawn
 * into the picture's window with SDL). */
void nv_window_photo_help(const char *text);      /* NULL: none */
void nv2a_set_hide_hud(int hide);
void nv2a_photo_pinned(int on);
void nv2a_photo_turned(int turned);
int  nv_window_take_photo_request(void);          /* a menu's Photo Mode, once */
#define PHOTO_HOLD   0.8       /* seconds of Back to start it */
#define PHOTO_SPEED  110.0f    /* units a second at full stick */
#define PHOTO_RISE   70.0f
#define PHOTO_YAW    1.8f      /* radians a second */
#define PHOTO_PITCH  1.3f
static const char k_photo_help[] =
    "PHOTO MODE\n"
    "LEFT STICK     MOVE          (W A S D)\n"
    "RIGHT STICK    LOOK          (ARROWS)\n"
    "LT / RT        DOWN / UP     (Q / E)\n"
    "Y              HUD ON / OFF  (L)\n"
    "A              HIDE HELP     (SPACE)\n"
    "B OR BACK      BACK TO GAME  (K / ESC)";
static struct {
    volatile int want;          /* asked for: enter at the next gameplay frame */
    int      on;
    double   back_since;        /* Back held since (0: not held) */
    int      back_was;          /* Back down at the last poll, while on */
    uint16_t btn_was;
    uint8_t  a_was, b_was, y_was;
    float    lx, ly, rx, ry, lt, rt;
    float    pos[3], yaw, pitch, yaw0, pitch0;
    int      help, hud_hidden;
} s_photo;
static float photo_axis(int16_t v)
{
    float x = (float)v / 32767.0f;
    if (x > -0.18f && x < 0.18f) return 0.0f;
    return x > 0 ? (x - 0.18f) / 0.82f : (x + 0.18f) / 0.82f;
}
static void photo_show_help(void)
{
    nv_window_photo_help(s_photo.on && s_photo.help ? k_photo_help : NULL);
}
static void photo_leave(const char *why)
{
    if (!s_photo.on) return;
    s_photo.on = 0;
    s_world_frozen = 0;
    nv2a_photo_pinned(0);
    if (s_photo.hud_hidden) nv2a_set_hide_hud(0);
    s_photo.hud_hidden = 0;
    photo_show_help();
    fprintf(stderr, "[PHOTO] off (%s)\n", why);
}
/* The pad, before the game sees it. Returns 1 when photo mode took it. */
static int photo_note_pad(XBOX_INPUT_STATE *s)
{
    double now = jsrf_now();
    { static int test = -1;   /* JSRF_PHOTO_HELP_TEST=1 (test only): the panel shown from the start */
      if (test < 0) { test = getenv("JSRF_PHOTO_HELP_TEST") ? 1 : 0; if (test) nv_window_photo_help(k_photo_help); } }
    int back = (s->Gamepad.wButtons & XBOX_GAMEPAD_BACK) != 0;
    if (!s_photo.on) {
        if (back) {
            if (!s_photo.back_since) s_photo.back_since = now;
            else if (now - s_photo.back_since >= PHOTO_HOLD) { s_photo.want = 1; s_photo.back_since = 0; }
        } else {
            s_photo.back_since = 0;
        }
        if (nv_window_take_photo_request()) s_photo.want = 1;
        return 0;
    }
    {
        uint8_t a = s->Gamepad.bAnalogButtons[XBOX_BUTTON_A] > 64;
        uint8_t b = s->Gamepad.bAnalogButtons[XBOX_BUTTON_B] > 64;
        uint8_t y = s->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] > 64;
        s_photo.lx = photo_axis(s->Gamepad.sThumbLX);
        s_photo.ly = photo_axis(s->Gamepad.sThumbLY);
        s_photo.rx = photo_axis(s->Gamepad.sThumbRX);
        s_photo.ry = photo_axis(s->Gamepad.sThumbRY);
        s_photo.lt = s->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] / 255.0f;
        s_photo.rt = s->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] / 255.0f;
        if (a && !s_photo.a_was) { s_photo.help = !s_photo.help; photo_show_help(); }
        if (y && !s_photo.y_was) {
            s_photo.hud_hidden = !s_photo.hud_hidden;
            nv2a_set_hide_hud(s_photo.hud_hidden);
        }
        if ((b && !s_photo.b_was) || (back && !s_photo.back_was)) photo_leave("B or Back");
        s_photo.a_was = a; s_photo.b_was = b; s_photo.y_was = y; s_photo.back_was = back;
    }
    /* The game sees an idle pad (the packet number still moves). */
    memset(&s->Gamepad, 0, sizeof s->Gamepad);
    return 1;
}
/* Each game frame, with the camera just published. 1 when photo mode set it. */
/* JSRF_PHOTO_ON_CAN=<s> (test only): photo mode <s> seconds after the first can
 * is picked up -- with that can on its way to the counter -- then a picture
 * at 0.2 s, the HUD off and a picture at 0.8 s, the camera turned from 0.8 s
 * to 1.6 s and a picture at 2 s (RECOMP_GL_DUMP_FB). The pad's sticks are
 * left out meanwhile. */
static void photo_can_test(float dt)
{
    static int on = -1, step; static double t, delay, since;
    if (on < 0) { const char *e = getenv("JSRF_PHOTO_ON_CAN"); on = e ? 1 : 0; delay = e ? atof(e) : 0; }
    if (!on || (step >= 6 + 2 * 6 && step < 99) || step >= 121) return;
    if (step == 0) { if (s_can_pickups) since += dt;
                     if (s_can_pickups && since >= delay) { s_photo.want = 1; step = 1; t = 0;
                       fprintf(stderr, "[PHOTOTEST] a can picked up %.2f s ago: photo mode\n", since); } return; }
    if (!s_photo.on) return;
    t += dt;
    s_photo.lx = s_photo.ly = s_photo.ry = s_photo.lt = s_photo.rt = 0.0f;   /* the script's sticks: not the camera's */
    s_photo.rx = (step == 4 || step == 5) && t < 1.6 ? 0.8f : 0.0f;
    if (step == 1 && t >= 0.2) { nv2a_gl_dump_burst(1, 1); step = 2;
                                 fprintf(stderr, "[PHOTOTEST] picture: HUD on\n"); }
    else if (step == 2 && t >= 0.4) { s_photo.hud_hidden = 1; nv2a_set_hide_hud(1); s_photo.help = 0;
                                      photo_show_help(); step = 3; }
    else if (step == 3 && t >= 0.8) { nv2a_gl_dump_burst(1, 1); step = 4;
                                      if (getenv("JSRF_PHOTO_TRACE")) nv2a_gl_trace_frames(1);
                                      fprintf(stderr, "[PHOTOTEST] picture: HUD off\n"); }
    else if (step == 4 && t >= 1.0) step = 5;
    else if (step == 5 && t >= 2.0) { nv2a_gl_dump_burst(1, 1); step = 6;
                                      fprintf(stderr, "[PHOTOTEST] picture: HUD off, turned\n"); }
    /* JSRF_PHOTO_SKIP_TEX=<hex>: then the 1st..6th draw from that texture
     * left out in turn, a picture of each. */
    else if (step >= 6) {
        static uint32_t tex; static int init; static double at;
        static int bits;
        if (!init) { const char *e = getenv("JSRF_PHOTO_SKIP_TEX"); init = 1; tex = e ? (uint32_t)strtoul(e, 0, 16) : 0;
                     bits = getenv("JSRF_PHOTO_SKIP_BITS") ? 1 : 0;
                     if (!tex && !bits) step = 99; at = t; if (bits) step = 100; }
        /* JSRF_PHOTO_SKIP_BITS=1: instead, ten pictures, each with the 3D draws
         * whose place in the frame has bit 0..9 set left out, then the frame's
         * draws traced: which pictures lose something tells which draw it is. */
        if (step >= 100 && step < 120) {
            int i = (step - 100) / 2;
            if (((step - 100) & 1) == 0 && t >= at + 0.3) { nv2a_gl_test_skip_bit(i); step++; at = t; }
            else if (((step - 100) & 1) == 1 && t >= at + 0.3) {
                nv2a_gl_dump_burst(1, 1); step++; at = t;
                fprintf(stderr, "[PHOTOTEST] picture: bit %d left out\n", i);
                if (step == 120) { nv2a_gl_test_skip_bit(-1); }
            }
            return;
        }
        if (step == 120 && t >= at + 0.3) { nv2a_gl_trace_frames(3); step = 121; return; }
        if (step >= 99) return;
        if (((step - 6) & 1) == 0 && t >= at + 0.3) { nv2a_gl_test_skip(tex, (step - 6) / 2); step++; at = t; }
        else if (((step - 6) & 1) == 1 && t >= at + 0.3) { nv2a_gl_dump_burst(1, 1); step++; at = t;
            fprintf(stderr, "[PHOTOTEST] picture: draw %d of %08X left out\n", (step - 7) / 2, tex); }
    }
}
static int photo_frame(uint8_t *m, uint32_t cam, uint32_t mode, float dt)
{
    float *e = (float *)(m + cam + 0x44), *t = (float *)(m + cam + 0x5C);
    float f[3], r[3], cp, sp;
    photo_can_test(dt);
    if (s_photo.on && (mode == 12 || mode == 14)) photo_leave("an event");
    if (s_photo.want && !s_photo.on) {
        float v[3] = { t[0] - e[0], t[1] - e[1], t[2] - e[2] };
        s_photo.want = 0;
        if (mode == 12 || mode == 14) return 0;
        s_photo.on = 1;
        s_frozen_cam = cam;
        s_world_frozen = photo_freeze_on();
        s_photo.help = 1;
        s_photo.hud_hidden = 0;
        s_photo.back_was = 1;          /* the Back that started it does not end it */
        s_photo.a_was = s_photo.b_was = s_photo.y_was = 1;
        memcpy(s_photo.pos, e, 12);
        s_photo.yaw = atan2f(v[0], v[2]);
        s_photo.pitch = atan2f(v[1], sqrtf(v[0] * v[0] + v[2] * v[2]));
        s_photo.yaw0 = s_photo.yaw; s_photo.pitch0 = s_photo.pitch;
        if (s_world_frozen) nv2a_photo_pinned(1);   /* what is pinned to the screen: see nv2a_gl.c */
        photo_show_help();
        fprintf(stderr, "[PHOTO] on, at %.1f %.1f %.1f\n", e[0], e[1], e[2]);
    }
    s_photo.want = 0;
    if (!s_photo.on) return 0;
    s_photo.yaw += s_photo.rx * PHOTO_YAW * dt;
    s_photo.pitch += s_photo.ry * PHOTO_PITCH * dt;
    if (s_photo.pitch > 1.45f) s_photo.pitch = 1.45f;
    if (s_photo.pitch < -1.45f) s_photo.pitch = -1.45f;
    nv2a_photo_turned(fabsf(s_photo.yaw - s_photo.yaw0) > 0.02f || fabsf(s_photo.pitch - s_photo.pitch0) > 0.02f);
    cp = cosf(s_photo.pitch); sp = sinf(s_photo.pitch);
    f[0] = sinf(s_photo.yaw) * cp; f[1] = sp; f[2] = cosf(s_photo.yaw) * cp;
    r[0] = cosf(s_photo.yaw); r[1] = 0.0f; r[2] = -sinf(s_photo.yaw);   /* the right of (x, z) is (z, -x) */
    {
        int i;
        for (i = 0; i < 3; i++)
            s_photo.pos[i] += (f[i] * s_photo.ly + r[i] * s_photo.lx) * PHOTO_SPEED * dt;
        s_photo.pos[1] += (s_photo.rt - s_photo.lt) * PHOTO_RISE * dt;
        memcpy(s_rcam.game_eye, e, 12);
        memcpy(s_rcam.game_tgt, t, 12);
        memcpy(e, s_photo.pos, 12);
        for (i = 0; i < 3; i++) t[i] = s_photo.pos[i] + f[i] * 40.0f;
        memcpy(s_rcam.our_eye, e, 12);
        memcpy(s_rcam.our_tgt, t, 12);
    }
    s_rcam.game_hd = s_rcam.our_hd = *(int32_t *)(m + cam + 0x228);
    s_rcam.cam = cam;
    s_rcam.applied = 1;
    s_rcam.photo = 1;
    s_rcam.yaw = s_rcam.pitch = 0.0f;
    return 1;
}

#include "jsrf_camera_experiment.inc"
static void rcam_note_pad(XBOX_INPUT_STATE *s)
{
    if (picker_note_pad(s) || photo_note_pad(s)) { jcx_pad_failed(); s_rcam.rx = s_rcam.ry = 0.0f; s_rcam.lt = 0; return; }
    if (jcx_enabled()) jcx_note_pad(s);
    float x = (float)s->Gamepad.sThumbRX / 32767.0f, y = (float)s->Gamepad.sThumbRY / 32767.0f;
    float mag = sqrtf(x * x + y * y);
    if (mag < RCAM_DEAD) {
        x = y = 0.0f;
    } else {
        float k = ((mag > 1.0f ? 1.0f : mag) - RCAM_DEAD) / (1.0f - RCAM_DEAD) / mag;
        x *= k; y *= k;
    }
    s_rcam.rx = x;
    s_rcam.ry = y;
    s_rcam.lt = s->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] > 64;
    /* While the camera is turning with it, the game does not see the stick
     * (its first-person look). Elsewhere -- menus, conversations, events --
     * it gets it as always. */
    if (jsrf_now() < s_rcam.active_until) {
        s->Gamepad.sThumbRX = 0;
        s->Gamepad.sThumbRY = 0;
    }
}

/* 0 off, 1 the camera, 2 tracking (see above). */
static int rcam_mode(void)
{
    static int env = -2;
    int v;
    if (env == -2) {
        const char *e = getenv("RECOMP_RSTICK_CAMERA");
        env = (e && *e) ? (atoi(e) == 2 ? 2 : atoi(e) != 0) : -1;
    }
    if (env >= 0) return env;
    v = nv_window_pref_rstick_camera();
    return v == 2 ? 2 : v > 0;
}

static int rcam_log_on(void)
{
    static int on = -1;
    if (on < 0) { const char *v = getenv("JSRF_RCAM_LOG"); on = v ? (atoi(v) == 2 ? 2 : 1) : 0; }
    return on;
}

/* Before the camera's update and its publication: what it published, back --
 * unless something else moved the eye meanwhile. */
static void rcam_restore(uint8_t *m, uint32_t cam)
{
    float *e;
    int32_t *hd;
    if (!s_rcam.applied) return;
    s_rcam.applied = 0;
    if (cam != s_rcam.cam || cam < 0x10000u || cam + 0x240u >= (64u << 20)) return;
    e = (float *)(m + cam + 0x44);
    hd = (int32_t *)(m + cam + 0x228);
    if (s_rcam.photo) {
        float *t = (float *)(m + cam + 0x5C);
        if (!memcmp(t, s_rcam.our_tgt, sizeof s_rcam.our_tgt)) memcpy(t, s_rcam.game_tgt, sizeof s_rcam.game_tgt);
        s_rcam.photo = 0;
    }
    if (memcmp(e, s_rcam.our_eye, sizeof s_rcam.our_eye) != 0) {
        if (rcam_log_on())
            fprintf(stderr, "[RCAM] t=%.2f the eye was moved by something else: turn dropped\n", jsrf_now());
        s_rcam.yaw = s_rcam.pitch = 0.0f;
        jcx_forget();
        return;
    }
    memcpy(e, s_rcam.game_eye, sizeof s_rcam.game_eye);
    if (*hd == s_rcam.our_hd) *hd = s_rcam.game_hd;
}

/* Right after the publication: the turn worked out from the stick, and made. */
static void rcam_apply(uint8_t *m, uint32_t cam)
{
    double now = jsrf_now();
    float dt;
    const uint32_t *c;
    float *e, *t, v[3], h, len, el, el2, ca, sa, lo, hi;
    int32_t *hd;
    static double fixed = -1.0e9;
    static double next_log, dump_at = -2.0;
    static int by_clock = -1;
    uint32_t mode, frames;
    int ok, i, how;
    if (by_clock < 0) by_clock = getenv("JSRF_RCAM_CLOCK") ? 1 : 0;
    /* The game's frames since the last turn (one, as a rule), a sixtieth of a
     * second each; up to six, after a pause. */
    frames = s_rcam.last > 0.0 ? s_rcam_frames - s_rcam.last_frame : 0u;
    if (frames > 6u) frames = 6u;
    if (by_clock) {
        dt = s_rcam.last > 0.0 ? (float)(now - s_rcam.last) : 0.0f;
        if (dt > 0.1f) dt = 0.1f;      /* a hitch, or the first frame */
    } else {
        dt = (float)frames * (1.0f / 60.0f);
    }
    s_rcam.last = now;
    s_rcam.last_frame = s_rcam_frames;
    if (cam < 0x10000u || cam + 0x240u >= (64u << 20)) return;
    c = (const uint32_t *)(m + cam);
    if (dump_at == -2.0) {
        const char *d = getenv("JSRF_RCAM_DUMPCAM");
        dump_at = d ? atof(d) : -1.0;
    }
    if (dump_at >= 0.0 && now >= dump_at) {
        dump_at = -1.0;
        fprintf(stderr, "[RCAM] camera %08X at t=%.2f, as floats:", cam, now);
        for (i = 0; i < 0x240 / 4; i++) {
            if (i % 8 == 0) fprintf(stderr, "\n  +%03X", i * 4);
            fprintf(stderr, " %10.3f", ((const float *)c)[i]);
        }
        fprintf(stderr, "\n");
    }
    if (fixed == -1.0e9) {
        const char *f = getenv("JSRF_RCAM_FIXED");
        fixed = f ? atof(f) * 3.14159265358979 / 180.0 : -1.0e8;
    }
    mode = c[0x208 / 4];
    if (photo_frame(m, cam, mode, dt)) { jcx_forget(); s_rcam.active_until = 0.0; return; }
    if (jcx_apply(m, cam, dt)) return;
    how = rcam_mode();
    ok = how > 0 && mode != 12 && mode != 14;
    s_rcam.active_until = ok ? now + 0.25 : 0.0;
    if (!ok) {
        s_rcam.yaw = s_rcam.pitch = 0.0f;
        s_rcam.idle = 0.0f;
    } else if (fixed > -1.0e7) {
        s_rcam.yaw = (float)fixed;
        s_rcam.pitch = 0.0f;
    } else if (s_rcam.lt) {
        s_rcam.yaw = s_rcam.pitch = 0.0f;   /* the game puts its camera back behind as well */
        s_rcam.idle = 0.0f;
    } else if (s_rcam.rx != 0.0f || s_rcam.ry != 0.0f) {
        s_rcam.yaw   += RCAM_CURVE(s_rcam.rx) * RCAM_YAW * dt;
        s_rcam.pitch += RCAM_CURVE(s_rcam.ry) * RCAM_PITCH * dt;
        s_rcam.idle = 0.0f;
    } else {
        s_rcam.idle += dt;
        if (s_rcam.idle > RCAM_HOLD) {
            float k = expf(-dt / RCAM_TAU);
            s_rcam.yaw *= k;
            s_rcam.pitch *= k;
        }
    }
    while (s_rcam.yaw >  3.14159265f) s_rcam.yaw -= 6.28318531f;
    while (s_rcam.yaw < -3.14159265f) s_rcam.yaw += 6.28318531f;
    if (s_rcam.pitch >  RCAM_PMAX) s_rcam.pitch =  RCAM_PMAX;
    if (s_rcam.pitch < -RCAM_PMAX) s_rcam.pitch = -RCAM_PMAX;
    if (fabsf(s_rcam.yaw) < 1e-4f && fabsf(s_rcam.pitch) < 1e-4f) s_rcam.yaw = s_rcam.pitch = 0.0f;

    e = (float *)(m + cam + 0x44);
    t = (float *)(m + cam + 0x5C);
    hd = (int32_t *)(m + cam + 0x228);
    if (ok && (s_rcam.yaw != 0.0f || s_rcam.pitch != 0.0f)) {
        /* The eye about the point looked at: turned about the vertical so
         * that the view turns right for a positive yaw (in this left-handed
         * world the right of (x, z) is (z, -x)), then raised or lowered --
         * looking up lowers it -- within limits that never take it past
         * where the game itself put it. */
        memcpy(s_rcam.game_eye, e, sizeof s_rcam.game_eye);
        v[0] = e[0] - t[0]; v[1] = e[1] - t[1]; v[2] = e[2] - t[2];
        ca = cosf(s_rcam.yaw); sa = sinf(s_rcam.yaw);
        {
            float x = v[0] * ca + v[2] * sa, z = v[2] * ca - v[0] * sa;
            v[0] = x; v[2] = z;
        }
        h = sqrtf(v[0] * v[0] + v[2] * v[2]);
        len = sqrtf(h * h + v[1] * v[1]);
        if (s_rcam.pitch != 0.0f && h > 1e-3f && len > 1e-3f) {
            el = atan2f(v[1], h);
            lo = el < RCAM_EL_MIN ? el : RCAM_EL_MIN;
            hi = el > RCAM_EL_MAX ? el : RCAM_EL_MAX;
            el2 = el - s_rcam.pitch;
            if (el2 < lo) el2 = lo;
            if (el2 > hi) el2 = hi;
            v[1] = len * sinf(el2);
            v[0] *= len * cosf(el2) / h;
            v[2] *= len * cosf(el2) / h;
        }
        e[0] = t[0] + v[0]; e[1] = t[1] + v[1]; e[2] = t[2] + v[2];
        memcpy(s_rcam.our_eye, e, sizeof s_rcam.our_eye);
        /* The heading turned with it, in the game's range (-0x8000..0x8000,
         * as sub_0004A7A0 gives it) -- or, tracking, left as it is. */
        s_rcam.game_hd = *hd;
        if (how != 2)
            *hd = (int32_t)(int16_t)(uint16_t)(s_rcam.game_hd + (int32_t)lrintf(s_rcam.yaw * (65536.0f / 6.28318531f)));
        s_rcam.our_hd = *hd;
        s_rcam.cam = cam;
        s_rcam.applied = 1;
    }
    if (rcam_log_on() == 2) {
        /* Every frame: the step it took, to see that it is even. */
        fprintf(stderr, "[RCAM] t=%.3f flip %u frame %u (+%u) dt %.4f | stick %.2f %.2f | yaw %.3f pitch %.3f\n",
                now, nv2a_gl_flips(), s_rcam_frames, frames, dt, s_rcam.rx, s_rcam.ry,
                s_rcam.yaw * 57.29578f, s_rcam.pitch * 57.29578f);
    } else if (rcam_log_on() && now >= next_log) {
        next_log = now + 0.1;
        fprintf(stderr, "[RCAM] t=%.2f flip %u mode %u on %d how %d | stick %.2f %.2f lt %d | yaw %.1f pitch %.1f | "
                "eye (%.2f %.2f %.2f) game (%.2f %.2f %.2f) tgt (%.2f %.2f %.2f) | hd %d game %d | "
                "player (%.2f %.2f %.2f)\n",
                now, nv2a_gl_flips(), mode, ok, how, s_rcam.rx, s_rcam.ry, s_rcam.lt,
                s_rcam.yaw * 57.29578f, s_rcam.pitch * 57.29578f, e[0], e[1], e[2],
                s_rcam.applied ? s_rcam.game_eye[0] : e[0], s_rcam.applied ? s_rcam.game_eye[1] : e[1],
                s_rcam.applied ? s_rcam.game_eye[2] : e[2], t[0], t[1], t[2],
                (int)*hd, s_rcam.applied ? (int)s_rcam.game_hd : (int)*hd,
                s_player_pos[0], s_player_pos[1], s_player_pos[2]);
    }
}


/* ---- the camera -------------------------------------------------------------
 * Object 0x4C (class 0x1CD518) is the camera; sub_000A5070 its per-frame
 * update. It picks a mode from its target character's state (+0xE58, with
 * overrides from +0xF18, +0xEC8, +0xE38/+0xE3C), switches mode through the
 * table at 0x215534 when that changes (+0x208 holds the current one), runs
 * the mode's update from 0x215570, and returns early while +0x1E0 is set.
 * Eye at +0x44, target at +0x5C. JSRF_CAM_LOG=1 prints every change of the
 * mode inputs and the eye position once a second. */
extern void sub_000A5070_gen(void);
void sub_000A5070(void)
{
    static int on = -1;
    static uint32_t last[10];
    static double next;
    const uint8_t *m = (const uint8_t *)g_xbox_mem_offset;
    const uint32_t lim = 64u << 20;
    uint32_t cam = g_ecx;
    if (on < 0) on = getenv("JSRF_CAM_LOG") != NULL;
    s_rcam_frames++;
    jct_sample(3, 0, cam, s_rcam_frames, jct_begin_call());
    rcam_restore((uint8_t *)(uintptr_t)m, cam);     /* the game's own eye, for its update */
    sub_000A5070_gen();
    if (cam >= 0x10000u && cam + 0x240u < lim)
        s_cam_mode_now = ((const uint32_t *)(m + cam))[0x208 / 4];
    { static int hooks = -1;
      if (hooks < 0) hooks = (getenv("JSRF_CANS") || getenv("JSRF_BOOST_DUMP") || getenv("JSRF_PHOTO_ON_CAN")) ? 1 : 0;
      if (hooks && cam >= 0x10000u && cam + 0x240u < lim) {
          uint32_t who = ((const uint32_t *)(m + cam))[0x24 / 4];
          if (who >= 0x10000u && who + 0x1000u < lim)
              test_hooks_step((uint8_t *)(uintptr_t)(m + who));
      } }
    if (cam >= 0x10000u && cam + 0x240u < lim) {
        uint32_t who = ((const uint32_t *)(m + cam))[0x24 / 4];
        if (who >= 0x10000u && who + 0x1000u < lim) {
            const float *chf = (const float *)(m + who);
            s_player_pos[0] = chf[0xCA4 / 4]; s_player_pos[1] = chf[0xCA8 / 4];
            s_player_pos[2] = chf[0xCAC / 4];
            s_player_va = who;
        }
    }
    save_menu_open_step();
    mission_at_step();
    find_proj_step();
    wide_reapply_step();
    if (cam >= 0x10000u && cam + 0x240u < lim) {
        uint32_t who = ((const uint32_t *)(m + cam))[0x24 / 4];
        if (who >= 0x10000u && who + 0x1000u < lim) {
            /* The teleport no longer needs an autopilot to be set as well:
             * it was only ever called from inside that branch. */
            teleport_step((float *)(uintptr_t)(m + who));
            if (s_auto_n != 0)
                auto_step((const float *)(m + cam), (const float *)(m + who));
        }
    }
    if (!on || cam < 0x10000u || cam + 0x240u >= lim) return;
    {
        const uint32_t *c = (const uint32_t *)(m + cam);
        const float *cf = (const float *)(m + cam);
        uint32_t tgt = c[0x24 / 4];          /* sub_00011BD0: the character it follows */
        uint32_t now[10];
        int i, changed = 0;
        now[0] = c[0x208 / 4]; now[1] = c[0x1E0 / 4]; now[2] = c[0x40 / 4];
        now[3] = c[0x98 / 4];  now[4] = c[0x1DC / 4];
        now[5] = now[6] = now[7] = now[8] = now[9] = 0;
        if (tgt >= 0x10000u && tgt + 0x1000u < lim) {
            const uint32_t *t = (const uint32_t *)(m + tgt);
            now[5] = t[0xE58 / 4]; now[6] = t[0xF18 / 4]; now[7] = t[0xEC8 / 4];
            now[8] = t[0xE38 / 4]; now[9] = t[0xE3C / 4];
        }
        for (i = 0; i < 10; i++) if (now[i] != last[i]) changed = 1;
        if (changed || jsrf_now() >= next) {
            next = jsrf_now() + 1.0;
            fprintf(stderr, "[CAM] t=%.2f flip %u cam %08X mode=%u +1E0=%u +40=%08X +98=%u +1DC=%u | char %08X E58=%u F18=%u EC8=%u E38=%u E3C=%u | eye (%.1f %.1f %.1f) tgt (%.1f %.1f %.1f)%s\n",
                    jsrf_now(), nv2a_gl_flips(), cam, now[0], now[1], now[2], now[3], now[4],
                    tgt, now[5], now[6], now[7], now[8], now[9],
                    cf[0x44 / 4], cf[0x48 / 4], cf[0x4C / 4], cf[0x5C / 4], cf[0x60 / 4], cf[0x64 / 4],
                    changed ? "  <- changed" : "");
            fflush(stderr);
            memcpy(last, now, sizeof last);
        }
    }
}

/* ---- camera call counters (JSRF_CAM_LOG=1): which camera methods and mode
 * handlers actually run, printed every two seconds from the poser. */
static unsigned long jsrf_camcnt[18];
static const uint32_t jsrf_camva[18] = { 0x000A28B0u, 0x000A2960u, 0x000A3A70u, 0x000A3BD0u, 0x000A3D10u, 0x000A59B0u, 0x000A58B0u, 0x000A58C0u, 0x000A58E0u, 0x000A3E50u, 0x000A4100u, 0x000A5A00u, 0x000A5A70u, 0x000A5AC0u, 0x000A5AF0u, 0x000A4890u, 0x000A4D80u, 0x000A4C30u };
extern void sub_000A28B0_gen(void);
/* The camera's publication (see the camera on the right stick, above): what
 * was turned put back first, the turn made again after. */
void sub_000A28B0(void)
{
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    uint32_t cam = g_ecx;
    jsrf_camcnt[0]++;
    if (s_world_frozen) s_rcam_frames++;   /* its update, which counts the frames, is held */
    rcam_restore(m, cam);
    sub_000A28B0_gen();
    uint32_t trace_call = jct_begin_call();
    jct_sample(3, 1, cam, s_rcam_frames, trace_call);
    rcam_apply(m, cam);
    jct_sample(3, 2, cam, s_rcam_frames, trace_call);
}
/* The publish walk (sub_00011D00, from the scene tick at 0x12547 and from
 * itself): while the world is frozen it is left out, and only the camera's
 * publish is made, as the walk makes it -- this pointer, a return address. */
extern void sub_00011D00_gen(void);
void sub_001BA8A0(void);
void sub_00011D00(void)
{
    uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    uint32_t cam = s_frozen_cam;
    if (!s_world_frozen || *(const uint32_t *)(m + g_esp) != 0x0001254Cu) { sub_00011D00_gen(); return; }
    g_esp += 4;                                 /* the walk's own ret */
    if (cam < 0x10000u || cam + 0x240u >= (64u << 20) || *(const uint32_t *)(m + cam) != 0x001CD518u) return;
    g_esp -= 4; *(uint32_t *)(m + g_esp) = 0x00011D63u;
    sub_001BA8A0();                             /* the walk's matrix to identity first */
    g_ecx = cam;
    g_esp -= 4; *(uint32_t *)(m + g_esp) = 0x00011D6Au;
    sub_000A28B0();                             /* each pops its return address, as a ret */
}
extern void sub_000A2960_gen(void);
void sub_000A2960(void) { jsrf_camcnt[1]++; sub_000A2960_gen(); }
extern void sub_000A3A70_gen(void);
void sub_000A3A70(void) { jsrf_camcnt[2]++; sub_000A3A70_gen(); }
extern void sub_000A3BD0_gen(void);
void sub_000A3BD0(void) { jsrf_camcnt[3]++; sub_000A3BD0_gen(); }
extern void sub_000A3D10_gen(void);
void sub_000A3D10(void) { jsrf_camcnt[4]++; sub_000A3D10_gen(); }
extern void sub_000A59B0_gen(void);
void sub_000A59B0(void) { jsrf_camcnt[5]++; sub_000A59B0_gen(); }
extern void sub_000A58B0_gen(void);
void sub_000A58B0(void) { jsrf_camcnt[6]++; sub_000A58B0_gen(); }
extern void sub_000A58C0_gen(void);
void sub_000A58C0(void) { jsrf_camcnt[7]++; sub_000A58C0_gen(); }
extern void sub_000A58E0_gen(void);
void sub_000A58E0(void) { jsrf_camcnt[8]++; sub_000A58E0_gen(); }
extern void sub_000A3E50_gen(void);
void sub_000A3E50(void) { jsrf_camcnt[9]++; sub_000A3E50_gen(); }
extern void sub_000A4100_gen(void);
void sub_000A4100(void) { jsrf_camcnt[10]++; sub_000A4100_gen(); }
extern void sub_000A5A00_gen(void);
void sub_000A5A00(void) { jsrf_camcnt[11]++; sub_000A5A00_gen(); }
extern void sub_000A5A70_gen(void);
void sub_000A5A70(void) { jsrf_camcnt[12]++; sub_000A5A70_gen(); }
extern void sub_000A5AC0_gen(void);
void sub_000A5AC0(void) { jsrf_camcnt[13]++; sub_000A5AC0_gen(); }
extern void sub_000A5AF0_gen(void);
void sub_000A5AF0(void) { jsrf_camcnt[14]++; sub_000A5AF0_gen(); }
extern void sub_000A4890_gen(void);
void sub_000A4890(void) { jsrf_camcnt[15]++; sub_000A4890_gen(); }
extern void sub_000A4D80_gen(void);
void sub_000A4D80(void) { jsrf_camcnt[16]++; sub_000A4D80_gen(); }
extern void sub_000A4C30_gen(void);
void sub_000A4C30(void) { jsrf_camcnt[17]++; sub_000A4C30_gen(); }
static void jsrf_cam_counts(void)
{
    static int on = -1; static double next;
    int i; char line[1024]; int len = 0;
    if (on < 0) on = getenv("JSRF_CAM_LOG") != NULL;
    if (!on || jsrf_now() < next) return;
    next = jsrf_now() + 2.0;
    len += snprintf(line + len, sizeof line - len, "[CAMCNT] t=%.1f", jsrf_now());
    for (i = 0; i < (int)(sizeof jsrf_camcnt / sizeof jsrf_camcnt[0]); i++)
        if (jsrf_camcnt[i])
            len += snprintf(line + len, sizeof line - len, " %05X:%lu", jsrf_camva[i], jsrf_camcnt[i]);
    fprintf(stderr, "%s\n", line); fflush(stderr);
}

/* ---- The brake sparks: sub_0014C410 with the console's rsqrtss -----------
 *
 * Braking (the stick forward, then back) throws red sparks and smoke from the
 * skates. Here, as in xemu, they came out only now and then, and stayed at a
 * fixed spot instead of following the skates; on the console and in
 * Cxbx-Reloaded they come every time. Miggas found why, in JSRF-Recomp
 * (https://github.com/Miggas/JSRF-Recomp, its v125-v127): the console's
 * Pentium III answers rsqrtss with a 12-bit approximation, and the lifter --
 * like xemu -- computes 1/sqrt(x) exactly. One site needs the console's value:
 * the rsqrtss in sub_0014C410, which normalises the vector at edx into ecx
 * (x, y, z each times 1/sqrt(x^2 + y^2 + z^2)). Approximating the other sites
 * as well broke geometry and shading in his tests, so only this one is.
 *
 * The body below is the generated one, statement for statement, but for that
 * instruction, which gives the Pentium III's result bit for bit on any host
 * (sse_p3.h, his table). RECOMP_SSE_P3=0: the exact value, as before.
 */
#include "sse_p3.h"
extern void sub_0014C410_gen(void);
static int sse_p3_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_SSE_P3");
        on = !(e && *e == '0');
        fprintf(stderr, "[SSE] the brake sparks' vector normalise (sub_0014C410): %s\n",
                on ? "the console's rsqrtss" : "exact 1/sqrt (RECOMP_SSE_P3=0)");
    }
    return on;
}
static float rsqrtss_p3(float x)
{
    uint32_t u;
    memcpy(&u, &x, 4);
    u = p3_rsq_bits(u);
    memcpy(&x, &u, 4);
    return x;
}
void sub_0014C410(void)
{
    if (!sse_p3_on()) { sub_0014C410_gen(); return; }
    g_xmm0 = XMM_SCALAR(MEMF(g_edx));                                    /* movss */
    g_xmm1 = XMM_SCALAR(MEMF(g_edx + 4));                                /* movss */
    g_xmm2 = XMM_SCALAR(MEMF(g_edx + 8));                                /* movss */
    g_xmm4.f[0] = g_xmm0.f[0];                                           /* movss */
    g_xmm0.f[0] = g_xmm0.f[0] * g_xmm0.f[0];                             /* mulss */
    g_xmm5.f[0] = g_xmm1.f[0];                                           /* movss */
    g_xmm1.f[0] = g_xmm1.f[0] * g_xmm1.f[0];                             /* mulss */
    g_xmm6.f[0] = g_xmm2.f[0];                                           /* movss */
    g_xmm2.f[0] = g_xmm2.f[0] * g_xmm2.f[0];                             /* mulss */
    g_xmm0.f[0] = g_xmm0.f[0] + g_xmm1.f[0];                             /* addss */
    g_xmm0.f[0] = g_xmm0.f[0] + g_xmm2.f[0];                             /* addss */
    g_xmm0.f[0] = rsqrtss_p3(g_xmm0.f[0]);                               /* rsqrtss */
    g_xmm4.f[0] = g_xmm4.f[0] * g_xmm0.f[0];                             /* mulss */
    g_xmm5.f[0] = g_xmm5.f[0] * g_xmm0.f[0];                             /* mulss */
    g_xmm6.f[0] = g_xmm6.f[0] * g_xmm0.f[0];                             /* mulss */
    MEMWF(g_ecx, g_xmm4.f[0]);                                           /* movss */
    MEMWF(g_ecx + 4, g_xmm5.f[0]);                                       /* movss */
    MEMWF(g_ecx + 8, g_xmm6.f[0]);                                       /* movss */
    g_esp += 4;                                                          /* ret */
}

/* ---- Draw distance: more of the map's areas drawn ---------------------------
 *
 * Buildings appear as you get near them -- on the console too: it is the
 * game's own draw distance. A map is split into areas (up to 256). Every
 * frame sub_0006A970 sets, in the bitset at 0x20C6C8 (8 words of the object
 * at 0x20C6C0), the areas whose volume holds the camera plus those each of
 * them lists as visible; the map's draw passes skip every area whose bit is
 * clear. Found by Miggas in JSRF-Recomp (its v198-v201, MIT): the map is at
 * +4 of that object (its areas at +4, their count at +8), an area record is
 * 0x154 bytes with its visible list at +0x98 and the count at +0x9C; and his
 * "hops" -- the game's own set, widened twice through those lists -- reaches
 * far ahead without crossing to the far end of a looping map, where drawing
 * every area shows a copy of a street in the sky (Dogenzaka Hill).
 *
 * Here: for the length of three of the draw passes (sub_0006B6A0,
 * sub_0006B8F0, sub_0006C360; sub_0006BA80, the backdrop's, is left to the
 * game, as in his default) the bitset holds the widened set, and the game's
 * own is put back after, so objects, collision and triggers keep the game's
 * areas -- only the drawing changes. Areas no list names (the garage's
 * invisible walls) are never added. Only on the maps listed, as the cost is
 * a draw call per piece added: by default Skyscraper District, where the
 * towers and satellite dishes on the way to 99th Street popped in worst.
 *
 * A map is known by an ID worked out from its area layout (his: FNV-1a over
 * the area count and every area's first 18 words), logged when it loads:
 * "[AREAS] map ID xxxx".
 *   RECOMP_DRAW_AREAS=game (or 0)    the game's areas everywhere, as on the Xbox
 *   RECOMP_DRAW_AREAS=hops<N>         N steps (default 2)
 *   RECOMP_DRAW_AREAS_MAPS=ID,ID|all  the maps it applies to
 *   JSRF_AREAS_LOG=1                  every 2 s: the game's count and the drawn count
 */
extern void sub_0006B6A0_gen(void);
extern void sub_0006B8F0_gen(void);
extern void sub_0006C360_gen(void);
#define AREA_OBJ 0x20C6C0u
#define AREA_REC 0x154u
#define AREA_MAPS_DEFAULT "07FC"        /* Skyscraper District (Stg34): 30 areas */
static int areas_guest_ok(uint32_t a, uint32_t len)
{
    return a >= 0x10000u && a + len >= a && a + len < (64u << 20);
}
static int areas_hops(void)
{
    static int hops = -1;
    if (hops < 0) {
        const char *e = getenv("RECOMP_DRAW_AREAS");
        hops = 2;
        if (e && *e) {
            if (!strncmp(e, "hops", 4)) hops = e[4] ? atoi(e + 4) : 2;
            else hops = 0;                         /* game, 0, off */
        }
        if (hops > 16) hops = 16;
        if (hops < 0) hops = 0;
        fprintf(stderr, "[AREAS] draw distance: %s\n", hops ? "more areas on the maps listed" :
                "the game's own areas (RECOMP_DRAW_AREAS)");
    }
    return hops;
}
static int areas_log_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("JSRF_AREAS_LOG") ? 1 : 0;
    return on;
}
static int areas_map_listed(uint32_t id)
{
    const char *e = getenv("RECOMP_DRAW_AREAS_MAPS");
    if (!e) e = AREA_MAPS_DEFAULT;
    if (!strcmp(e, "all")) return 1;
    while (*e) {
        char *end;
        unsigned long v = strtoul(e, &end, 16);
        if (end == e) { e++; continue; }
        if (v == id) return 1;
        e = end;
    }
    return 0;
}
/* The current map: its areas, their count, its ID, the areas some list names,
 * and whether it is one of the maps listed. Worked out again when the map
 * object, its area table, the count or the first area's first word change. */
static struct {
    uint32_t map, areas, n, a0, id;
    uint32_t drawable[8];
    int listed;
} s_amap;
static int areas_map(uint32_t *areas_out, uint32_t *n_out)
{
    uint32_t map = MEM32(AREA_OBJ + 4), areas, n, i, j, q;
    if (!areas_guest_ok(map, 12)) return 0;
    areas = MEM32(map + 4);
    n = MEM32(map + 8);
    if (!n || n > 256 || !areas_guest_ok(areas, n * AREA_REC)) return 0;
    if (map != s_amap.map || areas != s_amap.areas || n != s_amap.n || MEM32(areas) != s_amap.a0) {
        uint32_t h = 2166136261u, cnt = 0;
        s_amap.map = map; s_amap.areas = areas; s_amap.n = n; s_amap.a0 = MEM32(areas);
        memset(s_amap.drawable, 0, sizeof s_amap.drawable);
        for (i = 0; i < n; i++) {
            uint32_t a = areas + i * AREA_REC, list = MEM32(a + 0x98), c = MEM32(a + 0x9C);
            if (!c || c > 256 || !areas_guest_ok(list, c * 4u)) continue;
            for (j = 0; j < c; j++) {
                uint32_t v = MEM32(list + 4 * j);
                if (v < 256) s_amap.drawable[v >> 5] |= 1u << (v & 31);
            }
        }
        h = (h ^ n) * 16777619u;
        for (i = 0; i < n; i++)
            for (q = 0; q < 18; q++)
                h = (h ^ MEM32(areas + i * AREA_REC + 4 * q)) * 16777619u;
        s_amap.id = ((h >> 16) ^ h) & 0xFFFFu;
        s_amap.listed = areas_map_listed(s_amap.id);
        for (i = 0; i < 8; i++) cnt += (uint32_t)__builtin_popcount(s_amap.drawable[i]);
        fprintf(stderr, "[AREAS] map ID %04X: %u areas, %u drawn by some view; draw distance %s\n",
                s_amap.id, n, cnt, s_amap.listed && areas_hops() ? "widened" : "the game's");
    }
    *areas_out = areas; *n_out = n;
    return 1;
}
/* The widened set for this frame (worked out at the first pass of a frame). */
static uint32_t s_area_wide[8];
static uint32_t s_area_wide_flip = ~0u;
static int areas_prepare(void)
{
    uint32_t areas, n, cur[8], nxt[8], i, j, k, flip;
    int hops = areas_hops(), step;
    if (!hops || !areas_map(&areas, &n) || !s_amap.listed) return 0;
    flip = nv2a_gl_flips();
    if (flip == s_area_wide_flip) return 1;
    s_area_wide_flip = flip;
    for (k = 0; k < 8; k++) cur[k] = MEM32(AREA_OBJ + 8 + 4 * k);
    for (step = 0; step < hops; step++) {
        memcpy(nxt, cur, sizeof nxt);
        for (i = 0; i < n; i++) {
            uint32_t a = areas + i * AREA_REC, list, c;
            if (!(cur[i >> 5] & (1u << (i & 31)))) continue;
            list = MEM32(a + 0x98); c = MEM32(a + 0x9C);
            if (!c || c > 256 || !areas_guest_ok(list, c * 4u)) continue;
            for (j = 0; j < c; j++) {
                uint32_t v = MEM32(list + 4 * j);
                if (v < 256) nxt[v >> 5] |= 1u << (v & 31);
            }
        }
        if (!memcmp(nxt, cur, sizeof cur)) break;
        memcpy(cur, nxt, sizeof cur);
    }
    for (k = 0; k < 8; k++)
        s_area_wide[k] = (cur[k] & s_amap.drawable[k]) | MEM32(AREA_OBJ + 8 + 4 * k);
    if (areas_log_on()) {
        static double last;
        double now = pad_now();
        if (now - last >= 2.0) {
            unsigned g = 0, w = 0;
            last = now;
            for (k = 0; k < 8; k++) {
                g += (unsigned)__builtin_popcount(MEM32(AREA_OBJ + 8 + 4 * k));
                w += (unsigned)__builtin_popcount(s_area_wide[k]);
            }
            fprintf(stderr, "[AREAS] t=%.1f map %04X: the game's %u areas, drawn %u (hops%d)\n",
                    now, s_amap.id, g, w, hops);
        }
    }
    return 1;
}
static void area_pass(void (*gen)(void))
{
    uint32_t keep[8], k;
    if (!areas_prepare()) { gen(); return; }
    for (k = 0; k < 8; k++) {
        keep[k] = MEM32(AREA_OBJ + 8 + 4 * k);
        MEM32(AREA_OBJ + 8 + 4 * k) = s_area_wide[k];
    }
    gen();
    for (k = 0; k < 8; k++) MEM32(AREA_OBJ + 8 + 4 * k) = keep[k];
}
void sub_0006B6A0(void) { area_pass(sub_0006B6A0_gen); }
void sub_0006B8F0(void) { area_pass(sub_0006B8F0_gen); }
void sub_0006C360(void) { area_pass(sub_0006C360_gen); }

/* Read-only diagnostics plus opt-in experiment; wrappers remain visible to
 * manual_scan.py and CMake. Flag off always invokes the unmodified bodies. */
#define JSRF_CAMERA_HOST_TRACE 1
#include "jsrf_camera_trace.inc"
#include "jsrf_camera_native_build.inc"
extern void sub_0008B200_gen(void);
void sub_0008B200(void)
{
    uint32_t trace_self = g_ecx;
    uint32_t trace_call = jct_begin_call();
    jct_sample(1, 0, trace_self, s_rcam_frames, trace_call);
    sub_0008B200_gen(); /* LT/LB and target lock retain native semantics. */
    jct_sample(1, 1, trace_self, s_rcam_frames, trace_call);
}
extern void sub_000A76E0_gen(void);
void sub_000A76E0(void)
{
    uint32_t trace_self = g_ecx;
    uint32_t trace_call = jct_begin_call();
    jct_sample(2, 0, trace_self, s_rcam_frames, trace_call);
    jcx_prepare_input(trace_self);
    if (jcx_enabled()) jcx_native_input(); else sub_000A76E0_gen();
    jct_sample(2, 1, trace_self, s_rcam_frames, trace_call);
}
