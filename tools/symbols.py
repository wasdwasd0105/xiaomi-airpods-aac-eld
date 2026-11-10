#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) the developer
"""Writes every function name a libbluetooth_jni.so carries to two text
files, so they can be searched with grep and used by tools/a64.py.

usage: tools/symbols.py path/to/libbluetooth_jni.so

Writes next to the library:
  <lib>.mangled.txt     address, type, mangled name   (what src/target.cpp uses)
  <lib>.demangled.txt   address, type, readable name  (what a64.py prints)

The names come from the library's dynamic symbols and from its mini debug
symbols, the .gnu_debugdata section, which is an xz-compressed ELF with a
symbol table. If the library has no such section the files hold only the
few exported names, and the module cannot work on that library by name; see
docs/PORTING.md, "No symbols at all".

Needs llvm-objcopy and llvm-nm from the Android NDK, and xz."""
import glob, os, subprocess, sys, tempfile

def ndk_tools():
    ndk = os.environ.get('ANDROID_NDK') or os.environ.get('ANDROID_NDK_HOME') or os.environ.get('ANDROID_NDK_ROOT')
    if not ndk:
        sdk = os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT')
        found = sorted(sum((glob.glob(os.path.join(os.path.expanduser(d), 'ndk', '*'))
                            for d in ([sdk] if sdk else []) + ['~/Library/Android/sdk', '~/Android/Sdk']), []))
        if not found:
            sys.exit('No Android NDK found. Install one and set ANDROID_NDK to its folder.')
        ndk = found[-1]
    return glob.glob(os.path.join(ndk, 'toolchains/llvm/prebuilt/*/bin'))[0]

def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    lib = sys.argv[1]
    tools = ndk_tools()
    nm = os.path.join(tools, 'llvm-nm')
    rows = {False: set(), True: set()}
    with tempfile.TemporaryDirectory() as tmp:
        xz = os.path.join(tmp, 'mini.xz'); elf = os.path.join(tmp, 'mini.elf')
        subprocess.run([os.path.join(tools, 'llvm-objcopy'), '--dump-section', '.gnu_debugdata=' + xz,
                        lib, os.path.join(tmp, 'copy')], check=False, stderr=subprocess.DEVNULL)
        sources = [[nm, '-D', '--defined-only', lib]]
        if os.path.exists(xz) and os.path.getsize(xz):
            open(elf, 'wb').write(subprocess.run(['xz', '-dc', xz], capture_output=True, check=True).stdout)
            sources.append([nm, '--defined-only', elf])
            print('mini debug symbols: present')
        else:
            print('mini debug symbols: NONE (.gnu_debugdata is missing)')
        for cmd in sources:
            for demangle in (False, True):
                out = subprocess.run(cmd + (['-C'] if demangle else []), capture_output=True, text=True).stdout
                rows[demangle] |= {l.rstrip() for l in out.splitlines() if l.strip()}
    for demangle, suffix in ((False, '.mangled.txt'), (True, '.demangled.txt')):
        path = lib + suffix
        open(path, 'w').write('\n'.join(sorted(rows[demangle])) + '\n')
        print('%6d names -> %s' % (len(rows[demangle]), path))

main()
