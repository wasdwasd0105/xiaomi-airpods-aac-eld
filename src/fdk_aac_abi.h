// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) the developer
// Minimal ABI declarations for the FDK-AAC encoder that is statically linked
// into libbluetooth_jni.so. Only what this module calls is declared here, so
// there is no dependency on an external FDK-AAC checkout and no chance of
// linking against a different build than the one in the Bluetooth process.
#pragma once
#include <stdint.h>

typedef signed int   INT;
typedef unsigned int UINT;
typedef void*        HANDLE_AACENCODER;

// AACENC_ERROR
enum { AACENC_OK = 0x0000 };

// AACENC_BufferIdentifier
enum {
  IN_AUDIO_DATA      = 0,
  OUT_BITSTREAM_DATA = 3,
};

// AACENC_PARAM, confirmed present in this build: aacEncoder_SetParam
// dispatches on 0x100..0x108, 0x200, 0x202, 0x203, 0x207, 0x300..0x306.
enum {
  AACENC_AOT            = 0x0100,
  AACENC_BITRATE        = 0x0101,
  AACENC_BITRATEMODE    = 0x0102,
  AACENC_SAMPLERATE     = 0x0103,
  AACENC_SBR_MODE       = 0x0104,
  AACENC_GRANULE_LENGTH = 0x0105,
  AACENC_CHANNELMODE    = 0x0106,
  AACENC_CHANNELORDER   = 0x0107,
  AACENC_SBR_RATIO      = 0x0108,
  AACENC_AFTERBURNER    = 0x0200,
  AACENC_PEAK_BITRATE   = 0x0207,
  AACENC_TRANSMUX       = 0x0300,
  AACENC_SIGNALING_MODE = 0x0302,
};

// AUDIO_OBJECT_TYPE
enum { AOT_ER_AAC_ELD = 39 };

// TRANSPORT_TYPE
enum { TT_MP4_RAW = 0 };

// CHANNEL_MODE
enum { MODE_1 = 1, MODE_2 = 2 };

typedef struct {
  INT    numBufs;
  void** bufs;
  INT*   bufferIdentifiers;
  INT*   bufSizes;
  INT*   bufElSizes;
} AACENC_BufDesc;

typedef struct {
  INT numInSamples;
  INT numAncBytes;
} AACENC_InArgs;

// numOutBytes/numInSamples/numAncBytes are the historic three fields.
// FDK gained bitResState later; the trailing pad keeps this struct at least as
// large as any released layout so the encoder cannot write past our storage.
typedef struct {
  INT  numOutBytes;
  INT  numInSamples;
  INT  numAncBytes;
  UINT bitResState;
  UINT reserved[4];
} AACENC_OutArgs;

typedef INT (*aacEncOpen_t)(HANDLE_AACENCODER*, UINT encModules, UINT maxChannels);
typedef INT (*aacEncClose_t)(HANDLE_AACENCODER*);
typedef INT (*aacEncEncode_t)(HANDLE_AACENCODER, const AACENC_BufDesc* inBufDesc,
                              const AACENC_BufDesc* outBufDesc,
                              const AACENC_InArgs* inargs, AACENC_OutArgs* outargs);
typedef INT (*aacEncoder_SetParam_t)(HANDLE_AACENCODER, INT param, UINT value);
// The real AACENC_InfoStruct layout has shifted across FDK releases, so this
// is read as raw bytes and interpreted outside rather than declared here.
typedef INT (*aacEncInfo_t)(HANDLE_AACENCODER, void* info);
