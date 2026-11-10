# AAC-ELD for AirPods on Android

AirPods offer three codecs to any phone: SBC, AAC, and Apple's variant of
AAC-ELD, the low-delay codec they use with Apple's own devices. Android does
not know the third one and always ends up on AAC.

This is a root module that teaches the Bluetooth stack of a **Xiaomi 17 Pro
Max** that codec. The phone then negotiates AAC-ELD with AirPods and encodes
it itself, at up to 320 kbps. Nothing on the system partition is changed: the
module hooks the Bluetooth process in memory, and uninstalling it puts
everything back.

**Other phones:** it has been run on that one phone only. It is built to find
its way on other builds by itself, and
[docs/PORTING.md](docs/PORTING.md) is a step-by-step guide for trying it on
another Android phone, written so that a developer or an AI agent new to the
Bluetooth stack can follow it.

## Status

| | |
|---|---|
| Phone | Xiaomi 17 Pro Max (`popsicle`, Snapdragon, arm64) |
| Systems | HyperOS 3 (Android 16) and HyperOS 4.0.6.0 (Android 17) |
| Headsets | AirPods 5, AirPods Pro |
| Root | KernelSU Next with NeoZygisk. Magisk and APatch install paths are written but have not been run |
| Version | v0.15 |

What has been checked and what has not, per system, is in
[docs/notes/development-log.md](docs/notes/development-log.md), "What has
been verified on which system". In short: it plays, at every bit-rate
setting; reconnecting, switching the codec, volume changes and walking out
of range have been tested; on HyperOS 4 fewer of these than on HyperOS 3.

Not measured: the end-to-end delay, battery drain, and whether 288 and 320
kbps sound better than 256.

## What you need

- A Xiaomi 17 Pro Max on one of the systems above. Anything else: see
  [docs/PORTING.md](docs/PORTING.md) first.
- Root with Zygisk: KernelSU or APatch with a Zygisk module (NeoZygisk,
  ReZygisk, Zygisk Next), or Magisk with Zygisk on.
- AirPods that offer the codec. AirPods 5 and AirPods Pro do.
- Optional: an Xposed framework (LSPosed, Vector) for the switches in
  Bluetooth settings. Without it the settings are system properties.

## Install

1. Get `AAC-ELD-AirPods-<version>.zip` from the releases page, or build it
   (below).
2. Flash it in your root manager and read what the installer prints: it sets
   up what it can and lists what is left for you.
3. Reboot.
4. Connect the AirPods. In Bluetooth settings their badge reads "AAC-ELD".

To check from a computer:

```bash
adb shell dumpsys bluetooth_manager | grep -m1 'mCodecConfig:'
```

AAC-ELD shows as `codecName:Opus` with `mCodecSpecific1:5013505`. It borrows
the place of Opus in the stack, and that number (0x4C8001) is its mark.

```bash
adb logcat -s AacEldA2dp
```

shows what the module found, negotiated and sent.

## Settings

With the Settings add-on these are two switches and a slider on the AirPods'
page in Bluetooth settings. Without it:

| Property | Default | Meaning |
|---|---|---|
| `persist.bluetooth.aaceld.enabled` | true | false: AirPods get AAC again |
| `persist.bluetooth.aaceld.vbr` | true | Variable bit rate, as macOS sends. false: constant |
| `persist.bluetooth.aaceld.bitrate` | 256000 | Maximum bit rate, 64000 to 320000 |

```bash
adb shell su -c 'setprop persist.bluetooth.aaceld.bitrate 320000'
```

All settings, the measurements behind the defaults, and the known
limitations are in [module/README.md](module/README.md).

## Turning it off, removing it

- AirPods back on AAC: the AAC-ELD switch on their page, or set
  `persist.bluetooth.aaceld.enabled` to `false` and reconnect.
- The module out of the stack without uninstalling:
  `persist.aaceld.enabled` to `false`, then Bluetooth off and on.
- Gone: uninstall it in the root manager and reboot. Uninstall the app
  "AAC-ELD in Bluetooth settings" like any app.

If Bluetooth ever crashes while the module is starting, the module stays out
at the next start by itself, so a build it does not fit costs one Bluetooth
restart and not a boot loop.

## Building

Needs the Android NDK, `cmake` and `zip`; for the Settings add-on also the
Android SDK build-tools and a JDK.

```bash
./build.sh
```

produces `AAC-ELD-AirPods-<version>.zip`. Everything it compiles is in this
repository; nothing is downloaded.

## How it works, in six lines

1. A Zygisk module loads into `com.android.bluetooth` and finds the stack's
   functions by name, in the debug symbols that Android leaves in the
   library.
2. It hooks the small functions that answer questions about vendor codecs,
   so that Apple's codec is recognised.
3. It borrows the stack's Opus codec slot for the negotiation, since no
   AirPod uses Opus.
4. It supplies an encoder that drives the FDK-AAC library already inside the
   stack: 48 kHz, 10 ms frames, three to a packet, each behind Apple's
   four-byte header.
5. It corrects the details AirPods are particular about, above all the RTP
   timestamps.
6. Hardware offload stays on: every other codec is encoded by the phone's
   audio chip as before.

[docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) has the long version, including
the exact bytes on the air.

## Documentation

| | |
|---|---|
| [docs/PORTING.md](docs/PORTING.md) | Trying it on another phone or system version, step by step |
| [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | Symptom, check, cause, fix |
| [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) | The design, and the codec on the air |
| [module/README.md](module/README.md) | Every setting, measurements, limitations |
| [label/README.md](label/README.md) | The add-on for Xiaomi's Settings app |
| [docs/notes/](docs/notes/) | The working notes: how each thing was found |

## Repository layout

| | |
|---|---|
| `src/` | The module: start-up and symbol lookup (`target.cpp`), hooks (`hooks.cpp`), encoder and packets (`eld_encoder.cpp`) |
| `module/` | What goes in the flashable zip besides the library: installer, boot script, uninstaller |
| `label/` | Xposed add-on for Xiaomi's Settings: codec badge and the rows on the headset's page |
| `tools/` | For porting and testing: pull and check a stack library, list its symbols, disassemble, find code by log text, run the start-up in a test process, watch the log, play test files |
| `third_party/` | The hook engine (ShadowHook) and the Zygisk API header, under their own licences |
| `docs/` | See above |

## Things to know

- Keep **"Keep volume consistent"** on for the AirPods (absolute volume).
  With it off the sound is clearly worse; the add-on enforces it.
- Xiaomi's **"Dimensional audio"** cannot be on together with AAC-ELD. The
  phone's spatial effect is wired to the hardware-encoded path only.
- Developer options list the codec as **Opus**.
- The settings are phone-wide, not per headset.
- A headset that really speaks Opus would now be given Opus on HyperOS 4,
  where the stock system would not. `persist.aaceld.priority=0` restores the
  stock ranking. Not tried with such a headset.
- Other headsets are meant to behave exactly as without the module; only AAC
  to AirPods has been checked.

## Contributing

Reports from other phones are the most useful thing: see "Reporting a port"
at the end of [docs/PORTING.md](docs/PORTING.md). Please never attach a
Bluetooth capture (`btsnoop_hci.log`): it contains your link keys.

## Licence and credits

Copyright (C) the developer.

This project is free software under the **GNU General Public License,
version 2** (GPL-2.0-only); see [LICENSE](LICENSE). It comes with no
warranty. It changes how a system process behaves on a rooted phone; use it
at your own risk.

Third-party code keeps its own licences, listed in
[third_party/README.md](third_party/README.md): ShadowHook and xDL (MIT),
linux-syscall-support (BSD), the Zygisk API header.

The description of Apple's AAC-ELD stream that this work started from, and
the reference sender it was compared against, come from
[USBPods-Pico2W](https://github.com/wasdwasd0105/USBPods-Pico2W).

The FDK-AAC encoder is not part of this repository or of the module. The
module calls the copy that the phone's own Bluetooth stack already contains.

Not affiliated with Apple or Xiaomi. AirPods is a trademark of Apple Inc.
