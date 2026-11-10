// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) the developer
// Teaches the Bluetooth stack's vendor codec dispatchers about Apple's
// AAC-ELD codec, and routes the resulting stream to this module's encoder.
//
// Nothing new is registered in A2dpCodecs. Instead the codec slot that Opus
// already occupies is taken over: Opus is in the priority table but gated off
// by an unset property, and no AirPod speaks it, so the slot is free. That
// avoids having to build a C++ subclass and vtable at runtime.
#include "target.h"
#include "apple_eld.h"
#include "eld_encoder.h"
#include "log.h"

#include <string.h>
#include <atomic>
#include <stdio.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <shadowhook.h>

namespace {

// ---- originals ----------------------------------------------------------
bool  (*o_IsVendorSourceCodecValid)(const uint8_t*) = nullptr;
bool  (*o_IsVendorPeerSourceCodecValid)(const uint8_t*) = nullptr;
bool  (*o_IsVendorSinkCodecSupported)(const uint8_t*) = nullptr;
bool  (*o_VendorCodecTypeEquals)(const uint8_t*, const uint8_t*) = nullptr;
int   (*o_VendorSourceCodecIndex)(const uint8_t*) = nullptr;
int   (*o_VendorSinkCodecIndex)(const uint8_t*) = nullptr;
bool  (*o_VendorInitCodecConfig)(int, void*) = nullptr;
int   (*o_VendorGetBitRate)(const uint8_t*) = nullptr;
bool  (*o_VendorUsesRtpHeader)(bool, const uint8_t*) = nullptr;
bool  (*o_VendorBuildCodecHeader)(const uint8_t*, BT_HDR*, uint16_t) = nullptr;
const A2dpEncoderInterface* (*o_VendorGetEncoderInterface)(const uint8_t*) = nullptr;
const char* (*o_VendorCodecIndexStr)(int) = nullptr;
const char* (*o_VendorCodecName)(const uint8_t*) = nullptr;
const char* (*o_CodecIndexStr)(int) = nullptr;
const char* (*o_CodecName)(const uint8_t*) = nullptr;
bool  (*o_VendorAdjustCodec)(uint8_t*) = nullptr;
int   (*o_VendorGetSinkTrackChannelType)(const uint8_t*) = nullptr;
bool  (*o_GetPacketTimestamp)(const uint8_t*, const uint8_t*, uint32_t*) = nullptr;
// These two look alike and are not. setCodecConfig returns a tA2DP_STATUS,
// where zero means success: the stock Opus body ends `mov w0, w23` with w23
// zero on the good path and 0xc4, 0xc6, 0x29 on the bad ones. Returning a C++
// `true` there reads as status 1, an error, and the stack logs "cannot set
// source codec".
//
// setPeerCodecCapabilities is an ordinary bool, true for success: the stock
// body ends `tst w20,#0xff; cset w20, eq; mov w0, w20`, and its caller
// A2dpCodecs::setPeerSinkCodecCapabilities returns `w0 & 1`. Returning the
// status-style zero from it is what made the stack log "cannot update peer
// codec capabilities for AAC-ELD" and leave the codec out of the selectable
// list on every connection.
int   (*o_OpusSetCodecConfig)(void*, const uint8_t*, bool, uint8_t*) = nullptr;
bool  (*o_OpusSetPeerCodecCapabilities)(void*, const uint8_t*) = nullptr;
void  (*o_OpusSourceCtor)(void*, int) = nullptr;
bool  (*o_SourceSetupCodec)(const void*, void*) = nullptr;
void  (*o_SetLowLatencyCodec)(bool) = nullptr;
void  (*o_SwitchCodecCb)(bool) = nullptr;

constexpr int kCodecOk = 0;
constexpr int kCodecFail = 1;

// Set once this peer has been seen advertising Apple's AAC-ELD endpoint.
std::atomic<bool> g_peer_offers_eld{false};

// Nothing in this file treats Apple's codec as its own until start-up has
// finished: every function found, every hook in, the codec index known and
// the codec object's layout measured and checked. If any of that fails the
// hooks stay in place but pass everything through, so the stack behaves as if
// the module were not installed. g_probing lets the one measurement that
// needs the stack to accept Apple's element run before that.
std::atomic<bool> g_ready{false};
std::atomic<bool> g_probing{false};

inline bool ours(const uint8_t* ci) {
  return (g_ready.load(std::memory_order_relaxed) || g_probing.load(std::memory_order_relaxed)) &&
         eld_is_apple(ci);
}


// Returns an L2CAP data-write result, not a predicate: 0 failed, 1 success,
// 2 congested. Declaring this bool truncates 2 to 1, so the stack never sees
// back-pressure, keeps writing into a congested channel and the link drops.
// That is what disconnected the headset when this hook was first added.
uint8_t (*o_AvdtAdWriteReq)(uint8_t, void*, void*, BT_HDR*) = nullptr;
int g_logged_wire = 0;
// Only touched from the stack's main thread, in the write hook.
uint32_t g_seq_stream = 0xFFFFFFFFu;
uint32_t g_seq_sent = 0;
uint16_t g_seq_lost = 0;
uint16_t g_seq_carried = 0;
uint16_t g_last_rtp_seq = 0;

// avdt_ad_write_req's first argument: 0 signalling, 1 media, 2 reporting.
constexpr uint8_t kAvdtChanSig = 0;
constexpr uint8_t kAvdtChanMedia = 1;

// AVDTP service categories and the SET_CONFIGURATION signal identifier.
constexpr uint8_t kAvdtCatMediaCodec = 0x07;
constexpr uint8_t kAvdtCatDelayReport = 0x08;
constexpr uint8_t kAvdtSigSetConfig = 0x03;

// Drops the Delay Reporting category from an outgoing SET_CONFIGURATION that
// configures the Apple codec, when persist.aaceld.delay_report=false.
//
// This stack offers Delay Reporting on every source endpoint, so the AirPods
// answer with a DELAYREPORT as soon as the stream is opened. The USBPods
// firmware configures Media Transport and Media Codec only and never receives
// one. Tested both ways on AirPods 5 and it makes no difference to playback,
// so it is kept by default; the switch stays for comparing sessions.
void maybe_strip_delay_report(BT_HDR* buf) {
  if (buf == nullptr || buf->len < 6) return;
  auto* d = reinterpret_cast<uint8_t*>(buf + 1) + buf->offset;
  // Single packet, command, SET_CONFIGURATION.
  if ((d[0] & 0x0F) != 0x00 || (d[1] & 0x3F) != kAvdtSigSetConfig) return;

  bool eld = false;
  size_t delay_at = 0;
  size_t i = 4;   // header, signal, ACP SEID, INT SEID
  while (i + 2 <= buf->len) {
    const uint8_t cat = d[i];
    const size_t losc = d[i + 1];
    if (i + 2 + losc > buf->len) return;   // not a well-formed list; leave it alone
    // The element this module's helpers take starts at the LOSC byte.
    if (cat == kAvdtCatMediaCodec && ours(d + i + 1)) eld = true;
    if (cat == kAvdtCatDelayReport && losc == 0) delay_at = i;
    i += 2 + losc;
  }
  if (!eld || delay_at == 0 || delay_at + 2 != buf->len) return;
  if (g_stack.osi_property_get_bool("persist.aaceld.delay_report", true)) {
    LOGI("SET_CONFIGURATION for AAC-ELD keeps Delay Reporting");
    return;
  }
  buf->len = (uint16_t)(buf->len - 2);
  LOGI("SET_CONFIGURATION for AAC-ELD: Delay Reporting removed");
}

void* g_stubs[32] = {};
int   g_stub_count = 0;

// The bit rate written into SET_CONFIGURATION. It is the stream's ceiling, not
// what the encoder runs at: macOS always configures the 256 kbps the AirPods
// advertise and then sends less. Keeping it fixed means the bit-rate setting
// can change without renegotiating the stream.
constexpr uint32_t kNegotiatedBitrate = 256000;

uint32_t negotiated_bitrate(uint32_t peer_max) {
  uint32_t rate = kNegotiatedBitrate;
  if (peer_max != 0 && rate > peer_max) rate = peer_max;
  return rate;
}

// The switch on the headset's page in Settings. Read every time the stack
// considers the codec, so it takes effect at the next codec selection without
// restarting Bluetooth. It lives under persist.bluetooth. because that is a
// property the Settings app is allowed to write.
bool eld_enabled() {
  return g_stack.osi_property_get_bool("persist.bluetooth.aaceld.enabled", true);
}

// ---- replacements -------------------------------------------------------

bool h_IsVendorSourceCodecValid(const uint8_t* ci) {
  if (ours(ci)) { LOGI("hit: IsVendorSourceCodecValid -> true"); return true; }
  return o_IsVendorSourceCodecValid(ci);
}

bool h_IsVendorPeerSourceCodecValid(const uint8_t* ci) {
  if (ours(ci)) { LOGI("hit: IsVendorPeerSourceCodecValid -> true"); return true; }
  return o_IsVendorPeerSourceCodecValid(ci);
}

bool h_IsVendorSinkCodecSupported(const uint8_t* ci) {
  if (ours(ci)) { LOGI("hit: IsVendorSinkCodecSupported -> true"); return true; }
  return o_IsVendorSinkCodecSupported(ci);
}

bool h_VendorCodecTypeEquals(const uint8_t* a, const uint8_t* b) {
  bool a_eld = ours(a);
  bool b_eld = ours(b);
  if (a_eld || b_eld) return a_eld && b_eld;
  return o_VendorCodecTypeEquals(a, b);
}

int h_VendorSourceCodecIndex(const uint8_t* ci) {
  if (ours(ci)) {
    g_peer_offers_eld.store(true, std::memory_order_relaxed);
    return ELD_CODEC_INDEX;
  }
  return o_VendorSourceCodecIndex(ci);
}

int h_VendorSinkCodecIndex(const uint8_t* ci) {
  if (ours(ci)) { LOGI("hit: VendorSinkCodecIndex -> %d", ELD_CODEC_INDEX); return ELD_CODEC_INDEX; }
  return o_VendorSinkCodecIndex(ci);
}

// codec_info is the first member of AvdtpSepConfig, confirmed by
// A2DP_VendorInitCodecConfigLdac writing the element at offset 0.
bool h_VendorInitCodecConfig(int codec_index, void* cfg) {
  if (codec_index != ELD_CODEC_INDEX) return o_VendorInitCodecConfig(codec_index, cfg);
  LOGI("hit: VendorInitCodecConfig for our index");
  if (cfg == nullptr) return false;
  bool vbr = g_stack.osi_property_get_bool("persist.aaceld.vbr", true);
  eld_build_info(static_cast<uint8_t*>(cfg), negotiated_bitrate(0), vbr, ELD_CH_STEREO);
  return true;
}

int h_VendorGetBitRate(const uint8_t* ci) {
  if (ours(ci)) return (int)eld_peer_max_bitrate(ci);
  return o_VendorGetBitRate(ci);
}

// AAC-ELD rides in a standard RTP media packet.
bool h_VendorUsesRtpHeader(bool cp_enabled, const uint8_t* ci) {
  // Asked once per media packet, so nothing is logged here.
  if (ours(ci)) return true;
  return o_VendorUsesRtpHeader(cp_enabled, ci);
}

// There is no per packet codec header. The four byte Apple headers are per
// frame and the encoder writes them inside the payload itself.
bool h_VendorBuildCodecHeader(const uint8_t* ci, BT_HDR* buf, uint16_t frames) {
  if (ours(ci)) return true;
  return o_VendorBuildCodecHeader(ci, buf, frames);
}

const A2dpEncoderInterface* h_VendorGetEncoderInterface(const uint8_t* ci) {
  if (ours(ci)) { LOGI("hit: VendorGetEncoderInterface -> our encoder"); return eld_encoder_interface(); }
  return o_VendorGetEncoderInterface(ci);
}

const char* h_VendorCodecIndexStr(int codec_index) {
  if (codec_index == ELD_CODEC_INDEX) return "AAC-ELD";
  return o_VendorCodecIndexStr(codec_index);
}

const char* h_VendorCodecName(const uint8_t* ci) {
  if (ours(ci)) return "AAC-ELD";
  return o_VendorCodecName(ci);
}

// The generic versions, hooked only on a stack that has no vendor ones.
const char* h_CodecIndexStr(int codec_index) {
  if (g_ready.load(std::memory_order_relaxed) && codec_index == ELD_CODEC_INDEX) return "AAC-ELD";
  return o_CodecIndexStr(codec_index);
}

const char* h_CodecName(const uint8_t* ci) {
  if (ours(ci)) return "AAC-ELD";
  return o_CodecName(ci);
}

bool h_VendorAdjustCodec(uint8_t* ci) {
  if (ours(ci)) return true;
  return o_VendorAdjustCodec(ci);
}

int h_VendorGetSinkTrackChannelType(const uint8_t* ci) {
  // 3 is the stack's stereo track type, matching what AAC and SBC return.
  if (ours(ci)) return 3;
  return o_VendorGetSinkTrackChannelType(ci);
}

// The one hook that decides whether there is any audio at all.
//
// Every encoder leaves its RTP timestamp in the first four bytes of the
// packet's data area, and bta_av_co_audio_source_data_path reads it back
// through this function before handing the packet to AVDTP. The function is
// not a vendor dispatcher: it compares the codec ID against a fixed list (SBC,
// AAC, aptX, aptX HD, LDAC, Opus, aptX Adaptive) and returns false for
// everything else. On false the caller logs "unsupported codec type (255)",
// frees the packet and returns null.
//
// So before this hook existed the headset accepted SET_CONFIGURATION, OPEN and
// START and then received nothing whatsoever, which it answers with silence
// and, after a while, by dropping the link. Every development capture
// that has encoder output also has one "A2DP_GetPacketTimestamp: unsupported
// codec id 0x8001004cff" per packet, and none has a packet reaching
// avdt_ad_write_req.
bool h_GetPacketTimestamp(const uint8_t* ci, const uint8_t* data, uint32_t* timestamp) {
  if (!ours(ci)) return o_GetPacketTimestamp(ci, data, timestamp);
  if (data == nullptr || timestamp == nullptr) return false;
  memcpy(timestamp, data, sizeof(*timestamp));
  return true;
}

// ---- negotiation on the hijacked codec object ---------------------------

void write_cfg(uint8_t* cfg, int32_t priority, bool stereo) {
  *reinterpret_cast<int32_t*>(cfg + CFG_CODEC_TYPE) = ELD_CODEC_INDEX;
  *reinterpret_cast<int32_t*>(cfg + CFG_CODEC_PRIORITY) = priority;
  *reinterpret_cast<int32_t*>(cfg + CFG_SAMPLE_RATE) = BTAV_SAMPLE_RATE_48000;
  *reinterpret_cast<int32_t*>(cfg + CFG_BITS_PER_SAMPLE) = BTAV_BITS_PER_SAMPLE_16;
  *reinterpret_cast<int32_t*>(cfg + CFG_CHANNEL_MODE) =
      stereo ? BTAV_CHANNEL_MODE_STEREO : BTAV_CHANNEL_MODE_MONO;
  *reinterpret_cast<int64_t*>(cfg + CFG_SPECIFIC_1) = ELD_CODEC_SPECIFIC_MARK;
}

void copy_ota(uint8_t* dst, const uint8_t* src) {
  size_t len = (size_t)src[CI_LOSC] + 1;
  if (len > g_layout.ota_size) len = g_layout.ota_size;
  memset(dst, 0, g_layout.ota_size);
  memcpy(dst, src, len);
}

// Where the slot ranks. The stack tries a headset's codecs in order of
// priority and takes the first that fits, and the Bluetooth app hands each
// codec its priority when the stack starts. On the Android 16 build the Opus
// slot got 8001, above AAC's 2001, so AirPods took AAC-ELD. The Android 17
// build gives it 1501, below AAC, with no setting to change it, and AirPods
// connected on AAC every time. So the priority is set here, as the codec
// object is made: 8001 unless persist.aaceld.priority says otherwise, and 0
// there leaves the stack's own figure alone.
constexpr int kDefaultSlotPriority = 8001;

void h_OpusSourceCtor(void* self, int priority) {
  if (g_ready.load(std::memory_order_relaxed)) {
    const int wanted = g_stack.osi_property_get_int32("persist.aaceld.priority", kDefaultSlotPriority);
    if (wanted > 0 && wanted != priority) {
      LOGI("codec slot priority %d -> %d", priority, wanted);
      priority = wanted;
    }
  }
  o_OpusSourceCtor(self, priority);
}

// Some builds keep the audio session running across a codec change instead of
// ending it before and starting it after (HyperOS on Android 17 logs "Reconfig
// done - Ignore calling startSession(), just update codec to audio HAL"). That
// holds while both codecs are encoded by the DSP. A change between such a
// codec and this one, which the stack encodes, moves to the other kind of
// session: the setup below ends the old one and nobody starts the new one. The
// audio service then finds no session for the headset, drops it and plays
// through the phone speaker. So when the setup has changed the kind of
// session, the session is started here, as the stack itself does on a fresh
// connection; there the stack's own start follows and finds it already done.
//
// The setup returns whether it worked, and the caller acts on it: the body
// ends `and w0, w20, #1`, and on a connection btif_a2dp_source_start_session_
// delayed does `tbz w0, #0` past its own start_session() and past fulfilling
// the promise. v0.14 declared this hook void, so what the caller read was
// whatever the last call here had left in w0, which was the answer to "is the
// session DSP encoded?". With AAC-ELD that is no, so every connection made
// while the software session was already selected (any reconnection without
// restarting Bluetooth) was told the setup had failed, no session was started
// and the music stayed on the phone speaker. The result is passed back as it
// came.
bool h_SourceSetupCodec(const void* peer, void* promise) {
  const bool can = g_ready.load(std::memory_order_relaxed) &&
                   g_stack.a2dp_is_hal_offloading != nullptr && g_stack.a2dp_start_session != nullptr;
  const bool before = can && g_stack.a2dp_is_hal_offloading();
  const bool ok = o_SourceSetupCodec(peer, promise);
  if (!can || !ok) return ok;
  const bool after = g_stack.a2dp_is_hal_offloading();
  if (before == after) return ok;
  if (!g_stack.osi_property_get_bool("persist.aaceld.session_start", true)) {
    LOGI("audio session changed to %s encoding; not started here (persist.aaceld.session_start)",
         after ? "DSP" : "software");
    return ok;
  }
  LOGI("audio session changed to %s encoding: starting it", after ? "DSP" : "software");
  g_stack.a2dp_start_session();
  return ok;
}

// Android treats Opus as its low-latency codec and nothing else: each time the
// audio service starts a stream it says whether it wants low latency, and the
// stack answers by putting Opus at the highest priority or by disabling it.
// The Android 17 build does that inside the stack, so five seconds after
// AirPods connected on AAC-ELD the first stream start disabled the slot
// (priority -1) and they went back to AAC. Older builds pass the question to
// the Bluetooth app instead. The slot is not Opus any more, so neither answer
// applies: both entry points are swallowed.
void note_low_latency_request(bool low_latency, const char* where) {
  static std::atomic<int> last{-1};
  if (last.exchange(low_latency ? 1 : 0, std::memory_order_relaxed) != (low_latency ? 1 : 0)) {
    LOGI("%s: low latency %s; the slot is AAC-ELD, not Opus, so the codec stays as it is",
         where, low_latency ? "wanted" : "not wanted");
  }
}

void h_SetLowLatencyCodec(bool low_latency) {
  if (!g_ready.load(std::memory_order_relaxed)) { o_SetLowLatencyCodec(low_latency); return; }
  note_low_latency_request(low_latency, "stream start");
}

void h_SwitchCodecCb(bool low_latency) {
  if (!g_ready.load(std::memory_order_relaxed)) { o_SwitchCodecCb(low_latency); return; }
  note_low_latency_request(low_latency, "codec switch request");
}

int h_OpusSetCodecConfig(void* self, const uint8_t* peer, bool is_capability, uint8_t* result) {
  if (!ours(peer)) return o_OpusSetCodecConfig(self, peer, is_capability, result);
  if (self == nullptr || result == nullptr) return kCodecFail;

  if ((eld_rd_u16be(peer + CI_OBJECT_TYPE) & ELD_OBJ_AAC_ELD) == 0) {
    LOGW("peer AAC-ELD element does not advertise the ELD object type");
    return kCodecFail;
  }
  if ((eld_freq_bitmap(peer) & ELD_SF_48000) == 0) {
    LOGW("peer AAC-ELD element does not advertise 48 kHz");
    return kCodecFail;
  }
  uint8_t chans = eld_chan_bitmap(peer);
  if ((chans & (ELD_CH_STEREO | ELD_CH_MONO)) == 0) {
    LOGW("peer AAC-ELD element advertises no usable channel mode");
    return kCodecFail;
  }
  bool stereo = (chans & ELD_CH_STEREO) != 0;
  auto* obj = static_cast<uint8_t*>(self);

  // Switched off: refuse, and the stack moves on to the next codec in its
  // priority order, which for AirPods is AAC. The headset's offer is still
  // recorded as selectable, so Settings can tell that this headset has
  // AAC-ELD and show the switch that turns it back on.
  if (!eld_enabled()) {
    g_stack.recursive_mutex_lock(obj + g_layout.mutex);
    int32_t priority = *reinterpret_cast<int32_t*>(obj + g_layout.priority);
    write_cfg(obj + g_layout.selectable_capability, priority, stereo);
    copy_ota(obj + g_layout.ota_peer_capability, peer);
    g_stack.recursive_mutex_unlock(obj + g_layout.mutex);
    LOGI("setCodecConfig: AAC-ELD is switched off (persist.bluetooth.aaceld.enabled), declining");
    return kCodecFail;
  }

  uint32_t rate = negotiated_bitrate(eld_peer_max_bitrate(peer));
  // The headset advertises VBR as supported, and macOS's own SET_CONFIGURATION
  // carries the bit set, so this matches the one configuration known to be
  // accepted whatever rate mode the encoder then runs.
  bool vbr = g_stack.osi_property_get_bool("persist.aaceld.vbr", true);
  eld_build_info(result, rate, vbr, stereo ? ELD_CH_STEREO : ELD_CH_MONO);

  g_stack.recursive_mutex_lock(obj + g_layout.mutex);
  int32_t priority = *reinterpret_cast<int32_t*>(obj + g_layout.priority);
  write_cfg(obj + g_layout.config, priority, stereo);
  write_cfg(obj + g_layout.local_capability, priority, stereo);
  write_cfg(obj + g_layout.selectable_capability, priority, stereo);
  copy_ota(obj + g_layout.ota_config, result);
  copy_ota(obj + g_layout.ota_peer_capability, peer);
  if (!is_capability) copy_ota(obj + g_layout.ota_peer_config, peer);
  g_stack.recursive_mutex_unlock(obj + g_layout.mutex);

  LOGI("hit: setCodecConfig(is_capability=%d) accepted AAC-ELD: 48 kHz %s, %u bps, VBR %s",
       (int)is_capability, stereo ? "stereo" : "mono", rate, vbr ? "on" : "off");
  return kCodecOk;
}

bool h_OpusSetPeerCodecCapabilities(void* self, const uint8_t* peer) {
  if (!ours(peer)) return o_OpusSetPeerCodecCapabilities(self, peer);
  LOGI("hit: setPeerCodecCapabilities for AAC-ELD");
  g_peer_offers_eld.store(true, std::memory_order_relaxed);
  if (self == nullptr) return false;

  uint8_t chans = eld_chan_bitmap(peer);
  bool stereo = (chans & ELD_CH_STEREO) != 0;

  auto* obj = static_cast<uint8_t*>(self);
  g_stack.recursive_mutex_lock(obj + g_layout.mutex);
  int32_t priority = *reinterpret_cast<int32_t*>(obj + g_layout.priority);
  write_cfg(obj + g_layout.selectable_capability, priority, stereo);
  copy_ota(obj + g_layout.ota_peer_capability, peer);
  g_stack.recursive_mutex_unlock(obj + g_layout.mutex);
  return true;
}

// The last point at which the finished on-air packet can be seen and touched:
// AVDTP has built the RTP header and is handing the packet to L2CAP.
//
// Two jobs. The first is the marker bit. bta_av_start_ok logs "use rtp header
// marker bit" for this stream, because the stack asks the codec object and the
// object is the hijacked Opus one, so every packet would go out with it set.
// macOS never sets it on this codec, and the working USBPods firmware always
// sends zero; that firmware once used the bit to flag a discontinuity, which
// is the meaning a receiver gives it. It is cleared here.
//
// The second is the only honest log of what was transmitted, since media
// payloads do not appear in this device's HCI snoop.
uint8_t h_AvdtAdWriteReq(uint8_t type, void* ccb, void* scb, BT_HDR* buf) {
  bool ours = false;
  if (type == kAvdtChanSig) maybe_strip_delay_report(buf);
  if (type == kAvdtChanMedia && buf != nullptr && buf->len >= 20 && eld_stream_active()) {
    auto* d = reinterpret_cast<uint8_t*>(buf + 1) + buf->offset;
    // Only packets that are provably this module's: a plain RTP version 2
    // header followed by the Apple per-frame header's two marker nibbles.
    if (d[0] == 0x80 && (d[12] & 0xF0) == 0xB0 && (d[14] & 0xF0) == 0x10) {
      ours = true;
      const unsigned marker_in = (unsigned)((d[1] >> 7) & 1);
      if (!eld_keep_rtp_marker()) d[1] = (uint8_t)(d[1] & 0x7F);
      // The SSRC. This stack derives it from the codec element, media type in
      // the high half and codec type in the low byte, which for a vendor
      // codec comes out as 0x000000FF. macOS sends zero on every packet,
      // 3732 out of 3732 on the wire, and the USBPods firmware patches its
      // stack to do the same.
      const unsigned ssrc_in = ((unsigned)d[8] << 24) | ((unsigned)d[9] << 16) |
                               ((unsigned)d[10] << 8) | d[11];
      if (!eld_keep_rtp_ssrc()) d[8] = d[9] = d[10] = d[11] = 0;

      uint16_t seq = (uint16_t)((d[2] << 8) | d[3]);
      // The stack's number plus the packets it has thrown away, so that they
      // show as missing in the RTP sequence as well. See g_seq_gaps.
      const uint32_t ts = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) | ((uint32_t)d[6] << 8) | d[7];
      uint32_t stream = 0, made = 0;
      bool new_stream = false;
      if (eld_packet_count_for(ts, &stream, &made)) {
        if (stream != g_seq_stream) {
          g_seq_stream = stream;
          g_seq_carried = (uint16_t)(g_seq_carried + g_seq_lost);   // never backwards
          g_seq_sent = 0;
          g_seq_lost = 0;
          new_stream = true;
        }
        g_seq_sent++;
        if (made > g_seq_sent) g_seq_lost = (uint16_t)(made - g_seq_sent);
        seq = (uint16_t)(seq + g_seq_carried + g_seq_lost);
        d[2] = (uint8_t)(seq >> 8);
        d[3] = (uint8_t)(seq & 0xFF);
      }
      // The first packets of a stream are logged, and so are the first after
      // link trouble.
      if (new_stream || g_last_rtp_seq == 0 || eld_take_wire_rearm()) g_logged_wire = 0;
      g_last_rtp_seq = seq == 0 ? 1 : seq;
      if (g_logged_wire < 6) {
        g_logged_wire++;
        LOGI("wire %d: len=%u pt=%u marker=%u->%u seq=%u ts=%u ssrc=%08x->%02x%02x%02x%02x "
             "hdr=%02x %02x %02x %02x au=%02x %02x %02x %02x",
             g_logged_wire, buf->len, (unsigned)(d[1] & 0x7f), marker_in,
             (unsigned)((d[1] >> 7) & 1), (unsigned)seq,
             ((unsigned)d[4] << 24) | ((unsigned)d[5] << 16) | ((unsigned)d[6] << 8) | d[7],
             ssrc_in, d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15],
             d[16], d[17], d[18], d[19]);
      }
    }
  }
  uint8_t result = o_AvdtAdWriteReq(type, ccb, scb, buf);
  if (ours) eld_note_write_result(result);
  return result;
}

// ---- installation -------------------------------------------------------

// Reads the first instruction word at each target and logs it, without
// patching anything. Comparing these against the library on disk proves the
// offsets land where intended inside this process before any code is written.
void dry_run_report(uintptr_t vaddr, const char* name) {
  if (vaddr == 0) { LOGI("  %-42s not in this build", name); return; }
  auto* p = reinterpret_cast<const uint32_t*>(target_addr(vaddr));
  LOGI("  %-42s vaddr 0x%07lx  addr %p  first words %08x %08x",
       name, (unsigned long)vaddr, (const void*)p, p[0], p[1]);
}

bool install(uintptr_t vaddr, void* replacement, void** original, const char* name) {
  // Logged before the attempt, so that if the engine faults while patching it
  // is clear from the log which target it was working on.
  LOGI("hooking %s at %p", name, target_addr(vaddr));
  void* stub = shadowhook_hook_func_addr(target_addr(vaddr), replacement, original);
  if (stub == nullptr) {
    int err = shadowhook_get_errno();
    LOGE("hook %s at 0x%lx failed: %s", name, (unsigned long)vaddr, shadowhook_to_errmsg(err));
    return false;
  }
  if (g_stub_count < (int)(sizeof(g_stubs) / sizeof(g_stubs[0]))) g_stubs[g_stub_count++] = stub;
  return true;
}

}  // namespace

#define INSTALL(field, fn) \
  ok &= install(kOffsets.field, (void*)h_##fn, (void**)&o_##fn, #field)

// For the functions a build may not have: hooked if present, otherwise left.
#define INSTALL_IF_PRESENT(field, fn)                                            \
  do {                                                                           \
    if (kOffsets.field != 0) {                                                   \
      ok &= install(kOffsets.field, (void*)h_##fn, (void**)&o_##fn, #field);     \
    } else {                                                                     \
      LOGI("%s is not in this build, not hooked", #field);                       \
    }                                                                            \
  } while (0)

#define REPORT(field) dry_run_report(kOffsets.field, #field)

// Mirrors what the hook engine's trampoline allocator does, from inside this
// very process: an anonymous read-write-execute mapping, then a full-length
// write into it. The engine faults doing exactly this, so running it here with
// logging says whether such memory is usable in the Bluetooth process at all.
void probe_rwx(size_t len) {
  void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    LOGE("  probe: mmap %zu RWX failed, errno %d", len, errno);
    return;
  }
  LOGI("  probe: mmap %zu RWX -> %p", len, p);

  // Report how the kernel actually mapped it.
  FILE* f = fopen("/proc/self/maps", "re");
  if (f != nullptr) {
    char line[512];
    auto want = reinterpret_cast<uintptr_t>(p);
    while (fgets(line, sizeof(line), f) != nullptr) {
      uintptr_t lo = 0, hi = 0;
      if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && want >= lo && want < hi) {
        LOGI("  probe: maps says %s", line);
        break;
      }
    }
    fclose(f);
  }

  // Write in the same shape the allocator does: whole region, then one slot.
  memset(p, 0, len);
  LOGI("  probe: memset of %zu bytes OK", len);

  // The allocator names the mapping with prctl after handing out its first
  // slot. That is the one operation between a successful whole-page write and
  // the slot write that faults, so it is reproduced here.
  int pr = prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, p, len, "shadowhook-probe");
  LOGI("  probe: prctl(PR_SET_VMA_ANON_NAME) returned %d errno %d", pr, pr == 0 ? 0 : errno);

  FILE* f2 = fopen("/proc/self/maps", "re");
  if (f2 != nullptr) {
    char line[512];
    auto want = reinterpret_cast<uintptr_t>(p);
    while (fgets(line, sizeof(line), f2) != nullptr) {
      uintptr_t lo = 0, hi = 0;
      if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && want >= lo && want < hi) {
        LOGI("  probe: after prctl maps says %s", line);
        break;
      }
    }
    fclose(f2);
  }

  memset(static_cast<char*>(p) + 0x100, 0, 256);
  LOGI("  probe: memset at +0x100 OK");
  memset(static_cast<char*>(p) + 0x200, 0, 256);
  LOGI("  probe: memset at +0x200 OK");
  munmap(p, len);
}

bool hooks_dry_run() {
  LOGI("dry run: library base %p", (void*)g_stack.base);
  probe_rwx(4096);
  probe_rwx(16384);
  REPORT(A2DP_IsVendorSourceCodecValid);
  REPORT(A2DP_IsVendorPeerSourceCodecValid);
  REPORT(A2DP_IsVendorSinkCodecSupported);
  REPORT(A2DP_VendorCodecTypeEquals);
  REPORT(A2DP_VendorSourceCodecIndex);
  REPORT(A2DP_VendorSinkCodecIndex);
  REPORT(A2DP_VendorInitCodecConfig);
  REPORT(A2DP_VendorGetBitRate);
  REPORT(A2DP_VendorUsesRtpHeader);
  REPORT(A2DP_VendorBuildCodecHeader);
  REPORT(A2DP_VendorGetEncoderInterface);
  REPORT(A2DP_VendorCodecIndexStr);
  REPORT(A2DP_VendorCodecName);
  REPORT(A2DP_VendorAdjustCodec);
  REPORT(A2DP_VendorGetSinkTrackChannelType);
  REPORT(A2DP_GetPacketTimestamp);
  REPORT(avdt_ad_write_req);
  REPORT(OpusBase_setCodecConfig);
  REPORT(OpusBase_setPeerCodecCapabilities);
  REPORT(aacEncOpen);
  REPORT(aacEncoder_SetParam);
  REPORT(osi_malloc);
  LOGI("dry run complete, nothing was patched");
  return true;
}

bool hooks_install() {
  // Where codec_config_ and codec_user_config_ are, before anything is
  // patched: if this build's codec object is not the class this module
  // knows, it is left completely alone.
  if (!target_measure_getters()) {
    LOGE("the codec object could not be measured; nothing was hooked");
    return false;
  }

  bool ok = true;
  INSTALL(A2DP_IsVendorSourceCodecValid, IsVendorSourceCodecValid);
  INSTALL(A2DP_IsVendorPeerSourceCodecValid, IsVendorPeerSourceCodecValid);
  INSTALL(A2DP_IsVendorSinkCodecSupported, IsVendorSinkCodecSupported);
  INSTALL(A2DP_VendorCodecTypeEquals, VendorCodecTypeEquals);
  INSTALL(A2DP_VendorSourceCodecIndex, VendorSourceCodecIndex);
  INSTALL(A2DP_VendorSinkCodecIndex, VendorSinkCodecIndex);
  INSTALL(A2DP_VendorInitCodecConfig, VendorInitCodecConfig);
  INSTALL_IF_PRESENT(A2DP_VendorGetBitRate, VendorGetBitRate);
  INSTALL(A2DP_VendorUsesRtpHeader, VendorUsesRtpHeader);
  INSTALL(A2DP_VendorBuildCodecHeader, VendorBuildCodecHeader);
  INSTALL(A2DP_VendorGetEncoderInterface, VendorGetEncoderInterface);
  if (kOffsets.A2DP_VendorCodecIndexStr != 0) {
    INSTALL(A2DP_VendorCodecIndexStr, VendorCodecIndexStr);
  } else {
    INSTALL_IF_PRESENT(A2DP_CodecIndexStr, CodecIndexStr);
  }
  if (kOffsets.A2DP_VendorCodecName != 0) {
    INSTALL(A2DP_VendorCodecName, VendorCodecName);
  } else {
    INSTALL_IF_PRESENT(A2DP_CodecName, CodecName);
  }
  INSTALL_IF_PRESENT(A2DP_VendorAdjustCodec, VendorAdjustCodec);
  INSTALL(A2DP_VendorGetSinkTrackChannelType, VendorGetSinkTrackChannelType);
  INSTALL(A2DP_GetPacketTimestamp, GetPacketTimestamp);

  ok &= install(kOffsets.OpusBase_setCodecConfig, (void*)h_OpusSetCodecConfig,
                (void**)&o_OpusSetCodecConfig, "A2dpCodecConfigOpusBase::setCodecConfig");
  ok &= install(kOffsets.OpusBase_setPeerCodecCapabilities, (void*)h_OpusSetPeerCodecCapabilities,
                (void**)&o_OpusSetPeerCodecCapabilities,
                "A2dpCodecConfigOpusBase::setPeerCodecCapabilities");
  ok &= install(kOffsets.avdt_ad_write_req, (void*)h_AvdtAdWriteReq,
                (void**)&o_AvdtAdWriteReq, "avdt_ad_write_req");
  if (kOffsets.OpusSource_ctor != 0) {
    ok &= install(kOffsets.OpusSource_ctor, (void*)h_OpusSourceCtor,
                  (void**)&o_OpusSourceCtor, "A2dpCodecConfigOpusSource constructor");
  } else {
    LOGI("the Opus codec constructor is not in this build; the slot keeps the stack's priority");
  }
  if (kOffsets.btif_a2dp_source_setup_codec != 0 && g_stack.a2dp_start_session != nullptr) {
    ok &= install(kOffsets.btif_a2dp_source_setup_codec, (void*)h_SourceSetupCodec,
                  (void**)&o_SourceSetupCodec, "btif_a2dp_source_setup_codec");
  }
  if (kOffsets.btif_av_source_set_low_latency_codec != 0) {
    ok &= install(kOffsets.btif_av_source_set_low_latency_codec, (void*)h_SetLowLatencyCodec,
                  (void**)&o_SetLowLatencyCodec, "btif_av_source_set_low_latency_codec");
  }
  if (kOffsets.invoke_switch_codec_cb != 0) {
    ok &= install(kOffsets.invoke_switch_codec_cb, (void*)h_SwitchCodecCb,
                  (void**)&o_SwitchCodecCb, "invoke_switch_codec_cb");
  }

  LOGI("installed %d hooks, %s", g_stub_count, ok ? "all succeeded" : "some failed");
  if (!ok) {
    LOGE("not every hook went in; the ones that did pass everything through");
    return false;
  }

  // Which codec index is Opus's: ask the stack, with Opus's own vendor codec
  // element (Google's vendor ID 0x00E0, codec ID 0x0001).
  static const uint8_t kOpusInfo[16] = {9, 0x00, 0xFF, 0xE0, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};
  const int index = (int)o_VendorSourceCodecIndex(kOpusInfo);
  if (index <= 0 || index >= 32) {
    LOGE("the stack does not know an Opus source codec (index %d); hooks pass everything through", index);
    return false;
  }
  g_eld_codec_index = index;

  g_probing.store(true, std::memory_order_relaxed);
  const bool measured = target_measure_ota();
  g_probing.store(false, std::memory_order_relaxed);
  if (!measured) {
    LOGE("the codec object could not be measured; hooks pass everything through");
    return false;
  }

  g_ready.store(true, std::memory_order_relaxed);
  LOGI("ready: AAC-ELD takes the Opus slot, codec index %d", g_eld_codec_index);
  return true;
}
