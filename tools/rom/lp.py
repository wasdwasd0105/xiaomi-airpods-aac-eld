#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) the developer
"""Lists the partitions in an Android super image (liblp metadata) and can
carve one out. usage: lp.py super.img [partition out.img]"""
import struct, sys
f = open(sys.argv[1], 'rb')
f.seek(4096); geo = f.read(52)
magic, gsize, _sha, meta_max, slots, lbs = struct.unpack('<II32sIII', geo[:52])
assert magic == 0x616c4467, hex(magic)
f.seek(4096 * 3); hdr = f.read(256)
(hmagic, major, minor, hsize) = struct.unpack('<IHHI', hdr[:12])
assert hmagic == 0x414C5030, hex(hmagic)
tsize, = struct.unpack('<I', hdr[44:48])
def desc(o): return struct.unpack('<III', hdr[o:o+12])
parts, exts, groups, bdevs = desc(80), desc(92), desc(104), desc(116)
tables = f.read(tsize)
P = []
for i in range(parts[1]):
    e = tables[parts[0] + i*parts[2]: parts[0] + (i+1)*parts[2]]
    name = e[:36].rstrip(b'\0').decode(); attrs, first, num, grp = struct.unpack('<IIII', e[36:52])
    E = []
    for j in range(first, first+num):
        x = tables[exts[0] + j*exts[2]: exts[0] + (j+1)*exts[2]]
        nsec, ttype, tdata, tsrc = struct.unpack('<QIQI', x[:24]); E.append((nsec, ttype, tdata))
    P.append((name, E))
if len(sys.argv) < 4:
    for name, E in P: print('%-22s %8.1f MB  %d extents' % (name, sum(n for n,_,_ in E)*512/1e6, len(E)))
else:
    for name, E in P:
        if name == sys.argv[2]:
            out = open(sys.argv[3], 'wb')
            for nsec, ttype, tdata in E:
                if ttype != 0: out.write(b'\0' * nsec * 512); continue
                f.seek(tdata * 512); left = nsec * 512
                while left: b = f.read(min(left, 1 << 24)); out.write(b); left -= len(b)
            print('wrote', sys.argv[3]); break
    else: print('no such partition')
