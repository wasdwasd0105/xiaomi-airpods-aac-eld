# Device baseline (recorded before any change)

Device: Xiaomi 17 Pro Max (2509FPN0BC, codename `popsicle`)
SoC: SM8850 (platform `canoe`), arm64-v8a
OS: HyperOS 3.0 / Android 16 (SDK 36), build `OS3.0.319.0.WPBCNXM`, patch 2026-08-01
Kernel: 6.12.23-android16, SELinux enforcing
Root: KernelSU (ksud 3.4.2), NeoZygisk v2.4, Vector (Xposed-compatible) v2.2

## Properties to restore if needed

| Property | Baseline value |
|---|---|
| ro.bluetooth.a2dp_offload.supported | true |
| persist.bluetooth.a2dp_offload.disabled | false |
| persist.bluetooth.a2dp_offload.cap | sbc-aac-aptx-aptxhd-ldac-lhdc-mihc |
| persist.vendor.qcom.bluetooth.a2dp_offload_codec_extensibility | false |
| ro.bluetooth.library_name | libbluetooth_qti.so |

Rollback is `setprop persist.bluetooth.a2dp_offload.disabled false` plus a Bluetooth restart.
