#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) the developer
# Plays one file to the headset and steps the AAC-ELD bit-rate setting while
# it plays, the way the drop-down in Settings does. Prints, per step, the
# average bit rate on the air and what the encoder cost in CPU time, plus
# anything the headset did about it.
#
# Usage: AACELD_MEDIA_ID=<id> tools/test/rate_sweep.sh <label> [seconds per step] [rate ...]
#   rate: the maximum bit rate setting in bit/s; 0 is the default, 256000.
#   Default steps: 0 256000 192000 128000
# The variable bit rate switch is left as it is; set it first with
#   adb shell su -c 'setprop persist.bluetooth.aaceld.vbr false'   (constant)
# to sweep constant rates, which is what the figures in module/README.md are.
#
# Extra encoder properties for the whole run go in AACELD_PROPS, for example
#   AACELD_PROPS="persist.aaceld.sbr=1" ... to measure variable rate with SBR.
# They are removed again at the end.
#
# Nothing is played unless music is routed to the headset, and playback is
# stopped at the end.
set -u
LABEL="${1:?label}"; shift
SECS=12
if [[ "${1:-}" =~ ^[0-9]+$ ]] && [ "${1}" -lt 1000 ]; then SECS="$1"; shift; fi
RATES=("$@"); [ ${#RATES[@]} -eq 0 ] && RATES=(0 256000 192000 128000)
OUT="build/logs/sweep_${LABEL}.log"
mkdir -p build/logs
MEDIA_ID="${AACELD_MEDIA_ID:?set AACELD_MEDIA_ID to a MediaStore audio id, see tools/test/make_test_tone.py}"
PROP=persist.bluetooth.aaceld.bitrate

sh_root() { adb shell "su -c '$1'" 2>/dev/null | tr -d '\r'; }
stop_playback() { adb shell 'input keyevent 127; am force-stop com.miui.mediaviewer' >/dev/null 2>&1; }

if ! adb shell 'dumpsys audio' 2>/dev/null | grep -A8 '^- STREAM_MUSIC' | grep -q 'Devices: bt_a2dp'; then
  echo "music is not routed to the headset; not playing anything"; exit 1
fi
# Never over the top of something the user is already listening to.
if adb shell dumpsys bluetooth_manager 2>/dev/null | grep -m1 'mIsPlaying' | grep -q true; then
  echo "something is already playing to the headset; not interrupting it"; exit 1
fi

BEFORE=$(sh_root "getprop $PROP")
for kv in ${AACELD_PROPS:-}; do sh_root "setprop ${kv%%=*} ${kv##*=}"; done
sh_root "setprop $PROP ${RATES[0]}"
# The phone's log buffer does not hold a whole run, so the lines that matter
# are filtered out of a live stream instead of read back at the end.
KEEP='AacEldA2dp|sendMediaKeyEvent: device=.*pushed=true|REMOTE_SUSPEND\(|BTA_AV_SUSPEND_EVT|BTA_AV_CLOSE_EVT|avdt.*disconnect'
sh_root "logcat -c"
adb shell "su -c 'logcat -v time'" 2>/dev/null | grep --line-buffered -a -E "$KEEP" > "$OUT" &
LOGPID=$!
trap 'kill $LOGPID 2>/dev/null' EXIT
adb shell "am start -a android.intent.action.VIEW -d content://media/external/audio/media/${MEDIA_ID} -t audio/wav --grant-read-uri-permission" >/dev/null 2>&1
sleep 2
enq=$(sh_root 'dumpsys bluetooth_manager | grep "Counts (enqueue/dequeue/readbuf)"' | awk -F: '{print $2}' | awk '{print $1}')
if [ "${enq:-0}" = "0" ]; then
  stop_playback
  echo "=== $LABEL: A2DP stream did not start, playback stopped ==="; exit 2
fi

sleep "$SECS"
for rate in "${RATES[@]:1}"; do
  sh_root "setprop $PROP $rate"
  sleep "$SECS"
done
# Putting the setting back also makes the encoder log the last step.
sh_root "setprop $PROP ${BEFORE:-0}"
if [ "${BEFORE:-0}" = "${RATES[${#RATES[@]}-1]}" ]; then sh_root "setprop $PROP 1"; sleep 2; sh_root "setprop $PROP ${BEFORE:-0}"; fi
sleep 2
stop_playback
sleep 1
kill $LOGPID 2>/dev/null
for kv in ${AACELD_PROPS:-}; do sh_root "resetprop -p --delete ${kv%%=*}"; done
[ -z "$BEFORE" ] && sh_root "resetprop -p --delete $PROP"

mod() { grep -a 'AacEldA2dp' "$OUT" | sed -E 's/^.*AacEldA2dp[^:]*: //'; }
echo "=== $LABEL: steps ${RATES[*]}, ${SECS}s each ==="
mod | grep -E 'encoder up|bit rate setting changed|^stats' \
  | sed -E 's/, queue max.*l2cap/, l2cap/; s/ in [0-9]+ ms \(([0-9.]+) pkt\/s, / (\1 pkt\/s, /'
echo "--- headset reaction"
grep -a -E 'sendMediaKeyEvent: device=.*pushed=true|REMOTE_SUSPEND\(|BTA_AV_SUSPEND_EVT|BTA_AV_CLOSE_EVT|avdt.*disconnect|aacEncEncode failed' "$OUT" \
  | sed -E 's#system/[a-z_/]*/##' | awk '{ $1=""; $3=""; $4=""; print }' | cut -c1-150 | head -6
echo "(nothing above means no pause, suspend or disconnect)"
