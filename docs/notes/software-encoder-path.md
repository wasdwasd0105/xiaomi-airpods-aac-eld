# Where the software A2DP encoder lives on this device

Everything below was read off the device (Xiaomi 17 Pro Max, HyperOS 3.0 / Android 16,
SM8850) and out of its own `libbluetooth_jni.so`. Addresses are virtual addresses in
that library, build ID `ea12c5b9edc07b7e826bd2aaec5a5f5b`.

## 1. The two datapaths, and what chooses between them

The Bluetooth stack picks one of two datapaths when a stream starts. Both exist in the
shipped binaries; only the choice differs.

**The choice is made per codec, and nothing has to be switched.** With offload on, which
is the stock setting, `bluetooth::audio::aidl::a2dp::setup_codec` (0x71235c) asks
`codec::IsCodecOffloadingEnabled` (0x75dc90) whether the DSP can encode the selected
codec. The DSP's list is `persist.bluetooth.a2dp_offload.cap`,
`sbc-aac-aptx-aptxhd-ldac-lhdc-mihc`. The slot AAC-ELD borrows is not on it, so the stack
takes its software HAL session for that codec (the branch that logs "Switching
BluetoothAudio HAL to Software") and encodes in-process, while AAC, SBC and the rest stay
on the DSP.

What was observed on the device, from a clean boot with offload on
(`A2dpOffloadEnabled: true`): the AirPods negotiate AAC-ELD, the module's encoder starts
and its packets go out, and the headset plays them. Switching the headset to AAC from
Settings and back needs no reconnect, and audio flows in both states. The log line itself
was not captured.

The rest of this section describes the global switch, which is how the software path was
first reached and what the module used up to v0.4. It is not needed, and it has a cost:
with offload off for everything, this phone's audio HAL cannot route a standard codec to a
headset (`createAudioPatch` fails with -22 and the A2DP stream never starts), so plain AAC
and every other headset break. The module no longer sets it.

| Datapath | Who encodes | Session type string |
|---|---|---|
| Hardware offload | Qualcomm ADSP, via `android.hardware.bluetooth.audio-impl-qti.so` | `A2DP_HARDWARE_OFFLOAD_ENCODING_DATAPATH` |
| Software | the Bluetooth process itself, in `libbluetooth_jni.so` | `A2DP_SOFTWARE_ENCODING_DATAPATH` |

The global decision is made in Java, in one line:

```
AdapterProperties.java:505
mA2dpOffloadEnabled = SystemProperties.getBoolean("ro.bluetooth.a2dp_offload.supported", false)
                   && !SystemProperties.getBoolean("persist.bluetooth.a2dp_offload.disabled", false);
```

`ro.bluetooth.a2dp_offload.supported` is read-only and true. So the whole choice hangs on
one persistent property:

```bash
setprop persist.bluetooth.a2dp_offload.disabled true
```

After a Bluetooth restart the stack reports `A2dpOffloadEnabled: false` and the DSP is out
of the picture entirely. This was verified on the device. Nothing in `/vendor` is touched,
no DSP module is replaced, and the change is reversible with the same property.

A missing `audio_policy_configuration_a2dp_offload_disabled.xml` is harmless here: the
audio policy manager tries that filename first and falls back to the normal config. The
software A2DP port already exists either way, as the `bluetooth` HAL module (policy handle
34, mixPort `a2dp output`), registered by `android.hardware.bluetooth.audio_sw.so` through
`vendor_audio_interfaces.xml`.

## 2. The software encode path, function by function

Once offload is off, PCM arrives over a fast message queue and is encoded in-process. The
tick-driven chain is:

| Stage | Symbol | Address |
|---|---|---|
| Periodic encode tick | `btif_a2dp_source_audio_handle_timer` | 0x619da4 |
| Pull PCM from the HAL | `btif_a2dp_source_read_callback` | 0x618ff0 |
| Hand a packet to AVDTP | `btif_a2dp_source_enqueue_callback` | 0x619218 |
| Choose the encoder | `A2DP_GetEncoderInterface` | 0x8ef88c |
| Vendor codec dispatch | `A2DP_VendorGetEncoderInterface` | 0x8fecd4 |
| Set the HAL session up | `bluetooth::audio::a2dp::setup_codec` | 0x710b48 |
| Offload or not, at runtime | `bluetooth::audio::a2dp::is_hal_offloading` | 0x710758 |

The stack was built from Xiaomi's fork; the compiled-in source paths read
`packages/modules/MiuiBluetooth/system/...`. The per-codec software encoders are all there,
including `stack/a2dp/a2dp_aac_encoder.cc`.

## 3. FDK-AAC is already inside the Bluetooth process, and it can do ELD

No new encoder library is needed. Fraunhofer's encoder is statically linked into
`libbluetooth_jni.so`:

| Symbol | Address |
|---|---|
| `aacEncOpen` | 0x88e388 |
| `aacEncoder_SetParam` | 0x890864 |
| `aacEncEncode` | 0x88e8d4 |
| `aacEncClose` | 0x88e800 |
| `aacEncInfo` | 0x890fb8 |

Two things confirm it can encode AAC-ELD specifically, which is the feasibility gate for
this whole project:

- `FDKaacEnc_Transform_Real_Eld` at 0x8a3c34. That is the ELD-specific MDCT, only compiled
  when the ELD encoder is built in.
- `aacEncoder_SetParam` dispatches on parameter IDs 0x100 through 0x108, which covers
  `AACENC_GRANULE_LENGTH` (0x105), `AACENC_SBR_MODE` (0x104) and `AACENC_SBR_RATIO` (0x108),
  plus 0x200, 0x202, 0x203, 0x207, 0x300 to 0x306, 0x500 and 0x600.

The 480-sample granule the AirPods require is therefore settable, and the SBR encoder
(`FDKsbrEnc_*`) is linked in too should it ever be wanted.

## 4. The encoder interface Android expects

`A2DP_VendorGetEncoderInterface` returns a pointer to a table of eight function pointers.
Layout confirmed by resolving the LDAC table at 0xe06528 and the aptX table at 0xe06468
against the symbol table:

| Slot | Offset | Member |
|---|---|---|
| 0 | +0x00 | `encoder_init(peer_params, codec_config, read_cb, enqueue_cb)` |
| 1 | +0x08 | `encoder_cleanup()` |
| 2 | +0x10 | `feeding_reset()` |
| 3 | +0x18 | `feeding_flush()` |
| 4 | +0x20 | `get_encoder_interval_ms()` |
| 5 | +0x28 | `get_effective_frame_size()` |
| 6 | +0x30 | `send_frames(timestamp_us)` |
| 7 | +0x38 | `set_transmit_queue_length(len)`, may be null |

aptX leaves slot 7 null, LDAC fills it. So a null slot 7 is acceptable to the caller.

## 5. The exact place AAC-ELD has to be added

`A2DP_VendorGetEncoderInterface` reads the 32-bit vendor ID at codec-info offset 3 and the
16-bit codec ID at offset 7, then runs a flat comparison chain:

| Vendor ID | Codec ID | Codec |
|---|---|---|
| 0x4f | 0x01 | aptX |
| 0xd7 | 0x24 | aptX HD |
| 0xd7 | 0xad | aptX Adaptive |
| 0x12d | 0xaa | LDAC |
| 0xe0 | 0x01 | LHDC |
| 0x53a | 0x53a, 0x38f | Xiaomi MIHC and one more |

Apple is vendor 0x4c with codec 0x8001. It is absent from that chain, and absent from
every sibling dispatcher. Those siblings are what must also learn about it:

| Function | Address |
|---|---|
| `A2DP_IsVendorSourceCodecValid` | 0x8fe0e4 |
| `A2DP_VendorCodecTypeEquals` | 0x8fe54c |
| `A2DP_VendorSourceCodecIndex` | 0x8ff0ac |
| `A2DP_VendorSinkCodecIndex` | 0x8ff32c |
| `A2DP_VendorInitCodecConfig` | 0x8ff3a8 |
| `A2DP_VendorGetBitRate` | 0x8fe7c0 |
| `A2DP_VendorUsesRtpHeader` | 0x8fe3a8 |
| `A2DP_VendorBuildCodecHeader` | 0x8feb2c |
| `A2DP_VendorCodecInfoString` | 0x8ff514 |
| `A2DP_VendorCodecIndexStr` | 0x8ff37c |

`A2DP_VendorBuildCodecHeader` is where the four-byte Apple per-frame header described in
`aac-eld-apple.md` gets written.

## 6. Why a new codec index is the wrong first move

`A2dpCodecs::init()` at 0x8ebf54 builds a fixed table of `A2dpCodecConfig*` objects, one
per codec index. Adding an index means constructing a new C++ subclass and vtable at
runtime. The cheaper route is to take over a codec that is already registered but unused.

Opus fits. It is in the priority table at 8001, its config object and vendor functions are
all present, `persist.bluetooth.opus.enabled` is unset so the stack logs
`OPUS codec disabled, updated priority to ...`, and AirPods do not speak Opus, so there is
nothing to collide with. Its class surface is two methods, both hookable:

| Function | Address |
|---|---|
| `A2dpCodecConfigOpusBase::setCodecConfig` | 0x917444 |
| `A2dpCodecConfigOpusBase::setPeerCodecCapabilities` | 0x917a34 |
| `A2DP_IsCodecValidOpus` | 0x915544 |
| `A2DP_ParseInfoOpus` | 0x9155d8 |
| `A2DP_BuildInfoOpus` | 0x917118 |
| `A2DP_VendorCodecTypeEqualsOpus` | 0x915b54 |
| `A2DP_VendorGetEncoderInterfaceOpus` | 0x916f70 |
| `A2DP_VendorGetBitRateOpus` | 0x915da8 |
| `A2DP_VendorCodecInfoStringOpus` | 0x91660c |
| `A2DP_VendorGetTrackSampleRateOpus` | 0x9161d4 |
| `A2DP_VendorGetTrackChannelCountOpus` | 0x915ef0 |
| `A2DP_VendorGetFrameSizeOpus` | 0x916038 |
| `A2DP_VendorAdjustCodecOpus` | 0x9170a8 |
| `A2DP_IsVendorSinkCodecSupportedOpus` | 0x915930 |

## 7. Delivery vehicle

The device runs KernelSU with NeoZygisk, so the stack can be hooked in place rather than
patched on disk. A Zygisk module that attaches to `com.android.bluetooth` and inline-hooks
the functions above avoids modifying the `/apex/com.android.bt` APEX altogether, which
matters because APEX contents are verified and awkward to overlay.

The module needs no bundled encoder: it can call the already-linked `aacEncOpen` and
friends at the offsets in section 3, located by build ID so a wrong firmware is detected
rather than silently mis-hooked.

## 8. What happened when this was actually run

Written after taking the module live on the device. Three things stood between a
correct set of offsets and a working codec, none of them visible from the
binary.

### The hook engine needed two changes

ShadowHook 2.0.1 does not work out of the box in this process:

- `shadowhook_init` aborts with `SHADOWHOOK_ERRNO_INIT_LINKER`, because it wants
  the dynamic linker's private `soinfo::call_constructors` symbols and cannot
  find them on this build. That path only serves hooks on libraries that are not
  loaded yet, which this module does not use.
- Its branch-island strategy writes into the loader's PROT_NONE alignment
  padding inside the target library and crashes.
- Reusing a second trampoline slot in a page that already holds a live
  trampoline crashes, apparently because this kernel revokes write access to an
  anonymous RWX page once code has been executed from it.

The three patches, with the evidence for each, are in `third_party/shadowhook-patches/`.
With them, all 18 hooks install and the Bluetooth process is stable.

### Zygisk had to be told to leave the process alone

`com.android.bluetooth` had `umount_modules` set in its KernelSU app profile, so
Zygisk logged `[com.android.bluetooth] is on the denylist`. Clearing it is a
one-off:

```bash
ksud profile set <json with non_root.umount_modules = false>
```

This turned out not to be the cause of the crashes, but it is still wrong for a
process the module needs to live in.

### MIUI pins the remembered codec above everything else

This is the interesting one, and it is why correct hooks still produced a plain
AAC stream.

`A2dpService` remembers a codec per device in the `codec_status` shared
preference and, on connect, calls `setCodecConfigPreference` with priority
**1000000**:

```java
setCodecConfigPreference(device, new BluetoothCodecConfig(i, z ? 1000000 : -1, ...));
```

The AirPods are recorded there under `AAC`, so AAC is pinned far above the
priority of the codec slot this module occupies, and the stack configures the
peer's plain AAC endpoint without ever offering the AAC-ELD one. An HCI capture
shows it plainly: `DISCOVER` finds all three endpoints, `GET_ALL_CAPABILITIES`
reads AAC-ELD off endpoint 3, and then `SET_CONFIGURATION` goes straight to
endpoint 2 with `00 02 80 01 04 82 84 88`, plain AAC.

So the module also hooks `A2dpCodecConfigAacBase::setCodecConfig` and makes the
stock AAC codec decline while the peer is known to offer AAC-ELD. The stack then
moves on to the next codec with a matching endpoint, which is ours. It is gated
behind `persist.aaceld.force`, because declining AAC also removes it as a
fallback.
