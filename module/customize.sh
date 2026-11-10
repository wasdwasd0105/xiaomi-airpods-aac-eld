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

# --- language ---------------------------------------------------------------
# Messages are in Chinese when the phone's language is Chinese, otherwise in
# English. The language the user picked is the first entry of the
# system_locales setting; the two properties are what is left where that
# cannot be read (they say what the phone shipped with).
LOCALE="$(settings get system system_locales 2>/dev/null)"
case "$LOCALE" in ""|null) LOCALE="$(getprop persist.sys.locale)" ;; esac
[ -n "$LOCALE" ] || LOCALE="$(getprop ro.product.locale)"
case "$LOCALE" in zh*) ZH=1 ;; *) ZH=0 ;; esac
# t "English text" "中文文本"
t() { if [ "$ZH" = 1 ]; then printf '%s\n' "$2"; else printf '%s\n' "$1"; fi; }

ui_print "- AAC-ELD for AirPods $(grep '^version=' "$MODPATH/module.prop" | cut -d= -f2)"

[ "$ARCH" = "arm64" ] || abort "$(t "! Only arm64 phones are supported." "! 仅支持 arm64 手机。")"

if [ "$KNOWN_FP" != "$ACTUAL_FP" ]; then
  ui_print "$(t "- This is not the build the module was developed on" "- 当前系统不是开发本模块时所用的版本")"
  ui_print "  ($ACTUAL_FP)."
  ui_print "$(t "  It looks everything up by name and checks it before use." "  模块会按名称查找所需函数，检查通过后才会启用。")"
fi

if [ -z "$(find /apex /system/lib64 -name libbluetooth_jni.so 2>/dev/null | head -n 1)" ]; then
  ui_print "$(t "! No libbluetooth_jni.so found. The module will do nothing here." "! 未找到 libbluetooth_jni.so，模块在此系统上不会生效。")"
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
  ui_print "$(t "- Zygisk: found" "- Zygisk：已找到")"
else
  ui_print "$(t "! Zygisk was not found. This is a Zygisk module." "! 未找到 Zygisk。本模块需要 Zygisk 才能工作。")"
  todo "$(t "Install or enable Zygisk (NeoZygisk, ReZygisk or Zygisk Next on KernelSU and APatch; the Zygisk switch in Magisk)." \
            "安装或启用 Zygisk（KernelSU 和 APatch 可用 NeoZygisk、ReZygisk 或 Zygisk Next；Magisk 请打开 Zygisk 开关）。")"
fi

# --- KernelSU: let modules stay in the Bluetooth process ---------------------
# KernelSU unmounts modules for apps without root, and Zygisk then treats the
# app as on the deny list and stays out of it. The Bluetooth app is where this
# module lives, so its app profile gets "Umount modules" switched off.
UMOUNT_TODO="$(t "KernelSU manager > Superuser > show system apps > Bluetooth: switch \"Umount modules\" off." \
                 "KernelSU 管理器 > 超级用户 > 显示系统应用 > 蓝牙：关闭“卸载模块”。")"
if [ "$KSU" = "true" ]; then
  KSUD=/data/adb/ksud
  PROFILE="$TMPDIR/aaceld_bt_profile.json"
  if [ -x "$KSUD" ] && "$KSUD" profile get com.android.bluetooth > "$PROFILE" 2>/dev/null \
      && grep -q '"non_root"' "$PROFILE"; then
    if grep -q '"umount_modules": *false' "$PROFILE" && grep -q '"use_default": *false' "$PROFILE"; then
      ui_print "$(t "- KernelSU: modules already stay mounted for Bluetooth" "- KernelSU：蓝牙应用已保留模块挂载")"
    else
      sed -i -e 's/"use_default": *true/"use_default": false/' \
             -e 's/"umount_modules": *true/"umount_modules": false/' \
             -e 's/"stored": *false/"stored": true/' "$PROFILE"
      if "$KSUD" profile set "$PROFILE" >/dev/null 2>&1 \
          && "$KSUD" profile get com.android.bluetooth 2>/dev/null | grep -q '"umount_modules": *false'; then
        ui_print "$(t "- KernelSU: switched \"Umount modules\" off for Bluetooth" "- KernelSU：已为蓝牙应用关闭“卸载模块”")"
      else
        todo "$UMOUNT_TODO"
      fi
    fi
  else
    todo "$UMOUNT_TODO"
  fi
elif [ "$APATCH" != "true" ]; then
  todo "$(t "Magisk: make sure Bluetooth is not on the DenyList." "Magisk：确认蓝牙应用不在排除列表（DenyList）中。")"
fi

# --- the Settings add-on ------------------------------------------------------
# An Xposed module: the codec badge, and the AAC-ELD switch, variable bit rate
# switch and bit-rate slider on the headset's page. The stack module works
# without it; the two settings are then properties, see README.md.
APK="$MODPATH/aaceld-label.apk"
if [ -f "$APK" ]; then
  if pm install -r -S "$(stat -c %s "$APK")" < "$APK" >/dev/null 2>&1; then
    ui_print "$(t "- Settings add-on installed ($LABEL_PKG)" "- 已安装设置扩展（$LABEL_PKG）")"
    CLI=/data/adb/modules/zygisk_vector/cli
    if [ -x "$CLI" ] && "$CLI" modules enable "$LABEL_PKG" >/dev/null 2>&1 \
        && "$CLI" scope set "$LABEL_PKG" com.android.settings/0 >/dev/null 2>&1; then
      ui_print "$(t "- Settings add-on enabled in Vector for the Settings app" "- 已在 Vector 中为“设置”启用该扩展")"
      am force-stop com.android.settings >/dev/null 2>&1
    else
      todo "$(t "In your Xposed manager (LSPosed or Vector), enable \"AAC-ELD in Bluetooth settings\" for the Settings app, then force-stop Settings. Without an Xposed framework the codec still works; only the rows on the headset's page are missing." \
                "在 Xposed 管理器（LSPosed 或 Vector）中为“设置”启用“AAC-ELD 蓝牙设置扩展”，然后强行停止“设置”。没有 Xposed 框架时编码照常工作，只是耳机页面上没有相关选项。")"
    fi
  else
    ui_print "$(t "! Could not install the Settings add-on." "! 设置扩展安装失败。")"
    todo "$(t "Install $APK by hand, then enable it for the Settings app in your Xposed manager." \
              "请手动安装 $APK，然后在 Xposed 管理器中为“设置”启用它。")"
  fi
fi

ui_print " "
ui_print "$(t "- Next:" "- 接下来：")"
ui_print "$(t "  - Reboot." "  - 重启手机。")"
if [ -n "$TODO" ]; then
  echo "$TODO" | while IFS= read -r line; do [ -n "$line" ] && ui_print "$line"; done
fi
ui_print "$(t "  - Connect AirPods. Their badge in Bluetooth settings reads AAC-ELD," "  - 连接 AirPods。蓝牙设置中它的编码标识会显示为 AAC-ELD，")"
ui_print "$(t "    and their page has the AAC-ELD switch and bit-rate slider." "    耳机页面中会出现 AAC-ELD 开关和码率滑块。")"
ui_print "$(t "  - To see what the module decided:  adb logcat -s AacEldA2dp" "  - 查看模块的运行日志：adb logcat -s AacEldA2dp")"
