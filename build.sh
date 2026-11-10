#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) the developer
#
# Builds the Zygisk module, the test harness and (if the Android SDK build
# tools and a JDK are there) the Settings add-on, and packs the flashable zip.
#
#   ./build.sh            everything, result: AAC-ELD-AirPods-<version>.zip
#   ./build.sh native     only build/libaaceld.so and build/aaceld_harness
#
# Needs: Android NDK (ANDROID_NDK, or found under the Android SDK), cmake,
# zip. Ninja is used when installed. For the add-on: SDK build-tools, a
# platform android.jar and a JDK 11 or newer; without them the zip is still
# built, without the add-on.
set -euo pipefail
cd "$(dirname "$0")"

say() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# --- find the NDK ------------------------------------------------------------
NDK="${ANDROID_NDK:-${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}}"
if [ -z "$NDK" ]; then
  for sdk in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" "$HOME/Library/Android/sdk" "$HOME/Android/Sdk"; do
    [ -n "$sdk" ] && [ -d "$sdk/ndk" ] || continue
    NDK="$(ls -d "$sdk"/ndk/* 2>/dev/null | sort | tail -n 1)"
    [ -n "$NDK" ] && break
  done
fi
[ -n "$NDK" ] && [ -f "$NDK/build/cmake/android.toolchain.cmake" ] \
  || die "Android NDK not found. Install it and set ANDROID_NDK to its folder."
command -v cmake >/dev/null || die "cmake not found."
say "NDK: $NDK"

# --- native: the module library and the harness ------------------------------
GEN=()
command -v ninja >/dev/null && GEN=(-GNinja)
cmake -S src -B build "${GEN[@]}" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build
STRIP="$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-strip | head -n 1)"
"$STRIP" -o build/libaaceld.stripped.so build/libaaceld.so
say "built build/libaaceld.so ($(wc -c < build/libaaceld.stripped.so | tr -d ' ') bytes stripped) and build/aaceld_harness"
[ "${1:-all}" = "native" ] && exit 0

# --- the Settings add-on (optional) ------------------------------------------
APK=""
if label/build.sh >build/label.log 2>&1; then
  APK=label/aaceld-label.apk
  say "built $APK"
else
  say "Settings add-on not built (see build/label.log); the zip will not carry it."
  say "The codec works without it; its settings are then system properties."
fi

# --- the zip -------------------------------------------------------------------
VERSION="$(grep '^version=' module/module.prop | cut -d= -f2)"
ZIP="AAC-ELD-AirPods-$VERSION.zip"
STAGE=build/zip
rm -rf "$STAGE" "$ZIP"
mkdir -p "$STAGE/zygisk"
cp -R module/META-INF module/customize.sh module/post-fs-data.sh module/uninstall.sh \
      module/module.prop module/README.md "$STAGE/"
cp build/libaaceld.stripped.so "$STAGE/zygisk/arm64-v8a.so"
[ -n "$APK" ] && cp "$APK" "$STAGE/aaceld-label.apk"
(cd "$STAGE" && zip -q -r -X "../../$ZIP" . -x '*.DS_Store')
say "packed $ZIP"
say "install:  adb push $ZIP /sdcard/Download/   then flash it in your root manager and reboot"
