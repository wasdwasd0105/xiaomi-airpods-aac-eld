// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) the developer
// Runs the module's start-up against a copy of a Bluetooth stack library in a
// scratch process: name lookup, layout measurement, hook installation, and
// then a few calls through the hooked functions on objects the stack itself
// builds. For checking a build of the stack that is not the one on the phone.
//
//   aaceld_harness /data/local/tmp/<dir>/libbluetooth_jni.so
//
// The library's own dependencies come from LD_LIBRARY_PATH; see
// tools/harness/run.sh. Nothing here talks to Bluetooth: the stack is only
// loaded, never started.
#include "target.h"
#include "log.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <shadowhook.h>

bool hooks_install();

namespace {

// Apple's capability element as the stack hands it around: length, media
// type, codec type, then the 14 bytes of aac-eld-apple.md.
const uint8_t kAppleCapability[45] = {0x10, 0x00, 0xff, 0x4c, 0x00, 0x00, 0x00, 0x01, 0x80,
                                      0x00, 0x80, 0x00, 0x8c, 0x00, 0x83, 0xe8, 0x00};

int in_child(const char* what, int (*fn)()) {
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) _exit(fn());
  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFSIGNALED(status)) {
    printf("FAIL  %s: died with signal %d\n", what, WTERMSIG(status));
    return 1;
  }
  printf("%s  %s\n", WEXITSTATUS(status) == 0 ? "ok  " : "FAIL", what);
  return WEXITSTATUS(status) == 0 ? 0 : 1;
}

int test_low_latency() {
  if (kOffsets.invoke_switch_codec_cb != 0) {
    reinterpret_cast<void (*)(bool)>(target_addr(kOffsets.invoke_switch_codec_cb))(false);
    reinterpret_cast<void (*)(bool)>(target_addr(kOffsets.invoke_switch_codec_cb))(true);
    printf("      invoke_switch_codec_cb(false/true) returned\n");
  }
  if (kOffsets.btif_av_source_set_low_latency_codec != 0) {
    reinterpret_cast<void (*)(bool)>(target_addr(kOffsets.btif_av_source_set_low_latency_codec))(false);
    reinterpret_cast<void (*)(bool)>(target_addr(kOffsets.btif_av_source_set_low_latency_codec))(true);
    printf("      btif_av_source_set_low_latency_codec(false/true) returned\n");
  }
  return 0;
}

// Has the stack build its own Opus source codec object with the priority the
// Android 17 build gives it, and reads back what the object ended up with.
void* g_object = nullptr;

int test_ctor() {
  if (kOffsets.OpusSource_ctor == 0) { printf("      no constructor in this build\n"); return 0; }
  reinterpret_cast<void (*)(void*, int)>(target_addr(kOffsets.OpusSource_ctor))(g_object, 1501);
  const int priority = *reinterpret_cast<int*>(static_cast<char*>(g_object) + g_layout.priority);
  printf("      constructed with 1501, object holds priority %d\n", priority);
  return priority == 8001 ? 0 : 1;
}

// The same object, then handed Apple's capability as the peer's, the way the
// stack does when AirPods connect.
int test_set_config() {
  if (kOffsets.OpusSource_ctor == 0) return 0;
  reinterpret_cast<void (*)(void*, int)>(target_addr(kOffsets.OpusSource_ctor))(g_object, 1501);
  uint8_t result[64] = {};
  auto set_caps = reinterpret_cast<int (*)(void*, const uint8_t*)>(
      target_addr(kOffsets.OpusBase_setPeerCodecCapabilities));
  auto set_config = reinterpret_cast<int (*)(void*, const uint8_t*, bool, uint8_t*)>(
      target_addr(kOffsets.OpusBase_setCodecConfig));
  const int caps = set_caps(g_object, kAppleCapability);
  const int rc = set_config(g_object, kAppleCapability, true, result);
  printf("      setPeerCodecCapabilities -> %d, setCodecConfig -> %d, result:", caps, rc);
  for (int i = 0; i < 17; i++) printf(" %02x", result[i]);
  printf("\n");
  // What the object now says through the stack's own accessors.
  uint8_t ota[64] = {};
  reinterpret_cast<int (*)(void*, uint8_t*)>(
      target_addr(kOffsets.CodecConfig_copyOutOtaCodecConfig))(g_object, ota);
  printf("      the object's over-the-air config:");
  for (int i = 0; i < 17; i++) printf(" %02x", ota[i]);
  printf("\n");
  // The configuration the source sends back: 48 kHz stereo, AAC-ELD, 256000.
  static const uint8_t want[17] = {0x10, 0x00, 0xff, 0x4c, 0x00, 0x00, 0x00, 0x01, 0x80,
                                   0x00, 0x80, 0x00, 0x84, 0x00, 0x83, 0xe8, 0x00};
  return memcmp(ota, want, sizeof(want)) == 0 && memcmp(result, want, sizeof(want)) == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s /path/to/libbluetooth_jni.so\n", argv[0]); return 2; }
  setvbuf(stdout, nullptr, _IONBF, 0);
  void* lib = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
  if (lib == nullptr) { printf("FAIL  could not load %s: %s\n", argv[1], dlerror()); return 3; }
  printf("ok    loaded %s\n", argv[1]);

  int err = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, true);
  if (err != SHADOWHOOK_ERRNO_OK) { printf("FAIL  shadowhook_init: %s\n", shadowhook_to_errmsg(err)); return 3; }

  if (!target_resolve()) { printf("FAIL  name lookup (see logcat -s AacEldA2dp)\n"); target_startup_done(); return 1; }
  printf("ok    name lookup\n");
  const bool installed = hooks_install();
  target_startup_done();
  if (!installed) { printf("FAIL  layout measurement or hook installation (see logcat)\n"); return 1; }
  printf("ok    layout measured and hooks installed: priority %#zx config %#zx user %#zx ota %#zx, codec index %d\n",
         g_layout.priority, g_layout.config, g_layout.user_config, g_layout.ota_config, g_eld_codec_index);

  g_object = mmap(nullptr, 1 << 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  int failed = 0;
  failed += in_child("low-latency requests are swallowed", test_low_latency);
  failed += in_child("the stack's Opus constructor ends up with priority 8001", test_ctor);
  failed += in_child("a stack-built object accepts Apple's capability and configures AAC-ELD", test_set_config);
  printf("%s\n", failed == 0 ? "PASS" : "FAILED");
  return failed == 0 ? 0 : 1;
}
