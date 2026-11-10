#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) the developer
#
# Copies the Bluetooth stack library off the connected phone, for
# tools/check_lib.py and the other tools. Nothing on the phone is changed.
#
#   tools/pull_lib.sh [folder]      default folder: work/
#
# Writes <folder>/libbluetooth_jni.so and <folder>/phone.txt (what phone and
# build it came from). The library is the phone maker's file: keep it out of
# git (work/ is in .gitignore) and do not redistribute it.
set -eu
OUT="${1:-work}"
mkdir -p "$OUT"

adb get-state >/dev/null 2>&1 || { echo "no phone on adb; connect one and allow USB debugging" >&2; exit 1; }

# Where the running Bluetooth process loaded it from, if root is there to ask;
# otherwise the usual places, newest Android first.
path="$(adb shell "su -c 'grep -m1 -o \"/[^ ]*libbluetooth_jni.so\" /proc/\$(pidof com.android.bluetooth)/maps'" 2>/dev/null | tr -d '\r' | head -n 1)"
if [ -z "$path" ]; then
  for p in /apex/com.android.bt/lib64/libbluetooth_jni.so \
           /apex/com.android.btservices/lib64/libbluetooth_jni.so \
           /apex/com.android.bluetooth/lib64/libbluetooth_jni.so \
           /system/lib64/libbluetooth_jni.so; do
    if adb shell "[ -f $p ] && echo yes" 2>/dev/null | grep -q yes; then path="$p"; break; fi
  done
fi
[ -n "$path" ] || { echo "libbluetooth_jni.so not found on the phone. Is Bluetooth switched on? See docs/PORTING.md, step 1." >&2; exit 1; }

adb pull "$path" "$OUT/libbluetooth_jni.so" >/dev/null
# Some Bluetooth properties are only readable as root.
prop() {
  v="$(adb shell "su -c 'getprop $1'" 2>/dev/null | tr -d '\r')"
  [ -n "$v" ] || v="$(adb shell getprop "$1" | tr -d '\r')"
  echo "${v:-unset}"
}
{
  echo "library:     $path"
  echo "fingerprint: $(adb shell getprop ro.build.fingerprint | tr -d '\r')"
  echo "android:     $(adb shell getprop ro.build.version.release | tr -d '\r') (SDK $(adb shell getprop ro.build.version.sdk | tr -d '\r'))"
  echo "platform:    $(adb shell getprop ro.board.platform | tr -d '\r')"
  echo "abi:         $(adb shell getprop ro.product.cpu.abi | tr -d '\r')"
  echo "offload:     supported=$(prop ro.bluetooth.a2dp_offload.supported) disabled=$(prop persist.bluetooth.a2dp_offload.disabled) dsp codecs=$(prop persist.bluetooth.a2dp_offload.cap)"
} > "$OUT/phone.txt"
cat "$OUT/phone.txt"
echo "saved $OUT/libbluetooth_jni.so ($(wc -c < "$OUT/libbluetooth_jni.so" | tr -d ' ') bytes)"
echo "next:  tools/check_lib.py $OUT/libbluetooth_jni.so"
