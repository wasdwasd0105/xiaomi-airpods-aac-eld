#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) the developer
#
# Shows, live, the log lines that say what the module and the Bluetooth stack
# are doing about AAC-ELD: start-up, codec negotiation, the audio session, the
# first packets, and the audio service accepting or refusing the headset.
#
#   tools/watch.sh              print to the screen until Ctrl-C
#   tools/watch.sh file.log     also save to file.log
#
# Start it BEFORE the thing you want to see (a reboot, a connection): a busy
# phone overwrites its log within a minute or two, so lines not captured as
# they happen are gone. docs/TROUBLESHOOTING.md says what each line means.
set -u
KEEP='AacEldA2dp'
KEEP="$KEEP|OPUS codec|Opus priority|codec: Opus priority|priority: -1"
KEEP="$KEEP|cannot set source codec|cannot update peer codec|unsupported codec"
KEEP="$KEEP|Switching BluetoothAudio|Reconfig done|ignore ending session"
KEEP="$KEEP|: start_session|: end_session|StartSession|EndSession|startSession - |session has not started|is not ready"
KEEP="$KEEP|btif_a2dp_source_(start|restart|end)_session|btif_a2dp_source_setup_codec"
KEEP="$KEEP|OnEnter: state=|A2DP (sink )?device addr|APM failed|APM handleDeviceConfigChange"
KEEP="$KEEP|SetCodecUserConfig|setCodecConfigPreference|REMOTE_SUSPEND|sendMediaKeyEvent"
# Hide the Bluetooth addresses, so a saved log can be shared.
MASK='s/([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}/XX:XX:XX:XX:XX:XX/g; s/([0-9a-fx]{2}:){4}[0-9a-f]{2}:[0-9a-f]{2}/xx:xx:xx:xx:xx:xx/g'
# Line-buffered sed is -u on GNU sed and -l on the BSD sed of macOS.
if echo x | sed -u -E 's/x/y/' >/dev/null 2>&1; then UNBUF=-u; else UNBUF=-l; fi
if [ -n "${1:-}" ]; then
  adb logcat -v time | grep --line-buffered -a -E "$KEEP" | sed $UNBUF -E "$MASK" | tee "$1"
else
  adb logcat -v time | grep --line-buffered -a -E "$KEEP" | sed $UNBUF -E "$MASK"
fi
