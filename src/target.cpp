// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) the developer
#include "target.h"
#include "apple_eld.h"
#include "log.h"

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <unistd.h>

// xDL, which the hook engine bundles. xdl_dsym reads .symtab and, failing
// that, the LZMA-compressed mini debug symbols in .gnu_debugdata.
extern "C" {
void* xdl_open(const char* filename, int flags);
void* xdl_close(void* handle);
void* xdl_sym(void* handle, const char* symbol, size_t* symbol_size);
void* xdl_dsym(void* handle, const char* symbol, size_t* symbol_size);
}

TargetOffsets kOffsets = {};
StackApi g_stack = {};
CodecLayout g_layout;
int g_eld_codec_index = -1;
bool g_target_unusable = false;
bool g_interval_in_us = false;

namespace {

// Every function by its mangled name. The parameter types are part of the
// name, so each of these is also a check that the function still takes what
// this module passes it.
//
// Four of the vendor dispatchers are not required. The Android 16 stack this
// was developed on has them; the Android 17 one (HyperOS 4) has dropped
// A2DP_VendorAdjustCodec and A2DP_VendorGetBitRate and folded
// A2DP_VendorCodecName and A2DP_VendorCodecIndexStr into the generic
// functions. None of them decides whether audio flows: two give names for
// logs and dumps, one reports a bit rate, one tidies a codec element.
struct Name {
  uintptr_t TargetOffsets::*field;
  const char* symbol;
  bool required;
};

const Name kNames[] = {
    {&TargetOffsets::aacEncOpen, "aacEncOpen", true},
    {&TargetOffsets::aacEncClose, "aacEncClose", true},
    {&TargetOffsets::aacEncEncode, "aacEncEncode", true},
    {&TargetOffsets::aacEncoder_SetParam, "aacEncoder_SetParam", true},
    {&TargetOffsets::aacEncInfo, "aacEncInfo", true},

    {&TargetOffsets::osi_malloc, "_Z10osi_mallocm", true},
    {&TargetOffsets::osi_free, "_Z8osi_freePv", true},
    {&TargetOffsets::osi_property_get_bool, "_Z21osi_property_get_boolPKcb", true},
    {&TargetOffsets::osi_property_get_int32, "_Z22osi_property_get_int32PKci", true},

    {&TargetOffsets::recursive_mutex_lock, "_ZNSt3__115recursive_mutex4lockEv", true},
    {&TargetOffsets::recursive_mutex_unlock, "_ZNSt3__115recursive_mutex6unlockEv", true},

    {&TargetOffsets::A2DP_IsVendorSourceCodecValid, "_Z29A2DP_IsVendorSourceCodecValidPKh", true},
    {&TargetOffsets::A2DP_IsVendorPeerSourceCodecValid, "_Z33A2DP_IsVendorPeerSourceCodecValidPKh", true},
    {&TargetOffsets::A2DP_IsVendorSinkCodecSupported, "_Z31A2DP_IsVendorSinkCodecSupportedPKh", true},
    {&TargetOffsets::A2DP_VendorCodecTypeEquals, "_Z26A2DP_VendorCodecTypeEqualsPKhS0_", true},
    {&TargetOffsets::A2DP_VendorSourceCodecIndex, "_Z27A2DP_VendorSourceCodecIndexPKh", true},
    {&TargetOffsets::A2DP_VendorSinkCodecIndex, "_Z25A2DP_VendorSinkCodecIndexPKh", true},
    {&TargetOffsets::A2DP_VendorInitCodecConfig,
     "_Z26A2DP_VendorInitCodecConfig23btav_a2dp_codec_index_tP14AvdtpSepConfig", true},
    {&TargetOffsets::A2DP_VendorGetBitRate, "_Z21A2DP_VendorGetBitRatePKh", false},
    {&TargetOffsets::A2DP_VendorUsesRtpHeader, "_Z24A2DP_VendorUsesRtpHeaderbPKh", true},
    {&TargetOffsets::A2DP_VendorBuildCodecHeader, "_Z27A2DP_VendorBuildCodecHeaderPKhP6BT_HDRt", true},
    {&TargetOffsets::A2DP_VendorGetEncoderInterface, "_Z30A2DP_VendorGetEncoderInterfacePKh", true},
    {&TargetOffsets::A2DP_VendorCodecInfoString, "_Z26A2DP_VendorCodecInfoStringPKh", true},
    {&TargetOffsets::A2DP_VendorCodecIndexStr, "_Z24A2DP_VendorCodecIndexStr23btav_a2dp_codec_index_t", false},
    {&TargetOffsets::A2DP_VendorCodecName, "_Z20A2DP_VendorCodecNamePKh", false},
    {&TargetOffsets::A2DP_VendorAdjustCodec, "_Z22A2DP_VendorAdjustCodecPh", false},
    {&TargetOffsets::A2DP_VendorGetSinkTrackChannelType, "_Z34A2DP_VendorGetSinkTrackChannelTypePKh", true},

    {&TargetOffsets::A2DP_GetPacketTimestamp, "_Z23A2DP_GetPacketTimestampPKhS0_Pj", true},
    {&TargetOffsets::A2DP_CodecName, "_Z14A2DP_CodecNamePKh", false},
    {&TargetOffsets::A2DP_CodecIndexStr, "_Z18A2DP_CodecIndexStr23btav_a2dp_codec_index_t", false},

    {&TargetOffsets::OpusBase_setCodecConfig, "_ZN23A2dpCodecConfigOpusBase14setCodecConfigEPKhbPh", true},
    {&TargetOffsets::OpusBase_setPeerCodecCapabilities,
     "_ZN23A2dpCodecConfigOpusBase24setPeerCodecCapabilitiesEPKh", true},

    {&TargetOffsets::OpusSource_ctor, "_ZN25A2dpCodecConfigOpusSourceC1E26btav_a2dp_codec_priority_t", false},

    {&TargetOffsets::Codecs_setCodecUserConfig,
     "_ZN10A2dpCodecs18setCodecUserConfigERK24btav_a2dp_codec_config_tPK30tA2DP_ENCODER_INIT_PEER_PARAMSPKhPhPbS9_S9_",
     false},

    {&TargetOffsets::avdt_ad_write_req, "_Z17avdt_ad_write_reqhP8AvdtpCcbP8AvdtpScbP6BT_HDR", true},

    {&TargetOffsets::CodecConfig_getCodecConfig, "_ZN15A2dpCodecConfig14getCodecConfigEv", true},
    {&TargetOffsets::CodecConfig_getCodecUserConfig, "_ZN15A2dpCodecConfig18getCodecUserConfigEv", true},
    {&TargetOffsets::CodecConfig_copyOutOtaCodecConfig, "_ZN15A2dpCodecConfig21copyOutOtaCodecConfigEPh", true},

    {&TargetOffsets::btif_av_stream_suspend, "_Z22btif_av_stream_suspendv", false},
    {&TargetOffsets::btif_av_stream_start, "_Z20btif_av_stream_start8A2dpType", false},

    // Only builds that keep the audio session across a codec change have the
    // first of these under this name, and only they need it.
    {&TargetOffsets::btif_a2dp_source_setup_codec,
     "_Z28btif_a2dp_source_setup_codecRK10RawAddressRNSt3__17promiseIvEE", false},
    {&TargetOffsets::a2dp_is_hal_offloading, "_ZN9bluetooth5audio4a2dp17is_hal_offloadingEv", false},
    {&TargetOffsets::a2dp_start_session, "_ZN9bluetooth5audio4a2dp13start_sessionEv", false},

    {&TargetOffsets::btif_av_source_set_low_latency_codec, "_Z36btif_av_source_set_low_latency_codecb", false},
    {&TargetOffsets::invoke_switch_codec_cb, "_Z22invoke_switch_codec_cbb", false},
};

// Not called and not hooked. Their presence under exactly these names says
// that the encoder interface this module fills in still has the shape it was
// written for: what encoder_init is handed, including the two callbacks'
// signatures, and what send_frames takes.
const char* const kShapeChecks[] = {
    "_Z21a2dp_aac_encoder_initPK30tA2DP_ENCODER_INIT_PEER_PARAMSP15A2dpCodecConfigPFjPhjEPFbP6BT_HDRmjE",
    "_Z20a2dp_aac_send_framesm",
};
// The stock AAC encoder's own interval function says which unit the stack
// expects from get_encoder_interval.
const char* const kIntervalMs = "_Z32a2dp_aac_get_encoder_interval_msv";
const char* const kIntervalUs = "_Z32a2dp_aac_get_encoder_interval_usv";

// Set to the build ID from the moment this module starts working on the
// library until it has finished, hooks and measurements included, and
// cleared then. If the Bluetooth process dies in between, the next start
// finds it and leaves the stack alone instead of dying again, so a build
// this module does not get on with costs one crash and not a crash loop.
// Under persist.bluetooth. because that is what this process may write.
const char* const kPropProbe = "persist.bluetooth.aaceld.probe";

char g_build_id[128] = "unknown";

struct FindResult {
  uintptr_t base = 0;
  const ElfW(Phdr)* phdr = nullptr;
  ElfW(Half) phnum = 0;
  char path[512] = {};
  bool found = false;
};

int phdr_cb(struct dl_phdr_info* info, size_t, void* data) {
  auto* out = static_cast<FindResult*>(data);
  if (info->dlpi_name == nullptr) return 0;
  const char* slash = strrchr(info->dlpi_name, '/');
  const char* leaf = slash ? slash + 1 : info->dlpi_name;
  if (strcmp(leaf, TARGET_LIB_NAME) != 0) return 0;
  out->base = static_cast<uintptr_t>(info->dlpi_addr);
  out->phdr = info->dlpi_phdr;
  out->phnum = info->dlpi_phnum;
  snprintf(out->path, sizeof(out->path), "%s", info->dlpi_name);
  out->found = true;
  return 1;
}

// Reads NT_GNU_BUILD_ID out of the mapped PT_NOTE segments and hex formats it.
bool read_build_id(const FindResult& lib, char* out, size_t out_len) {
  for (ElfW(Half) i = 0; i < lib.phnum; i++) {
    const ElfW(Phdr)& ph = lib.phdr[i];
    if (ph.p_type != PT_NOTE) continue;
    auto cur = reinterpret_cast<const uint8_t*>(lib.base + ph.p_vaddr);
    const uint8_t* end = cur + ph.p_memsz;
    while (cur + sizeof(ElfW(Nhdr)) <= end) {
      auto* nh = reinterpret_cast<const ElfW(Nhdr)*>(cur);
      const uint8_t* name = cur + sizeof(ElfW(Nhdr));
      const uint8_t* desc = name + ((nh->n_namesz + 3) & ~3u);
      if (desc + nh->n_descsz > end) break;
      if (nh->n_type == NT_GNU_BUILD_ID && nh->n_namesz == 4 &&
          memcmp(name, "GNU", 4) == 0) {
        if (out_len < nh->n_descsz * 2 + 1) return false;
        for (uint32_t b = 0; b < nh->n_descsz; b++) {
          snprintf(out + b * 2, 3, "%02x", desc[b]);
        }
        out[nh->n_descsz * 2] = '\0';
        return true;
      }
      cur = desc + ((nh->n_descsz + 3) & ~3u);
    }
  }
  return false;
}

void* lookup(void* handle, const char* symbol) {
  void* p = xdl_sym(handle, symbol, nullptr);
  if (p == nullptr) p = xdl_dsym(handle, symbol, nullptr);
  return p;
}

// ---- calling into the stack on a dummy object, guarded --------------------

sigjmp_buf g_jmp;
volatile pid_t g_probe_tid = 0;
const int kFaultSignals[] = {SIGSEGV, SIGBUS, SIGILL};
struct sigaction g_old_actions[3];

void on_fault(int sig, siginfo_t*, void*) {
  if (gettid() == g_probe_tid) siglongjmp(g_jmp, sig);
  // Somebody else's fault: put the real handlers back and return, so the
  // faulting instruction runs again and is reported the normal way.
  for (size_t i = 0; i < 3; i++) sigaction(kFaultSignals[i], &g_old_actions[i], nullptr);
}

void guard_begin() {
  g_probe_tid = gettid();
  struct sigaction sa = {};
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  for (size_t i = 0; i < 3; i++) sigaction(kFaultSignals[i], &sa, &g_old_actions[i]);
}

void guard_end() {
  for (size_t i = 0; i < 3; i++) sigaction(kFaultSignals[i], &g_old_actions[i], nullptr);
  g_probe_tid = 0;
}

bool marker_begin() {
  char value[PROP_VALUE_MAX] = {};
  __system_property_get(kPropProbe, value);
  if (value[0] != '\0' && strcmp(value, g_build_id) == 0) {
    LOGE("the last start on this build did not get through start-up, so it is not tried "
         "again and the stack is left alone. Delete %s to retry.", kPropProbe);
    return false;
  }
  __system_property_set(kPropProbe, g_build_id);
  value[0] = '\0';
  __system_property_get(kPropProbe, value);
  if (strcmp(value, g_build_id) != 0) {
    LOGW("%s could not be set, so a crash during start-up would repeat", kPropProbe);
  }
  return true;
}

}  // namespace

void target_startup_done() { __system_property_set(kPropProbe, ""); }

namespace {

constexpr size_t kDummySize = 0x2000;
constexpr size_t kOutSize = 256;
constexpr uint32_t kTag = 0xA5000000u;
constexpr size_t kPatternFrom = 0x30;   // past the vtable pointer and the mutex

}  // namespace

// Calls a member function that returns a struct by value: `this` in x0, the
// address the result is written to in x8. Written out so that the result
// lands in a buffer this module filled first, which is how the size of the
// struct is seen.
extern "C" void aaceld_call_sret(void* fn, void* self, void* out);
__asm__(
    ".text\n"
    ".global aaceld_call_sret\n"
    ".hidden aaceld_call_sret\n"
    ".type aaceld_call_sret,%function\n"
    "aaceld_call_sret:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  mov x29, sp\n"
    "  mov x9, x0\n"
    "  mov x0, x1\n"
    "  mov x8, x2\n"
    "  blr x9\n"
    "  ldp x29, x30, [sp], #16\n"
    "  ret\n"
    ".size aaceld_call_sret, .-aaceld_call_sret\n");

namespace {

// Calls one struct-returning accessor on a dummy object whose every word says
// its own offset, twice into differently filled buffers. What is the same in
// both was written by the accessor: its length is the struct's size, and its
// first word says where in the object it was copied from.
bool measure_struct_getter_from(uintptr_t fn_offset, const char* what, size_t pattern_from,
                               size_t* where, size_t* size, bool* below) {
  *below = false;
  auto* dummy = static_cast<uint8_t*>(calloc(1, kDummySize));
  if (dummy == nullptr) return false;
  for (size_t off = pattern_from; off + 4 <= kDummySize; off += 4) {
    uint32_t word = kTag | (uint32_t)off;
    memcpy(dummy + off, &word, 4);
  }
  uint8_t out_a[kOutSize], out_b[kOutSize];
  memset(out_a, 0xEE, sizeof(out_a));
  memset(out_b, 0x11, sizeof(out_b));

  bool called = false;
  guard_begin();
  if (sigsetjmp(g_jmp, 1) == 0) {
    aaceld_call_sret(target_addr(fn_offset), dummy, out_a);
    aaceld_call_sret(target_addr(fn_offset), dummy, out_b);
    called = true;
  }
  guard_end();
  free(dummy);
  if (!called) {
    LOGE("measuring %s: the accessor faulted on the dummy object", what);
    return false;
  }

  size_t n = 0;
  while (n < kOutSize && out_a[n] == out_b[n]) n++;
  uint32_t first = 0;
  memcpy(&first, out_a, 4);
  if (n >= 8 && n % 4 == 0 && first == 0) {
    *below = true;   // copied from the part of the dummy left empty
    return false;
  }
  if (n < 8 || n % 4 != 0 || (first & 0xFF000000u) != kTag) {
    LOGE("measuring %s: unexpected result, %zu bytes written, first word %08x", what, n, first);
    return false;
  }
  const size_t from = first & 0x00FFFFFFu;
  for (size_t i = 0; i < n; i += 4) {
    uint32_t word = 0;
    memcpy(&word, out_a + i, 4);
    if (word != (kTag | (uint32_t)(from + i))) {
      LOGE("measuring %s: result is not one contiguous copy (word %zu is %08x)", what, i / 4, word);
      return false;
    }
  }
  *where = from;
  *size = n;
  return true;
}

// Everything ahead of the pattern is left zero, which reads as an unlocked
// mutex and an empty name. That matters: on Android 17 getCodecConfig logs
// the codec's name before it copies, and a name made of pattern bytes could
// be a wild pointer. The name ends by 0x58 on every build seen, so the
// pattern starts there, and further down only if the struct turns out to
// begin earlier.
bool measure_struct_getter(uintptr_t fn_offset, const char* what, size_t* where, size_t* size) {
  const size_t starts[] = {0x58, 0x50, 0x48, 0x40, kPatternFrom};
  for (size_t from : starts) {
    bool below = false;
    if (measure_struct_getter_from(fn_offset, what, from, where, size, &below)) return true;
    if (!below) return false;
  }
  LOGE("measuring %s: not found", what);
  return false;
}

// copyOutOtaCodecConfig copies ota_codec_config_ out if the stack thinks it
// holds a valid source codec, which, with this module's hooks in, Apple's
// element is. So the element is put where ota_codec_config_ should be on an
// otherwise empty dummy object: if it comes back, that is the place, and the
// number of bytes copied is the array's size.
bool try_ota_at(size_t at, size_t* size) {
  auto* dummy = static_cast<uint8_t*>(calloc(1, kDummySize));
  if (dummy == nullptr) return false;
  uint8_t info[ELD_CODEC_INFO_LEN];
  eld_build_info(info, 256000, true, ELD_CH_STEREO);
  memcpy(dummy + at, info, sizeof(info));

  uint8_t out_a[kOutSize], out_b[kOutSize];
  memset(out_a, 0xEE, sizeof(out_a));
  memset(out_b, 0x11, sizeof(out_b));
  auto copy_out = reinterpret_cast<bool (*)(void*, uint8_t*)>(
      target_addr(kOffsets.CodecConfig_copyOutOtaCodecConfig));

  bool called = false, ok_a = false, ok_b = false;
  guard_begin();
  if (sigsetjmp(g_jmp, 1) == 0) {
    ok_a = copy_out(dummy, out_a);
    ok_b = copy_out(dummy, out_b);
    called = true;
  }
  guard_end();
  free(dummy);
  if (!called) {
    LOGE("measuring the OTA config: copyOutOtaCodecConfig faulted on the dummy object");
    return false;
  }
  if (!ok_a || !ok_b || memcmp(out_a, info, sizeof(info)) != 0) return false;
  size_t n = 0;
  while (n < kOutSize && out_a[n] == out_b[n]) n++;
  *size = n;
  return true;
}

}  // namespace

bool target_resolve() {
  if (g_stack.base != 0) return true;
  if (g_target_unusable) return false;

  FindResult lib;
  dl_iterate_phdr(phdr_cb, &lib);
  if (!lib.found) return false;

  if (!read_build_id(lib, g_build_id, sizeof(g_build_id))) {
    snprintf(g_build_id, sizeof(g_build_id), "none");
  }
  const bool known = strcmp(g_build_id, KNOWN_BUILD_ID) == 0;
  LOGI("%s, build ID %s: %s", lib.path, g_build_id,
       known ? "the build this module was developed on"
             : "not a build this module has been run on; everything is found by name and "
               "checked before use");

  if (!marker_begin()) {
    g_target_unusable = true;
    return false;
  }

  void* handle = xdl_open(lib.path, 0);
  if (handle == nullptr) {
    LOGE("could not open %s for symbol lookup, leaving the stack alone", lib.path);
    target_startup_done();
    g_target_unusable = true;
    return false;
  }

  int missing = 0, found = 0;
  for (const Name& n : kNames) {
    void* p = lookup(handle, n.symbol);
    if (p != nullptr) found++;
    if (p == nullptr) {
      if (n.required) {
        LOGE("not in this build: %s", n.symbol);
        missing++;
      }
      continue;
    }
    kOffsets.*(n.field) = reinterpret_cast<uintptr_t>(p) - lib.base;
  }
  for (const char* symbol : kShapeChecks) {
    if (lookup(handle, symbol) == nullptr) {
      LOGE("not in this build: %s", symbol);
      missing++;
    }
  }
  if (lookup(handle, kIntervalMs) != nullptr) {
    g_interval_in_us = false;
  } else if (lookup(handle, kIntervalUs) != nullptr) {
    g_interval_in_us = true;
  } else {
    LOGE("this build has neither %s nor %s", kIntervalMs, kIntervalUs);
    missing++;
  }
  xdl_close(handle);

  if (missing != 0) {
    LOGE("%d of the functions this module needs are missing or have a different signature "
         "in this build, so nothing will be hooked", missing);
    target_startup_done();
    g_target_unusable = true;
    return false;
  }

  g_stack.base = lib.base;
  g_stack.aacEncOpen  = reinterpret_cast<aacEncOpen_t>(target_addr(kOffsets.aacEncOpen));
  g_stack.aacEncClose = reinterpret_cast<aacEncClose_t>(target_addr(kOffsets.aacEncClose));
  g_stack.aacEncEncode = reinterpret_cast<aacEncEncode_t>(target_addr(kOffsets.aacEncEncode));
  g_stack.aacEncoder_SetParam =
      reinterpret_cast<aacEncoder_SetParam_t>(target_addr(kOffsets.aacEncoder_SetParam));
  g_stack.aacEncInfo = reinterpret_cast<aacEncInfo_t>(target_addr(kOffsets.aacEncInfo));
  g_stack.osi_malloc = reinterpret_cast<void* (*)(size_t)>(target_addr(kOffsets.osi_malloc));
  g_stack.osi_free = reinterpret_cast<void (*)(void*)>(target_addr(kOffsets.osi_free));
  g_stack.osi_property_get_bool =
      reinterpret_cast<bool (*)(const char*, bool)>(target_addr(kOffsets.osi_property_get_bool));
  g_stack.osi_property_get_int32 =
      reinterpret_cast<int (*)(const char*, int)>(target_addr(kOffsets.osi_property_get_int32));
  g_stack.recursive_mutex_lock =
      reinterpret_cast<void (*)(void*)>(target_addr(kOffsets.recursive_mutex_lock));
  g_stack.recursive_mutex_unlock =
      reinterpret_cast<void (*)(void*)>(target_addr(kOffsets.recursive_mutex_unlock));

  if (kOffsets.btif_av_stream_suspend != 0 && kOffsets.btif_av_stream_start != 0) {
    g_stack.btif_av_stream_suspend =
        reinterpret_cast<void (*)()>(target_addr(kOffsets.btif_av_stream_suspend));
    g_stack.btif_av_stream_start =
        reinterpret_cast<void (*)(int)>(target_addr(kOffsets.btif_av_stream_start));
  }
  if (kOffsets.a2dp_is_hal_offloading != 0 && kOffsets.a2dp_start_session != 0) {
    g_stack.a2dp_is_hal_offloading =
        reinterpret_cast<bool (*)()>(target_addr(kOffsets.a2dp_is_hal_offloading));
    g_stack.a2dp_start_session =
        reinterpret_cast<void (*)()>(target_addr(kOffsets.a2dp_start_session));
  }

  LOGI("found every function it needs by name (%d of %zu, the rest optional) at base %p; "
       "encoder interval is in %s",
       found, sizeof(kNames) / sizeof(kNames[0]), (void*)lib.base,
       g_interval_in_us ? "microseconds" : "milliseconds");
  return true;
}

bool target_measure_getters() {
  g_layout = CodecLayout{};

  size_t config = 0, config_size = 0, user = 0, user_size = 0;
  const bool measured =
      measure_struct_getter(kOffsets.CodecConfig_getCodecConfig, "codec_config_", &config, &config_size) &&
      measure_struct_getter(kOffsets.CodecConfig_getCodecUserConfig, "codec_user_config_", &user, &user_size);
  if (!measured) return false;

  // One btav_a2dp_codec_config_t is 56 bytes in every version of the
  // framework header this module's writes are laid out for.
  if (config_size != CFG_SIZE || user_size != CFG_SIZE) {
    LOGE("a codec config is %zu bytes here, this module writes %d byte ones", config_size, CFG_SIZE);
    return false;
  }
  // config, [capability,] local capability, selectable capability, user config.
  const size_t between = (user > config) ? (user - config) : 0;
  if (config < 0x40 || between % CFG_SIZE != 0 ||
      (between / CFG_SIZE != 3 && between / CFG_SIZE != 4)) {
    LOGE("codec_config_ at 0x%zx and codec_user_config_ at 0x%zx do not fit the class this "
         "module knows", config, user);
    return false;
  }

  g_layout.priority = config - 8;
  g_layout.config = config;
  g_layout.user_config = user;
  g_layout.selectable_capability = user - CFG_SIZE;
  g_layout.local_capability = user - 2 * CFG_SIZE;
  g_layout.audio_config = user + CFG_SIZE;
  g_layout.ota_config = user + 2 * CFG_SIZE;   // confirmed by target_measure_ota
  LOGI("measured: codec_config_ 0x%zx, codec_user_config_ 0x%zx, %s codec_capability_",
       config, user, between / CFG_SIZE == 4 ? "with" : "no");
  return true;
}

bool target_measure_ota() {
  if (g_layout.user_config == 0) return false;

  // Straight after codec_audio_config_ is where it belongs. The two
  // neighbours are tried too, in case a build has one config more or fewer
  // behind codec_user_config_.
  const size_t candidates[] = {g_layout.user_config + 2 * CFG_SIZE,
                               g_layout.user_config + 3 * CFG_SIZE,
                               g_layout.user_config + 1 * CFG_SIZE};
  size_t at = 0, size = 0;
  for (size_t c : candidates) {
    if (try_ota_at(c, &size)) { at = c; break; }
  }

  if (at == 0) {
    LOGE("ota_codec_config_ was not found behind codec_user_config_");
    return false;
  }
  if (size < ELD_CODEC_INFO_LEN || size > 96) {
    LOGE("ota_codec_config_ is %zu bytes here, which cannot hold or is not a codec element", size);
    return false;
  }
  g_layout.ota_config = at;
  g_layout.ota_size = size;
  g_layout.ota_peer_capability = at + size;
  g_layout.ota_peer_config = at + 2 * size;
  g_layout.audio_config = at - CFG_SIZE;
  g_layout.ok = true;
  LOGI("layout: priority 0x%zx config 0x%zx local 0x%zx selectable 0x%zx user 0x%zx audio 0x%zx "
       "ota 0x%zx peer capability 0x%zx peer config 0x%zx, %zu bytes each",
       g_layout.priority, g_layout.config, g_layout.local_capability,
       g_layout.selectable_capability, g_layout.user_config, g_layout.audio_config,
       g_layout.ota_config, g_layout.ota_peer_capability, g_layout.ota_peer_config,
       g_layout.ota_size);
  return true;
}
