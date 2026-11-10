#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) the developer
# Runs the module's start-up against a stack library copy on the phone.
#   tools/harness/run.sh <label> <path to libbluetooth_jni.so on this computer>
# The library is loaded in a scratch root process; Bluetooth itself is not
# touched. The start-up marker property is cleared afterwards whatever
# happens, so a crash here cannot keep the real module out.
set -u
LABEL="${1:?label}"; LIB="${2:?library}"
D=/data/local/tmp/aaceld_harness
adb shell "mkdir -p $D/$LABEL" && adb push "$LIB" "$D/$LABEL/libbluetooth_jni.so" >/dev/null \
  && adb push build/aaceld_harness "$D/aaceld_harness" >/dev/null && adb shell "chmod 755 $D/aaceld_harness" || exit 1
# The stack's own dependencies from the phone's Bluetooth APEX (its name
# differs between Android versions), then the system; libxml2 (HyperOS 3
# only) is in neither, so a VNDK copy last. If the harness prints "cannot
# load" with a library name, find that library on the phone and add its
# folder here.
LP=/apex/com.android.bt/lib64:/apex/com.android.btservices/lib64:/system/lib64:/apex/com.android.vndk.v34/lib64
adb shell -n "su -c 'logcat -c'" 2>/dev/null
adb shell -n "su -c 'LD_LIBRARY_PATH=$LP $D/aaceld_harness $D/$LABEL/libbluetooth_jni.so'" 2>&1
echo "exit $?"
adb shell -n "su -c 'resetprop -p --delete persist.bluetooth.aaceld.probe'" >/dev/null 2>&1
echo "--- module log"
adb shell "su -c 'logcat -d -s AacEldA2dp'" 2>/dev/null | grep -a -v "hooking \|beginning of" | sed -E 's/^[0-9-]+ [0-9:.]+ +[0-9]+ +[0-9]+ . AacEldA2dp: //' | cut -c1-200
