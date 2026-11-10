#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) the developer
# The codec slot being taken over is the one Opus occupies, which the stack
# leaves disabled unless this is set.
resetprop persist.bluetooth.opus.enabled true

# A2DP hardware offload is left alone. The stack chooses per codec: anything
# the DSP cannot encode, which includes this one, gets the stack's software
# path on its own ("Switching BluetoothAudio HAL to Software"), and AAC, SBC,
# LDAC and the rest stay on Xiaomi's normal offload path. Up to v0.4 this
# script switched offload off for everything, which was never needed and
# broke plain AAC: with offload off this phone's audio HAL cannot route a
# standard codec to the headset at all.

# Everything else has a working default built in. The two settings on the
# headset's page in Bluetooth settings are persist.bluetooth.aaceld.enabled
# and persist.bluetooth.aaceld.bitrate; persist.aaceld.* properties only need
# setting to change something. See README.md.
