#define _GNU_SOURCE 1
/**
 * Jet Set Radio Future - Recompiled Game Entry Point
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * Customize this file for your game:
 *   - Set YOUR_GAME_ENTRY_POINT to the XBE entry point address
 *   - Set YOUR_GAME_XBE_PATH to where the XBE file lives
 *   - Set YOUR_GAME_DIR to the game data directory
 *   - Add any CRT global pre-initialization your game needs
 *   - Customize the VEH handler for game-specific crash diagnosis
 *
 * XBE Details (fill in from xbe_parser output):
 *   Title:       Jet Set Radio Future
 *   Title ID:    0x00000000
 *   Base addr:   0x00010000
 *   Entry point: 0x00000000
 *   Code size:   ~??? KB (.text)
 *   Sections:    ?? (list them)
 *   Kernel imports: ??
 */

#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <execinfo.h>
/* <ucontext.h> on Darwin is the deprecated getcontext/makecontext header and
 * refuses to compile without _XOPEN_SOURCE, which would in turn hide the BSD
 * declarations the rest of this file needs. All that is wanted from it is the
 * ucontext_t type a signal handler is handed, and <signal.h> already declares
 * that on both platforms. */
#ifndef __APPLE__
#include <ucontext.h>
#endif
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
#include "win32_compat.h"
#include "recomp_icall_feedback.h"
#include "apu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>
#include "ohci.h"

/* generated dispatch table (src/recomp/gen/recomp_types.h) */
int recomp_dispatch_init(void);

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* RECOMP_TLS is not optional here. The runtime defines these thread-local, and
 * a plain `extern` referencing a __declspec(thread) variable does not resolve
 * to the calling thread's copy -- it resolves to the image's TLS template. The
 * host side then writes g_esp somewhere the generated code never reads, so the
 * guest starts with every register at zero and faults immediately, having
 * apparently ignored the setup that visibly ran. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* x87 and SSE state. Global for the same reason the volatile GPRs are: one
 * guest routine can lift to several C functions, so a value written in one
 * body is read in the next. Defined in xbox_memory_layout.c like the rest of
 * the register file. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

/*
 * TODO: Set these from your xbe_parser output.
 * Run: py -3 -m tools.xbe_parser game/default.xbe
 */
#define YOUR_GAME_ENTRY_POINT   0x00148023  /* XBE entry point VA */
#define YOUR_GAME_DIR_DEFAULT    "game"

/* ── Where the game's files are ─────────────────────────────────────────────
 *
 * The recompiled code is in this binary, but the title's data is not: the XBE
 * is still opened for its sections, and every asset the game loads goes
 * through the path layer to this directory. It used to be the literal string
 * "game", resolved against the working directory, which means the binary only
 * runs from one place and a launcher cannot point it at a library elsewhere.
 *
 * JSRF_GAME_DIR wins, then the first command-line argument, then "game", so
 * running it by hand from the build tree behaves exactly as before.
 */
static const char *g_game_dir_arg;

static const char *game_dir(void)
{
    const char *e = getenv("JSRF_GAME_DIR");
    if (e && e[0]) return e;
    if (g_game_dir_arg && g_game_dir_arg[0]) return g_game_dir_arg;
    return YOUR_GAME_DIR_DEFAULT;
}

static const char *game_xbe_path(void)
{
    static char buf[1024];
    snprintf(buf, sizeof buf, "%s/default.xbe", game_dir());
    return buf;
}

/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);
static void *guest_thread_main(void *arg);
static volatile int g_guest_returned;

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
static void print_guest_context(void)
{
    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
    }
}

/* APU register window handlers: offset within the 512 KB APU aperture. */
extern MCPXAPUState *g_apu_state;
static uint32_t jsrf_apu_mmio_read(uint32_t addr, unsigned size)
{
    uint32_t off = addr - 0xFE800000u;
    if (off < 0x30000u && g_apu_state)
        return (uint32_t)mcpx_apu_mmio_read(g_apu_state, off, size);
    /* The USB host controllers, at the top of the same aperture. */
    if (addr >= 0xFED00000u && addr < 0xFED09000u && xbox_OhciEnabled())
        return xbox_OhciRead(addr, (int)size);
    switch (size) {
    case 1:  return *(volatile uint8_t *)((uintptr_t)addr + g_xbox_mem_offset);
    case 2:  return *(volatile uint16_t *)((uintptr_t)addr + g_xbox_mem_offset);
    default: return *(volatile uint32_t *)((uintptr_t)addr + g_xbox_mem_offset);
    }
}
static void jsrf_apu_mmio_write(uint32_t addr, uint32_t val, unsigned size)
{
    uint32_t off = addr - 0xFE800000u;
    if (off < 0x30000u && g_apu_state) {
        mcpx_apu_mmio_write(g_apu_state, off, val, size);
        return;
    }
    if (addr >= 0xFED00000u && addr < 0xFED09000u && xbox_OhciEnabled()) {
        xbox_OhciWrite(addr, val, (int)size);
        return;
    }
    /* AC'97 bus master (0xFEC00100..): the RR (reset registers) bit of a
     * channel control register self-clears on hardware, and DirectSound
     * spins on a register copy it read right after writing it. */
    if (addr >= 0xFEC00100u && addr < 0xFEC00200u && size == 1 && (val & 2))
        *(volatile uint8_t *)((uintptr_t)addr + g_xbox_mem_offset) = (uint8_t)(val & ~2u);
}

static void term_handler(int sig)
{
    (void)sig;
    RECOMP_ICALL_FEEDBACK_DUMP();
    fflush(stderr);
    _exit(0);
}

/* JSRF_PROTECT_DATA=1: write-protect the XBE's .data while the title music
 * starts, so the first write into it is caught with a backtrace. Armed by
 * jsrf_protect_data_arm() (called from a file-open hook); each fault logs,
 * unprotects that page and resumes. */
#include <sys/mman.h>
#include <dlfcn.h>
static int g_protect_data_armed;
#define JSRF_DATA_LO 0x001EC000u
#define JSRF_DATA_HI 0x0027E000u
/* Pages to trap, through every alias of RAM (primary, the 28 mirrors at
 * 64 MB steps, the tiled aperture at 0xF0000000). */
static const uint32_t k_trap_pages[] = { 0x0025E000u, 0x0022D000u };
static int jsrf_trap_page_of(uint32_t va, uint32_t *alias_base)
{
    uint32_t off = va % 0x04000000u;
    for (size_t i = 0; i < sizeof k_trap_pages / sizeof k_trap_pages[0]; i++)
        if (off >= k_trap_pages[i] && off < k_trap_pages[i] + 0x1000) { *alias_base = va - off; return 1; }
    if (va >= 0xF0000000u) {
        off = va - 0xF0000000u;
        for (size_t i = 0; i < sizeof k_trap_pages / sizeof k_trap_pages[0]; i++)
            if (off >= k_trap_pages[i] && off < k_trap_pages[i] + 0x1000) { *alias_base = 0xF0000000u; return 1; }
    }
    return 0;
}
void jsrf_protect_data_arm(void)
{
    if (g_protect_data_armed || !getenv("JSRF_PROTECT_DATA")) return;
    g_protect_data_armed = 1;
    for (size_t i = 0; i < sizeof k_trap_pages / sizeof k_trap_pages[0]; i++) {
        for (uint32_t k = 0; k < 29; k++) {
            uint32_t va = k * 0x04000000u + k_trap_pages[i];
            mprotect((void *)((uintptr_t)va + g_xbox_mem_offset), 0x1000, PROT_READ);
        }
        mprotect((void *)((uintptr_t)0xF0000000u + k_trap_pages[i] + g_xbox_mem_offset), 0x1000, PROT_READ);
    }
    fprintf(stderr, "[PROTECT] trap pages armed through all aliases\n");
    fflush(stderr);
}


/*
 * The faulting instruction pointer and stack pointer, whatever the host is.
 *
 * Every platform spells this differently and none of the spellings compile
 * anywhere else: Linux keeps a gregs[] array indexed by x86 register names,
 * Darwin hangs a per-architecture struct off a pointer. Getting it wrong is
 * a compile error rather than a wrong answer, which is the one mercy here.
 */
static void host_fault_regs(void *ctx, uintptr_t *pc, uintptr_t *sp)
{
    *pc = 0; *sp = 0;
    if (!ctx) return;
#if defined(__APPLE__)
    {
        ucontext_t *uc = (ucontext_t *)ctx;
#  if defined(__aarch64__) || defined(__arm64__)
        *pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
        *sp = (uintptr_t)uc->uc_mcontext->__ss.__sp;
#  else
        *pc = (uintptr_t)uc->uc_mcontext->__ss.__rip;
        *sp = (uintptr_t)uc->uc_mcontext->__ss.__rsp;
#  endif
    }
#elif defined(__linux__) && (defined(__x86_64__) || defined(__i386__))
    {
        ucontext_t *uc = (ucontext_t *)ctx;
        *pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
        *sp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
    }
#elif defined(__linux__) && defined(__aarch64__)
    {
        ucontext_t *uc = (ucontext_t *)ctx;
        *pc = (uintptr_t)uc->uc_mcontext.pc;
        *sp = (uintptr_t)uc->uc_mcontext.sp;
    }
#else
    (void)ctx;
#endif
}

static void segv_handler(int sig, siginfo_t *si, void *ctx)
{
    uintptr_t fault_addr = (uintptr_t)si->si_addr;
    if (g_protect_data_armed && ctx) {
        uint32_t va = (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);
        uint32_t alias_base;
        if (jsrf_trap_page_of(va, &alias_base)) {
            static int shown;
            uintptr_t rip, rsp_unused;
            Dl_info di;
            host_fault_regs(ctx, &rip, &rsp_unused);
            if (shown++ < 40) {
                fprintf(stderr, "[PROTECT] write via VA 0x%08X (alias base 0x%08X) from native %s+0x%lx (guest esp=%08X eax=%08X ecx=%08X edx=%08X esi=%08X edi=%08X)\n",
                        va, alias_base, (dladdr((void *)rip, &di) && di.dli_sname) ? di.dli_sname : "?",
                        (di.dli_saddr ? (unsigned long)(rip - (uintptr_t)di.dli_saddr) : 0ul), g_esp, g_eax, g_ecx, g_edx, g_esi, g_edi);
                print_guest_context();
                fflush(stderr);
            }
            /* Whole host pages: Apple Silicon's are 16 KB, and rounding to
             * 4 KB here would leave the faulting address still unwritable
             * and the handler in a loop on it. */
            { size_t ps = (size_t)sysconf(_SC_PAGESIZE);
              uintptr_t pg = (uintptr_t)fault_addr & ~(uintptr_t)(ps - 1);
              mprotect((void *)pg, ps, PROT_READ | PROT_WRITE); }
            return;
        }
    }
    /* si_code says what kind of bad access this was, and the answer differs
     * by platform in ways that change what to look for. Darwin reports a
     * protection failure as SIGBUS where Linux reports SIGSEGV, so "signal
     * 10" alone does not distinguish "wrote to a read-only page" from
     * "misaligned" from "past the end of a mapping" -- and those want three
     * different fixes. */
    {
        const char *what = "?";
        if (sig == SIGSEGV)
            what = si->si_code == SEGV_MAPERR ? "no mapping at that address"
                 : si->si_code == SEGV_ACCERR ? "mapped, but not with that access"
                 : "SIGSEGV";
        else if (sig == SIGBUS)
            what = si->si_code == BUS_ADRALN ? "misaligned access"
                 : si->si_code == BUS_ADRERR ? "no physical object at that address"
                 : si->si_code == BUS_OBJERR ? "object-specific hardware error"
                 : "SIGBUS (on Darwin, usually a protection failure)";
        fprintf(stderr, "[CRASH] signal %d (si_code %d: %s), fault addr=0x%llX\n",
                sig, si->si_code, what, (unsigned long long)fault_addr);
        /* The faulting instruction itself.
         *
         * "Misaligned access" on arm64 is not the whole answer, because plain
         * unaligned loads and stores work there -- only some instructions
         * insist on alignment, and which one this is decides whether the fix
         * belongs in the memory accessors, the atomics, or the code that
         * emits them. One word, printed, ends the guessing. */
        {
            uintptr_t pc = 0, sp = 0;
            host_fault_regs(ctx, &pc, &sp);
#if defined(__aarch64__) || defined(__arm64__)
            if (pc) {
                uint32_t insn;
                memcpy(&insn, (const void *)pc, 4);
                Dl_info di;
                if (dladdr((void *)pc, &di) && di.dli_sname)
                    fprintf(stderr, "  faulting instruction at %p: %08X"
                            "  in %s+0x%lx\n", (void *)pc, insn, di.dli_sname,
                            di.dli_saddr ? (unsigned long)(pc - (uintptr_t)di.dli_saddr) : 0ul);
                else
                    fprintf(stderr, "  faulting instruction at %p: %08X\n",
                            (void *)pc, insn);
            }
#else
            if (pc) {
                const uint8_t *p = (const uint8_t *)pc;
                fprintf(stderr, "  faulting instruction at %p: "
                        "%02X %02X %02X %02X %02X %02X %02X %02X\n", (void *)pc,
                        p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
            }
#endif
        }
    }
    { extern void xbox_kernel_thread_report(void);
      xbox_kernel_thread_report(); }

    /* Where the bad pointer was stored.
     *
     * A fault on an address the guest computed says what the value was and
     * nothing about where it came from, and the same wrong value has now
     * turned up in the same place across many runs. Guest RAM is 64 MB --
     * small enough to search outright -- so scanning it for that exact dword
     * names the structures holding it. That is the difference between "a
     * pointer is wrong" and "this field of this object is wrong", and a
     * watchpoint on the address printed here names whatever wrote it.
     *
     * Searching for the fault address alone was a mistake that cost a whole
     * crash: a load of [esi+4] faults at esi+4, but what the guest stored --
     * and what is therefore findable in RAM -- is esi. The register values go
     * into the search with it, and the object around each hit is printed,
     * because "this dword is at 0x1C4A30" is a lead and "this dword is the
     * sibling field of the node at 0x1C4A00, whose vtable is zero" is an
     * answer. */
    {
        uint32_t bad = (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);
        uint32_t cand[8]; int ncand = 0, ci;
        uint32_t regs[7];
        regs[0] = bad;   regs[1] = g_eax; regs[2] = g_ecx; regs[3] = g_edx;
        regs[4] = g_ebx; regs[5] = g_esi; regs[6] = g_edi;
        for (ci = 0; ci < 7; ci++) {
            int j, dup = 0;
            /* Only values that cannot be guest addresses are interesting: a
             * live pointer into RAM turns up in a thousand places and says
             * nothing about which of them is the one that went wrong. */
            if (regs[ci] < (64u << 20))
                continue;
            for (j = 0; j < ncand; j++) if (cand[j] == regs[ci]) dup = 1;
            if (!dup) cand[ncand++] = regs[ci];
        }
        if (ncand && g_xbox_mem_offset) {
            const uint32_t *ram = (const uint32_t *)(uintptr_t)g_xbox_mem_offset;
            size_t words = (64u << 20) / 4, i;
            int found[8]; int total = 0;
            for (ci = 0; ci < ncand; ci++) found[ci] = 0;
            for (i = 0x1000; i < words && total < 24; i++) {
                uint32_t w = ram[i];
                for (ci = 0; ci < ncand; ci++) {
                    if (w != cand[ci] || found[ci] >= 4) continue;
                    found[ci]++; total++;
                    fprintf(stderr, "  %08X is stored at guest 0x%08X\n",
                            cand[ci], (unsigned)(i * 4));
                    if (found[ci] <= 2) {
                        /* The object around it. A scene-graph node is walked
                         * through +0x28 (child) and +0x30 (sibling), so the
                         * dump starts far enough back to show a node header
                         * whichever field the hit turned out to be. */
                        long base = (long)(i * 4) - 0x30;
                        int k;
                        if (base < 0) base = 0;
                        if ((size_t)(base >> 2) + 16 > words) base = 0;
                        for (k = 0; k < 16; k += 4) {
                            fprintf(stderr, "    +%04lX: %08X %08X %08X %08X\n",
                                    (unsigned long)(base + k * 4),
                                    ram[(base >> 2) + k + 0], ram[(base >> 2) + k + 1],
                                    ram[(base >> 2) + k + 2], ram[(base >> 2) + k + 3]);
                        }
                    }
                    break;
                }
            }
            for (ci = 0; ci < ncand; ci++)
                if (!found[ci])
                    fprintf(stderr, "  %08X is not stored anywhere in guest "
                            "RAM -- it was computed, not loaded\n", cand[ci]);
        }
    }
    fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n", g_eax, g_ecx, g_edx, g_esp);
    fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n", g_ebx, g_esi, g_edi);
    fprintf(stderr, "  Xbox VA of fault: 0x%08X\n", (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
    { extern uint32_t xbox_current_thread_stack_top(void);
      fprintf(stderr, "  thread stack top: 0x%08X (0 = main thread)\n", xbox_current_thread_stack_top()); }
    print_guest_context();
    if (ctx) {
        uintptr_t rip, rsp;
        Dl_info di;
        host_fault_regs(ctx, &rip, &rsp);
        fprintf(stderr, "  native RIP=%p", (void *)rip);
        if (dladdr((void *)rip, &di) && di.dli_sname) fprintf(stderr, " in %s+0x%lx", di.dli_sname, (unsigned long)(rip - (uintptr_t)di.dli_saddr));
        fprintf(stderr, "\n  native stack (symbols found in the first 2000 slots):\n");
        { int shown = 0; for (int i = 0; i < 2000 && shown < 20; i++) {
            uintptr_t v = ((uintptr_t *)rsp)[i];
            if (dladdr((void *)v, &di) && di.dli_sname && di.dli_fname && strstr(di.dli_fname, "jsrf_recomp")
                && strncmp(di.dli_sname, "sub_", 4) == 0) {
                fprintf(stderr, "    [rsp+%-5d] %s+0x%lx\n", i*8, di.dli_sname, (unsigned long)(v - (uintptr_t)di.dli_saddr)); shown++; }
        } }
    }
    {
        void *bt[32]; int n = backtrace(bt, 32);
        fprintf(stderr, "  native backtrace:\n");
        backtrace_symbols_fd(bt, n, 2);
    }
    { extern void recomp_icallv_dump(void); recomp_icallv_dump(); }
    RECOMP_ICALL_FEEDBACK_DUMP();
    fflush(stderr);
    _exit(139);
}

/* ── WinMain ───────────────────────────────────────────────── */

static int host_main(void)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;


    /* Unbuffered output for immediate visibility during debugging */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("=== Jet Set Radio Future - Static Recompilation ===\n");
    printf("Loading XBE...\n");

    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
    {
        struct sigaction sa; memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = segv_handler; sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL);
        signal(SIGTERM, term_handler); signal(SIGINT, term_handler);
    }

    /* Step 1: Load XBE */
    if (!load_xbe(game_xbe_path(), &xbe_data, &xbe_size)) {
        fprintf(stderr, "%s\n", "Failed to load default.xbe.\n"
                    "Place the game files in the 'game' subdirectory.");
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
    if (getenv("JSRF_RAM_MB")) {
        extern void xbox_SetTotalRam(size_t);
        extern void xbox_SetMapSize(size_t);
        size_t mb = (size_t)atoi(getenv("JSRF_RAM_MB"));
        xbox_SetTotalRam(mb << 20);
        xbox_SetMapSize(mb << 20);
    }
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        fprintf(stderr, "%s\n", "Failed to initialize Xbox memory layout.\n"
                    "The required virtual address range may be unavailable.");
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        xbox_path_init(game_dir(), "save");
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    { extern int g_icall_verbose; g_icall_verbose = getenv("JSRF_ICALLV") != NULL; }
    RECOMP_ICALL_FEEDBACK_INIT();

    /* .rdata is read-only on the console. Making it so here turns a stray
     * write into the kernel thunk table (or a vtable) into an immediate,
     * attributable fault instead of a mystery call to the wrong kernel
     * function three seconds later. Whole pages only: 0x1C4000-0x1EB000. */
    if (getenv("JSRF_RO_RDATA")) {
        DWORD old;
        VirtualProtect((void *)((uint8_t *)g_xbox_mem_offset + 0x1C4000u), 0x27000u, PAGE_READONLY, &old);
        fprintf(stderr, "  .rdata pages 0x001C4000-0x001EB000 made read-only\n");
    }

    /* JSRF's D3D8 (XDK 4134): the device struct pointer lives at 0x0019DCE0;
     * MakeSpace spins until *(dev+0x34) -- the GPU's GET write-back block, a
     * 0x60-byte contiguous allocation -- catches up with the CPU put pointer
     * at dev+0x30. Mirror PUT into it, which is what a GPU that has consumed
     * everything looks like. */
    xbox_Nv2aMirrorFence(0x0019DCE0u, 0x30u, 0x34u);

    /* Audio processor: the frame thread is what answers the DSP doorbell
     * (RECOMP_APU_DSP_ACK), which DirectSound spins on after downloading
     * its effects image. Physical address 0 is guest VA 0x80000000 here. */
    if (!getenv("JSRF_NO_APU")) {
        /* The APU addresses *physical* memory, and in this runtime physical
         * page P lives in the contiguous window at guest VA 0x80000000 + P
         * (MmAllocateContiguousMemory hands out addresses there), not at
         * VA P: the two are separate mappings. Pointing the emulation at
         * VA 0 made every voice-list read come back empty and every CBO
         * write-back land in the title's heap -- the scene graph corrupted
         * the moment the title music started. */
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)(g_xbox_mem_offset + 0x80000000u));
        /* Route the APU register window (main regs + voice processor) to
         * the emulation; the lifted code's memory macros call these for
         * any access in [RECOMP_MMIO_LO, RECOMP_MMIO_HI). Without it the
         * VP never runs and DirectSound's play cursor never moves. */
        if (!getenv("JSRF_NO_APU_MMIO")) {
            extern uint32_t (*g_recomp_mmio_read_fn)(uint32_t, unsigned);
            extern void (*g_recomp_mmio_write_fn)(uint32_t, uint32_t, unsigned);
            g_recomp_mmio_read_fn = jsrf_apu_mmio_read;
            g_recomp_mmio_write_fn = jsrf_apu_mmio_write;
            fprintf(stderr, "  APU: MMIO 0xFE800000..0xFED00000 routed (APU emulation + AC97 quirks)\n");
        }
    }

    /* The USB host controllers.
     *
     * A title reaches its gamepad through XAPI, which is linked into the
     * image and drives the OHCI registers directly -- there is no kernel call
     * to intercept. Without a controller answering, the title's own driver
     * finds no host controller and never asks for a report, which is exactly
     * what a run showed: the guest read the pad zero times in seventy
     * seconds. RECOMP_USB=1 turns it on. */
    xbox_OhciInit();

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

#if defined(__APPLE__)
    /*
     * On macOS the window has to belong to the main thread.
     *
     * AppKit refuses to create an NSWindow anywhere else, and event handling
     * has the same rule -- and the renderer's context comes up wherever the
     * title first touches the GPU, which is the pushbuffer thread. So when a
     * window is wanted, the guest moves to a thread of its own and the main
     * thread does nothing but hold the window open and drain its events. The
     * guest already runs on real host threads for its own PsCreateSystemThread
     * calls, so being on one more is not a new situation for it.
     *
     * Without RECOMP_WINDOW nothing changes: the guest runs on the main
     * thread exactly as before, and there is no window to service.
     */
    if (getenv("RECOMP_WINDOW")) {
        extern int   nv_window_prepare(int w, int h, const char *title);
        extern void  nv_window_pump(void);
        extern int   nv_window_should_close(void);
        pthread_t guest;

        /* The window's size is not the game's resolution.
         *
         * The game draws a 640x480 surface and always will; the renderer's
         * internal resolution is a separate multiple of that (RECOMP_GL_SCALE)
         * and the window is a third number again -- it just shows whatever
         * frame arrives, scaled to fit. Measured on an M2 Pro, four times the
         * internal resolution costs nothing at all, so there is no reason for
         * the window to stay at the console's. */
        int win_w = 1280, win_h = 960;
        { const char *w = getenv("RECOMP_WINDOW_W");
          const char *h = getenv("RECOMP_WINDOW_H");
          if (w) win_w = atoi(w);
          if (h) win_h = atoi(h);
          if (win_w < 320) win_w = 320;
          if (win_h < 240) win_h = 240; }
        if (!nv_window_prepare(win_w, win_h, "Jet Set Radio Future"))
            fprintf(stderr, "  [WIN] could not open a window; "
                            "running offscreen\n");
        if (pthread_create(&guest, NULL, guest_thread_main, NULL) == 0) {
            while (!nv_window_should_close() && !g_guest_returned) {
                nv_window_pump();
                usleep(4000);       /* ~240 Hz; cheap, and keeps drags smooth */
            }
            fprintf(stderr, "\nWindow closed.\n");
            fflush(stderr);
            _exit(0);
        }
        fprintf(stderr, "  [WIN] could not start the guest thread; "
                        "running on the main thread\n");
    }
#endif

    { extern void xbox_guest_lock_acquire(void); xbox_guest_lock_acquire(); }
    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* The guest, on a thread of its own, so that the main thread can own the
 * window. Only used when RECOMP_WINDOW asks for one. */
static void *guest_thread_main(void *arg)
{
    (void)arg;
    /*
     * The guest's registers are thread-local, and this is a different thread.
     *
     * Everything init did to prepare the guest -- above all the stack pointer
     * -- it did on the main thread, into storage that is per-thread by
     * declaration. Starting the guest over here therefore started it with
     * esp = 0, and the first push in the entry point stored through
     * 0xFFFFFFFC and died before a single kernel call. The window path never
     * ran further than that, and I did not notice because the runs I was
     * measuring were headless.
     *
     * The guest lock belongs here for the same reason and with worse
     * consequences: taken on the main thread, it was held by a thread that
     * then does nothing but pump events for the rest of the run, so every
     * guest thread that wanted it would have waited forever.
     */
    g_esp = XBOX_STACK_TOP;
    { extern void xbox_guest_lock_acquire(void); xbox_guest_lock_acquire(); }
    xbe_entry_point();
    g_guest_returned = 1;
    return NULL;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* Console entry point (for debugging -- lets you see printf output) */
int main(int argc, char **argv)
{
    if (argc > 1) g_game_dir_arg = argv[1];
    return host_main();
}
