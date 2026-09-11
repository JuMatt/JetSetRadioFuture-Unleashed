"""POSIX port of the XBE conformance phase (tools.conformance --xbe).

Same idea as __main__._run_xbe: lift a title's self-contained functions and
run each against its own machine code, but built with `gcc -m32` and guarded
with sigsetjmp instead of MSVC + SEH. Usage:

    python -m tools.conformance.posix_xbe game_files/default.xbe --limit 400
"""
import argparse, os, re, subprocess, sys, tempfile

from . import xbe_run
from tools.recomp.disasm import Disassembler

POSIX_PRELUDE = r'''
#include <sys/mman.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/wait.h>
static sigjmp_buf g_jmp;
static volatile int g_in_guard;
uint32_t recomp_mmio_read(uint32_t a, unsigned s) { (void)s; return *(volatile uint32_t *)(uintptr_t)a; }
void recomp_mmio_write(uint32_t a, uint32_t v, unsigned s) { (void)s; *(volatile uint32_t *)(uintptr_t)a = v; }
static void guard_handler(int sig) { if (g_in_guard == 1) { g_in_guard = 2; siglongjmp(g_jmp, sig); } _exit(3); }
static void install_guards(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = guard_handler; sa.sa_flags = SA_NODEFER | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0);
    sigaction(SIGFPE, &sa, 0);  sigaction(SIGILL, &sa, 0); sigaction(SIGALRM, &sa, 0);
    { static char altstack[65536]; stack_t ss; ss.ss_sp = altstack; ss.ss_size = sizeof altstack; ss.ss_flags = 0; sigaltstack(&ss, 0); }
}
'''

POSIX_COMMIT = r'''
static int commit(uintptr_t lo, uintptr_t hi) {
    uintptr_t a;
    for (a = lo & ~(uintptr_t)0xFFFF; a < hi; a += 0x10000) {
        void *p = mmap((void *)a, 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == (void *)a) continue;
        if (p != MAP_FAILED) { munmap(p, 0x10000); }
        /* already mapped from a shared granule: probe it */
        { unsigned char v; if (mincore((void *)a, 4096, &v) == 0) continue; }
        return 0;
    }
    return 1;
}
'''

POSIX_CALL_NATIVE = r'''
static void call_native(void *fn, const uint32_t *args) {
    g_fn = fn;
    g_args_p = args;
    __asm__ __volatile__ (
        "pushal\n\t"
        "movl g_args_p, %%esi\n\t"
        "movl %%esp, g_saved_sp\n\t"
        "movl $" NATIVE_STACK_GAP_S ", %%eax\n"
        "1:\n\t"
        "subl $4, %%esp\n\t"
        "movl $0xCDCDCDCD, (%%esp)\n\t"
        "subl $4, %%eax\n\t"
        "jnz 1b\n\t"
        "pushl 12(%%esi)\n\t"
        "pushl 8(%%esi)\n\t"
        "pushl 4(%%esi)\n\t"
        "pushl (%%esi)\n\t"
        "finit\n\t"
        "fldcw g_cw\n\t"
        "fldl g_fp_arg\n\t"
        "xorl %%eax, %%eax\n\t"
        "xorl %%ecx, %%ecx\n\t"
        "xorl %%edx, %%edx\n\t"
        "xorl %%ebx, %%ebx\n\t"
        "xorl %%esi, %%esi\n\t"
        "xorl %%edi, %%edi\n\t"
        "call *g_fn\n\t"
        "movl g_saved_sp, %%esp\n\t"
        "movl %%eax, g_nat_eax\n\t"
        "movl %%edx, g_nat_edx\n\t"
        "popal\n\t"
        : : : "memory", "cc");
}
'''


def posixify(src):
    src = src.replace('#include <windows.h>', POSIX_PRELUDE, 1)
    # commit(): replace the whole function body
    src = re.sub(r'static int commit\(uintptr_t lo, uintptr_t hi\) \{.*?\n\}\n',
                 lambda m: POSIX_COMMIT.lstrip('\n'), src, count=1, flags=re.S)
    # call_native(): replace the MSVC inline-asm version
    src = re.sub(r'static void call_native\(void \*fn, const uint32_t \*args\) \{.*?\n\}\n',
                 lambda m: POSIX_CALL_NATIVE.lstrip('\n'), src, count=1, flags=re.S)
    src = src.replace('#define NATIVE_STACK_GAP 0x8000',
                      '#define NATIVE_STACK_GAP 0x8000\n#define NATIVE_STACK_GAP_S "0x8000"')
    # SEH -> sigsetjmp guards
    src = src.replace('__try { call_native((void *)(uintptr_t)va, args); }\n'
                      '    __except (EXCEPTION_EXECUTE_HANDLER) { g_faulted = 1; }',
                      'g_in_guard = 1; alarm(2); if (sigsetjmp(g_jmp, 1) == 0) { call_native((void *)(uintptr_t)va, args); } else { g_faulted = 1; } alarm(0); g_in_guard = 0;')
    src = src.replace('__try { lifted(); }\n'
                      '    __except (EXCEPTION_EXECUTE_HANDLER) { g_faulted = 2; }',
                      'g_in_guard = 1; alarm(2); if (sigsetjmp(g_jmp, 1) == 0) { lifted(); } else { g_faulted = 2; } alarm(0); g_in_guard = 0;')
    src = src.replace('{ unsigned short cw = 0x027F; __asm { fldcw cw } }',
                      '{ unsigned short cw = 0x027F; __asm__ __volatile__("fldcw %0" : : "m"(cw)); install_guards(); }')
    # globals referenced from asm need C linkage names without static
    src = src.replace('        printf("@RUN %08X\\n", g_funcs[i]);\n        fflush(stdout);\n        for (v = 0; v < (int)(sizeof g_args / sizeof g_args[0]); v++) {\n            g_fp_arg = g_fp_args[v];\n            run_one(g_funcs[i], g_args[v], v, &shown);\n        }\n        if (shown > 3) printf("       ... and %d more\\n", shown - 3);', '        printf("@RUN %08X\\n", g_funcs[i]);\n        fflush(stdout);\n        { pid_t pid = fork(); int st = 0;\n          if (pid == 0) {\n            for (v = 0; v < (int)(sizeof g_args / sizeof g_args[0]); v++) {\n                g_fp_arg = g_fp_args[v];\n                run_one(g_funcs[i], g_args[v], v, &shown);\n            }\n            if (shown > 3) printf("       ... and %d more\\n", shown - 3);\n            fflush(stdout);\n            _exit((g_total & 0x7F) | (g_fail ? 0x80 : 0));\n          }\n          waitpid(pid, &st, 0);\n          if (WIFEXITED(st)) { g_total += WEXITSTATUS(st) & 0x7F; if (WEXITSTATUS(st) & 0x80) g_fail++; }\n          else printf("DIED sub_%08X (signal %d)\\n", g_funcs[i], WTERMSIG(st));\n          fflush(stdout);\n        }')
    src = src.replace('static void *g_fn;', 'void *g_fn;')
    src = src.replace('static const uint32_t *g_args_p;', 'const uint32_t *g_args_p;')
    src = src.replace('static double g_fp_arg;', 'double g_fp_arg;')
    src = src.replace('static unsigned short g_cw = 0x027F;', 'unsigned short g_cw = 0x027F;')
    src = src.replace('static uint32_t g_saved_sp;', 'uint32_t g_saved_sp;')
    src = src.replace('static uint32_t g_nat_eax, g_nat_edx;', 'uint32_t g_nat_eax, g_nat_edx;')
    return src


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('xbe')
    ap.add_argument('--limit', type=int, default=200)
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('-v', action='store_true')
    ap.add_argument('--funcs', help='functions.json from tools.disasm to seed the scan')
    ap.add_argument('--offset', type=int, default=0)
    args = ap.parse_args(argv)

    data, sections, base = xbe_run.load(args.xbe)
    extra = ()
    if args.funcs:
        import json
        extra = [int(f['start'], 16) for f in json.load(open(args.funcs))]
    entries, needed, closure = xbe_run.find_candidates(data, sections, args.limit + args.offset, Disassembler()._cs, extra)
    entries = entries[args.offset:]
    if not entries:
        print('no candidates'); return 1
    lifted, rejected = xbe_run.lift(data, sections, needed)
    # drop functions whose lifted body references an unlifted callee (transitively)
    while True:
        have = {va for va, _, _ in lifted}
        keep = [row for row in lifted
                if all(int(m, 16) in have for m in re.findall(r'\blifted_([0-9A-Fa-f]{8})\b', row[2]))]
        if len(keep) == len(lifted):
            break
        lifted = keep
    have = {va for va, _, _ in lifted}
    callable_ = [row for row in lifted if row[0] in closure and closure[row[0]] <= have]
    print(f'{os.path.basename(args.xbe)}: {len(entries)} entry points, {len(needed)} in closure, '
          f'{len(callable_)} comparable, {len(rejected)} rejected')
    if args.v:
        for va, why in rejected[:30]:
            print(f'  rejected sub_{va:08X}: {why[0]}')
    src = xbe_run.harness_source(os.path.abspath(args.xbe), sections, lifted, callable_)
    src = posixify(src)
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    runtime_inc = os.path.join(root, 'templates', 'runtime')
    workdir = tempfile.mkdtemp(prefix='xboxrecomp-conf-')
    cpath = os.path.join(workdir, 'xbe_harness.c')
    with open(cpath, 'w') as f:
        f.write(src)
    exe = os.path.join(workdir, 'xbe_harness')
    r = subprocess.run(['gcc', '-m32', '-O1', '-w', '-DRECOMP_TLS=', f'-I{runtime_inc}',
                        cpath, '-o', exe, '-lm'], capture_output=True, text=True)
    if r.returncode != 0:
        print('build failed:\n' + r.stderr[:4000]); print('source kept at', cpath); return 2
    skips = []
    for _ in range(12):
        cmd = [exe, os.path.abspath(args.xbe)]
        if skips:
            cmd.append(','.join(f'{s:08X}' for s in skips))
        run = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        out = run.stdout
        if run.returncode in (0, 1):
            print(out)
            break
        print(out)
        m = re.findall(r'@RUN ([0-9A-F]{8})', out)
        if not m:
            print('harness died with no @RUN marker:', run.returncode, out[-800:]); break
        skips.append(int(m[-1], 16))
        print(f'  harness died in sub_{m[-1]} (exit {run.returncode}); skipping it')
    if args.keep:
        print('workdir:', workdir)
    return 0


if __name__ == '__main__':
    sys.exit(main())
