#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) the developer
#
# Builds label/aaceld-label.apk without Gradle: javac, d8, aapt2, apksigner.
#
# Needs the Android SDK (ANDROID_HOME, or the default location) with a
# build-tools version and a platform installed, and a JDK 11 or newer.
# BUILD_TOOLS and PLATFORM pick versions, for example
#   BUILD_TOOLS=36.1.0 PLATFORM=android-36 label/build.sh
# otherwise the newest installed ones are used.
set -euo pipefail
cd "$(dirname "$0")"

SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
if [ -z "$SDK" ]; then
  for d in "$HOME/Library/Android/sdk" "$HOME/Android/Sdk"; do [ -d "$d" ] && SDK="$d" && break; done
fi
[ -n "$SDK" ] && [ -d "$SDK" ] || { echo "Android SDK not found; set ANDROID_HOME" >&2; exit 1; }
BT="$SDK/build-tools/${BUILD_TOOLS:-$(ls "$SDK/build-tools" | sort -V | tail -n 1)}"
JAR="$SDK/platforms/${PLATFORM:-$(ls "$SDK/platforms" | sort -V | tail -n 1)}/android.jar"
[ -x "$BT/d8" ] || { echo "no d8 in $BT" >&2; exit 1; }
[ -f "$JAR" ] || { echo "no android.jar at $JAR" >&2; exit 1; }
# Homebrew's JDK is not on the PATH by default on macOS.
if [ -z "${JAVA_HOME:-}" ] && [ -d /opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home ]; then
  export JAVA_HOME=/opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home
fi
[ -n "${JAVA_HOME:-}" ] && PATH="$JAVA_HOME/bin:$PATH"
command -v javac >/dev/null || { echo "javac not found; install a JDK" >&2; exit 1; }

rm -rf build && mkdir -p build/stubs build/classes build/stage/assets
javac --release 11 -Xlint:-options -d build/stubs $(find stubs -name '*.java')
javac --release 11 -Xlint:-options -cp "$JAR:build/stubs" -d build/classes $(find src -name '*.java')
# Only the add-on's own classes go in the dex; the Xposed API comes from the framework.
"$BT/d8" --min-api 26 --lib "$JAR" --classpath build/stubs --output build/stage $(find build/classes -name '*.class')
# The app's name and description, in English and Chinese.
"$BT/aapt2" compile --dir res -o build/res.zip
"$BT/aapt2" link -o build/unsigned.apk --manifest AndroidManifest.xml -I "$JAR" build/res.zip
cp assets/xposed_init build/stage/assets/
(cd build/stage && zip -q -r ../unsigned.apk classes.dex assets)
"$BT/zipalign" -f 4 build/unsigned.apk build/aligned.apk
# A self-signed key, made on the first build and kept (it is in .gitignore) so
# that later builds can update the installed app. An APK signed with another
# key, such as a released one, cannot be installed over it: uninstall
# io.aaceld.label first.
[ -f signing.jks ] || keytool -genkeypair -keystore signing.jks -storepass android -keypass android \
    -alias label -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=aaceld-label" >/dev/null 2>&1
"$BT/apksigner" sign --ks signing.jks --ks-pass pass:android --key-pass pass:android \
    --out aaceld-label.apk build/aligned.apk
rm -f aaceld-label.apk.idsig
"$BT/apksigner" verify aaceld-label.apk && ls -la aaceld-label.apk
