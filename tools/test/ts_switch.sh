#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) the developer
# Plays one file to the headset on sample-count RTP timestamps, changes the
# running stream to millisecond timestamps, and reports what the headset did
# about it. Optionally presses the volume keys afterwards, for listening to
# whether both buds follow.
#
# Usage: AACELD_MEDIA_ID=<id> tools/test/ts_switch.sh <label> [seconds before] [seconds after] [keys]
#   seconds before: on the sample count before the change (default 6; 0 starts
#                   in milliseconds straight away)
#   seconds after:  on milliseconds after it (default 10)
#   keys:           volume keys to press once the "seconds after" are over,
#                   five seconds apart, for example "down down up"
#
# Extra properties for the run go in AACELD_PROPS, for example
#   AACELD_PROPS="persist.aaceld.rtp_ms_lag=true"
# They are removed again at the end, and so is persist.aaceld.rtp_ms.
# AACELD_NO_SWITCH=1 leaves the stream on the sample count, as a control.
#
# Nothing is played unless music is routed to the headset, and playback is
# stopped at the end.
set -u
LABEL="${1:?label}"
BEFORE="${2:-6}"
AFTER="${3:-10}"
KEYS="${4:-}"
OUT="build/logs/tsswitch_${LABEL}.log"
mkdir -p build/logs
MEDIA_ID="${AACELD_MEDIA_ID:?set AACELD_MEDIA_ID to a MediaStore audio id, see tools/test/make_test_tone.py}"
PROP=persist.aaceld.rtp_ms

sh_root() { adb shell "su -c '$1'" 2>/dev/null | tr -d '\r'; }
stop_playback() { adb shell 'input keyevent 127; am force-stop com.miui.mediaviewer' >/dev/null 2>&1; }
cleanup() {
  pkill -f "logcat -v time" 2>/dev/null
  sh_root "resetprop -p --delete $PROP"
  for kv in ${AACELD_PROPS:-}; do sh_root "resetprop -p --delete ${kv%%=*}"; done
}

if ! adb shell 'dumpsys audio' 2>/dev/null | grep -A8 '^- STREAM_MUSIC' | grep -q 'Devices: bt_a2dp'; then
  echo "music is not routed to the headset; not playing anything"; exit 1
fi
# Never over the top of something the user is already listening to.
if adb shell dumpsys bluetooth_manager 2>/dev/null | grep -m1 'mIsPlaying' | grep -q true; then
  echo "something is already playing to the headset; not interrupting it"; exit 1
fi

for kv in ${AACELD_PROPS:-}; do sh_root "setprop ${kv%%=*} ${kv##*=}"; done
if [ "$BEFORE" = "0" ]; then sh_root "setprop $PROP true"; else sh_root "resetprop -p --delete $PROP"; fi
KEEP='AacEldA2dp|sendMediaKeyEvent: device=.*pushed=true|REMOTE_SUSPEND\(|BTA_AV_SUSPEND_EVT|BTA_AV_CLOSE_EVT|avdt.*disconnect|sendVolumeChanged: device|OnEnter: state=Started'
sh_root "logcat -c"
adb shell "su -c 'logcat -v time'" 2>/dev/null | grep --line-buffered -a -E "$KEEP" > "$OUT" &
trap cleanup EXIT
adb shell "am start -a android.intent.action.VIEW -d content://media/external/audio/media/${MEDIA_ID} -t audio/wav --grant-read-uri-permission" >/dev/null 2>&1
sleep 2
enq=$(sh_root 'dumpsys bluetooth_manager | grep "Counts (enqueue/dequeue/readbuf)"' | awk -F: '{print $2}' | awk '{print $1}')
if [ "${enq:-0}" = "0" ]; then
  stop_playback
  echo "=== $LABEL: A2DP stream did not start, playback stopped ==="; exit 2
fi

if [ -n "${AACELD_NO_SWITCH:-}" ]; then
  sleep "$BEFORE"
  echo "$(date +%H:%M:%S) staying on the sample count"
elif [ "$BEFORE" != "0" ]; then
  sleep "$BEFORE"
  sh_root "setprop $PROP true"
  echo "$(date +%H:%M:%S) changed to milliseconds"
fi
sleep "$AFTER"
for k in $KEYS; do
  case "$k" in
    up)   adb shell input keyevent KEYCODE_VOLUME_UP ;;
    down) adb shell input keyevent KEYCODE_VOLUME_DOWN ;;
  esac
  echo "$(date +%H:%M:%S) volume $k"
  sleep 5
done
playing=$(adb shell dumpsys bluetooth_manager 2>/dev/null | grep -m1 'mIsPlaying' | tr -d '\r ')
stop_playback
sleep 1

mod() { grep -a 'AacEldA2dp' "$OUT" | sed -E 's/^([0-9:. -]+) .*AacEldA2dp[^:]*: /\1 /' | cut -c7-; }
echo "=== $LABEL: ${BEFORE}s on samples, ${AFTER}s on milliseconds, keys: ${KEYS:-none}; at the end $playing ==="
mod | grep -E 'encoder init|rtp clock now|rtp step (1|2|3|12):|^.{13}stats' \
  | sed -E 's/, encode [0-9]+ us.*l2cap/, l2cap/'
echo "--- headset reaction"
grep -a -E 'OnEnter: state=Started|sendMediaKeyEvent: device=.*pushed=true|REMOTE_SUSPEND\(|BTA_AV_SUSPEND_EVT|BTA_AV_CLOSE_EVT|avdt.*disconnect|aacEncEncode failed|sendVolumeChanged' "$OUT" \
  | sed -E 's#system/[a-z_/]*/##' | awk '{ $1=""; $3=""; $4=""; print }' | cut -c1-150 | head -12
echo "(key=70 is the headset pressing pause)"
