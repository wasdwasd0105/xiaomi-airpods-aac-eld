// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) the developer
// Layout of Apple's AAC-ELD A2DP vendor codec element, as described in
// aac-eld-apple.md (in the USBPods-Pico2W repository) and as observed from
// AirPods SEP capabilities. See docs/HOW-IT-WORKS.md.
#pragma once
#include <stdint.h>
#include <string.h>

#define APPLE_VENDOR_ID    0x0000004Cu
#define APPLE_ELD_CODEC_ID 0x8001u

#define A2DP_MEDIA_TYPE_AUDIO    0x00
#define A2DP_CODEC_TYPE_NON_A2DP 0xFF

// 1 LOSC byte + media type + codec type + 14 bytes of codec specific info.
#define ELD_CODEC_INFO_LEN 17
#define ELD_LOSC           16

// Byte offsets inside a codec_info array.
enum {
  CI_LOSC        = 0,
  CI_MEDIA_TYPE  = 1,
  CI_CODEC_TYPE  = 2,
  CI_VENDOR_ID   = 3,   // uint32 little endian
  CI_CODEC_ID    = 7,   // uint16 little endian
  CI_OBJECT_TYPE = 9,   // uint16 big endian bitmap
  CI_FREQ_CHAN   = 11,  // uint16 big endian: freq in the top 12 bits, channels in the low 4
  CI_RESERVED    = 13,
  CI_VBR_RATE    = 14,  // 24 bit big endian: bit 23 is VBR, bits 22..0 are the bit rate
};

#define ELD_OBJ_AAC_ELD 0x0080

#define ELD_SF_48000  0x008
#define ELD_CH_MONO   0x8
#define ELD_CH_STEREO 0x4

#define ELD_VBR_BIT 0x800000u

// AAC-ELD at 48 kHz in Apple's 10 ms mode.
#define ELD_GRANULE_SAMPLES 480
#define ELD_SAMPLE_RATE     48000
#define ELD_CHANNELS        2
// The 12 bit size field in the per frame header cannot express more than this.
#define ELD_MAX_AU_BYTES    2047

static inline uint32_t eld_rd_u32le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint16_t eld_rd_u16le(const uint8_t* p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline uint16_t eld_rd_u16be(const uint8_t* p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}
static inline uint32_t eld_rd_u24be(const uint8_t* p) {
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

// True when this codec element is Apple's AAC-ELD vendor codec.
static inline bool eld_is_apple(const uint8_t* ci) {
  if (ci == nullptr) return false;
  if (ci[CI_LOSC] < ELD_LOSC) return false;
  if ((ci[CI_MEDIA_TYPE] & 0xF0) != A2DP_MEDIA_TYPE_AUDIO) return false;
  if (ci[CI_CODEC_TYPE] != A2DP_CODEC_TYPE_NON_A2DP) return false;
  return eld_rd_u32le(ci + CI_VENDOR_ID) == APPLE_VENDOR_ID &&
         eld_rd_u16le(ci + CI_CODEC_ID) == APPLE_ELD_CODEC_ID;
}

static inline uint16_t eld_freq_bitmap(const uint8_t* ci) {
  return (uint16_t)(eld_rd_u16be(ci + CI_FREQ_CHAN) >> 4);
}
static inline uint8_t eld_chan_bitmap(const uint8_t* ci) {
  return (uint8_t)(eld_rd_u16be(ci + CI_FREQ_CHAN) & 0x0F);
}
static inline uint32_t eld_peer_max_bitrate(const uint8_t* ci) {
  return eld_rd_u24be(ci + CI_VBR_RATE) & ~ELD_VBR_BIT;
}

// Writes the 17 byte element for 48 kHz stereo AAC-ELD at the given bit rate.
static inline void eld_build_info(uint8_t* out, uint32_t bitrate, bool vbr, uint8_t chan_bitmap) {
  memset(out, 0, ELD_CODEC_INFO_LEN);
  out[CI_LOSC]       = ELD_LOSC;
  out[CI_MEDIA_TYPE] = A2DP_MEDIA_TYPE_AUDIO;
  out[CI_CODEC_TYPE] = A2DP_CODEC_TYPE_NON_A2DP;
  out[CI_VENDOR_ID + 0] = (uint8_t)(APPLE_VENDOR_ID & 0xFF);
  out[CI_VENDOR_ID + 1] = (uint8_t)((APPLE_VENDOR_ID >> 8) & 0xFF);
  out[CI_VENDOR_ID + 2] = (uint8_t)((APPLE_VENDOR_ID >> 16) & 0xFF);
  out[CI_VENDOR_ID + 3] = (uint8_t)((APPLE_VENDOR_ID >> 24) & 0xFF);
  out[CI_CODEC_ID + 0]  = (uint8_t)(APPLE_ELD_CODEC_ID & 0xFF);
  out[CI_CODEC_ID + 1]  = (uint8_t)((APPLE_ELD_CODEC_ID >> 8) & 0xFF);
  out[CI_OBJECT_TYPE + 0] = (uint8_t)((ELD_OBJ_AAC_ELD >> 8) & 0xFF);
  out[CI_OBJECT_TYPE + 1] = (uint8_t)(ELD_OBJ_AAC_ELD & 0xFF);
  uint16_t fc = (uint16_t)((ELD_SF_48000 << 4) | (chan_bitmap & 0x0F));
  out[CI_FREQ_CHAN + 0] = (uint8_t)((fc >> 8) & 0xFF);
  out[CI_FREQ_CHAN + 1] = (uint8_t)(fc & 0xFF);
  out[CI_RESERVED] = 0;
  uint32_t rate = (bitrate & ~ELD_VBR_BIT) | (vbr ? ELD_VBR_BIT : 0u);
  out[CI_VBR_RATE + 0] = (uint8_t)((rate >> 16) & 0xFF);
  out[CI_VBR_RATE + 1] = (uint8_t)((rate >> 8) & 0xFF);
  out[CI_VBR_RATE + 2] = (uint8_t)(rate & 0xFF);
}
