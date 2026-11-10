#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) the developer
"""Checks a libbluetooth_jni.so against the names the module looks up at
start-up (the table in src/target.cpp): which required ones are there, which
optional ones are not, and which unit the encoder interval is in.

usage: tools/check_lib.py path/to/libbluetooth_jni.so

The last line is the verdict. docs/PORTING.md, step 2, says what to do with
each kind of result.

Needs llvm-objcopy and llvm-nm from the NDK (ANDROID_NDK, or the default SDK
location) and xz. It says nothing about the object layout, which the module
measures on the phone; getCodecConfig, getCodecUserConfig and
copyOutOtaCodecConfig can be disassembled to see what it will find."""
import glob, os, re, subprocess, sys, tempfile
if len(sys.argv) != 2:
    sys.exit(__doc__)
lib = sys.argv[1]
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ndk = os.environ.get('ANDROID_NDK') or os.environ.get('ANDROID_NDK_HOME') or os.environ.get('ANDROID_NDK_ROOT')
if not ndk:
    sdk = os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT')
    found = sorted(sum((glob.glob(os.path.join(os.path.expanduser(d), 'ndk', '*'))
                        for d in ([sdk] if sdk else []) + ['~/Library/Android/sdk', '~/Android/Sdk']), []))
    if not found:
        sys.exit('No Android NDK found. Install one and set ANDROID_NDK to its folder.')
    ndk = found[-1]
tools = glob.glob(os.path.join(ndk, 'toolchains/llvm/prebuilt/*/bin'))[0]
have = set()
with tempfile.TemporaryDirectory() as tmp:
    xz = os.path.join(tmp, 'mini.xz'); elf = os.path.join(tmp, 'mini.elf')
    subprocess.run([os.path.join(tools, 'llvm-objcopy'), '--dump-section', '.gnu_debugdata=' + xz, lib, os.path.join(tmp, 'x')], check=False)
    if os.path.exists(xz) and os.path.getsize(xz):
        open(elf, 'wb').write(subprocess.run(['xz', '-dc', xz], capture_output=True, check=True).stdout)
        out = subprocess.run([os.path.join(tools, 'llvm-nm'), elf], capture_output=True, text=True).stdout
        have |= {l.split()[-1] for l in out.splitlines() if l.strip()}
    else:
        print('no .gnu_debugdata section: the module cannot find anything by name in this library')
    out = subprocess.run([os.path.join(tools, 'llvm-nm'), '-D', '--defined-only', lib], capture_output=True, text=True).stdout
    have |= {l.split()[-1] for l in out.splitlines() if l.strip()}
    note = subprocess.run([os.path.join(tools, 'llvm-readelf'), '-n', lib], capture_output=True, text=True).stdout
m = re.search(r'Build ID: (\w+)', note); print('build ID', m.group(1) if m else 'none', '-', len(have), 'symbols')
src = open(os.path.join(root, 'src/target.cpp')).read()
table = src[src.index('const Name kNames[] = {'):src.index('// Not called and not hooked.')]
entries = re.findall(r'"((?:_Z|aacEnc)[A-Za-z0-9_]+)",\s*(true|false)\}', table)
shape = re.findall(r'"(_Z[A-Za-z0-9_]+)"', src[src.index('const char* const kShapeChecks[]'):src.index('const char* const kIntervalMs')])
req = [n for n, r in entries if r == 'true']; opt = [n for n, r in entries if r == 'false']
miss = [n for n in req + shape if n not in have]
print('required %d of %d, optional %d of %d' % (len(req) + len(shape) - len(miss), len(req) + len(shape), sum(n in have for n in opt), len(opt)))
def name_parts(mangled):
    """The plain names inside a mangled one, innermost last:
    _Z10osi_mallocm -> [osi_malloc]
    _ZN15A2dpCodecConfig14getCodecConfigEv -> [A2dpCodecConfig, getCodecConfig]"""
    if not mangled.startswith('_Z'): return [mangled]
    rest = mangled[2:]
    nested = rest.startswith('N')
    if nested:
        rest = rest[1:].lstrip('K')
        if rest.startswith('St'): rest = rest[2:]
    names = []
    while True:
        m = re.match(r'(\d+)', rest)
        if not m: break
        n = int(m.group(1)); names.append(rest[m.end():m.end() + n]); rest = rest[m.end() + n:]
        if not nested: break
    return names or [mangled]
for n in miss:
    print('  REQUIRED, MISSING:', n)
    # The same function with other parameters, or a relative of it, is the
    # usual reason. Listing them here saves a search.
    keys = name_parts(n)[-2:]
    near = sorted(h for h in have if h != n and all(k in h for k in keys))[:6]
    for h in near: print('      this library has:', h)
    if not near: print('      nothing in this library is named like', '::'.join(keys))
for n in opt:
    if n not in have: print('  optional, absent: ', n)
eld = any('Transform_Real_Eld' in h for h in have)
print('AAC-ELD encoder inside the library:', 'yes (FDKaacEnc_Transform_Real_Eld is there)' if eld
      else 'NOT FOUND - without it there is nothing to encode with, see docs/PORTING.md')
unit = 'milliseconds' if '_Z32a2dp_aac_get_encoder_interval_msv' in have else 'microseconds' if '_Z32a2dp_aac_get_encoder_interval_usv' in have else None
print('encoder interval:', unit or 'NEITHER NAME FOUND')
print('verdict:', 'the module would start on this library' if not miss and unit else 'the module would stay out of this library')
if miss or not unit:
    print('next: docs/PORTING.md, step 3, for each line marked REQUIRED, MISSING')

