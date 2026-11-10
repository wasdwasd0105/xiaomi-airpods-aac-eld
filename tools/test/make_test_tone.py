#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) the developer
"""Writes a 60 s, 48 kHz stereo test melody that is easy to recognise by ear.

    python3 tools/test/make_test_tone.py /tmp/aaceld_test.wav
    adb push /tmp/aaceld_test.wav /sdcard/Music/aaceld_test.wav
    adb shell am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE \\
        -d file:///sdcard/Music/aaceld_test.wav
    adb shell content query --uri content://media/external/audio/media \\
        --projection _id:_display_name          # gives the id for run_test.sh
"""
import math, struct, sys, wave

RATE, SECONDS = 48000, 60
NOTES = [262, 330, 392, 523, 392, 330, 294, 349, 440, 587, 440, 349]

out = wave.open(sys.argv[1], "wb")
out.setnchannels(2); out.setsampwidth(2); out.setframerate(RATE)
buf = bytearray()
for n in range(RATE * SECONDS):
    t = n / RATE
    f = NOTES[int(t * 2) % len(NOTES)]          # two plucked notes a second
    phase = (t * 2) % 1.0
    env = min(1.0, phase * 40) * math.exp(-2.0 * phase)
    s = sum(a * math.sin(2 * math.pi * f * h * t)
            for h, a in ((1, 1.0), (2, 0.5), (3, 0.33), (4, 0.2))) * env * 0.42
    bass = 0.2 * math.sin(2 * math.pi * (NOTES[(int(t / 2) * 4) % len(NOTES)] / 4) * t)
    left = max(-1.0, min(1.0, s + bass)); right = max(-1.0, min(1.0, s * 0.9 + bass))
    buf += struct.pack("<hh", int(left * 31000), int(right * 31000))
out.writeframes(bytes(buf)); out.close()
