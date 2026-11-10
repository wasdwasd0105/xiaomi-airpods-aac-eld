// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) the developer
// Zygisk entry point. Attaches only to com.android.bluetooth, waits for the
// Bluetooth stack library to be loaded, verifies it is the build this module
// was made for, and then installs the AAC-ELD hooks.
#include "target.h"
#include "log.h"

#include <sys/system_properties.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <shadowhook.h>

#include "zygisk.hpp"

bool hooks_install();
bool hooks_dry_run();

namespace {

constexpr const char* kBluetoothProcess = "com.android.bluetooth";
constexpr const char* kPropEnabled = "persist.aaceld.enabled";
// While true the module only validates its addresses and logs them. Nothing in
// the stack is patched. Off unless asked for: target_resolve and the layout
// measurement already keep the hooks out of a build they do not fit.
constexpr const char* kPropDryRun = "persist.aaceld.dryrun";

// libbluetooth_jni.so is loaded when the adapter service starts, which is well
// after the process specializes. Rather than hooking the linker to be told, we
// look for it on a short timer; the cost is negligible and it keeps the module
// free of any runtime dependency beyond itself.
constexpr long kPollIntervalMs = 50;
constexpr int  kPollAttempts = 2400;  // two minutes

bool prop_bool(const char* name, bool fallback) {
  char value[PROP_VALUE_MAX] = {};
  if (__system_property_get(name, value) <= 0) return fallback;
  return strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0;
}

bool g_dry_run = false;

void run_once() {
  if (g_dry_run) {
    hooks_dry_run();
  } else {
    hooks_install();
  }
  target_startup_done();
}

void* waiter(void*) {
  for (int i = 0; i < kPollAttempts; i++) {
    if (target_resolve()) {
      run_once();
      return nullptr;
    }
    if (g_target_unusable) return nullptr;   // loaded, but not a build this fits
    struct timespec ts = {.tv_sec = 0, .tv_nsec = kPollIntervalMs * 1000000L};
    nanosleep(&ts, nullptr);
  }
  LOGW("%s never appeared, giving up", TARGET_LIB_NAME);
  return nullptr;
}

class AacEldModule : public zygisk::ModuleBase {
 public:
  void onLoad(zygisk::Api* api, JNIEnv* env) override {
    api_ = api;
    env_ = env;
  }

  void preAppSpecialize(zygisk::AppSpecializeArgs* args) override {
    bool is_bluetooth = false;
    if (args != nullptr && args->nice_name != nullptr) {
      const char* name = env_->GetStringUTFChars(args->nice_name, nullptr);
      if (name != nullptr) {
        is_bluetooth = strcmp(name, kBluetoothProcess) == 0;
        env_->ReleaseStringUTFChars(args->nice_name, name);
      }
    }
    if (!is_bluetooth) {
      // Not our process: let Zygisk unload this module entirely.
      api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
      return;
    }
    wanted_ = true;
  }

  void postAppSpecialize(const zygisk::AppSpecializeArgs*) override {
    if (!wanted_) return;

    if (!prop_bool(kPropEnabled, true)) {
      LOGI("%s is false, leaving the stack untouched", kPropEnabled);
      return;
    }

    g_dry_run = prop_bool(kPropDryRun, false);
    LOGI("starting, dry run %s", g_dry_run ? "on" : "off");

    if (!g_dry_run) {
      int err = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, true);
      if (err != SHADOWHOOK_ERRNO_OK) {
        LOGE("shadowhook_init failed: %s", shadowhook_to_errmsg(err));
        return;
      }
      shadowhook_set_debuggable(true);
    }

    if (target_resolve()) {
      run_once();
      return;
    }
    if (g_target_unusable) return;

    pthread_t tid;
    if (pthread_create(&tid, nullptr, waiter, nullptr) == 0) {
      pthread_detach(tid);
      LOGI("waiting for %s to load", TARGET_LIB_NAME);
    } else {
      LOGE("could not start the waiter thread");
    }
  }

 private:
  zygisk::Api* api_ = nullptr;
  JNIEnv* env_ = nullptr;
  bool wanted_ = false;
};

}  // namespace

REGISTER_ZYGISK_MODULE(AacEldModule)
