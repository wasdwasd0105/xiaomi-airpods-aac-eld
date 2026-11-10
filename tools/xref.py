#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) the developer
"""Finds the code that uses a text string inside libbluetooth_jni.so.

The Bluetooth stack logs a line for nearly everything it decides, and the
text of each line is stored in the library. So when the phone's log shows a
line next to the behaviour you want to change, this finds the function that
printed it.

usage: tools/xref.py LIB 'text' [BEFORE] [AFTER]

  text     the START of the stored string, exactly as stored. Log lines are
           stored without the "file.cc:123 function:" prefix that logcat
           shows, and values appear as {} in them: for the log line
             ... ProcessEvent: Peer 11:22:.. : Reconfig done - Ignore calling ...
           search for 'Peer {} : Reconfig done'. A piece from the middle of
           the string finds the string but not the code: the code points at
           the string's first byte.
  BEFORE   instructions to print before each use (default 40)
  AFTER    instructions to print after each use (default 20)

Each use is printed with the function it is in (if tools/symbols.py has been
run on the library) and the surrounding instructions; the line marked >> is
the one that loads the string. Calls a few lines above it are the checks that
led there; calls below are what the stack does next.

Needs:  pip install capstone
"""
import bisect, os, struct, sys

try:
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
except ImportError:
    sys.exit('capstone is missing. Install it with:  pip install capstone')

if len(sys.argv) < 3:
    sys.exit(__doc__)
path, needle = sys.argv[1], sys.argv[2].encode()
before = int(sys.argv[3]) if len(sys.argv) > 3 else 40
after = int(sys.argv[4]) if len(sys.argv) > 4 else 20
d = open(path, 'rb').read()
phoff, = struct.unpack_from('<Q', d, 0x20)
phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
segs = []
for i in range(phnum):
    t, fl, off, va, pa, fsz, msz, al = struct.unpack_from('<IIQQQQQQ', d, phoff + i * phentsize)
    if t == 1:
        segs.append((va, off, fsz, fl))

def off2va(o):
    for va, off, fsz, fl in segs:
        if off <= o < off + fsz:
            return va + o - off

syms = []
if os.path.exists(path + '.demangled.txt'):
    for line in open(path + '.demangled.txt', errors='replace'):
        p = line.rstrip('\n').split(' ', 2)
        if len(p) == 3 and p[1] in 'TtWw':
            try:
                syms.append((int(p[0], 16), p[2]))
            except ValueError:
                pass
    syms.sort()
addrs = [s[0] for s in syms]

def name(v):
    i = bisect.bisect_right(addrs, v) - 1
    if i < 0:
        return 'unknown function (run tools/symbols.py on the library for names)'
    a, n = syms[i]
    return n[:100] if a == v else '%s+%#x' % (n[:90], v - a)

targets = []
pos = d.find(needle)
while pos >= 0:
    v = off2va(pos)
    if v is not None:
        targets.append(v)
    pos = d.find(needle, pos + 1)
if not targets:
    sys.exit('the text is not in this library. Check the spelling, drop the log prefix, and use {} where the log shows a value.')
print('text found at', ', '.join(hex(t) for t in targets))

md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
uses = 0
for va, off, fsz, fl in segs:
    if not fl & 1:
        continue
    code = d[off:off + fsz]
    for i in range(0, len(code) - 24, 4):
        w, = struct.unpack_from('<I', code, i)
        if (w & 0x9f000000) != 0x90000000:      # adrp
            continue
        rd = w & 31
        imm = ((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3)
        if imm & (1 << 20):
            imm -= 1 << 21
        page = ((va + i) & ~0xfff) + (imm << 12)
        for j in range(1, 6):
            w2, = struct.unpack_from('<I', code, i + 4 * j)
            if (w2 & 0xffc00000) == 0x91000000 and ((w2 >> 5) & 31) == rd:   # add xN, xN, #imm
                if page + ((w2 >> 10) & 0xfff) in targets:
                    at = va + i
                    uses += 1
                    print('\n=== used at %#x, in %s' % (at, name(at)))
                    s = max(0, i - 4 * before); e = min(len(code), i + 4 * after)
                    for ins in md.disasm(code[s:e], va + s):
                        extra = ''
                        if ins.mnemonic in ('bl', 'b') and ins.op_str.startswith('#') and syms:
                            extra = '   ; ' + name(int(ins.op_str[1:], 16))
                        print('%s %#x: %s %s%s' % ('>>' if ins.address == at else '  ', ins.address, ins.mnemonic, ins.op_str, extra))
                break
if not uses:
    print('\nThe text is stored but no code points at its first byte. You searched for a piece from the\n'
          'middle of a longer string: search for the beginning of that string instead.')
