// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) the developer
#pragma once
#include "target.h"

// The eight slot encoder interface this module hands to the stack when the
// negotiated codec is Apple AAC-ELD.
const A2dpEncoderInterface* eld_encoder_interface();

// True while this module's encoder owns the software A2DP stream. Read from
// the stack's main thread by the packet-write hook.
bool eld_stream_active();

// macOS never sets the RTP marker bit on this codec. The stack sets it on
// every packet, because the hijacked codec object asks for it, so the write
// hook clears it unless this returns true.
bool eld_keep_rtp_marker();

// macOS also sends an all-zero RTP SSRC, where this stack would send
// 0x000000FF for a vendor codec. The write hook zeroes it unless this is true.
bool eld_keep_rtp_ssrc();

// Result of one L2CAP media write, for the periodic statistics line.
void eld_note_write_result(uint8_t result);

// True once after link trouble has ended: the write hook then logs the next
// few packets as they go on the air.
bool eld_take_wire_rearm();

// For the packet with this RTP timestamp: which stream it belongs to (a
// number that changes at every stream start) and how many packets this module
// had made in that stream up to and including it. False if the packet is not
// known or the feature is off.
bool eld_packet_count_for(uint32_t timestamp, uint32_t* stream, uint32_t* count);
