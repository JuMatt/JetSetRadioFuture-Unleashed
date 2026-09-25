#!/usr/bin/env python3
"""
snap_replay.py -- differential test of one recompiled call against the original x86.

Usage:  JSRF_XBE=/path/to/default.xbe python3 snap_replay.py snap_in.bin[.gz] snap_out.bin[.gz]
        [--trace-calls] [--ring] [--fpcw 0x027F]
Needs:  pip install unicorn capstone numpy

How the snapshots are made: see jsrf-recomp/src/recomp_manual.c (JSRF_SNAP,
JSRF_SNAP_VA, JSRF_SNAP_ARG, JSRF_SNAP_DIR). A function is snapshotted by
renaming its generated body to sub_XXXXXXXX_gen and wrapping it there.

The runtime (JSRF_SNAP) writes guest RAM + CPU state as a recompiled function
is entered (snap_in.bin) and as it returns (snap_out.bin).  This script loads
snap_in into Unicorn, runs the ORIGINAL x86 from the XBE starting at the same
entry point until the same return address, and compares the resulting memory
with snap_out.  Whatever differs is something the recompiled code computed that
the original instructions would not have (or another guest thread's writes,
which are reported separately because the emulator never made them).
"""
import sys, struct, gzip, ctypes, argparse, collections, math
import numpy as np
from unicorn import *
from unicorn.x86_const import *

import os
XBE = os.environ.get("JSRF_XBE",
      "/mnt/user-data/uploads/Xbox/JSRF - Jet Set Radio Future (USA)/extracted/default.xbe")

def load_snap(path):
    raw = gzip.open(path, 'rb').read() if path.endswith('.gz') else open(path, 'rb').read()
    h = raw[:512]
    assert h[:8] == b'JSRFSNAP', path
    w = struct.unpack_from('<16I', h, 8)
    s = dict(ver=w[0], va=w[1], phase=w[2], ram_size=w[3], eax=w[4], ecx=w[5], edx=w[6],
             ebx=w[7], esp=w[8], ebp=w[9], esi=w[10], edi=w[11], fs=w[12], fp_top=w[13],
             flip=w[14], idx=w[15])
    s['fp'] = struct.unpack_from('<8d', h, 72)
    s['xmm'] = [h[136 + 16 * i:152 + 16 * i] for i in range(8)]
    ram = raw[512:512 + s['ram_size']]
    s['truncated'] = len(ram) < s['ram_size']
    if s['truncated']:
        print("  note: %s holds only %d of %d RAM bytes; the rest is zero-filled" % (path, len(ram), s['ram_size']))
        ram = ram + bytes(s['ram_size'] - len(ram))
    s['ram'] = ram
    bm = raw[512 + s['ram_size']:]
    s['valid'] = None
    if len(bm) >= s['ram_size'] // 4096 // 8:
        s['valid'] = bm[:s['ram_size'] // 4096 // 8]
    return s

def xbe_sections(data):
    base = struct.unpack_from('<I', data, 0x104)[0]
    nsec = struct.unpack_from('<I', data, 0x11C)[0]
    shdr = struct.unpack_from('<I', data, 0x120)[0] - base
    out = []
    for i in range(nsec):
        fl, va, vsz, raddr, rsz, name_addr = struct.unpack_from('<6I', data, shdr + 0x38 * i)
        no = name_addr - base
        name = data[no:data.index(b'\0', no)].decode(errors='replace')
        out.append((name, va, vsz, raddr, rsz))
    return out

def gdt_entry(base, limit, access, flags):
    e = limit & 0xffff
    e |= (base & 0xffffff) << 16
    e |= (access & 0xff) << 40
    e |= ((limit >> 16) & 0xf) << 48
    e |= (flags & 0xf) << 52
    e |= ((base >> 24) & 0xff) << 56
    return struct.pack('<Q', e)

def as_float(u):
    return struct.unpack('<f', struct.pack('<I', u))[0]

def fp_close(a, b):
    fa, fb = as_float(a), as_float(b)
    if not (math.isfinite(fa) and math.isfinite(fb)):
        return False
    m = max(abs(fa), abs(fb))
    if m < 1e-30:
        return True
    # plausible float magnitudes only -- small integers look like denormals
    if m < 1e-20 or m > 1e20:
        return False
    return abs(fa - fb) <= 2e-4 * m + 1e-6

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('snap_in'); ap.add_argument('snap_out')
    ap.add_argument('--fpcw', type=lambda x: int(x, 0), default=0x027F)
    ap.add_argument('--max', type=int, default=400_000_000)
    ap.add_argument('--trace-calls', action='store_true')
    ap.add_argument('--show', type=int, default=60)
    ap.add_argument('--dump-final', default=None)
    ap.add_argument('--ring', action='store_true')
    a = ap.parse_args()

    si = load_snap(a.snap_in); so = load_snap(a.snap_out)
    size = si['ram_size']
    print("snapshot: va=%08X flip=%u call#%u  esp=%08X ret=%08X  ram=%d MB" % (
        si['va'], si['flip'], si['idx'], si['esp'],
        struct.unpack_from('<I', si['ram'], si['esp'])[0], size >> 20))
    buf = ctypes.create_string_buffer(si['ram'], size)
    ptr = ctypes.addressof(buf)

    mu = Uc(UC_ARCH_X86, UC_MODE_32)
    mu.mem_map_ptr(0x1000, size - 0x1000, UC_PROT_ALL, ptr + 0x1000)
    mu.mem_map_ptr(0x80000000, size, UC_PROT_ALL, ptr)          # contiguous-memory mirror
    # A flat code/data/stack set plus one segment for fs. Loading a GDT with
    # only the fs entry leaves ss pointing at the null descriptor, and every
    # push then silently goes nowhere.
    GDT = 0xC0000000
    mu.mem_map(GDT, 0x1000)
    mu.mem_write(GDT + 8 * 1, gdt_entry(0, 0xfffff, 0x9B, 0xC))
    mu.mem_write(GDT + 8 * 2, gdt_entry(0, 0xfffff, 0x93, 0xC))
    mu.mem_write(GDT + 8 * 3, gdt_entry(si['fs'], 0xfffff, 0x93, 0xC))
    mu.reg_write(UC_X86_REG_GDTR, (0, GDT, 8 * 4 - 1, 0))
    mu.reg_write(UC_X86_REG_CS, 1 << 3)
    for r in (UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS):
        mu.reg_write(r, 2 << 3)
    mu.reg_write(UC_X86_REG_FS, 3 << 3)

    # code: prefer the image as the XBE ships it (the runtime may patch its copy)
    xbe = open(XBE, 'rb').read()
    secs = xbe_sections(xbe)
    patched = 0
    for name, va, vsz, raddr, rsz in secs:
        if name in ('.text', 'D3D', 'D3DX', 'XGRPH', 'DSOUND', 'WMADEC', 'XONLINE', 'XNET', 'XPP', 'DOLBY', 'XMV'):
            if va + rsz <= size:
                cur = bytes(buf[va:va + rsz])
                orig = xbe[raddr:raddr + rsz]
                if cur != orig:
                    ndiff = sum(1 for i in range(0, rsz, 4096) if cur[i:i+4096] != orig[i:i+4096])
                    print("  section %-8s %08X+%X differs from XBE in %d pages%s" % (name, va, rsz, ndiff,
                          " -> restoring original bytes" if name == '.text' else " (left as the runtime has it)"))
                    if name == '.text':
                        mu.mem_write(va, orig); patched += 1
    ret = struct.unpack_from('<I', si['ram'], si['esp'])[0]

    regs = dict(eax=UC_X86_REG_EAX, ecx=UC_X86_REG_ECX, edx=UC_X86_REG_EDX, ebx=UC_X86_REG_EBX,
                esp=UC_X86_REG_ESP, ebp=UC_X86_REG_EBP, esi=UC_X86_REG_ESI, edi=UC_X86_REG_EDI)
    for k, r in regs.items():
        mu.reg_write(r, si[k])
    mu.reg_write(UC_X86_REG_EFLAGS, 0x202)
    mu.reg_write(UC_X86_REG_FPCW, a.fpcw)
    mu.reg_write(UC_X86_REG_FPTAG, 0xFFFF)
    try:
        mu.reg_write(UC_X86_REG_MXCSR, 0x1F80)
    except Exception:
        pass

    last_writer = {}
    def on_write(uc, access, addr, sz, val, ud):
        pc = uc.reg_read(UC_X86_REG_EIP)
        for i in range(0, sz, 4):
            last_writer[(addr + i) & ~3] = pc
    mu.hook_add(UC_HOOK_MEM_WRITE, on_write)

    bad = []
    def on_unmapped(uc, access, addr, sz, val, ud):
        pc = uc.reg_read(UC_X86_REG_EIP)
        bad.append((access, addr, sz, pc))
        print("  UNMAPPED access type=%d addr=%08X size=%d at pc=%08X" % (access, addr, sz, pc))
        return False
    mu.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)

    ring = collections.deque(maxlen=64)
    if a.ring:
        def on_ins(uc, addr, sz, ud):
            ring.append(addr)
        mu.hook_add(UC_HOOK_CODE, on_ins)
    calls = collections.Counter()
    if a.trace_calls:
        text = [s for s in secs if s[0] == '.text'][0]
        def on_code(uc, addr, sz, ud):
            b = uc.mem_read(addr, 1)[0]
            if b == 0xE8:
                rel = struct.unpack('<i', bytes(uc.mem_read(addr + 1, 4)))[0]
                calls[(addr + 5 + rel) & 0xffffffff] += 1
        mu.hook_add(UC_HOOK_CODE, on_code)

    try:
        mu.emu_start(si['va'], ret, count=a.max)
    except UcError as e:
        print("  emulation stopped: %s at eip=%08X" % (e, mu.reg_read(UC_X86_REG_EIP)))
    if ring:
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        print("  last instructions:")
        for ad in list(ring)[-40:]:
            try:
                code = bytes(mu.mem_read(ad, 16))
                ins = next(md.disasm(code, ad))
                print("     %08X  %s %s" % (ad, ins.mnemonic, ins.op_str))
            except Exception:
                print("     %08X  ?" % ad)
    eip = mu.reg_read(UC_X86_REG_EIP); esp = mu.reg_read(UC_X86_REG_ESP)
    print("x86 finished at eip=%08X (want %08X) esp=%08X (native %08X)  eax=%08X (native %08X)" % (
        eip, ret, esp, so['esp'], mu.reg_read(UC_X86_REG_EAX), so['eax']))
    if calls:
        print("calls made:", ", ".join("%08X x%d" % (k, v) for k, v in calls.most_common(40)))

    emu = np.frombuffer(bytes(buf[:size]), dtype='<u4')
    nat = np.frombuffer(so['ram'], dtype='<u4')
    ini = np.frombuffer(si['ram'], dtype='<u4')
    if a.dump_final:
        open(a.dump_final, 'wb').write(bytes(buf[:size]))
    diff = np.nonzero(emu != nat)[0]
    print("words differing between x86 result and recompiled result: %d" % len(diff))
    groups = []
    for i in diff:
        addr = int(i) * 4
        e, n, i0 = int(emu[i]), int(nat[i]), int(ini[i])
        who = last_writer.get(addr)
        close = fp_close(e, n)
        kind = ('fp~' if close else 'VAL') if who is not None else ('native-only' if n != i0 else 'x86-only?')
        if groups and addr - groups[-1][-1][0] <= 16 and groups[-1][-1][4] == kind:
            groups[-1].append((addr, e, n, i0, kind, who))
        else:
            groups.append([(addr, e, n, i0, kind, who)])
    summary = collections.Counter(g[0][4] for g in groups)
    print("difference groups by kind:", dict(summary))
    shown = 0
    for g in groups:
        kind = g[0][4]
        if kind == 'fp~':
            continue
        if shown >= a.show:
            break
        shown += 1
        writers = sorted(set("%08X" % w[5] for w in g if w[5] is not None))
        print("-- %s  %08X..%08X  (%d words)  last x86 writer(s): %s" % (
            kind, g[0][0], g[-1][0] + 3, len(g), " ".join(writers[:6])))
        for (addr, e, n, i0, k, who) in g[:8]:
            print("     %08X  x86=%08X (%12.5g)  recomp=%08X (%12.5g)  before=%08X" % (
                addr, e, as_float(e), n, as_float(n), i0))
    fpg = [g for g in groups if g[0][4] == 'fp~']
    print("float-rounding-only groups: %d (%d words)" % (len(fpg), sum(len(g) for g in fpg)))

if __name__ == '__main__':
    main()
