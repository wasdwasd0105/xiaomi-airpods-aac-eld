#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) the developer
# Runs inside the root manager's installer (KernelSU, Magisk or APatch) when
# the zip is flashed. It checks what it can, does the two bits of set-up that
# live outside the module where the tools for them exist, and says what is
# left to do by hand.
#
# Nothing here decides whether the module will work on this phone's Bluetooth
# stack. The module finds that out itself at start-up and stays out of a
# build it does not fit, so installing it on another build is harmless.

LABEL_PKG=io.aaceld.label
KNOWN_FP="Xiaomi/popsicle/popsicle:16/BP2A.250605.031.A3/OS3.0.319.0.WPBCNXM:user/release-keys"
ACTUAL_FP="$(getprop ro.build.fingerprint)"
TODO=""
todo() { TODO="$TODO
  - $1"; }

ui_print "- AAC-ELD for AirPods $(grep '^version=' "$MODPATH/module.prop" | cut -d= -f2)"

[ "$ARCH" = "arm64" ] || abort "! Only arm64 phones are supported."

if [ "$KNOWN_FP" != "$ACTUAL_FP" ]; then
  ui_print "- This is not the build the module was developed on"
  ui_print "  ($ACTUAL_FP)."
  ui_print "  It looks everything up by name and checks it before use."
fi

if [ -z "$(find /apex /system/lib64 -name libbluetooth_jni.so 2>/dev/null | head -n 1)" ]; then
  ui_print "! No libbluetooth_jni.so found. The module will do nothing here."
fi

# --- Zygisk -----------------------------------------------------------------
ZYGISK=no
if [ "$KSU" = "true" ] || [ "$APATCH" = "true" ]; then
  for d in /data/adb/modules/zygisksu /data/adb/modules/rezygisk /data/adb/modules/zygisk_next \
           /data/adb/modules/neozygisk; do
    [ -d "$d" ] && [ ! -f "$d/disable" ] && ZYGISK=yes
  done
else
  [ "$(magisk --sqlite "SELECT value FROM settings WHERE key='zygisk'" 2>/dev/null)" = "value=1" ] && ZYGISK=yes
  for d in /data/adb/modules/zygisksu /data/adb/modules/rezygisk; do
    [ -d "$d" ] && [ ! -f "$d/disable" ] && ZYGISK=yes
  done
fi
if [ "$ZYGISK" = "yes" ]; then
  ui_print "- Zygisk: found"
else
  ui_print "! Zygisk was not found. This is a Zygisk module."
  todo "Install or enable Zygisk (NeoZygisk, ReZygisk or Zygisk Next on KernelSU and APatch; the Zygisk switch in Magisk)."
fi

# --- KernelSU: let modules stay in the Bluetooth process ---------------------
# KernelSU unmounts modules for apps without root, and Zygisk then treats the
# app as on the deny list and stays out of it. The Bluetooth app is where this
# module lives, so its app profile gets "Umount modules" switched off.
if [ "$KSU" = "true" ]; then
  KSUD=/data/adb/ksud
  PROFILE="$TMPDIR/aaceld_bt_profile.json"
  if [ -x "$KSUD" ] && "$KSUD" profile get com.android.bluetooth > "$PROFILE" 2>/dev/null \
      && grep -q '"non_root"' "$PROFILE"; then
    if grep -q '"umount_modules": *false' "$PROFILE" && grep -q '"use_default": *false' "$PROFILE"; then
      ui_print "- KernelSU: modules already stay mounted for Bluetooth"
    else
      sed -i -e 's/"use_default": *true/"use_default": false/' \
             -e 's/"umount_modules": *true/"umount_modules": false/' \
             -e 's/"stored": *false/"stored": true/' "$PROFILE"
      if "$KSUD" profile set "$PROFILE" >/dev/null 2>&1 \
          && "$KSUD" profile get com.android.bluetooth 2>/dev/null | grep -q '"umount_modules": *false'; then
        ui_print "- KernelSU: switched \"Umount modules\" off for Bluetooth"
      else
        todo "KernelSU manager > Superuser > show system apps > Bluetooth: switch \"Umount modules\" off."
      fi
    fi
  else
    todo "KernelSU manager > Superuser > show system apps > Bluetooth: switch \"Umount modules\" off."
  fi
elif [ "$APATCH" != "true" ]; then
  todo "Magisk: make sure Bluetooth is not on the DenyList."
fi

# --- the Settings add-on ------------------------------------------------------
# An Xposed module: the codec badge, and the AAC-ELD switch, variable bit rate
# switch and bit-rate slider on the headset's page. The stack module works
# without it; the two settings are then properties, see README.md.
APK="$MODPATH/aaceld-label.apk"
if [ -f "$APK" ]; then
  if pm install -r -S "$(stat -c %s "$APK")" < "$APK" >/dev/null 2>&1; then
    ui_print "- Settings add-on installed ($LABEL_PKG)"
    CLI=/data/adb/modules/zygisk_vector/cli
    if [ -x "$CLI" ] && "$CLI" modules enable "$LABEL_PKG" >/dev/null 2>&1 \
        && "$CLI" scope set "$LABEL_PKG" com.android.settings/0 >/dev/null 2>&1; then
      ui_print "- Settings add-on enabled in Vector for the Settings app"
      am force-stop com.android.settings >/dev/null 2>&1
    else
      todo "In your Xposed manager (LSPosed or Vector), enable \"AAC-ELD in Bluetooth settings\" for the Settings app, then force-stop Settings. Without an Xposed framework the codec still works; only the rows on the headset's page are missing."
    fi
  else
    ui_print "! Could not install the Settings add-on."
    todo "Install $APK by hand, then enable it for the Settings app in your Xposed manager."
  fi
fi

ui_print " "
ui_print "- Next:"
ui_print "  - Reboot."
if [ -n "$TODO" ]; then
  echo "$TODO" | while IFS= read -r line; do [ -n "$line" ] && ui_print "$line"; done
fi
ui_print "  - Connect AirPods. Their badge in Bluetooth settings reads AAC-ELD,"
ui_print "    and their page has the AAC-ELD switch and bit-rate slider."
ui_print "  - To see what the module decided:  adb logcat -s AacEldA2dp"
