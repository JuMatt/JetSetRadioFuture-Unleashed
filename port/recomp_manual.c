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

/* The Win32 shim's thread id, so traces here and the kernel's [WAITLOG]
 * name the same thread. */
extern uint32_t GetCurrentThreadId(void);

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
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

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
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

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
    fprintf(stderr, "[ADXTRAP] sub_0006F730: ADXT error code %d (eax=%08X)\n", (int)(int16_t)g_eax, g_eax);
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
    static int n;
    if (h && *(uint32_t *)(m + h + 0x14) && n++ < 60)
        fprintf(stderr, "[ADXT] trapchk h=%08X state=%d thresh[0x38]=%d err[0x60]=%d cnt68=%d cnt6a=%d mode6d=%d stm=%08X\n",
                h, (int8_t)m[h + 1], *(int32_t *)(m + h + 0x38), *(int16_t *)(m + h + 0x60),
                *(int16_t *)(m + h + 0x68), *(int16_t *)(m + h + 0x6a), m[h + 0x6d], *(uint32_t *)(m + h + 0x14));
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
    sub_0013D300_gen();
}

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
void sub_00011070(void)
{
    static int check = -1;
    if (check < 0) check = getenv("JSRF_TREE_CHECK") ? 1 : 0;
    if (check && g_ecx) jsrf_check_tree(g_ecx, 0, 0, "root");
    sub_00011070_gen();
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
     || getenv("JSRF_BONE_CHECK"))
        recomp_trace_enable_hooks();
}

void recomp_trace_user_hook(const char *name, uint32_t va)
{
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
void xbox_file_open_hook(const char *path)
{
    extern void jsrf_protect_data_arm(void);
    const char *t = getenv("JSRF_PROTECT_AT");
    if (t && strstr(path, t)) jsrf_protect_data_arm();
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

static struct { double t; uint16_t buttons; uint8_t analog[8];
                char name[16]; int said; } s_pad_script[64];
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
        s_pad_script[s_pad_script_n].buttons = 0;
        s_pad_script[s_pad_script_n].said = 0;
        memcpy(s_pad_script[s_pad_script_n].name, name, sizeof name);
        memset(s_pad_script[s_pad_script_n].analog, 0, 8);
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
/* Host pad state for a port: real pad if attached, else the script. */
static int pad_read(uint32_t port, XBOX_INPUT_STATE *st)
{
    static int inited; static uint32_t packet;
    if (!inited) { inited = 1; xbox_InputInit(); pad_script_load(); }
    { static int polls; if (++polls == 1 || polls == 600)
        fprintf(stderr, "[PAD] title polled the pad (%d times, t=%.1fs)\n",
                polls, pad_now()); }
    memset(st, 0, sizeof *st);
    if (port == 0 && s_pad_script_n > 0) {
        double now = pad_now();
        for (int i = 0; i < s_pad_script_n; i++)
            if (now >= s_pad_script[i].t && now < s_pad_script[i].t + 0.25) {
                st->Gamepad.wButtons |= s_pad_script[i].buttons;
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
void sub_001C3E14(void)
{
    uint32_t h = GARG(1), st = GARG(2); uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    XBOX_INPUT_STATE s;
    static uint16_t last_btn; static uint8_t last_a;
    if ((h & 0xFFFF0000u) != XPP_HANDLE_BASE || !st) { g_eax = 0x57; g_esp += 12; return; }
    if (!pad_read(h & 3, &s)) { g_eax = 0x48F; g_esp += 12; return; }   /* ERROR_DEVICE_NOT_CONNECTED */
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
void sub_001C3E85(void)
{
    uint32_t fb = GARG(2); uint8_t *m = (uint8_t *)g_xbox_mem_offset;
    if (fb) {
        uint32_t ev = *(uint32_t *)(m + fb + 4);
        *(uint32_t *)(m + fb) = 0;                    /* Header.dwStatus = ERROR_SUCCESS */
        XBOX_VIBRATION v = { *(uint16_t *)(m + fb + 0x40), *(uint16_t *)(m + fb + 0x42) };
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
