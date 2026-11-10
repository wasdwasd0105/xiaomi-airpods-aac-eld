#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) the developer
"""Minimal EROFS reader: path lookup, directory listing, and extraction of
files stored uncompressed (flat plain, flat inline, chunk based).
usage: erofs.py image ls /path | stat /path | cat /path out"""
import struct, sys, mmap
f = open(sys.argv[1], 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
sb = m[1024:1024+128]
magic, _c, fcompat, blkbits, extslots, root_nid = struct.unpack('<IIIBBH', sb[:16])
assert magic == 0xE0F5E1E2
meta_blk, xattr_blk = struct.unpack('<II', sb[40:48]); fincompat, = struct.unpack('<I', sb[80:84])
BS = 1 << blkbits
def inode(nid):
    o = meta_blk * BS + nid * 32
    fmt, xcnt, mode = struct.unpack('<HHH', m[o:o+6]); ext = fmt & 1; layout = (fmt >> 1) & 7
    if ext: size, = struct.unpack('<Q', m[o+8:o+16]); iu, = struct.unpack('<I', m[o+16:o+20]); isz = 64
    else: size, = struct.unpack('<I', m[o+8:o+12]); iu, = struct.unpack('<I', m[o+16:o+20]); isz = 32
    xsz = (12 + (xcnt - 1) * 4) if xcnt else 0
    return dict(nid=nid, mode=mode, size=size, layout=layout, iu=iu, data_off=o + isz + xsz, fmt=fmt)
def read(ino):
    L, size = ino['layout'], ino['size']
    if L == 0: return m[ino['iu'] * BS: ino['iu'] * BS + size]
    if L == 2:
        full = size // BS * BS
        return m[ino['iu'] * BS: ino['iu'] * BS + full] + m[ino['data_off']: ino['data_off'] + size - full]
    if L == 4:
        fmtc = ino['iu'] & 0xffff; bits = blkbits + (fmtc & 0x1f); idx = fmtc & 0x20
        csz = 1 << bits; n = (size + csz - 1) // csz; out = bytearray(); o = (ino['data_off'] + 7) & ~7 if idx else ino['data_off']
        for i in range(n):
            if idx: _adv, dev, blk = struct.unpack('<HHI', m[o + i*8: o + i*8 + 8])
            else: blk, = struct.unpack('<I', m[o + i*4: o + i*4 + 4])
            ln = min(csz, size - i * csz)
            out += b'\0' * ln if blk == 0xffffffff else m[blk * BS: blk * BS + ln]
        return bytes(out)
    raise SystemExit('compressed file (layout %d), needs a real EROFS tool' % L)
def listdir(ino):
    d = read(ino); out = {}
    for b in range(0, len(d), BS):
        blk = d[b:b+BS]
        if len(blk) < 12: break
        n = struct.unpack('<H', blk[8:10])[0] // 12
        ents = [struct.unpack('<QHBB', blk[i*12:i*12+12]) for i in range(n)]
        for i, (nid, off, ft, _r) in enumerate(ents):
            end = ents[i+1][1] if i + 1 < n else len(blk)
            out[blk[off:end].split(b'\0')[0].decode('utf-8', 'replace')] = (nid, ft)
    return out
def lookup(path):
    ino = inode(root_nid)
    for part in [p for p in path.split('/') if p]:
        ino = inode(listdir(ino)[part][0])
    return ino
if __name__ == '__main__':
    cmd, path = sys.argv[2], sys.argv[3]
    if cmd == 'ls':
        for name, (nid, ft) in sorted(listdir(lookup(path)).items()):
            i = inode(nid); print('%10d  layout %d  %s' % (i['size'], i['layout'], name))
    elif cmd == 'stat': print('blocksize', BS, 'incompat %#x' % fincompat, lookup(path))
    elif cmd == 'cat': open(sys.argv[4], 'wb').write(read(lookup(path))); print('wrote', sys.argv[4])
