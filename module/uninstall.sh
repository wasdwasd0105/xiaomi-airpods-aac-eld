#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) the developer
# Drops every property this module sets or reads. The offload line is for
# installs that came from v0.4 or earlier, which switched A2DP offload off.
resetprop -p --delete persist.bluetooth.a2dp_offload.disabled
resetprop -p --delete persist.bluetooth.opus.enabled
resetprop -p --delete persist.bluetooth.aaceld.enabled
resetprop -p --delete persist.bluetooth.aaceld.bitrate
resetprop -p --delete persist.bluetooth.aaceld.vbr
resetprop -p --delete persist.bluetooth.aaceld.probe
for p in enabled dryrun bitrate bitratemode sbr sbr_ratio peak_bitrate afterburner \
         frames_per_packet vbr rtp_ms rtp_ssrc rtp_marker max_queue \
         max_packets_per_tick delay_report force rtp_ms_jitter_us pcm_stats \
         rtp_ms_after rtp_ms_lag rtp_seq_gaps recover_kick_ms priority session_start; do
  resetprop -p --delete persist.aaceld.$p
done
