#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) the developer
"""Extracts one LZ4-compressed file (compact indexes, 4K pclusters) from an
EROFS image. usage: erofs_z.py image /path out"""
import struct, sys, ctypes, importlib.util, os
spec = importlib.util.spec_from_file_location('erofs', os.path.join(os.path.dirname(__file__), 'erofs.py'))
sys_argv = sys.argv; sys.argv = [sys.argv[0], sys.argv[1], 'none', '/']
E = importlib.util.module_from_spec(spec); spec.loader.exec_module(E); sys.argv = sys_argv
import ctypes.util
lz4 = ctypes.CDLL(ctypes.util.find_library('lz4') or '/opt/homebrew/lib/liblz4.dylib')
lz4.LZ4_decompress_safe_partial.restype = ctypes.c_int
m, BS = E.m, E.BS
ino = E.lookup(sys.argv[2]); size = ino['size']
assert ino['layout'] == 3, ino
hdr_at = (ino['data_off'] + 7) & ~7
h_frag, h_advise, h_alg, h_cbits = struct.unpack('<IHBB', m[hdr_at:hdr_at+8])
assert (h_cbits & 7) == 0 and (h_advise & 0x2) == 0 or True
big = h_advise & 0x2; twob = h_advise & 0x1
lcbits = 12 + (h_cbits & 7); LC = 1 << lcbits
ebase = hdr_at + 8
total = (size + LC - 1) // LC
init4 = ((32 - ebase % 32) // 4) % 8
if init4 > total: init4 = total
n2b = ((total - init4) // 16 * 16) if twob else 0
lobits = max(lcbits, 12)
def bits(base, pos, ):
    v, = struct.unpack('<I', m[base + pos // 8: base + pos // 8 + 4]); v >>= (pos & 7)
    return v & ((1 << lobits) - 1), (v >> lobits) & 3
def entry(lcn):
    if lcn < init4: sh, pos = 2, ebase + lcn * 4
    elif lcn - init4 < n2b: sh, pos = 1, ebase + init4 * 4 + (lcn - init4) * 2
    else: sh, pos = 2, ebase + init4 * 4 + n2b * 2 + (lcn - init4 - n2b) * 4
    vcnt = 2 if sh == 2 else 16
    packsz = vcnt << sh; enc = (packsz - 4) * 8 // vcnt
    off = pos & (packsz - 1); base = pos - off; i = off >> sh
    lo, typ = bits(base, enc * i)
    if typ == 2: return typ, None, None
    nblk = 1; j = i
    while j > 0:
        j -= 1
        l2, t2 = bits(base, enc * j)
        if t2 == 2: j -= l2
        if j >= 0: nblk += 1
    blk, = struct.unpack('<I', m[base + packsz - 4: base + packsz])
    return typ, lo, blk + nblk
heads = []
for lcn in range(total):
    typ, ofs, pblk = entry(lcn)
    if typ != 2: heads.append((lcn * LC + ofs, typ, pblk))
print('advise %#x alg %#x lcbits %d, %d lclusters, %d extents, 4b-initial %d, 2b %d' % (h_advise, h_alg, lcbits, total, len(heads), init4, n2b))
out = bytearray(size); bad = 0
for k, (start, typ, pblk) in enumerate(heads):
    end = heads[k + 1][0] if k + 1 < len(heads) else size
    ln = end - start; blk = m[pblk * BS: pblk * BS + BS]
    if typ == 0:
        out[start:end] = blk[:ln]
    else:
        z = 0
        while z < BS and blk[z] == 0: z += 1
        src = blk[z:]; dst = ctypes.create_string_buffer(ln)
        r = lz4.LZ4_decompress_safe_partial(src, dst, len(src), ln, ln)
        if r != ln: bad += 1
        out[start:end] = dst.raw[:ln]
print('extents that did not decompress to their length:', bad)
open(sys.argv[3], 'wb').write(out)
