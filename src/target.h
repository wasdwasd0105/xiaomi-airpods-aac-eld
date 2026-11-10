// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) the developer
// Resolution of the Bluetooth stack library and everything this module needs
// to know about it. Nothing here is tied to one build any more:
//
//  - Function addresses are looked up by name at start-up, in the mini debug
//    symbols (.gnu_debugdata) the stripped libbluetooth_jni.so carries. The
//    names are mangled C++ names, so they include the parameter types: a
//    build where a function's signature changed simply does not have the
//    name, and the module stays out.
//  - The layout of the codec object is measured, not assumed, by calling the
//    stack's own accessors on a dummy object. See target_measure_layout.
//
// Anything that cannot be found or does not pass its checks leaves the stack
// untouched.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "fdk_aac_abi.h"

// The build this was developed and listened to on: Xiaomi 17 Pro Max,
// HyperOS 3.0, OS3.0.319.0.WPBCNXM. Only used to say in the log whether the
// build in front of it is that one.
#define KNOWN_BUILD_ID "ea12c5b9edc07b7e826bd2aaec5a5f5b"
#define TARGET_LIB_NAME "libbluetooth_jni.so"

struct TargetOffsets {
  // Statically linked FDK-AAC encoder.
  uintptr_t aacEncOpen;
  uintptr_t aacEncClose;
  uintptr_t aacEncEncode;
  uintptr_t aacEncoder_SetParam;
  uintptr_t aacEncInfo;

  // Stack allocator and property helpers.
  uintptr_t osi_malloc;
  uintptr_t osi_free;
  uintptr_t osi_property_get_bool;
  uintptr_t osi_property_get_int32;

  // libc++ recursive_mutex, used to take the codec object's own lock the way
  // the stack's own accessors do.
  uintptr_t recursive_mutex_lock;
  uintptr_t recursive_mutex_unlock;

  // Generic vendor codec dispatchers. These are the functions that currently
  // have no entry for Apple's vendor ID and so must be taught about it.
  uintptr_t A2DP_IsVendorSourceCodecValid;
  uintptr_t A2DP_IsVendorPeerSourceCodecValid;
  uintptr_t A2DP_IsVendorSinkCodecSupported;
  uintptr_t A2DP_VendorCodecTypeEquals;
  uintptr_t A2DP_VendorSourceCodecIndex;
  uintptr_t A2DP_VendorSinkCodecIndex;
  uintptr_t A2DP_VendorInitCodecConfig;
  uintptr_t A2DP_VendorGetBitRate;
  uintptr_t A2DP_VendorUsesRtpHeader;
  uintptr_t A2DP_VendorBuildCodecHeader;
  uintptr_t A2DP_VendorGetEncoderInterface;
  uintptr_t A2DP_VendorCodecInfoString;
  uintptr_t A2DP_VendorCodecIndexStr;
  uintptr_t A2DP_VendorCodecName;
  uintptr_t A2DP_VendorAdjustCodec;
  uintptr_t A2DP_VendorGetSinkTrackChannelType;

  // Not a vendor dispatcher. It carries its own fixed list of codec IDs and
  // returns false for anything else, and its only caller,
  // bta_av_co_audio_source_data_path, then frees the media packet instead of
  // sending it. Without a hook here no AAC-ELD packet ever reaches AVDTP.
  uintptr_t A2DP_GetPacketTimestamp;

  // Android 17's stack has no A2DP_VendorCodecName and no
  // A2DP_VendorCodecIndexStr; the generic functions do the vendor codecs
  // themselves. They are hooked instead where the vendor ones are missing.
  uintptr_t A2DP_CodecName;
  uintptr_t A2DP_CodecIndexStr;

  // The codec object whose slot is taken over. Opus is registered in the
  // priority table but gated off by an unset property, and no AirPod speaks it.
  uintptr_t OpusBase_setCodecConfig;
  uintptr_t OpusBase_setPeerCodecCapabilities;
  // The Opus source codec's constructor, which is handed its priority.
  // Optional: hooked to set where the slot ranks among the codecs.
  uintptr_t OpusSource_ctor;

  // MIUI pins whichever codec it remembers for a device at priority 1000000,
  // which outranks the slot this module uses. Rather than sabotaging the stock
  // AAC codec, the pinned request itself is redirected to this module's slot.
  uintptr_t Codecs_setCodecUserConfig;   // not required, not hooked at present

  // The AVDTP layer writes the RTP header itself, so this is the only place
  // the finished on-air packet can be inspected. Media payloads are absent
  // from this device's HCI snoop, so there is no other way to see them.
  uintptr_t avdt_ad_write_req;

  // Only called, never hooked: the accessors the layout is measured with.
  uintptr_t CodecConfig_getCodecConfig;
  uintptr_t CodecConfig_getCodecUserConfig;
  uintptr_t CodecConfig_copyOutOtaCodecConfig;

  // Optional: an AVDTP suspend and start, for persist.aaceld.recover_kick_ms.
  uintptr_t btif_av_stream_suspend;
  uintptr_t btif_av_stream_start;

  // Optional: for starting the audio session after a codec change that the
  // stack leaves without one (see h_SourceSetupCodec). The first is hooked,
  // the other two are called.
  uintptr_t btif_a2dp_source_setup_codec;
  uintptr_t a2dp_is_hal_offloading;
  uintptr_t a2dp_start_session;

  // Optional, hooked: where the stack acts on "Opus is the low-latency codec"
  // (see h_SetLowLatencyCodec). A build has one or the other.
  uintptr_t btif_av_source_set_low_latency_codec;
  uintptr_t invoke_switch_codec_cb;
};

// Addresses relative to the library's load address, filled by target_resolve.
extern TargetOffsets kOffsets;

// Where the members of A2dpCodecConfig are, measured at start-up.
//
// The class is, in AOSP and in Qualcomm's and Xiaomi's forks alike:
//
//   vtable, std::recursive_mutex codec_mutex_, codec_index_, name_, ...
//   codec_priority_, default_codec_priority_,
//   codec_config_, [codec_capability_,] codec_local_capability_,
//   codec_selectable_capability_, codec_user_config_, codec_audio_config_,
//   ota_codec_config_[N], ota_codec_peer_capability_[N], ota_codec_peer_config_[N]
//
// What differs between builds is what sits before codec_priority_, whether
// codec_capability_ exists (AOSP has it, the build this was developed on does
// not) and N (20 in AOSP, 45 on that build). So three things are measured,
// each by calling the stack's own accessor on a dummy object filled with a
// pattern that says where every byte came from:
//
//   getCodecConfig()         -> where codec_config_ is, and how big one is
//   getCodecUserConfig()     -> where codec_user_config_ is
//   copyOutOtaCodecConfig()  -> where ota_codec_config_ is, and N
//
// and the rest follows from the order above, counted from codec_user_config_
// so that codec_capability_ being there or not makes no difference. The
// measurements are cross-checked against each other before anything is used.
struct CodecLayout {
  bool   ok = false;
  size_t mutex = 8;            // right after the vtable pointer
  size_t priority = 0;
  size_t config = 0;
  size_t local_capability = 0;
  size_t selectable_capability = 0;
  size_t user_config = 0;
  size_t audio_config = 0;
  size_t ota_config = 0;
  size_t ota_peer_capability = 0;
  size_t ota_peer_config = 0;
  size_t ota_size = 0;         // AVDT_CODEC_SIZE of this build
};
extern CodecLayout g_layout;

// btav_a2dp_codec_config_t, 56 bytes.
enum {
  CFG_CODEC_TYPE      = 0x00,
  CFG_CODEC_PRIORITY  = 0x04,
  CFG_SAMPLE_RATE     = 0x08,
  CFG_BITS_PER_SAMPLE = 0x0c,
  CFG_CHANNEL_MODE    = 0x10,
  CFG_SPECIFIC_1      = 0x18,
  CFG_SPECIFIC_2      = 0x20,
  CFG_SPECIFIC_3      = 0x28,
  CFG_SPECIFIC_4      = 0x30,
  CFG_SIZE            = 0x38,
};

// btav_a2dp_codec_* bit masks from the framework headers.
enum {
  BTAV_SAMPLE_RATE_48000     = 0x2,
  BTAV_BITS_PER_SAMPLE_16    = 0x1,
  BTAV_CHANNEL_MODE_MONO     = 0x1,
  BTAV_CHANNEL_MODE_STEREO   = 0x2,
};

// The codec index whose slot is reused: SOURCE_OPUS, asked of the stack at
// start-up (6 on every build seen so far).
extern int g_eld_codec_index;
#define ELD_CODEC_INDEX (g_eld_codec_index)

// Written to codec_specific_1 of every config this module puts in the Opus
// slot: Apple's vendor ID 0x004C and codec ID 0x8001. The Java side sees it
// as BluetoothCodecConfig.getCodecSpecific1(), which is the only way anything
// above the stack can tell an AAC-ELD session from a genuine Opus one, since
// both report codec type 6. label/ uses it to name the codec in Settings.
#define ELD_CODEC_SPECIFIC_MARK 0x004C8001LL

// BT_HDR, and the two framing constants read out of a2dp_aac_send_frames:
// osi_malloc(0x1010) and an offset of 23.
struct BT_HDR {
  uint16_t event;
  uint16_t len;
  uint16_t offset;
  uint16_t layer_specific;
};
#define BT_DEFAULT_BUFFER_SIZE 4112
#define AVDT_MEDIA_OFFSET      23

struct A2dpEncoderInitPeerParams {
  bool     is_peer_edr;
  bool     peer_supports_3mbps;
  uint16_t peer_mtu;
};

typedef uint32_t (*a2dp_read_cb_t)(uint8_t* buf, uint32_t len);
typedef bool (*a2dp_enqueue_cb_t)(BT_HDR* buf, size_t frames_n, uint32_t bytes_read);

// The eight slot table A2DP_VendorGetEncoderInterface hands back, confirmed by
// resolving the LDAC table at 0xe06528 and the aptX one at 0xe06468.
struct A2dpEncoderInterface {
  void     (*encoder_init)(const A2dpEncoderInitPeerParams*, void* codec_config,
                           a2dp_read_cb_t, a2dp_enqueue_cb_t);
  void     (*encoder_cleanup)(void);
  void     (*feeding_reset)(void);
  void     (*feeding_flush)(void);
  uint64_t (*get_encoder_interval_ms)(void);
  int      (*get_effective_frame_size)(void);
  void     (*send_frames)(uint64_t timestamp_us);
  void     (*set_transmit_queue_length)(size_t len);
};

struct StackApi {
  uintptr_t base;
  aacEncOpen_t          aacEncOpen;
  aacEncClose_t         aacEncClose;
  aacEncEncode_t        aacEncEncode;
  aacEncoder_SetParam_t aacEncoder_SetParam;
  aacEncInfo_t          aacEncInfo;
  void* (*osi_malloc)(size_t);
  void  (*osi_free)(void*);
  bool  (*osi_property_get_bool)(const char*, bool);
  int   (*osi_property_get_int32)(const char*, int);
  void  (*recursive_mutex_lock)(void*);
  void  (*recursive_mutex_unlock)(void*);
  // Optional, for the recovery kick only.
  void  (*btif_av_stream_suspend)();
  void  (*btif_av_stream_start)(int a2dp_type);
  // Optional, for the session start after a codec change only.
  bool  (*a2dp_is_hal_offloading)();
  void  (*a2dp_start_session)();
};

extern StackApi g_stack;

// Finds the loaded stack library and looks up every function this module
// needs by name. Returns false if the library is not loaded yet. Sets
// g_target_unusable, and returns false, if it is loaded but a name is
// missing; then nothing may be hooked, now or later.
bool target_resolve();
extern bool g_target_unusable;

// Measures the codec object's layout (see CodecLayout). The first two
// measurements need nothing from this module and are made before any hook is
// installed; the third needs the stack to accept Apple's codec element as a
// valid one, so it is made right after. Both return false if the result does
// not pass its checks, and leave g_layout.ok false.
bool target_measure_getters();
bool target_measure_ota();

// target_resolve leaves a marker behind that makes the next start stay out of
// this build if this one never finishes. Called when start-up is over, with
// or without success.
void target_startup_done();

// True if the stack's encoder interface counts its interval in microseconds
// (newer AOSP) rather than milliseconds.
extern bool g_interval_in_us;

static inline void* target_addr(uintptr_t vaddr) {
  return reinterpret_cast<void*>(g_stack.base + vaddr);
}
