# Troubleshooting

Find the symptom, run the check, apply the fix. Every entry here is a problem
that was actually met while building this, except where it says "not met".

Before anything else, have the log running. Most checks below look for a
line in it.

```bash
tools/watch.sh build/problem.log
```

Three commands are used again and again:

```bash
# Which codec the headset is on
adb shell dumpsys bluetooth_manager | grep -m1 'mCodecConfig:'
```

```bash
# Where music is routed: bt_a2dp is the headset, speaker is the phone
adb shell dumpsys audio | grep -A8 '^- STREAM_MUSIC' | grep Devices
```

```bash
# Only the module's own lines
adb logcat -s AacEldA2dp
```

To get back to stock Bluetooth at any time without uninstalling:

```bash
adb shell su -c 'setprop persist.aaceld.enabled false'
```

then switch Bluetooth off and on. Uninstalling the module in the root manager
and rebooting removes everything.

## Index

Start-up

- [No AacEldA2dp lines at all](#no-aacelda2dp-lines-at-all)
- ["not in this build: ..."](#not-in-this-build)
- ["the last start on this build did not get through start-up"](#the-last-start-on-this-build-did-not-get-through-start-up)
- [Bluetooth crashes at start](#bluetooth-crashes-at-start)
- ["the codec object could not be measured"](#the-codec-object-could-not-be-measured)
- ["the stack does not know an Opus source codec"](#the-stack-does-not-know-an-opus-source-codec)

Connecting

- [The headset does not offer AAC-ELD](#the-headset-does-not-offer-aac-eld)
- [Connects on AAC although the module is ready](#connects-on-aac-although-the-module-is-ready)
- [Falls back to AAC a few seconds after connecting](#falls-back-to-aac-a-few-seconds-after-connecting)
- ["cannot update peer codec capabilities"](#cannot-update-peer-codec-capabilities)
- ["cannot set source codec"](#cannot-set-source-codec)
- [AAC-ELD is not listed as selectable](#aac-eld-is-not-listed-as-selectable)

Sound

- [Music plays from the phone speaker](#music-plays-from-the-phone-speaker)
- [Packets are made but none is sent](#packets-are-made-but-none-is-sent)
- [The headset pauses the stream after three seconds](#the-headset-pauses-the-stream-after-three-seconds)
- [The headset disconnects while playing](#the-headset-disconnects-while-playing)
- [The sound is clearly worse than AAC](#the-sound-is-clearly-worse-than-aac)
- [One bud is louder after a volume change](#one-bud-is-louder-after-a-volume-change)
- [Silent for a long time after walking out of range](#silent-for-a-long-time-after-walking-out-of-range)
- [No sound after pause and resume](#no-sound-after-pause-and-resume)
- [Every headset plays from the phone speaker](#every-headset-plays-from-the-phone-speaker)
- [Spatial audio will not switch on](#spatial-audio-will-not-switch-on)

---

## Start-up

### No AacEldA2dp lines at all

`adb logcat -s AacEldA2dp` shows nothing after Bluetooth starts.

**Cause.** The module is not being loaded into the Bluetooth process.

**Check, in this order.**

1. Is it installed and enabled?
   `adb shell su -c 'ls /data/adb/modules/aaceld_a2dp; cat /data/adb/modules/aaceld_a2dp/module.prop'`.
   A file named `disable` or `remove` in that folder means it is off.
2. Did you reboot after installing? Some Zygisk implementations keep the old
   library until a reboot; restarting Bluetooth is not enough.
3. Is Zygisk running? On KernelSU and APatch a Zygisk module (NeoZygisk,
   ReZygisk, Zygisk Next) must be installed and enabled; in Magisk the Zygisk
   switch must be on.
4. Is the Bluetooth app excluded? Look in the full log for
   `[com.android.bluetooth] is on the denylist`. On KernelSU: manager >
   Superuser > show system apps > Bluetooth > switch "Umount modules" off. On
   Magisk: take Bluetooth off the DenyList.
5. Is it switched off by property? The log would say
   `persist.aaceld.enabled is false, leaving the stack untouched`.

### "not in this build"

```
not in this build: _Z...
N of the functions this module needs are missing or have a different signature in this build, so nothing will be hooked
```

**Cause.** This stack does not have a function under the exact name the
module expects.

**Fix.** [PORTING.md](PORTING.md), step 3, for each name.

### "the last start on this build did not get through start-up"

**Cause.** The previous start of Bluetooth crashed, or was killed, while the
module was starting. The module remembers that and stays out so that
Bluetooth keeps working.

**Fix.** Find out why it crashed (next entry), change something, then let it
try again:

```bash
adb shell su -c 'resetprop -p --delete persist.bluetooth.aaceld.probe'
```

and restart Bluetooth.

### Bluetooth crashes at start

Bluetooth switches itself off and on, or the log stops right after a
`hooking <name> at 0x...` line.

**Check.**

```bash
adb shell su -c 'ls -t /data/tombstones | head -3'
```

```bash
adb shell su -c 'head -60 /data/tombstones/<newest file>'
```

The backtrace says where it died.

| Backtrace goes through | Cause | Fix |
|---|---|---|
| `shadowhook`, `sh_island`, `sh_trampo`, `memset` | The hook engine writing where this phone does not allow it | The three changes in [third_party/shadowhook-patches/](../third_party/shadowhook-patches/) were made for exactly this on the development phone; each file says what it saw. A new phone may need a fourth. Set `persist.aaceld.dryrun true` and restart Bluetooth: the module then patches nothing and logs whether read-write-execute memory is usable in the process (`probe:` lines) |
| the function named in the last `hooking` line | The function is too short to hook, or does not start the way the engine expects | `tools/a64.py LIB fn <name> 12` and look at its first instructions. If it is a few instructions long, hook its caller instead |
| a stack function called by the layout measurement (`getCodecConfig`, `copyOutOtaCodecConfig`) | The accessor did something with the dummy object that needs a real one | See the next entry |
| one of the module's `h_...` functions | A hook has the wrong parameters for this build | Compare the mangled name in `kNames` with the hook's declaration |

### "the codec object could not be measured"

The line before it says which measurement failed.

| Line | Meaning | What to do |
|---|---|---|
| `measuring codec_config_: the accessor faulted on the dummy object` | `getCodecConfig()` touched something the dummy does not have (it may log the codec's name, lock something, or call a virtual function) | Read it: `tools/a64.py LIB fn _ZN15A2dpCodecConfig14getCodecConfigEv 60`. See what it reads from the object before the copy, and make the dummy valid for that in `measure_struct_getter_from()` in `src/target.cpp` |
| `measuring ...: unexpected result, N bytes written, first word ...` | The accessor did not return one plain copy of a block | The struct may be built field by field on this build. Read the accessor as above |
| `a codec config is N bytes here, this module writes 56 byte ones` | `btav_a2dp_codec_config_t` has a different size | Compare `hardware/libhardware/include/hardware/bt_av.h` for your Android version with the `CFG_*` offsets in `src/target.h` and adjust them |
| `codec_config_ at 0x.. and codec_user_config_ at 0x.. do not fit the class this module knows` | There are not 3 or 4 config blocks between the two | The class gained or lost a member. Find the order in AOSP's `a2dp_codec_api.h` for your version and adjust `target_measure_getters()` |
| `ota_codec_config_ was not found behind codec_user_config_` | The byte array is not where the class order says | Read `copyOutOtaCodecConfig`: the offset it copies from is the answer; add it to the `candidates` in `target_measure_ota()` |
| `ota_codec_config_ is N bytes here` | The array is smaller than 17 bytes or implausibly large | A stack with arrays under 17 bytes cannot hold Apple's element. Not met |

The table in [notes/object-layout.md](notes/object-layout.md) shows what a
correct result looked like on the development phone and how each field was
confirmed by hand.

### "the stack does not know an Opus source codec"

**Cause.** The stack has no Opus codec, so there is no slot to borrow.

**Fix.** Not done by this project. The way to do it: pick another vendor
codec that the stack has and your headsets will never use, and borrow its
slot instead. Replace the two `A2dpCodecConfigOpusBase` names in `kNames`
with that codec's `setCodecConfig` and `setPeerCodecCapabilities`, and the
Opus element in `hooks_install()` (`kOpusInfo`) with that codec's vendor and
codec numbers. The property that enables the slot will differ too.

---

## Connecting

### The headset does not offer AAC-ELD

The log has no `hit: setPeerCodecCapabilities for AAC-ELD` when the headset
connects.

**Check.** Does the log have `hit: IsVendorSourceCodecValid -> true`? If
neither line appears, the stack never showed the module Apple's element.

**Causes.**

1. The headset has no such endpoint. AirPods 5 and AirPods Pro do. To see
   what a headset offers, capture one connection and decode it:

   ```bash
   adb shell su -c 'setprop persist.bluetooth.btsnooplogmode full'
   ```

   Switch Bluetooth off and on, let the headset connect, then

   ```bash
   adb shell su -c 'ls /data/misc/bluetooth/logs/'
   ```

   pull the newest `btsnoop_hci*.log` and run `tools/avdtp.py` on it. Look
   for an endpoint whose codec starts `ff 4c 00 00 00 01 80`. Then set the
   property back to `disabled`, and **delete the capture, on the phone and
   on the computer: it contains the link keys of every paired device.**
2. The stack drops vendor elements before the functions the module hooks.
   Not met. Find where: `tools/xref.py LIB 'unsupported codec'` and similar
   messages near the capability parsing.

### Connects on AAC although the module is ready

Start-up ends in `ready`, `hit: setPeerCodecCapabilities for AAC-ELD`
appears, and still `mCodecConfig: {codecName:AAC`.

Go through these; each has its own telltale.

| Telltale | Cause | Fix |
|---|---|---|
| Module log: `setCodecConfig: AAC-ELD is switched off` | The switch is off | `adb shell su -c 'setprop persist.bluetooth.aaceld.enabled true'`, reconnect |
| Stack log: `OPUS codec disabled, updated priority to ...` | The stack's own Opus switch is off | `adb shell getprop persist.bluetooth.opus.enabled` must print `true` **before Bluetooth starts**. The module's `post-fs-data.sh` sets it at boot; if it is empty, that script did not run (module not enabled, or installed without a reboot) |
| `adb shell dumpsys bluetooth_manager \| grep -A1 'A2DP Opus State'` shows a priority below AAC's (AAC is 2001) | This stack ranks the slot low | The module log should show `codec slot priority N -> 8001` when Bluetooth starts. If it says `the Opus codec constructor is not in this build`, the priority is set somewhere else on this stack: find it with `tools/xref.py LIB 'OPUS codec'` and the callers of what you find |
| `mCodecConfig: {codecName:AAC,...,mCodecPriority:1000000` | The phone maker's Bluetooth app asked for AAC with the highest priority. Xiaomi's does this from a per-headset codec switch | On Xiaomi the add-on in `label/` handles it. Elsewhere: look for a codec or "HD audio" switch on the headset's page in Settings and turn it off. Choosing "Opus" under Developer options > Bluetooth audio codec should ask for the borrowed slot in the same way; that has not been tried |
| Developer options > Bluetooth audio codec is set to something | It overrides everything until the headset reconnects | Set it back to the default |

### Falls back to AAC a few seconds after connecting

The headset connects on AAC-ELD and changes to AAC when the first sound
starts.

**Check.** The stack log has a codec request for Opus with `priority: -1`
at that moment.

**Cause.** Android treats Opus as its low-latency codec only, and on some
stacks disables it whenever a stream starts without low-latency mode.

**Fix.** The module swallows that request where it knows the function
(`btif_av_source_set_low_latency_codec`, or `invoke_switch_codec_cb` on older
stacks) and then logs `stream start: low latency not wanted; the slot is
AAC-ELD, not Opus, so the codec stays as it is`. If that line is missing,
neither function exists under those names on your stack. Find the one that
does the job: `tools/xref.py LIB 'low latency'` or
`grep -i low_latency LIB.demangled.txt`, read its callers, and add a hook
like `h_SetLowLatencyCodec`.

### "cannot update peer codec capabilities"

Stack log, on every connection:
`cannot update peer codec capabilities for ...`

**Cause.** The hook on `setPeerCodecCapabilities` returns the wrong kind of
value. On the builds seen it returns `bool`, true for success.

**Fix.** Read the original's return, as described in
[PORTING.md](PORTING.md), "Writing a hook", and make `h_OpusSetPeerCodecCapabilities` match.

### "cannot set source codec"

**Cause.** The same, for `setCodecConfig`. On the builds seen it returns a
status where **0 is success**; returning `true` (1) reads as an error.

**Fix.** As above, for `h_OpusSetCodecConfig` (`kCodecOk`, `kCodecFail`).

### AAC-ELD is not listed as selectable

`adb shell dumpsys bluetooth_manager` shows the headset's
`mCodecsSelectableCapabilities` without an entry carrying
`mCodecSpecific1:5013505`, so nothing above the stack can ask for it.

**Cause.** The module wrote the "selectable" block at the wrong place in the
codec object.

**Fix.** The layout is measured; compare the `layout:` line in the module's
log with the code of `A2dpCodecConfig::debug_codec_dump`, which prints
"Selectable" from that field: `tools/xref.py LIB 'Selectable'`.

---

## Sound

### Music plays from the phone speaker

The headset is connected and on AAC-ELD, and music comes out of the phone.
The route check says `Devices: speaker(2)`. Bluetooth settings may show the
headset as active for calls only.

**Check.** The log has one of:

```
APM failed to make available A2DP device addr=... error=1
APM handleDeviceConfigChange failed for A2DP device ... codec=AUDIO_FORMAT_OPUS
session_type=A2DP_SOFTWARE_ENCODING_DATAPATH is not ready
```

**Cause.** There is no audio session of the kind this codec needs. AAC-ELD is
encoded in the stack, on the software session; AAC, SBC and the rest are on
the hardware session. Android's audio system refuses a headset whose session
is not there, and then nothing asks again until the headset reconnects.

Which situation is it?

| When | Telltale in the log | Cause | Fix |
|---|---|---|---|
| After switching between AAC and AAC-ELD while connected | `Reconfig done - Ignore calling startSession(), just update codec to audio HAL`, then `Switching BluetoothAudio HAL to Software` (or `Hardware`), and no `start_session` after it | This stack keeps the session across codec changes and starts none when the kind changes | The module's hook on `btif_a2dp_source_setup_codec` does it and logs `audio session changed to software encoding: starting it`. If that line is missing the hook is not installed: `tools/check_lib.py` lists the three names it needs as absent. Find this stack's equivalent with `tools/xref.py LIB 'Switching BluetoothAudio HAL'` |
| After disconnecting and connecting again, first connection after boot was fine | `btif_a2dp_source_setup_codec` followed by no `start_session` | The stack's own session start is being skipped. In module v0.14 this was the module's bug (a hook returning the wrong value) | Use v0.15 or later. On a new stack: read `tools/a64.py LIB fn btif_a2dp_source_start_session_delayed 200` and see what decides whether `start_session` is called |
| On the very first connection | No `Switching BluetoothAudio HAL to Software`; the audio HAL logs a `HARDWARE_OFFLOAD` session for this codec | Not met. The stack believes the DSP can encode the borrowed slot's codec | Check `adb shell getprop persist.bluetooth.a2dp_offload.cap`: if it lists `opus`, this phone's DSP takes Opus and the stack will hand it the stream. The slot of a codec the DSP does not list is needed |

**To recover the phone** once it is in this state: disconnect the headset and
connect it again (Bluetooth settings > the headset's page > Disconnect), or
switch Bluetooth off and on. Tapping the headset does nothing, because the
Bluetooth app already considers it active.

Do not "fix" this by switching hardware offload off for the whole phone.
See [Every headset plays from the phone speaker](#every-headset-plays-from-the-phone-speaker).

### Packets are made but none is sent

The module logs `frame` and `packet` lines and a `stats` line with packets
counted, but no `wire` lines. The headset is silent and drops the connection
after a while.

**Check.** The stack log has, once per packet:

```
A2DP_GetPacketTimestamp: unsupported codec id 0x8001004cff
GetNextSourceDataPacket: unsupported codec type (255)
```

**Cause.** `A2DP_GetPacketTimestamp` knows a fixed list of codecs and makes
the stack free every packet of any other.

**Fix.** The module hooks it. If you see this, that hook is not in: check
the start-up log for `hooking A2DP_GetPacketTimestamp`.

### The headset pauses the stream after three seconds

`wire` lines appear, there is no sound, and about 2.9 s after the stream
starts the music app is paused as if you had tapped the headset. If the
stream is forced to continue the headset drops the link.

**Check.** The log has `sendMediaKeyEvent ... pushed=true` or
`REMOTE_SUSPEND` about three seconds after `OnEnter: state=Started`.

**Causes.**

1. The stream started on millisecond timestamps. The encoder's start line
   must read `rtp clock samples then milliseconds`. If it reads
   `milliseconds` alone, `persist.aaceld.rtp_ms` is true with
   `persist.aaceld.rtp_ms_after` at 0: delete both properties.
2. The frames are not 480 samples. The module would log
   `encoder frame length is N, not 480`. The FDK in your stack ignores the
   480-sample setting; without it AirPods do not play.

`tools/test/ts_switch.sh` plays a file and reports exactly this reaction.

### The headset disconnects while playing

Sound for a while, then the link drops, most easily when the phone is far
from the headset or the bit rate is high.

**Check.** The `stats` line ends `l2cap ok/congested/failed a/b/c`. If `b`
is always 0 even when the link is poor, the stack is never being told
"congested".

**Cause.** `avdt_ad_write_req` returns 0 for failed, 1 for sent and 2 for
congested. A hook declared as returning `bool` turns 2 into 1; the stack then
keeps writing into a full channel.

**Fix.** The hook must return the original's value unchanged, as an 8-bit
number (`uint8_t`).

### The sound is clearly worse than AAC

Quiet passages are noisy, the sound is thin.

**Check.** `adb shell su -c 'setprop persist.aaceld.pcm_stats true'`, pause
and resume, and read the `pcm:` lines. They give the level entering the
encoder, which should be the level of the recording itself. On the
development phone, with absolute volume off, music arrived at -55 to -65
dBFS: the phone had turned it down before the encoder.

**Cause.** Absolute volume is off. The phone then sets the headset to full
volume and attenuates the samples itself, on the software path's 16 bits, so
most of the resolution is thrown away before encoding.

**Fix.** Turn absolute volume on. Xiaomi: "Keep volume consistent" on the
headset's page. Other phones: Developer options > "Disable absolute volume"
must be off. Set `pcm_stats` back to false.

### One bud is louder after a volume change

After changing the volume while music plays, the sound moves to one side.
Switching the codec off and on clears it.

**Cause.** The stream is on sample-count timestamps. The buds synchronise on
the timestamps and expect Apple's millisecond ones.

**Fix.** Leave `persist.aaceld.rtp_ms_after` at its default (6000). The log
shows `rtp clock now counts milliseconds` six seconds into each stream;
volume changes after that line keep the buds level. A volume change in the
first six seconds has not been tested.

### Silent for a long time after walking out of range

**Cause.** Only with `persist.aaceld.max_queue` set above 0, which was the
behaviour of early versions: the encoder stopped while the link was down, and
old packets were sent when it came back.

**Fix.** Delete the property (the default 0 keeps encoding in real time).

### No sound after pause and resume

**Cause.** Only with `persist.aaceld.rtp_seq_gaps` true on module v0.12.

**Fix.** Set it to false, or use a later version, where it is off by default.

### Every headset plays from the phone speaker

Other headsets, or these AirPods on plain AAC, play from the speaker.

**Check.** `adb shell getprop persist.bluetooth.a2dp_offload.disabled`
prints `true`.

**Cause.** Hardware offload was switched off for the whole phone. On the
development phone the audio system then cannot route any standard codec to a
headset at all. Versions of this module up to v0.4 did this; it was never
needed.

**Fix.**

```bash
adb shell su -c 'resetprop -p --delete persist.bluetooth.a2dp_offload.disabled'
```

and restart Bluetooth.

### Spatial audio will not switch on

Xiaomi's "Dimensional audio" (and possibly other makers' spatial effects)
cannot be enabled while AAC-ELD is the active codec.

**Cause.** By design of the phone's audio routing, not a fault: the
spatializer's output is connected to the hardware Bluetooth path only, and
AAC-ELD uses the software path. The system log says `can spatialize media
5.1: false` with AAC-ELD and `true` within a second of switching to AAC.

**Fix.** None in this project. Switch AAC-ELD off for spatial audio, on for
low delay. [notes/development-log.md](notes/development-log.md),
"Dimensional audio and AAC-ELD", has the details.

---

## If nothing here fits

Use the method in [PORTING.md](PORTING.md), "When the stack behaves
differently": capture the log of the moment, find the stack's own sentence
about what it decided, find the code that printed it with `tools/xref.py`,
and change the smallest thing. Then please add your case to this file.
