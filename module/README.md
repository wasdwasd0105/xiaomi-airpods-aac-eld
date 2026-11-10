# AAC-ELD for A2DP, Xiaomi 17 Pro Max

Adds Apple's AAC-ELD A2DP vendor codec (vendor 0x4C, codec 0x8001) to the
Bluetooth stack and encodes it in software, using the FDK-AAC encoder that is
already statically linked into `libbluetooth_jni.so`. The Qualcomm DSP audio
HAL is not touched and no vendor file is replaced.

## What it does, concretely

1. Leaves A2DP hardware offload on. The stack already falls back to its
   software path for any codec the DSP cannot encode, which is how this one
   gets encoded in the Bluetooth process while AAC, SBC and LDAC stay on the
   DSP as before.
2. Attaches to `com.android.bluetooth` through Zygisk and inline-hooks the
   stack's vendor codec dispatchers so they recognise Apple's codec element.
3. Takes over the codec slot Opus occupies, since Opus is registered in the
   priority table but gated off by an unset property and no AirPod speaks it.
   This avoids building a new C++ codec subclass and vtable at runtime.
4. Hooks `A2DP_GetPacketTimestamp`, whose fixed codec list otherwise makes
   the stack free every media packet of this codec instead of sending it.
5. Supplies an encoder that produces 480-sample AAC-ELD frames, three per RTP
   packet, each behind the four-byte Apple header. The RTP timestamps
   count 48 kHz samples for the first six seconds of a stream and then carry
   on from a millisecond clock, which is what Apple's own senders use.

## Which builds it fits

Up to v0.9 the module carried hardcoded addresses and object offsets from one
exact build and refused any other. Since v0.10 it works these out at start-up
on whatever build is in front of it:

- **Functions** are found by name. The stripped library exports almost
  nothing, but it carries mini debug symbols (`.gnu_debugdata`) with every
  function this module hooks or calls. 32 are required; up to 15 more are
  used where a build has them. The names are mangled C++
  names, so they contain the parameter types: where a function's signature
  changed, the name is not there.
- **The codec object's layout** is measured by calling the stack's own
  accessors on a dummy object filled with a pattern that says where each byte
  came from. `src/target.h` describes the three measurements and what
  follows from them. It covers the differences known between AOSP and the
  fork on this phone: what sits ahead of the configs, whether
  `codec_capability_` exists, and the size of the OTA config arrays.
- **The Opus codec index** is asked of the stack, and **the unit of the
  encoder interval** (milliseconds here, microseconds in newer AOSP) is read
  from the name of the stock AAC encoder's interval function.

If a name is missing, a measurement does not pass its cross-checks, or a hook
does not go in, the module leaves the stack alone and the headset gets AAC as
if the module were not installed. A marker property,
`persist.bluetooth.aaceld.probe`, is set for the length of start-up: if the
Bluetooth process dies in that window, the next start finds the marker and
stays out, so a build the module does not get on with costs one Bluetooth
crash rather than a crash loop. Delete the property to let it try again.

What this has been run on is two builds of one phone:

```
libbluetooth_jni.so  build ID ea12c5b9edc07b7e826bd2aaec5a5f5b
Xiaomi/popsicle/popsicle:16/BP2A.250605.031.A3/OS3.0.319.0.WPBCNXM

libbluetooth_jni.so  build ID e2911fd00d152b418728edd5b3742931
Xiaomi/popsicle/popsicle:17/CP2A.260605.016/OS4.0.6.0.XPBCNXM
```

On both the measured layout came out identical to the one reversed by hand
(`docs/notes/object-layout.md`). The Android 17 stack needed v0.14: there the slot
ranks below AAC, the stack disables Opus at every ordinary stream start, and
it starts no audio session after a change between AAC and AAC-ELD
(`docs/notes/development-log.md`, "v0.14"). v0.14 itself broke every reconnection there
that did not follow a Bluetooth restart, music staying on the phone speaker;
v0.15 fixes that (`docs/notes/development-log.md`, "v0.15"). `tools/check_lib.py` does the name
check for any `libbluetooth_jni.so`. Other builds, MediaTek phones and older
HyperOS versions are untested. Things that would still stop it: a build without the
mini debug symbols; a renamed or removed function; a stack that sends a
codec it cannot offload somewhere other than the software encoder, which is
this stack's own decision and not something the module steers.

## Install

`AAC-ELD-AirPods-v0.15.1.zip` is an
ordinary root-manager module: flash it in the KernelSU, Magisk or APatch
manager and reboot. It needs Zygisk. From a computer:

```bash
adb push AAC-ELD-AirPods-v0.15.1.zip /sdcard/Download/
```

```bash
adb shell su -c '/data/adb/ksud module install /sdcard/Download/AAC-ELD-AirPods-v0.15.1.zip'
```

The installer (`customize.sh`) does what it can of the set-up that lives
outside the module, and lists whatever is left. Its messages are in Chinese
when the phone's language is Chinese, and in English otherwise:

- On KernelSU it switches "Umount modules" off in the Bluetooth app's
  profile (`ksud profile set`). With it on, Zygisk treats the Bluetooth app
  as on the deny list and never loads the module into it.
- It installs the Settings add-on, `label/`, which is inside the zip as
  `aaceld-label.apk`, and where the Vector command-line tool is present it
  enables the add-on for the Settings app. With LSPosed that last step is
  done by hand in its manager. Without any Xposed framework the codec still
  works; only the badge and the rows on the headset's page are missing.
- It warns if no Zygisk implementation is found.

Run on the development phone through KernelSU Next: it found Zygisk, found
the Bluetooth profile already set, installed the add-on and enabled it in
Vector. The Magisk and APatch paths, the LSPosed path and the profile change
itself (it was already made on that phone) have not been run.

Uninstalling the module in the manager removes it and its properties after a
reboot. The Settings add-on is a normal app and is uninstalled like one.

## Settings

Three settings are meant for everyday use. With `label/` installed they are
two switches and a slider on the headset's page in Bluetooth settings;
without it, set them with `setprop` as root. They are phone-wide, not per
headset.

| Property | Default | Meaning |
|---|---|---|
| `persist.bluetooth.aaceld.enabled` | true | false makes the stack decline AAC-ELD, so AirPods get AAC. Read every time a codec is chosen: it applies at the next connection, or at once when the switch in Settings asks the stack to choose again |
| `persist.bluetooth.aaceld.vbr` | true | Variable bit rate: only the bits the music needs, as macOS sends, never a frame above the maximum bit rate or above 256 kbps. false is constant bit rate at the maximum. Picked up within a second while playing |
| `persist.bluetooth.aaceld.bitrate` | 0 | The maximum bit rate in bit/s, 64000 to 320000; 0 or unset is 256000. With variable bit rate on it bounds every frame, and anything above 256000 counts as 256000. With it off it is the constant bit rate. Above 256000 is more than the headset advertises, see below. A change is picked up within a second while playing, without a gap |

They live under `persist.bluetooth.` because that is where the Settings app
is allowed to write.

The rest is for tuning and experiments. Change them with `setprop` as root;
they take effect the next time a stream starts.

| Property | Default | Meaning |
|---|---|---|
| `persist.aaceld.enabled` | true | Set false to leave the stack alone without uninstalling: no hooks are installed at all. Needs a Bluetooth restart |
| `persist.aaceld.bitratemode` | 5 | FDK rate mode while variable bit rate is on: 5 is the highest-quality VBR, 0 is CBR. FDK steps the mode down by itself when the maximum is below what it needs (192 kbps for mode 5) |
| `persist.aaceld.sbr` | 0 | 1 adds LD-SBR at ratio 1. Only used with VBR |
| `persist.aaceld.bitrate` | 256000 | CBR rate when `bitratemode` is 0 while variable bit rate is on |
| `persist.aaceld.peak_bitrate` | 256000 | Hard bound on one frame: 320 bytes at 256000 |
| `persist.aaceld.frames_per_packet` | 3 | Frames per RTP packet; 3 gives Apple's 30 ms cadence. Read when the codec is set up |
| `persist.aaceld.vbr` | true | Sets the VBR bit in the negotiated element, matching what macOS sends |
| `persist.aaceld.rtp_ms` | false | Looked at once a second: flipping it changes the timestamp unit of the running stream. With `rtp_ms_after` at 0, true also starts every stream on millisecond stamps, which AirPods 5 reject (they pause the stream after 2.9 s) |
| `persist.aaceld.rtp_ms_after` | 6000 | Every stream starts on a 48 kHz sample count and changes to millisecond stamps, as macOS sends, this many milliseconds after its first packet, carrying on from the last stamp. Needed for both buds to follow a volume change. 0 stays on the sample count |
| `persist.aaceld.rtp_ms_lag` | false | Only with millisecond stamps: stamp each packet with the time the previous one was sent, as the USBPods firmware does |
| `persist.aaceld.rtp_ms_jitter_us` | 3000 | Random offset added to each millisecond stamp, giving steps of 27 to 33 like macOS's. 0 gives an exact +30 |
| `persist.aaceld.pcm_stats` | false | Diagnostic: once a second, log the level of each channel entering the encoder and the time offset between them |
| `persist.aaceld.delay_report` | true | false removes Delay Reporting from the stream setup, as the USBPods firmware does. Makes no audible difference |
| `persist.aaceld.rtp_ssrc` | false | true keeps the stack's own SSRC of 0x000000FF instead of zero |
| `persist.aaceld.rtp_marker` | false | true leaves the RTP marker bit as the stack set it |
| `persist.aaceld.priority` | 8001 | Where the codec slot ranks among the stack's codecs; it has to be above AAC (2001) for AirPods to take AAC-ELD by themselves. HyperOS 4 gives the slot 1501 by default, which is why this exists. 0 leaves the stack's figure. Read when Bluetooth starts |
| `persist.aaceld.session_start` | true | HyperOS 4 (Android 17) keeps the audio session across a codec change and so starts none when the change is between AAC, which the DSP encodes, and AAC-ELD, which the stack encodes; music then plays through the phone speaker. With this on, the module starts the session itself in that case. Builds that restart the session themselves are not touched |
| `persist.aaceld.max_queue` | 0 | 0: when the link cannot take the audio the encoder carries on and the stack discards what it cannot send, as macOS does. A positive number is the old behaviour: with that many packets waiting, stop encoding. That left the AirPods silent for a minute or more after a walk out of range |
| `persist.aaceld.rtp_seq_gaps` | false | Experimental: adds the packets the stack threw away to the RTP sequence number, so the loss shows there too, as it does from macOS. The first version of this (v0.12, on by default) numbered packets from 1 again at every resume and silenced the AirPods; that build needs it set to false |
| `persist.aaceld.recover_kick_ms` | 0 | Experimental, untested: after link trouble lasting at least this many milliseconds, suspend and restart the AVDTP stream once the link is back. 0 never |
| `persist.aaceld.dryrun` | false | Validates and logs every target address without patching anything |

Encoder properties are read at every stream start, so a pause and resume
picks up a change. SET_CONFIGURATION always carries the 256000 the headset
advertises, whatever the encoder then runs at, which is what macOS does too.

### Variable or constant, SBR or not

Leave variable bit rate on with the maximum at 256, which is the default
and was called "Automatic" up to v0.8, and leave SBR off. Measured on this
phone with `tools/test/rate_sweep.sh`, playing to AirPods 5:

| Setting | Tonal test melody | Dense mix with noise | Encoder CPU per 10 ms frame |
|---|---|---|---|
| Variable, maximum 256 (VBR 5), no SBR | 157 kbps | 220 kbps | 0.30 and 0.34 ms |
| Variable, maximum 256 (VBR 5) with SBR | 150 kbps | 218 kbps | 0.42 and 0.44 ms |
| Constant 256 kbps | 256 kbps | 256 kbps | 0.27 and 0.33 ms |
| Constant 192 kbps | 192 kbps | 192 kbps | 0.27 and 0.36 ms |
| Constant 128 kbps | 128 kbps | 128 kbps | 0.30 and 0.40 ms |

- **Variable rate costs next to nothing and saves airtime.** Its encoder cost
  is 3 to 12 % above constant 256, and it puts 15 to 40 % fewer bits on the
  air depending on the material, which is less radio time for the phone and
  the headset. No frame exceeds the 320 bytes the headset is told to expect.
- **SBR costs about 30 to 40 % more encoder CPU and saves 1 to 5 % of the
  bits.** SBR pays off below roughly 64 kbps per channel; at these rates the
  core already carries the full band. It also adds a decoding stage in the
  headset and 32 samples of delay, and Apple's own stream does not use it.
- A lower constant rate saves airtime in proportion but not CPU: the
  encoder works slightly harder at 128 kbps than at 256.

The encoder is about 3 % of one core while music plays. These are CPU-time
and bit-rate measurements; battery drain itself was not measured.

### 288 and 320 kbps

Constant 288 and 320 kbps are above the 256 kbps the AirPods advertise as
their maximum, and above anything macOS sends. AirPods 5 play both: music
ran for a minute at 288 and half a minute at 320 with no pause from the
headset, no congestion and nothing dropped. Whether they sound better than
256 was not measured.

They change the shape of the stream. A frame is up to 361 bytes at 288 kbps
and 401 at 320, and three of those no longer fit the 986 byte packet, so
these rates go out two frames to a packet, 50 packets a second instead of
33. That is more radio time for the phone and both buds, 13 and 25 % more
bits than 256, and less margin when the link is poor. Range and dropouts at
these rates were not tested. Encoder CPU on music was 0.37 ms per frame at
288 and 0.26 ms at 320, the same order as the other rates.

Variable bit rate does not go there: with the slider above 256 it still
keeps every frame within what 256 kbps allows, the size the headset is told
to expect. Only constant bit rate uses 288 and 320.

With the slider below 256 and variable bit rate on, the slider bounds each
frame. Checked against the same FDK offline on a dense mix: bounds of 224,
192, 160 and 128 kbps gave 218, 188, 154 and 124 kbps on average with no
frame over the bound.

## Codec badge

Every config this module puts in the borrowed slot carries 0x004C8001 in
`codecSpecific1`, so anything above the stack can tell an AAC-ELD session
from a genuine Opus one. `label/` in the repository uses that to show
"AAC-ELD" beside the headset in Bluetooth settings.

## Watching it work

```bash
adb logcat -s AacEldA2dp
```

A healthy session logs the library and its build ID, the functions found, the
measured layout, the hook count, the
negotiated configuration, the encoder parameters, the first packets exactly as
they are handed to L2CAP, and a statistics line about every 30 seconds with
the average bit rate and the encoder's CPU time per frame.

## Reverting

To go back to AAC without uninstalling, turn the AAC-ELD switch off on the
headset's page, or:

```bash
adb shell su -c 'setprop persist.bluetooth.aaceld.enabled false'
```

and reconnect the headset. Uninstalling the module removes everything after a
reboot.

## Status

Working, on HyperOS 3 (Android 16) and HyperOS 4.0.6.0 (Android 17). AirPods
play AAC-ELD from this phone with the default settings, from a clean boot, at
every bit rate setting. `docs/notes/development-log.md` has what was
wrong and how it was found.

## Known limitations

- **Keep "Keep volume consistent" on** for a headset using AAC-ELD. With it
  off the phone attenuates the music before the encoder on a 16-bit path and
  the sound is clearly worse. `label/` enforces this on the headset's page.
- **A volume change in the first six seconds of a stream** is made before
  the timestamps change to milliseconds. Up to v0.6, when the whole stream
  was on the sample count, a volume change could leave one bud louder than
  the other until AAC-ELD was switched off and on. After the change-over it
  does not; the first six seconds have not been tried. See `docs/notes/development-log.md`.

- **"Dimensional audio" cannot be switched on while AAC-ELD is the active
  codec.** The phone's spatializer output is wired to the hardware Bluetooth
  path only, and AAC-ELD is encoded in software. The system log says so each
  time: "can spatialize media 5.1: false" with AAC-ELD, "true" within a
  second of switching back to AAC. Not something the rows on the page do.

- Other headsets have not been played with the module installed. Since v0.5
  they are on the stock offload path, untouched, so there is no known reason
  for them to behave differently.
- The two settings are phone-wide. The stack is not told which headset it is
  choosing a codec for at the point where the module decides.
- After a walk out of range the stream carries on by itself: the encoder
  keeps running and what could not be sent is discarded, see
  `docs/notes/development-log.md`, "v0.12 and v0.13". Nothing forces an AVDTP suspend and
  restart; that exists as `persist.aaceld.recover_kick_ms` and has never been
  run.
- Developer options list the codec as "Opus", the slot it borrows. The badge
  in the Bluetooth device list reads "AAC-ELD" once `label/` is installed.
  Picking AAC or SBC in developer options still overrides AAC-ELD until the
  headset reconnects.
- The engine patches in `third_party/shadowhook-patches/` are specific to this device's
  kernel and linker behaviour.
