#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) the developer
"""Small arm64 disassembly helper for looking inside libbluetooth_jni.so.

usage:
  tools/a64.py LIB fn    WHAT [COUNT]   disassemble COUNT instructions (default 80)
                                        from WHAT, with the names of called functions
  tools/a64.py LIB calls WHAT           every place that calls WHAT, the function it
                                        is in, and the three instructions after the
                                        call (shows whether the result in w0 is used)
  tools/a64.py LIB data  ADDRESS        code that touches the global at ADDRESS

WHAT is a hex address (69d774 or 0x69d774) or a function name, readable or
mangled (btif_a2dp_source_setup_codec, _Z10osi_mallocm). A name needs the
symbol files: run  tools/symbols.py LIB  once first. If a readable name fits
several functions they are listed and you pick one by address.

Needs:  pip install capstone
"""
import bisect, os, struct, sys

try:
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
except ImportError:
    sys.exit('capstone is missing. Install it with:  pip install capstone')

if len(sys.argv) < 4 or sys.argv[2] not in ('fn', 'calls', 'data'):
    sys.exit(__doc__)
path, mode, what = sys.argv[1], sys.argv[2], sys.argv[3]

d = open(path, 'rb').read()
if d[:4] != b'\x7fELF' or d[4] != 2 or struct.unpack_from('<H', d, 18)[0] != 183:
    sys.exit('%s is not a 64-bit ARM ELF file' % path)
phoff, = struct.unpack_from('<Q', d, 0x20)
phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
segs = []
for i in range(phnum):
    t, fl, off, va, pa, fsz, msz, al = struct.unpack_from('<IIQQQQQQ', d, phoff + i * phentsize)
    if t == 1:
        segs.append((va, off, fsz, fl))

def load_symbols(suffix):
    rows = []
    f = os.environ.get('SYMS') if suffix == '.demangled.txt' else None
    f = f or path + suffix
    if os.path.exists(f):
        for line in open(f, errors='replace'):
            p = line.rstrip('\n').split(' ', 2)
            if len(p) == 3 and p[1] in 'TtWw':
                try:
                    rows.append((int(p[0], 16), p[2]))
                except ValueError:
                    pass
    return sorted(rows)

syms = load_symbols('.demangled.txt')
addrs = [s[0] for s in syms]

def name(v):
    i = bisect.bisect_right(addrs, v) - 1
    if i < 0:
        return '?'
    a, n = syms[i]
    return n[:90] if a == v else '%s+%#x' % (n[:70], v - a)

def resolve(text):
    try:
        return int(text, 16)
    except ValueError:
        pass
    hits = [(a, n) for a, n in load_symbols('.mangled.txt') if n == text]
    if not hits:
        hits = [(a, n) for a, n in syms if n == text or n.startswith(text + '(')]
    if not hits:
        hits = [(a, n) for a, n in syms if text in n]
    if not syms:
        sys.exit('no symbol files next to %s: run  tools/symbols.py %s  first, or give an address' % (path, path))
    if not hits:
        sys.exit('no function named like "%s" in this library' % text)
    if len(hits) > 1:
        print('"%s" fits %d functions; run again with one of these addresses:' % (text, len(hits)))
        for a, n in hits[:40]:
            print('  %x  %s' % (a, n[:120]))
        sys.exit(1)
    return hits[0][0]

va0 = resolve(what)
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)

def code_at(v, n):
    for va, off, fsz, fl in segs:
        if va <= v < va + fsz:
            return d[off + v - va: off + v - va + 4 * n]
    sys.exit('address %#x is not inside the library' % v)

def show(v, n, mark=None):
    for ins in md.disasm(code_at(v, n), v):
        extra = ''
        if ins.mnemonic in ('bl', 'b') and ins.op_str.startswith('#'):
            extra = '   ; ' + name(int(ins.op_str[1:], 16))
        print('%s%#x: %s %s%s' % ('>> ' if ins.address == mark else '   ', ins.address, ins.mnemonic, ins.op_str, extra))

if mode == 'fn':
    print('%#x  %s' % (va0, name(va0)))
    show(va0, int(sys.argv[4]) if len(sys.argv) > 4 else 80)
else:
    found = 0
    for va, off, fsz, fl in segs:
        if not fl & 1:
            continue
        code = d[off:off + fsz]
        for i in range(0, len(code) - 8, 4):
            w, = struct.unpack_from('<I', code, i)
            if mode == 'calls':
                if (w & 0x7c000000) == 0x14000000:
                    imm = w & 0x3ffffff
                    if imm & (1 << 25):
                        imm -= 1 << 26
                    if va + i + imm * 4 == va0:
                        found += 1
                        print('\n%s at %#x, in %s' % ('call (bl)' if w >> 31 else 'jump (b, a tail call)', va + i, name(va + i)))
                        show(va + i, 4, mark=va + i)
            else:
                if (w & 0x9f000000) != 0x90000000:
                    continue
                rd = w & 31
                imm = ((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3)
                if imm & (1 << 20):
                    imm -= 1 << 21
                page = ((va + i) & ~0xfff) + (imm << 12)
                if page != va0 & ~0xfff:
                    continue
                for ins in md.disasm(code[i + 4:i + 28], va + i + 4):
                    if ('#%#x' % (va0 & 0xfff)) in ins.op_str and ('x%d' % rd) in ins.op_str:
                        found += 1
                        print('%#x %s %s  in %s' % (ins.address, ins.mnemonic, ins.op_str, name(ins.address)))
    if not found:
        print('nothing found. If the function is small the compiler may have inlined it into its callers.')
