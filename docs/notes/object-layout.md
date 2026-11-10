# `A2dpCodecConfig` object layout

Reversed from the constructor at 0x8e90d8, the destructor at 0x8e920c,
`getCodecConfig()` at 0x8ea320, `getCodecUserConfig()` at 0x8ea378 and
`debug_codec_dump()` at 0x8eb844 in this device's `libbluetooth_jni.so`.
Note this is Qualcomm/Xiaomi's fork, so it is not identical to AOSP.

Since v0.10 the module does not use this table. It measures the layout at
start-up (`target_measure_getters` and `target_measure_ota` in
`src/target.cpp`), and on this build the measurement gives exactly the
offsets below: priority 0x58, config 0x60, local capability 0x98, selectable
capability 0xd0, user config 0x108, audio config 0x140, OTA config 0x178,
peer capability 0x1a5, peer config 0x1d2, 45 bytes each. The table stays as
the record of what the measurement was checked against.

| Offset | Size | Member | Evidence |
|---|---|---|---|
| +0x00 | 8 | vptr, vtable at 0xde93b0 | `str x8,[x0],#8` in ctor, rewritten in dtor |
| +0x08 | 40 | `std::recursive_mutex` | `add x0,x0,#8` then lock at 0x31d640, unlock at 0x31d66c |
| +0x30 | 4 | `codec_index_` | `str w20,[x19,#0x30]`, w20 is arg 1 |
| +0x38 | 8 | `codec_id_` (`bluetooth::a2dp::CodecId`) | `str x23,[x19,#0x38]`, x23 is arg 2 |
| +0x40 | 24 | `name_` (`std::string`) | libc++ triple copied from arg 3; dtor tests the SSO bit at +0x40 |
| +0x58 | 4 | `codec_priority_`, effective | `stp w8,w21,[x19,#0x58]` |
| +0x5c | 4 | `default_codec_priority_`, as passed | same instruction |
| +0x60 | 56 | `codec_config_` | `getCodecConfig()` copies exactly 56 bytes from +0x60 |
| +0x98 | 56 | `codec_local_capability_` | `debug_codec_dump` prints "Local capability" from here |
| +0xd0 | 56 | `codec_selectable_capability_` | `debug_codec_dump` prints "Selectable" from here; `A2dpCodecs::getCodecConfigAndCapabilities` (0x8ee2f4) tests +0xd8, +0xdc, +0xe0 before listing the codec |
| +0x108 | 56 | `codec_user_config_` | `getCodecUserConfig()` copies 56 bytes from here |
| +0x140 | 56 | `codec_audio_config_` | what is left |
| +0x178 | 45 | `ota_codec_config_` | three consecutive 45-byte zero fills |
| +0x1a5 | 45 | `ota_codec_peer_capability_` | `add x8,x19,#0x1a5` |
| +0x1d2 | 45 | `ota_codec_peer_config_` | `add x8,x19,#0x1d2` |

Total size is 0x1ff, so 0x200 allocated.

Two things worth noting, because they differ from upstream AOSP:

- There are five `btav_a2dp_codec_config_t` members, not six. The one that
  is gone is AOSP's `codec_capability_`. An earlier version of this table
  guessed the order from the constructor alone and had the last four one slot
  off; the module then wrote its selectable capability into the user config,
  and Java never saw AAC-ELD among a headset's selectable codecs. Fixed in
  module v0.5.
- The on-the-air arrays are 45 bytes, not AOSP's 20. Qualcomm raises
  `AVDT_CODEC_SIZE` to 45 to fit aptX Adaptive. The Apple AAC-ELD element needs 17
  bytes, so it fits comfortably.

`btav_a2dp_codec_config_t` itself is 56 bytes:

| Offset | Field |
|---|---|
| +0x00 | `codec_type` |
| +0x04 | `codec_priority` |
| +0x08 | `sample_rate` |
| +0x0c | `bits_per_sample` |
| +0x10 | `channel_mode` |
| +0x14 | padding |
| +0x18 | `codec_specific_1` |
| +0x20 | `codec_specific_2` |
| +0x28 | `codec_specific_3` |
| +0x30 | `codec_specific_4` |
