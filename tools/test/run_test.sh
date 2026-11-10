#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) the developer
# One AAC-ELD test cycle: set encoder properties, restart Bluetooth so they
# are picked up, wait for the headset, play a test file for a few seconds and
# report what went on the air and how the headset reacted.
#
# Usage: AACELD_MEDIA_ID=<id> tools/test/run_test.sh <label> [seconds] [prop=value ...]
#
# Playback is stopped again at the end, and straight away if the A2DP stream
# does not start, so a failed route never leaves the phone speaker playing.
set -u
LABEL="${1:?label}"; shift
SECS=10
if [[ "${1:-}" =~ ^[0-9]+$ ]]; then SECS="$1"; shift; fi
OUT="build/logs/test_${LABEL}.log"
mkdir -p build/logs
# A MediaStore audio id to play; tools/test/make_test_tone.py says how to get one.
MEDIA_ID="${AACELD_MEDIA_ID:?set AACELD_MEDIA_ID to a MediaStore audio id, see tools/test/make_test_tone.py}"

sh_root() { adb shell "su -c '$1'" 2>/dev/null | tr -d '\r'; }
stop_playback() { adb shell 'input keyevent 127; am force-stop com.miui.mediaviewer' >/dev/null 2>&1; }

for kv in "$@"; do sh_root "setprop ${kv%%=*} ${kv##*=}"; done

if [ "${AACELD_NO_RESTART:-0}" != "1" ]; then
  sh_root "cmd bluetooth_manager disable; cmd bluetooth_manager wait-for-state:STATE_OFF; cmd bluetooth_manager enable; cmd bluetooth_manager wait-for-state:STATE_ON" >/dev/null
fi

connected=0
for _ in $(seq 1 20); do
  n=$(sh_root 'dumpsys bluetooth_manager | grep -c "mConnectionState: STATE_CONNECTED"')
  if [ "${n:-0}" != "0" ]; then connected=1; break; fi
  sleep 2
done
if [ "$connected" != "1" ]; then echo "headset did not connect"; exit 1; fi
# Never start playback until music is really routed to the headset, or the
# test tone comes out of the phone speaker instead.
routed=0
for _ in $(seq 1 15); do
  if adb shell 'dumpsys audio' 2>/dev/null | grep -A8 '^- STREAM_MUSIC' | grep -q 'Devices: bt_a2dp'; then routed=1; break; fi
  sleep 1
done
if [ "$routed" != "1" ]; then echo "music is not routed to the headset; not playing anything"; exit 1; fi
sleep 1

sh_root "logcat -c"
adb shell "am start -a android.intent.action.VIEW -d content://media/external/audio/media/${MEDIA_ID} -t audio/wav --grant-read-uri-permission" >/dev/null 2>&1

sleep 2
enq=$(sh_root 'dumpsys bluetooth_manager | grep "Counts (enqueue/dequeue/readbuf)"' | awk -F: '{print $2}' | awk '{print $1}')
if [ "${enq:-0}" = "0" ]; then
  stop_playback
  echo "=== $LABEL: A2DP stream did not start, playback stopped (audio was not going to the headset) ==="
  exit 2
fi

sleep "$SECS"
adb shell "su -c 'logcat -d'" > "$OUT" 2>/dev/null
stop_playback

mod() { grep -a 'AacEldA2dp' "$OUT" | sed 's/^.*AacEldA2dp: //'; }
echo "=== $LABEL ==="
echo "--- codec"
sh_root 'dumpsys bluetooth_manager' | grep -o -E 'mCodecConfig:\{codecName:[^,]*,mCodecType:[0-9]+,mCodecPriority:[0-9]+,mSampleRate:[^,]*' | tail -1
echo "--- encoder"
mod | grep -E 'encoder up' | tail -1
echo "--- on the air"
mod | grep -E '^wire ' | head -6
mod | grep -E '^stats' | tail -2
echo "--- headset reaction"
start=$(grep -a 'OnEnter: state=Started' "$OUT" | head -1 | awk '{print $2}')
echo "stream started at ${start:-never}"
grep -a -E 'sendMediaKeyEvent: device=.*pushed=true|REMOTE_SUSPEND\(|BTA_AV_SUSPEND_EVT|BTA_AV_CLOSE_EVT|avdt.*disconnect' "$OUT" \
  | sed -E 's#system/[a-z_/]*/##' | awk '{ $1=""; $3=""; $4=""; print }' | cut -c1-150 | head -6
echo "--- stack drops"
echo "unsupported-codec drops: $(grep -a -c 'A2DP_GetPacketTimestamp: unsupported' "$OUT")"
