// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) the developer
// AAC-ELD encoder for A2DP, driven by the Bluetooth stack's own software
// source path and using the FDK-AAC encoder already linked into the stack.
//
// The wire format is a standard 12 byte RTP header, then frames, each one a
// 4 byte Apple header followed by a raw AAC-ELD access unit. Frames are 480
// samples, exactly 10 ms at 48 kHz, and macOS always sends three per packet.
#include "eld_encoder.h"
#include "apple_eld.h"
#include "log.h"

#include <string.h>
#include <math.h>
#include <time.h>
#include <atomic>
#include <pthread.h>
#include <mutex>

namespace {

// Tunables. All are read when a stream starts, so changing one needs a pause
// and resume, not a rebuild or a reboot.
constexpr const char* kPropBitrate   = "persist.aaceld.bitrate";
constexpr const char* kPropFrames    = "persist.aaceld.frames_per_packet";
constexpr const char* kPropRateMode  = "persist.aaceld.bitratemode";
constexpr const char* kPropSbr       = "persist.aaceld.sbr";
constexpr const char* kPropSbrRatio  = "persist.aaceld.sbr_ratio";
constexpr const char* kPropPeak      = "persist.aaceld.peak_bitrate";
constexpr const char* kPropAfterburn = "persist.aaceld.afterburner";
constexpr const char* kPropRtpMs     = "persist.aaceld.rtp_ms";
constexpr const char* kPropMaxPkt    = "persist.aaceld.max_packets_per_tick";
constexpr const char* kPropMaxQueue  = "persist.aaceld.max_queue";
constexpr const char* kPropSeqGaps   = "persist.aaceld.rtp_seq_gaps";
constexpr const char* kPropKickMs    = "persist.aaceld.recover_kick_ms";
constexpr const char* kPropMarker    = "persist.aaceld.rtp_marker";
constexpr const char* kPropSsrc      = "persist.aaceld.rtp_ssrc";

// The two bit-rate settings on the headset's page in Settings. They live
// under persist.bluetooth. because that is a property the Settings app is
// allowed to write.
//
//   persist.bluetooth.aaceld.bitrate  the maximum bit rate in bits per
//       second, the slider. Unset or 0 is 256000.
//   persist.bluetooth.aaceld.vbr      the "Variable bit rate" switch, on
//       unless set to false. On: variable rate, no frame larger than the
//       maximum bit rate allows and never larger than 256 kbps allows,
//       which is the most the headset advertises and what macOS keeps to.
//       Off: constant bit rate at the maximum.
//
// Unset, the two give variable rate capped at 256 kbps, which is what the
// single "Automatic" setting of v0.5 to v0.8 was (bitrate 0).
constexpr const char* kPropUserBitrate = "persist.bluetooth.aaceld.bitrate";
constexpr const char* kPropUserVbr     = "persist.bluetooth.aaceld.vbr";
constexpr int kUserBitrateMin = 64000;
// 256000 is the most the headset advertises and what SET_CONFIGURATION says.
// AirPods 5 play constant 288000 and 320000 all the same; three frames of
// that size no longer fit one packet, see set_packet_shape.
constexpr int kUserBitrateMax = 320000;
constexpr int kUserBitrateDefault = 256000;
// Variable rate never asks the headset for a frame above this.
constexpr int kVbrCeiling = 256000;
// The setting is looked at again this often while a stream is running, so a
// change on the Settings page is heard within a second.
constexpr int kUserPollTicks = 33;

// Diagnostic, off by default: once a second, log the level of each channel of
// the PCM the stack hands over and how the two channels line up in time. It
// answers "is the stereo image already wrong before the encoder" without any
// capture. persist.aaceld.pcm_stats=true turns it on at the next stream start.
constexpr const char* kPropPcmStats = "persist.aaceld.pcm_stats";

// Millisecond RTP timestamps are taken from the real clock at the moment a
// packet is sent, as macOS and the USBPods firmware do. This stack sends so
// regularly that those come out as exactly +30 every packet, where macOS's
// steps wander 29 to 33 and USBPods' 21 to 35. Each stamp is therefore taken
// from the clock plus a small random offset below this many microseconds,
// which gives steps of 27 to 33 with the long-run rate still exactly real
// time. 0 turns it off.
constexpr const char* kPropRtpJitter = "persist.aaceld.rtp_ms_jitter_us";
constexpr int kDefaultRtpJitterUs = 3000;

// Every stream starts on the sample count and changes to millisecond stamps
// this long after its first packet. The stamps carry on from the last one in
// the new unit, so the headset sees a change of step and never a jump. See
// the note at rtp_ms_clock for why it is done this way round.
//   persist.aaceld.rtp_ms_after  the delay in milliseconds. 0 never changes.
//   persist.aaceld.rtp_ms        looked at once a second; flipping it changes
//                                the unit of the running stream. With
//                                rtp_ms_after at 0, true also starts every
//                                stream on milliseconds, which AirPods reject.
//   persist.aaceld.rtp_ms_lag    stamp each packet with the time the previous
//                                one was sent, which is what the USBPods
//                                firmware does (its first two stamps are 0).
constexpr const char* kPropRtpMsAfter = "persist.aaceld.rtp_ms_after";
constexpr const char* kPropRtpMsLag   = "persist.aaceld.rtp_ms_lag";
constexpr int kDefaultRtpMsAfter = 6000;
constexpr int kPcmStatsFrames = 100;

constexpr uint32_t kDefaultBitrate      = 256000;  // what AirPods advertise as the max
constexpr int      kDefaultFramesPerPkt = 3;       // 3 x 10 ms, a 30 ms packet
constexpr int      kMaxFramesPerPkt     = 6;

// Defaults: VBR mode 5 without SBR, every frame capped at the 256 kbps the
// headset advertises, which is 320 bytes per 10 ms frame.
//
// That is the shape of what macOS sends. Its access units start 0x85, 0x86 or
// 0x84, which in the ELD channel pair syntax is max_sfb 33 followed by the
// two bit ms_mask_present: a full-band core without SBR. This FDK with these
// settings starts its frames 0x85 and 0x86 too; with LD-SBR switched on it
// band-limits the core and they start 0x82 instead. macOS's frames are 4 to
// 320 bytes, never more, which is the peak cap.
//
// Both forms play on AirPods 5 from this phone. SBR at ratio 1
// (persist.aaceld.sbr=1) is what the USBPods firmware ships; it only rides
// VBR, see open_encoder.
constexpr int      kDefaultRateMode = 5;
constexpr int      kDefaultSbr      = 0;
constexpr int      kDefaultSbrRatio = 1;
constexpr uint32_t kDefaultPeak     = 256000;

// At most this many packets may be produced by one tick, so a late tick can
// catch up without turning a stall into a burst.
constexpr int kDefaultMaxPacketsPerTick = 2;

// What happens when the link cannot take the audio, out of range for
// instance. 0, the default: nothing special. The encoder keeps running at
// real time and the stack does what it does for every codec, which is to let
// its transmit queue fill and then throw it away, over and over. What is
// lost is lost, the Apple sequence numbers and the timestamps both show the
// hole, and nothing older than about a second is ever waiting to go out.
// That is what macOS does (its controller discards anything older than
// 200 ms and the encoder never pauses), and the AirPods conceal such gaps
// and carry on the moment the link is back.
//
// A positive value brings back what v0.2 to v0.11 did: with that many
// packets waiting, stop encoding and drop the PCM instead. It kept the
// sequence numbers consecutive, which was the idea, and it was wrong: the
// packets already queued sat there for the whole outage and were delivered
// when the link came back, however old, followed by frames whose timestamps
// had jumped by the length of the outage with no frame missing. The AirPods
// went silent on that for a minute or more.
constexpr int kDefaultMaxQueue = 0;
// While holding back, the queue length goes stale, because the stack only
// reports it when a packet is enqueued. After this many held ticks one packet
// is let through to get a fresh reading.
constexpr int kProbeTicks = 3;

// Apple's negotiated limit on the RTP payload for this codec, from the
// USBPods firmware. Three frames at the 256 kbps peak are 972 bytes.
constexpr size_t kApplePayloadMax = 986;

// One 480 sample frame at 48 kHz is exactly 10 ms.
constexpr int64_t kFrameUs = 10000;
// A tick that has been away longer than this is treated as a restart rather
// than a debt to repay.
constexpr int64_t kMaxCatchUpUs = 120000;

constexpr int      kLogFrames   = 6;
constexpr int      kLogPackets  = 4;
constexpr uint32_t kStatsPackets = 1000;   // one statistics line per ~30 s

uint64_t now_us() {
  struct timespec ts = {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

uint64_t thread_cpu_ns() {
  struct timespec ts = {};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

struct Stats {
  uint32_t packets = 0;
  uint32_t frames = 0;
  uint64_t au_bytes = 0;
  uint32_t au_max = 0;
  uint32_t short_reads = 0;      // ticks that ran out of PCM
  uint32_t skipped_frames = 0;   // PCM dropped under back-pressure
  uint32_t enc_errors = 0;
  uint32_t oversize = 0;
  size_t   queue_max = 0;
  uint64_t since_us = 0;
  uint64_t enc_cpu_ns = 0;       // thread CPU time spent inside aacEncEncode
};

// Channel statistics over one window of input PCM. corr[k] is the sum of
// L[n] * R[n + k - 2]: the largest one says which channel leads, by how many
// samples.
struct PcmStats {
  double sum_l2 = 0, sum_r2 = 0;
  double corr[5] = {};
  uint32_t samples = 0;
  int frames = 0;
  int peak_l = 0, peak_r = 0;
};

struct EncoderCb {
  std::mutex        lock;
  HANDLE_AACENCODER handle = nullptr;
  a2dp_read_cb_t    read_cb = nullptr;
  a2dp_enqueue_cb_t enqueue_cb = nullptr;

  uint16_t peer_mtu = 0;
  uint32_t bitrate = kDefaultBitrate;
  int      frames_per_packet = kDefaultFramesPerPkt;       // in use
  int      cfg_frames_per_packet = kDefaultFramesPerPkt;   // asked for
  int      rate_mode = kDefaultRateMode;
  int      sbr_mode = kDefaultSbr;
  int      sbr_ratio = kDefaultSbrRatio;
  uint32_t peak_bitrate = kDefaultPeak;
  int      afterburner = 0;
  int      max_packets_per_tick = kDefaultMaxPacketsPerTick;
  int      max_queue = kDefaultMaxQueue;
  bool     pcm_stats_on = false;
  PcmStats pcm_stats;
  int      user_bitrate = 0;       // kPropUserBitrate as last read
  bool     user_vbr = true;        // kPropUserVbr as last read
  int      open_sbr = 0;           // SBR mode the running encoder was opened with
  int      poll_ticks = 0;

  // RTP timestamp. A stream starts on a 48 kHz sample count, 480 per frame,
  // the ordinary A2DP way, and changes to a millisecond clock a few seconds
  // in. Both halves are needed.
  //
  // macOS stamps its packets from a millisecond clock, and so does the
  // USBPods firmware, whose stream the same AirPods play. From this phone the
  // AirPods reject millisecond stamps at the start of a stream: an AVRCP
  // PAUSE 2.9 s after every START, complete silence, and a stalled link and a
  // rebooted bud if the stream is kept going. It is the timestamps alone.
  // Everything else on the wire was byte-identical to the USBPods stream,
  // verified by receiving this phone's packets on a fake sink; a stream of
  // pure digital silence is rejected too, and so are stamps given the same
  // jitter as USBPods' (kPropRtpJitter). Why the start is different from
  // USBPods' is not known. The USBPods firmware notes that the buds spend
  // about the first 3 s after START locking onto the stream.
  //
  // The sample count alone plays, but has a cost. The buds discipline their
  // ELD playout clock on these timestamps, in milliseconds, and on a
  // sample-count stream a volume change made while audio is playing leaves
  // the two buds at different levels (heard with a signal identical in both
  // channels; AVRCP command and media stream verified clean in a capture).
  //
  // Changed to milliseconds once the stream is running, the AirPods carry on
  // without a pause and nothing is heard at the change, and volume changes
  // after it leave the buds balanced. See docs/notes/development-log.md.
  bool     rtp_ms_clock = false;     // the unit in use right now
  bool     rtp_ms_prop = false;      // persist.aaceld.rtp_ms as last read
  int      rtp_ms_after = kDefaultRtpMsAfter;
  bool     rtp_ms_lag = false;
  bool     rtp_auto_done = false;
  int      rtp_jitter_us = kDefaultRtpJitterUs;
  // Millisecond stamps are rtp_anchor_ts plus the time since rtp_anchor_us,
  // so the unit can be taken up in the middle of a stream.
  uint64_t rtp_anchor_us = 0;
  uint32_t rtp_anchor_ts = 0;
  uint64_t rtp_prev_send_us = 0;
  uint32_t rtp_last = 0;             // the last stamp written, in either unit
  uint32_t rtp_next_samples = 0;     // the next stamp if it counts samples
  uint32_t stream_packets = 0;       // packets since this stream started
  int      interval_asks = 0;        // only to log when the stack asks
  uint64_t stream_first_us = 0;
  uint32_t rtp_rng = 0x9e3779b9u;
  int      logged_steps = 0;
  uint16_t frame_seq = 1;          // 12 bit counter in the Apple header

  // Written by the stack from inside enqueue_cb, which this module calls with
  // the lock held, so it is deliberately not taken again there.
  size_t   tx_queue_len = 0;
  int      held_ticks = 0;

  // Link trouble, for the log: from the queue first backing up until it has
  // stayed short for a second.
  bool     trouble = false;
  uint64_t trouble_since_us = 0;
  uint64_t trouble_last_us = 0;
  size_t   trouble_queue_max = 0;
  uint32_t trouble_dumps = 0;        // times the stack threw its queue away
  uint32_t trouble_packets = 0;      // packets made while it lasted
  size_t   prev_queue_len = 0;
  int      kick_ms = 0;
  uint32_t rtp_seq = 0;              // packets made in this stream

  // One frame of PCM. A partially filled frame is carried between ticks.
  int16_t  pcm[ELD_GRANULE_SAMPLES * ELD_CHANNELS];
  size_t   staged = 0;
  uint8_t  au[ELD_MAX_AU_BYTES + 1];

  // The packet under assembly. It is carried between ticks too, so that what
  // goes on the air is always whole packets of frames_per_packet frames, the
  // one cadence macOS uses, rather than whatever a single tick happened to
  // deliver.
  uint8_t  pkt[kApplePayloadMax];
  size_t   pkt_len = 0;
  int      pkt_frames = 0;
  uint32_t pkt_pcm_bytes = 0;
  uint64_t pkt_first_us = 0;

  // Pacing. Frames are owed by elapsed time, the way the stock encoders do
  // it, because the tick is not exactly the encoder interval.
  uint64_t last_tick_us = 0;
  int64_t  owed_us = 0;

  int      logged_frames = 0;
  int      logged_packets = 0;
  Stats    stats;
};

EncoderCb g_cb;

// Read from the stack's main thread by the packet-write hook.
std::atomic<bool>     g_active{false};
std::atomic<bool>     g_keep_marker{false};
std::atomic<bool>     g_keep_ssrc{false};
std::atomic<uint32_t> g_write_ok{0};
std::atomic<uint32_t> g_write_congested{0};
std::atomic<uint32_t> g_write_failed{0};

// The stack numbers RTP packets when it sends them, so a packet it threw away
// while the link was down leaves no hole in the RTP sequence, although the
// audio, the Apple frame numbers and the timestamps all have one. On macOS the
// controller discards packets that were already numbered, so there the RTP
// sequence shows the loss too. To send the same picture, this module counts
// the packets it makes in each stream; the write hook counts the ones that
// reach it, and adds the difference to the stack's own number. The two meet
// through the timestamp, which is unique per packet within a stream.
//
// The stack's numbering is left as the base on purpose. It carries on across
// a pause and resume, and a first version of this that used the module's own
// count, which starts again with every stream, sent numbers that went
// backwards at each resume: the AirPods threw every packet away as old and
// there was no sound at all. Off by default until it has been heard to help;
// persist.aaceld.rtp_seq_gaps=true turns it on.
std::atomic<bool> g_seq_gaps{false};
std::atomic<bool> g_wire_rearm{false};
constexpr size_t kSeqMapSize = 64;
std::atomic<uint64_t> g_seq_map[kSeqMapSize];
// One entry: valid bit, stream number (7 bits), packet count in the stream
// (24 bits), RTP timestamp (32 bits).
constexpr uint64_t kSeqValid = 1ull << 63;
std::atomic<uint32_t> g_stream_epoch{0};

// Largest payload one packet may carry. peer_mtu is already the room left
// after the RTP header; going over it makes the stack fragment the packet,
// which splits a frame across two RTP packets.
size_t payload_budget() {
  size_t budget = kApplePayloadMax;
  if (g_cb.peer_mtu != 0 && g_cb.peer_mtu < budget) budget = g_cb.peer_mtu;
  return budget;
}

void reset_stream_state() {
  g_cb.rtp_anchor_us = 0;
  g_cb.rtp_anchor_ts = 0;
  g_cb.rtp_prev_send_us = 0;
  g_cb.rtp_last = 0;
  g_cb.rtp_next_samples = 0;
  g_cb.stream_packets = 0;
  g_cb.interval_asks = 0;
  g_cb.stream_first_us = 0;
  g_cb.rtp_auto_done = false;
  g_cb.logged_steps = 0;
  g_cb.frame_seq = 1;
  g_cb.tx_queue_len = 0;
  g_cb.held_ticks = 0;
  g_cb.trouble = false;
  g_cb.trouble_queue_max = 0;
  g_cb.trouble_dumps = 0;
  g_cb.trouble_packets = 0;
  g_cb.prev_queue_len = 0;
  g_cb.rtp_seq = 0;
  g_stream_epoch.fetch_add(1, std::memory_order_relaxed);
  g_cb.staged = 0;
  g_cb.pkt_len = 0;
  g_cb.pkt_frames = 0;
  g_cb.pkt_pcm_bytes = 0;
  g_cb.pkt_first_us = 0;
  g_cb.last_tick_us = 0;
  g_cb.owed_us = 0;
  g_cb.logged_frames = 0;
  g_cb.logged_packets = 0;
  g_cb.stats = Stats{};
  g_write_ok.store(0, std::memory_order_relaxed);
  g_write_congested.store(0, std::memory_order_relaxed);
  g_write_failed.store(0, std::memory_order_relaxed);
}

void close_encoder() {
  g_active.store(false, std::memory_order_relaxed);
  if (g_cb.handle != nullptr) {
    g_stack.aacEncClose(&g_cb.handle);
    g_cb.handle = nullptr;
  }
}

int effective_sbr() {
  return (g_cb.rate_mode == 0) ? 0 : g_cb.sbr_mode;
}

bool open_encoder() {
  if (g_stack.aacEncOpen == nullptr) return false;

  // SBR only rides the VBR path. FDK's own ELD/SBR table drops SBR for
  // stereo 48 kHz at these rates, so forcing it on under CBR leaves the
  // quantizer a budget it cannot meet: the USBPods firmware caught FDK's rate
  // loop never returning from aacEncEncode in exactly that configuration.
  // Here that would hang the Bluetooth media thread, so the pairing is
  // enforced rather than left to the properties.
  int sbr_mode = effective_sbr();
  if (sbr_mode != g_cb.sbr_mode) LOGW("SBR is only used with VBR; running CBR without it");
  g_cb.open_sbr = sbr_mode;

  // Module mask 0x03 is the AAC core plus the SBR module. FDK silently
  // ignores AACENC_SBR_MODE when the SBR module was not allocated at open.
  if (g_stack.aacEncOpen(&g_cb.handle, 0x03, ELD_CHANNELS) != AACENC_OK ||
      g_cb.handle == nullptr) {
    LOGE("aacEncOpen failed");
    g_cb.handle = nullptr;
    return false;
  }

  // Order follows the working firmware: bit rate before the format
  // parameters, then SBR, then the rate mode and its peak.
  struct Param { INT id; UINT value; const char* name; };
  Param params[12];
  int n = 0;
  params[n++] = {AACENC_AOT,            AOT_ER_AAC_ELD,         "AOT"};
  params[n++] = {AACENC_BITRATE,        g_cb.bitrate,           "BITRATE"};
  params[n++] = {AACENC_SAMPLERATE,     ELD_SAMPLE_RATE,        "SAMPLERATE"};
  params[n++] = {AACENC_CHANNELMODE,    MODE_2,                 "CHANNELMODE"};
  params[n++] = {AACENC_GRANULE_LENGTH, ELD_GRANULE_SAMPLES,    "GRANULE_LENGTH"};
  params[n++] = {AACENC_SBR_MODE,       (UINT)sbr_mode,         "SBR_MODE"};
  if (sbr_mode != 0) {
    params[n++] = {AACENC_SBR_RATIO,    (UINT)g_cb.sbr_ratio,   "SBR_RATIO"};
  }
  params[n++] = {AACENC_BITRATEMODE,    (UINT)g_cb.rate_mode,   "BITRATEMODE"};
  // The peak is set in CBR mode as well. FDK turns it into a hard bound on
  // the bits in one frame, and without it the CBR bit reservoir lets single
  // frames run to nearly twice the average: 470 bytes was seen at 192 kbps.
  // The headset is told a maximum bit rate in SET_CONFIGURATION and macOS
  // never sends a frame above it, so neither does this.
  params[n++] = {AACENC_PEAK_BITRATE,   g_cb.peak_bitrate,      "PEAK_BITRATE"};
  params[n++] = {AACENC_AFTERBURNER,    (UINT)g_cb.afterburner, "AFTERBURNER"};
  params[n++] = {AACENC_TRANSMUX,       TT_MP4_RAW,             "TRANSMUX"};

  for (int i = 0; i < n; i++) {
    const Param& p = params[i];
    INT err = g_stack.aacEncoder_SetParam(g_cb.handle, p.id, p.value);
    if (err != AACENC_OK) {
      LOGE("aacEncoder_SetParam(%s=%u) failed 0x%x", p.name, p.value, err);
      g_stack.aacEncClose(&g_cb.handle);
      g_cb.handle = nullptr;
      return false;
    }
  }

  // A call with no buffers makes FDK apply the configuration.
  if (g_stack.aacEncEncode(g_cb.handle, nullptr, nullptr, nullptr, nullptr) != AACENC_OK) {
    LOGE("encoder initialisation call failed");
    g_stack.aacEncClose(&g_cb.handle);
    g_cb.handle = nullptr;
    return false;
  }

  // Read back what FDK actually configured. A core delay different from the
  // total delay is the sign that the SBR stage really is in the path.
  unsigned frame_length = 0, delay = 0, delay_core = 0;
  if (g_stack.aacEncInfo != nullptr) {
    unsigned char info[256] = {};
    if (g_stack.aacEncInfo(g_cb.handle, info) == AACENC_OK) {
      unsigned w[7];
      memcpy(w, info, sizeof(w));
      frame_length = w[4];
      delay = w[5];
      delay_core = w[6];
    }
  }

  LOGI("AAC-ELD encoder up: 48 kHz stereo, frame %u, bitrate=%u ratemode=%d sbr=%d/ratio=%d "
       "peak=%u afterburner=%d delay=%u core=%u, %d frames per packet, payload budget %zu",
       frame_length, g_cb.bitrate, g_cb.rate_mode, sbr_mode, g_cb.sbr_ratio,
       g_cb.peak_bitrate, g_cb.afterburner, delay, delay_core, g_cb.frames_per_packet,
       payload_budget());
  if (frame_length != 0 && frame_length != ELD_GRANULE_SAMPLES) {
    LOGE("encoder frame length is %u, not %d; AirPods only play the 480 sample mode",
         frame_length, ELD_GRANULE_SAMPLES);
  }
  g_active.store(true, std::memory_order_relaxed);
  return true;
}

void note_pcm_stats() {
  PcmStats& st = g_cb.pcm_stats;
  const int16_t* x = g_cb.pcm;
  for (int n = 2; n < ELD_GRANULE_SAMPLES - 2; n++) {
    const double l = x[2 * n];
    const double r = x[2 * n + 1];
    st.sum_l2 += l * l;
    st.sum_r2 += r * r;
    for (int k = 0; k < 5; k++) st.corr[k] += l * (double)x[2 * (n + k - 2) + 1];
    const int al = x[2 * n] < 0 ? -x[2 * n] : x[2 * n];
    const int ar = x[2 * n + 1] < 0 ? -x[2 * n + 1] : x[2 * n + 1];
    if (al > st.peak_l) st.peak_l = al;
    if (ar > st.peak_r) st.peak_r = ar;
  }
  st.samples += ELD_GRANULE_SAMPLES - 4;
  if (++st.frames < kPcmStatsFrames) return;

  const double full = 32768.0 * 32768.0 * st.samples;
  const double l_db = st.sum_l2 > 0 ? 10.0 * log10(st.sum_l2 / full) : -120.0;
  const double r_db = st.sum_r2 > 0 ? 10.0 * log10(st.sum_r2 / full) : -120.0;
  const double norm = sqrt(st.sum_l2 * st.sum_r2);
  double c[5] = {};
  int best = 2;
  for (int k = 0; k < 5; k++) {
    c[k] = norm > 0 ? st.corr[k] / norm : 0.0;
    if (c[k] > c[best]) best = k;
  }
  LOGI("pcm: L %.2f dBFS R %.2f dBFS, L-R %+.2f dB, peak %d/%d, corr at R-L lag -2..+2: "
       "%.4f %.4f %.4f %.4f %.4f (max at %+d)",
       l_db, r_db, l_db - r_db, st.peak_l, st.peak_r, c[0], c[1], c[2], c[3], c[4], best - 2);
  st = PcmStats{};
}

// Encodes one 480 sample stereo frame. Returns the access unit length, or a
// negative value on failure.
int encode_one_frame() {
  if (g_cb.pcm_stats_on) note_pcm_stats();

  void* in_ptr = g_cb.pcm;
  INT in_id = IN_AUDIO_DATA;
  INT in_size = (INT)sizeof(g_cb.pcm);
  INT in_elem = (INT)sizeof(int16_t);

  void* out_ptr = g_cb.au;
  INT out_id = OUT_BITSTREAM_DATA;
  INT out_size = (INT)sizeof(g_cb.au);
  INT out_elem = 1;

  AACENC_BufDesc in_desc = {};
  in_desc.numBufs = 1;
  in_desc.bufs = &in_ptr;
  in_desc.bufferIdentifiers = &in_id;
  in_desc.bufSizes = &in_size;
  in_desc.bufElSizes = &in_elem;

  AACENC_BufDesc out_desc = {};
  out_desc.numBufs = 1;
  out_desc.bufs = &out_ptr;
  out_desc.bufferIdentifiers = &out_id;
  out_desc.bufSizes = &out_size;
  out_desc.bufElSizes = &out_elem;

  AACENC_InArgs in_args = {};
  in_args.numInSamples = ELD_GRANULE_SAMPLES * ELD_CHANNELS;

  AACENC_OutArgs out_args = {};
  const uint64_t cpu0 = thread_cpu_ns();
  INT err = g_stack.aacEncEncode(g_cb.handle, &in_desc, &out_desc, &in_args, &out_args);
  g_cb.stats.enc_cpu_ns += thread_cpu_ns() - cpu0;
  if (err != AACENC_OK) {
    g_cb.stats.enc_errors++;
    if (g_cb.stats.enc_errors <= 5 || g_cb.stats.enc_errors % 1000 == 0) {
      LOGE("aacEncEncode failed 0x%x (%u so far)", err, g_cb.stats.enc_errors);
    }
    return -1;
  }
  return out_args.numOutBytes;
}

// Takes one pass at completing a 480 sample frame of PCM. Whatever arrives is
// kept staged for the next tick rather than discarded, so no PCM is lost.
//
// Deliberately one read per call, never a retry loop. The stack's read
// callback waits about 10 ms when the audio HAL has nothing, so looping until
// a frame is complete can block the media thread for longer than the tick
// period, which makes the next tick late and drains the HAL harder.
bool stage_one_frame() {
  const size_t need = sizeof(g_cb.pcm);
  if (g_cb.staged < need) {
    uint32_t got = g_cb.read_cb(reinterpret_cast<uint8_t*>(g_cb.pcm) + g_cb.staged,
                                (uint32_t)(need - g_cb.staged));
    if (got > need - g_cb.staged) got = (uint32_t)(need - g_cb.staged);
    g_cb.staged += got;
  }
  if (g_cb.staged < need) return false;   // keep the partial frame for next time
  g_cb.staged = 0;
  return true;
}

void log_stats(uint64_t now) {
  const Stats& s = g_cb.stats;
  const uint64_t span_us = (s.since_us != 0 && now > s.since_us) ? now - s.since_us : 0;
  const unsigned kbps = span_us ? (unsigned)(s.au_bytes * 8ull * 1000ull / span_us) : 0;
  const unsigned pps10 = span_us ? (unsigned)((uint64_t)s.packets * 10000000ull / span_us) : 0;
  // Encoder cost: CPU microseconds per 10 ms frame, and the same as a share
  // of one core in hundredths of a percent.
  const unsigned enc_us = s.frames ? (unsigned)(s.enc_cpu_ns / 1000ull / s.frames) : 0;
  LOGI("stats: %u packets %u frames in %u ms (%u.%u pkt/s, %u kbps), au avg %u max %u, "
       "encode %u us/frame (%u.%02u%% of a core), "
       "queue max %zu, short reads %u, skipped frames %u, encode errors %u, oversize %u, "
       "l2cap ok/congested/failed %u/%u/%u",
       s.packets, s.frames, (unsigned)(span_us / 1000), pps10 / 10, pps10 % 10, kbps,
       s.frames ? (unsigned)(s.au_bytes / s.frames) : 0, s.au_max,
       enc_us, enc_us / 100, enc_us % 100, s.queue_max,
       s.short_reads, s.skipped_frames, s.enc_errors, s.oversize,
       g_write_ok.load(std::memory_order_relaxed),
       g_write_congested.load(std::memory_order_relaxed),
       g_write_failed.load(std::memory_order_relaxed));
}

// Changes the unit of the RTP timestamps. Safe in the middle of a stream: the
// next stamp follows the last one by one packet's worth in the new unit.
void set_rtp_unit(bool ms, const char* why) {
  if (ms == g_cb.rtp_ms_clock) return;
  g_cb.rtp_ms_clock = ms;
  g_cb.rtp_anchor_us = 0;   // taken again at the next packet
  g_cb.logged_steps = 0;
  LOGI("rtp clock now counts %s (%s), after packet %u, last stamp %u",
       ms ? "milliseconds" : "samples", why, g_cb.stream_packets, g_cb.rtp_last);
}

// The stamp for the packet under assembly.
uint32_t next_rtp_timestamp(uint64_t now) {
  const bool first = g_cb.stream_packets == 0;
  uint32_t ts;
  if (g_cb.rtp_ms_clock) {
    if (g_cb.rtp_anchor_us == 0) {
      g_cb.rtp_anchor_us = now;
      g_cb.rtp_anchor_ts = first ? 0 : g_cb.rtp_last + (uint32_t)g_cb.frames_per_packet * 10u;
    }
    uint64_t ref = now;
    if (g_cb.rtp_ms_lag) {
      ref = (g_cb.rtp_prev_send_us > g_cb.rtp_anchor_us) ? g_cb.rtp_prev_send_us : g_cb.rtp_anchor_us;
    }
    uint64_t t_us = ref - g_cb.rtp_anchor_us;
    if (g_cb.rtp_jitter_us > 0 && !g_cb.rtp_ms_lag) {
      g_cb.rtp_rng = g_cb.rtp_rng * 1664525u + 1013904223u;
      t_us += (uint64_t)((g_cb.rtp_rng >> 8) % (uint32_t)g_cb.rtp_jitter_us);
    }
    ts = g_cb.rtp_anchor_ts + (uint32_t)(t_us / 1000ull);
    // Never backwards, also when a late tick sends two packets at once.
    const int32_t min_step = g_cb.rtp_ms_lag ? 0 : 1;
    if (!first && (int32_t)(ts - g_cb.rtp_last) < min_step) ts = g_cb.rtp_last + (uint32_t)min_step;
    if (g_cb.logged_steps < 12 && !first) {
      g_cb.logged_steps++;
      LOGI("rtp step %d: +%u ms", g_cb.logged_steps, ts - g_cb.rtp_last);
    }
  } else {
    ts = first ? 0 : g_cb.rtp_next_samples;
  }
  g_cb.rtp_next_samples = ts + (uint32_t)g_cb.pkt_frames * ELD_GRANULE_SAMPLES;
  g_cb.rtp_last = ts;
  g_cb.rtp_prev_send_us = now;
  if (first) g_cb.stream_first_us = now;
  g_cb.stream_packets++;
  return ts;
}

// Hands the packet under assembly to the stack.
void flush_packet(uint64_t now) {
  if (g_cb.pkt_frames == 0) return;

  auto* buf = static_cast<BT_HDR*>(g_stack.osi_malloc(BT_DEFAULT_BUFFER_SIZE));
  if (buf != nullptr) {
    buf->event = 0;
    buf->len = (uint16_t)g_cb.pkt_len;
    buf->offset = AVDT_MEDIA_OFFSET;
    buf->layer_specific = (uint16_t)g_cb.pkt_frames;

    const uint32_t timestamp = next_rtp_timestamp(now);

    // The stack reads the RTP timestamp back from the first four bytes of the
    // data area, and builds the RTP header so that it ends at `offset`.
    auto* data = reinterpret_cast<uint8_t*>(buf + 1);
    memcpy(data, &timestamp, sizeof(timestamp));
    memcpy(data + buf->offset, g_cb.pkt, g_cb.pkt_len);

    if (g_cb.logged_packets < kLogPackets) {
      g_cb.logged_packets++;
      LOGI("packet %d: rtp_ts=%u frames=%d payload=%zu queue=%zu",
           g_cb.logged_packets, timestamp, g_cb.pkt_frames, g_cb.pkt_len, g_cb.tx_queue_len);
    }

    g_cb.rtp_seq = (g_cb.rtp_seq + 1) & 0xFFFFFFu;
    g_seq_map[g_cb.rtp_seq % kSeqMapSize].store(
        kSeqValid | ((uint64_t)(g_stream_epoch.load(std::memory_order_relaxed) & 0x7Fu) << 56) |
            ((uint64_t)g_cb.rtp_seq << 32) | timestamp,
        std::memory_order_relaxed);
    if (g_cb.trouble) g_cb.trouble_packets++;

    if (g_cb.stats.since_us == 0) g_cb.stats.since_us = now;
    g_cb.stats.packets++;
    g_cb.enqueue_cb(buf, (size_t)g_cb.pkt_frames, g_cb.pkt_pcm_bytes);
    if (g_cb.tx_queue_len > g_cb.stats.queue_max) g_cb.stats.queue_max = g_cb.tx_queue_len;

    if (g_cb.stats.packets % kStatsPackets == 0) log_stats(now);
  }

  g_cb.pkt_len = 0;
  g_cb.pkt_frames = 0;
  g_cb.pkt_pcm_bytes = 0;
}

// Adds the access unit in g_cb.au to the packet under assembly, behind its
// four byte Apple header, sending the packet when it is complete.
void append_frame(int au_len, uint64_t now) {
  const size_t budget = payload_budget();
  const size_t need = 4 + (size_t)au_len;

  if (au_len > ELD_MAX_AU_BYTES || need > budget) {
    // Cannot be carried at all. Its sequence number is still spent, so the
    // headset sees one missing frame rather than a renumbered stream.
    g_cb.stats.oversize++;
    LOGE("access unit of %d bytes does not fit a %zu byte payload, dropped", au_len, budget);
    g_cb.frame_seq = (uint16_t)((g_cb.frame_seq + 1) & 0x0FFF);
    return;
  }

  // An encoded frame is never thrown away for want of room: the packet so far
  // goes out short and this frame starts the next one.
  if (g_cb.pkt_len + need > budget) flush_packet(now);
  if (g_cb.pkt_frames == 0) g_cb.pkt_first_us = now;

  uint8_t* p = g_cb.pkt + g_cb.pkt_len;
  p[0] = (uint8_t)(0xB0 | ((g_cb.frame_seq >> 8) & 0x0F));
  p[1] = (uint8_t)(g_cb.frame_seq & 0xFF);
  p[2] = (uint8_t)(0x10 | ((au_len >> 8) & 0x0F));
  p[3] = (uint8_t)(au_len & 0xFF);
  memcpy(p + 4, g_cb.au, (size_t)au_len);

  if (g_cb.logged_frames < kLogFrames) {
    g_cb.logged_frames++;
    LOGI("frame %d: seq=%u au_len=%d hdr=%02x %02x %02x %02x au=%02x %02x %02x %02x %02x %02x",
         g_cb.logged_frames, g_cb.frame_seq, au_len, p[0], p[1], p[2], p[3],
         g_cb.au[0], g_cb.au[1], g_cb.au[2], g_cb.au[3], g_cb.au[4], g_cb.au[5]);
  }

  g_cb.pkt_len += need;
  g_cb.pkt_frames++;
  g_cb.pkt_pcm_bytes += (uint32_t)sizeof(g_cb.pcm);
  g_cb.frame_seq = (uint16_t)((g_cb.frame_seq + 1) & 0x0FFF);

  g_cb.stats.frames++;
  g_cb.stats.au_bytes += (uint64_t)au_len;
  if ((uint32_t)au_len > g_cb.stats.au_max) g_cb.stats.au_max = (uint32_t)au_len;

  if (g_cb.pkt_frames >= g_cb.frames_per_packet) flush_packet(now);
}

int clamp_int(int value, int lo, int hi) {
  return value < lo ? lo : (value > hi ? hi : value);
}

// Read at every stream start, not only when the codec is set up, so a change
// takes effect on the next pause and resume.
void load_properties() {
  // The persist.aaceld tuning values first.
  int rate = g_stack.osi_property_get_int32(kPropBitrate, (int)kDefaultBitrate);
  g_cb.bitrate = (rate > 0) ? (uint32_t)rate : kDefaultBitrate;
  int peak = g_stack.osi_property_get_int32(kPropPeak, (int)kDefaultPeak);
  g_cb.peak_bitrate = (peak > 0) ? (uint32_t)peak : kDefaultPeak;
  g_cb.rate_mode   = clamp_int(g_stack.osi_property_get_int32(kPropRateMode, kDefaultRateMode), 0, 5);

  // Then the two settings from the headset's page, which decide.
  g_cb.user_bitrate = g_stack.osi_property_get_int32(kPropUserBitrate, 0);
  g_cb.user_vbr = g_stack.osi_property_get_bool(kPropUserVbr, true);
  const uint32_t user_max = (uint32_t)clamp_int(
      g_cb.user_bitrate > 0 ? g_cb.user_bitrate : kUserBitrateDefault, kUserBitrateMin, kUserBitrateMax);
  if (g_cb.user_vbr) {
    // Variable rate: the maximum is a bound on each frame, at most the
    // ceiling. FDK steps its variable-rate mode down by itself when the
    // bound is below what the mode needs.
    const uint32_t bound = user_max < (uint32_t)kVbrCeiling ? user_max : (uint32_t)kVbrCeiling;
    if (g_cb.peak_bitrate > bound) g_cb.peak_bitrate = bound;
    if (g_cb.bitrate > g_cb.peak_bitrate) g_cb.bitrate = g_cb.peak_bitrate;
  } else {
    // Constant rate at the maximum. The tuning peak still bounds a frame
    // from above when it is the larger, as it always has for 256 and below.
    g_cb.bitrate = user_max;
    g_cb.rate_mode = 0;
  }
  if (g_cb.peak_bitrate < g_cb.bitrate) g_cb.peak_bitrate = g_cb.bitrate;
  g_cb.sbr_mode    = clamp_int(g_stack.osi_property_get_int32(kPropSbr, kDefaultSbr), 0, 1);
  g_cb.sbr_ratio   = clamp_int(g_stack.osi_property_get_int32(kPropSbrRatio, kDefaultSbrRatio), 0, 2);
  g_cb.afterburner = clamp_int(g_stack.osi_property_get_int32(kPropAfterburn, 0), 0, 1);
  g_cb.max_packets_per_tick = clamp_int(
      g_stack.osi_property_get_int32(kPropMaxPkt, kDefaultMaxPacketsPerTick), 1, 6);
  g_cb.max_queue = clamp_int(
      g_stack.osi_property_get_int32(kPropMaxQueue, kDefaultMaxQueue), 0, 20);
  g_seq_gaps.store(g_stack.osi_property_get_bool(kPropSeqGaps, false), std::memory_order_relaxed);
  g_cb.kick_ms = clamp_int(g_stack.osi_property_get_int32(kPropKickMs, 0), 0, 600000);
  // The unit in use is not touched here: this also runs for a bit-rate
  // change in the middle of a stream. See start_rtp_unit.
  g_cb.rtp_ms_prop = g_stack.osi_property_get_bool(kPropRtpMs, false);
  g_cb.rtp_ms_after = clamp_int(
      g_stack.osi_property_get_int32(kPropRtpMsAfter, kDefaultRtpMsAfter), 0, 600000);
  g_cb.rtp_ms_lag = g_stack.osi_property_get_bool(kPropRtpMsLag, false);
  g_cb.rtp_jitter_us = clamp_int(
      g_stack.osi_property_get_int32(kPropRtpJitter, kDefaultRtpJitterUs), 0, 8000);
  const bool pcm_stats = g_stack.osi_property_get_bool(kPropPcmStats, false);
  if (pcm_stats != g_cb.pcm_stats_on) g_cb.pcm_stats = PcmStats{};
  g_cb.pcm_stats_on = pcm_stats;
  g_keep_marker.store(g_stack.osi_property_get_bool(kPropMarker, false),
                      std::memory_order_relaxed);
  g_keep_ssrc.store(g_stack.osi_property_get_bool(kPropSsrc, false),
                    std::memory_order_relaxed);
}

// How many frames go in one packet: the number asked for (three, Apple's
// cadence) unless frames of the largest size the encoder may produce do not
// fit that many times in the payload. Above 256 kbps they do not: a frame is
// up to 361 bytes at 288 kbps and 401 at 320, and three of those with their
// headers are past the 986 byte payload, so those rates go out two frames to
// a packet, fifty packets a second. USBPods sends two-frame packets to the
// same AirPods as a matter of course.
void set_packet_shape() {
  // The peak rate is what bounds a frame, in constant-rate mode too, and
  // load_properties keeps it at or above the bit rate.
  const uint32_t rate = g_cb.peak_bitrate > g_cb.bitrate ? g_cb.peak_bitrate : g_cb.bitrate;
  // FDK's constant-rate frames run one byte over rate / 800.
  const size_t frame_bytes = 4 + (size_t)(rate / 800u) + 1;
  int fit = (int)(payload_budget() / frame_bytes);
  g_cb.frames_per_packet = clamp_int(fit, 1, g_cb.cfg_frames_per_packet);
}

// The unit a stream starts on. With a change-over time set it is always the
// sample count, whatever persist.aaceld.rtp_ms says.
void start_rtp_unit() {
  g_cb.rtp_ms_clock = g_cb.rtp_ms_prop && g_cb.rtp_ms_after == 0;
}

// Applies a bit-rate setting that changed while the stream is running. Rate
// mode, bit rate and peak can be changed on a live FDK encoder: it re-runs its
// configuration on the next encode call and keeps the filterbank state, so the
// bitstream carries on without a break and the headset sees nothing but
// frames of a different size. Whether SBR is in the stream cannot change that
// way, so if the new setting flips it the encoder is reopened instead.
void apply_user_bitrate_change() {
  const int before_mode = g_cb.rate_mode;
  const uint32_t before_rate = g_cb.bitrate;
  load_properties();
  // The packet under assembly stays as it is; the new count applies from the
  // next one. The tick keeps its old period until the next stream start,
  // which the pacing in eld_send_frames does not depend on.
  set_packet_shape();
  if (g_cb.handle == nullptr) return;

  bool ok = effective_sbr() == g_cb.open_sbr;
  if (ok) {
    ok = g_stack.aacEncoder_SetParam(g_cb.handle, AACENC_BITRATEMODE, (UINT)g_cb.rate_mode) == AACENC_OK &&
         g_stack.aacEncoder_SetParam(g_cb.handle, AACENC_BITRATE, g_cb.bitrate) == AACENC_OK &&
         g_stack.aacEncoder_SetParam(g_cb.handle, AACENC_PEAK_BITRATE, g_cb.peak_bitrate) == AACENC_OK &&
         g_stack.aacEncEncode(g_cb.handle, nullptr, nullptr, nullptr, nullptr) == AACENC_OK;
  }
  if (!ok) {
    LOGW("bit-rate change needs a fresh encoder, reopening");
    close_encoder();
    open_encoder();
  }
  if (g_cb.stats.packets != 0) log_stats(now_us());
  g_cb.stats = Stats{};
  LOGI("bit rate setting changed: ratemode %d -> %d, bitrate %u -> %u, peak %u, "
       "%d frames per packet (%s)",
       before_mode, g_cb.rate_mode, before_rate, g_cb.bitrate, g_cb.peak_bitrate,
       g_cb.frames_per_packet, ok ? "applied to the running encoder" : "encoder reopened");
}

// --- the eight interface slots -------------------------------------------

void eld_encoder_init(const A2dpEncoderInitPeerParams* peer, void* /*codec_config*/,
                      a2dp_read_cb_t read_cb, a2dp_enqueue_cb_t enqueue_cb) {
  std::lock_guard<std::mutex> guard(g_cb.lock);
  close_encoder();
  reset_stream_state();

  g_cb.read_cb = read_cb;
  g_cb.enqueue_cb = enqueue_cb;
  g_cb.peer_mtu = peer ? peer->peer_mtu : 0;

  // The packet size also sets the tick the stack runs this encoder at, which
  // it asks for once, so this one is fixed for the life of the codec setup.
  g_cb.cfg_frames_per_packet = clamp_int(
      g_stack.osi_property_get_int32(kPropFrames, kDefaultFramesPerPkt), 1, kMaxFramesPerPkt);
  load_properties();
  set_packet_shape();
  start_rtp_unit();

  LOGI("encoder init: peer MTU %u, rtp clock %s%s, marker %s, ssrc %s", g_cb.peer_mtu,
       g_cb.rtp_ms_clock ? "milliseconds" : "samples",
       g_cb.rtp_ms_after > 0 ? " then milliseconds" : "",
       g_keep_marker.load(std::memory_order_relaxed) ? "kept" : "cleared",
       g_keep_ssrc.load(std::memory_order_relaxed) ? "kept" : "zeroed");
  open_encoder();
}

void eld_encoder_cleanup() {
  std::lock_guard<std::mutex> guard(g_cb.lock);
  if (g_cb.stats.packets != 0) log_stats(now_us());
  close_encoder();
  g_cb.read_cb = nullptr;
  g_cb.enqueue_cb = nullptr;
}

// AAC-ELD carries its overlap across frames, so after a gap both ends have to
// start again from a clean state. The stack calls this at every stream start,
// which is also when the headset resets its decoder.
void eld_feeding_reset() {
  std::lock_guard<std::mutex> guard(g_cb.lock);
  if (g_cb.stats.packets != 0) log_stats(now_us());
  close_encoder();
  reset_stream_state();
  load_properties();
  set_packet_shape();
  start_rtp_unit();
  open_encoder();
}

void eld_feeding_flush() {
  eld_feeding_reset();
}

uint64_t eld_get_encoder_interval_ms() {
  // One tick is one packet. Older stacks take the figure in milliseconds,
  // newer AOSP in microseconds; target_resolve works out which this is.
  const uint64_t ms = (uint64_t)g_cb.frames_per_packet * 10u;
  if (g_cb.interval_asks < 2) {
    g_cb.interval_asks++;
    LOGI("stack asked for the encoder interval: %u ms", (unsigned)ms);
  }
  return g_interval_in_us ? ms * 1000u : ms;
}

int eld_get_effective_frame_size() {
  // The encoded size of one frame plus its Apple header, a per-frame figure
  // like the ones the stock encoders return here.
  uint32_t rate = g_cb.bitrate;
  if (g_cb.rate_mode != 0 && g_cb.peak_bitrate > rate) rate = g_cb.peak_bitrate;
  int au = (int)(rate / 800u);   // bytes per 10 ms frame
  if (au < 16) au = 16;
  if (au > ELD_MAX_AU_BYTES) au = ELD_MAX_AU_BYTES;
  return au + 4;
}

// An AVDTP suspend and start, which makes the headset throw its decoder state
// away and begin again; the USBPods notes have it as the recovery that always
// works. Off unless persist.aaceld.recover_kick_ms is set, and not yet tried
// on this phone. The start has to come from somewhere other than the encoder
// tick, which stops with the stream.
void* kick_restart(void*) {
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 600 * 1000000L};
  nanosleep(&ts, nullptr);
  if (g_stack.btif_av_stream_start != nullptr) {
    LOGI("recovery: starting the stream again");
    g_stack.btif_av_stream_start(0);   // A2dpType::kSource
  }
  return nullptr;
}

void kick_stream() {
  if (g_stack.btif_av_stream_suspend == nullptr || g_stack.btif_av_stream_start == nullptr) {
    LOGW("recovery: this build has no suspend and start to call");
    return;
  }
  LOGI("recovery: suspending the stream");
  g_stack.btif_av_stream_suspend();
  pthread_t tid;
  if (pthread_create(&tid, nullptr, kick_restart, nullptr) == 0) pthread_detach(tid);
}

// Says in the log when the link stopped taking audio and when it was back.
constexpr size_t kTroubleQueue = 5;          // packets waiting
constexpr uint64_t kTroubleOverUs = 1000000; // short again for this long

void note_link_state(uint64_t now) {
  const size_t q = g_cb.tx_queue_len;
  if (q >= kTroubleQueue) {
    if (!g_cb.trouble) {
      g_cb.trouble = true;
      g_cb.trouble_since_us = now;
      g_cb.trouble_queue_max = 0;
      g_cb.trouble_dumps = 0;
      g_cb.trouble_packets = 0;
      LOGI("link is not taking the audio: %zu packets waiting", q);
    }
    g_cb.trouble_last_us = now;
    if (q > g_cb.trouble_queue_max) g_cb.trouble_queue_max = q;
  } else if (g_cb.trouble && now - g_cb.trouble_last_us > kTroubleOverUs) {
    g_cb.trouble = false;
    const unsigned ms = (unsigned)((g_cb.trouble_last_us - g_cb.trouble_since_us) / 1000);
    LOGI("link is back: trouble lasted %u ms, queue peaked at %zu packets, the stack emptied "
         "it %u times, %u packets were made meanwhile; l2cap ok/congested/failed so far %u/%u/%u",
         ms, g_cb.trouble_queue_max, g_cb.trouble_dumps, g_cb.trouble_packets,
         g_write_ok.load(std::memory_order_relaxed),
         g_write_congested.load(std::memory_order_relaxed),
         g_write_failed.load(std::memory_order_relaxed));
    g_wire_rearm.store(true, std::memory_order_relaxed);   // log the next packets on the air
    if (g_cb.kick_ms > 0 && ms >= (unsigned)g_cb.kick_ms) kick_stream();
  }
  // A queue that was long and is suddenly short was thrown away by the stack.
  if (g_cb.trouble && g_cb.prev_queue_len >= kTroubleQueue && q + 4 < g_cb.prev_queue_len) {
    g_cb.trouble_dumps++;
  }
  g_cb.prev_queue_len = q;
}

void eld_send_frames(uint64_t /*timestamp_us*/) {
  std::lock_guard<std::mutex> guard(g_cb.lock);
  if (g_cb.handle == nullptr || g_cb.read_cb == nullptr || g_cb.enqueue_cb == nullptr) return;

  // Work out how much audio real time has passed, and owe that many frames. A
  // monotonic clock is used rather than the timestamp the stack passes, which
  // is not guaranteed to be a wall clock on this fork.
  if (++g_cb.poll_ticks >= kUserPollTicks) {
    g_cb.poll_ticks = 0;
    if (g_stack.osi_property_get_int32(kPropUserBitrate, 0) != g_cb.user_bitrate ||
        g_stack.osi_property_get_bool(kPropUserVbr, true) != g_cb.user_vbr) {
      apply_user_bitrate_change();
      if (g_cb.handle == nullptr) return;
    }
    const bool want_ms = g_stack.osi_property_get_bool(kPropRtpMs, false);
    if (want_ms != g_cb.rtp_ms_prop) {
      g_cb.rtp_ms_prop = want_ms;
      set_rtp_unit(want_ms, "persist.aaceld.rtp_ms changed");
    }
  }

  const uint64_t now = now_us();
  if (g_cb.rtp_ms_after > 0 && !g_cb.rtp_auto_done && g_cb.stream_first_us != 0 &&
      now - g_cb.stream_first_us >= (uint64_t)g_cb.rtp_ms_after * 1000ull) {
    g_cb.rtp_auto_done = true;
    set_rtp_unit(true, "persist.aaceld.rtp_ms_after");
  }
  const int64_t packet_us = kFrameUs * g_cb.frames_per_packet;
  if (g_cb.last_tick_us == 0) {
    g_cb.owed_us = packet_us;   // prime one packet
  } else {
    int64_t delta = (int64_t)(now - g_cb.last_tick_us);
    if (delta < 0 || delta > kMaxCatchUpUs) delta = packet_us;
    g_cb.owed_us += delta;
  }
  g_cb.last_tick_us = now;

  // The catch-up allowance is a length of time, so it does not shrink when
  // packets carry fewer frames.
  const int64_t cap_us = kFrameUs * g_cb.cfg_frames_per_packet * g_cb.max_packets_per_tick;
  if (g_cb.owed_us > cap_us) g_cb.owed_us = cap_us;

  // Rounded to the nearest frame, and the debt is allowed to go slightly
  // negative. A tick that arrives a millisecond early then still owes its
  // three frames, so each tick makes exactly one whole packet instead of the
  // count wobbling between two and four with the timer's jitter.
  int frames_due = (int)((g_cb.owed_us + kFrameUs / 2) / kFrameUs);
  if (frames_due <= 0) return;

  note_link_state(now);

  // The old back-pressure, only with persist.aaceld.max_queue set; see
  // kDefaultMaxQueue for why it is off.
  if (g_cb.max_queue > 0 && g_cb.tx_queue_len >= (size_t)g_cb.max_queue) {
    int dropped = 0;
    while (dropped < frames_due && stage_one_frame()) dropped++;
    g_cb.owed_us -= (int64_t)dropped * kFrameUs;
    g_cb.stats.skipped_frames += (uint32_t)dropped;
    if (++g_cb.held_ticks >= kProbeTicks) {
      g_cb.held_ticks = 0;
      g_cb.tx_queue_len = (size_t)g_cb.max_queue - 1;   // let one through to re-measure
    }
    return;
  }
  g_cb.held_ticks = 0;

  int done = 0;
  while (done < frames_due) {
    if (!stage_one_frame()) {
      g_cb.stats.short_reads++;
      break;   // out of PCM; the frames stay owed and the next tick catches up
    }
    done++;
    int au_len = encode_one_frame();
    if (au_len > 0) append_frame(au_len, now);
  }
  g_cb.owed_us -= (int64_t)done * kFrameUs;

  // A packet left short by an underflow is not held for ever.
  if (g_cb.pkt_frames > 0 && (int64_t)(now - g_cb.pkt_first_us) > packet_us) {
    flush_packet(now);
  }
}

void eld_set_transmit_queue_length(size_t len) {
  g_cb.tx_queue_len = len;
}

const A2dpEncoderInterface kInterface = {
    .encoder_init = eld_encoder_init,
    .encoder_cleanup = eld_encoder_cleanup,
    .feeding_reset = eld_feeding_reset,
    .feeding_flush = eld_feeding_flush,
    .get_encoder_interval_ms = eld_get_encoder_interval_ms,
    .get_effective_frame_size = eld_get_effective_frame_size,
    .send_frames = eld_send_frames,
    .set_transmit_queue_length = eld_set_transmit_queue_length,
};

}  // namespace

const A2dpEncoderInterface* eld_encoder_interface() { return &kInterface; }

bool eld_stream_active() { return g_active.load(std::memory_order_relaxed); }

bool eld_keep_rtp_marker() { return g_keep_marker.load(std::memory_order_relaxed); }

bool eld_keep_rtp_ssrc() { return g_keep_ssrc.load(std::memory_order_relaxed); }

bool eld_take_wire_rearm() { return g_wire_rearm.exchange(false, std::memory_order_relaxed); }

bool eld_packet_count_for(uint32_t timestamp, uint32_t* stream, uint32_t* count) {
  if (!g_seq_gaps.load(std::memory_order_relaxed)) return false;
  // Newest first, so a timestamp that also occurred in an earlier stream
  // (they all start at zero) resolves to the current one.
  const uint32_t epoch = g_stream_epoch.load(std::memory_order_relaxed) & 0x7Fu;
  for (auto& e : g_seq_map) {
    const uint64_t v = e.load(std::memory_order_relaxed);
    if ((v & kSeqValid) == 0 || (uint32_t)(v & 0xFFFFFFFFu) != timestamp) continue;
    if ((uint32_t)((v >> 56) & 0x7Fu) != epoch) continue;
    *stream = epoch;
    *count = (uint32_t)((v >> 32) & 0xFFFFFFu);
    return true;
  }
  return false;
}

void eld_note_write_result(uint8_t result) {
  // tL2CAP_DW_RESULT: 0 failed, 1 success, 2 congested.
  if (result == 1) g_write_ok.fetch_add(1, std::memory_order_relaxed);
  else if (result == 2) g_write_congested.fetch_add(1, std::memory_order_relaxed);
  else g_write_failed.fetch_add(1, std::memory_order_relaxed);
}
