# Where this stands

11 November 2026, module v0.15 on HyperOS 4.0.6.0 (the last version played on
HyperOS 3 is v0.13). **It works.** AirPods 5 play AAC-ELD from the
Xiaomi 17 Pro Max, from a clean boot with nothing set beyond the one property
the module's own script sets, and with A2DP hardware offload left on.
Confirmed by ear, at every bit rate setting. The one open problem of v0.6, a
volume change leaving one bud louder than the other, is fixed in v0.7; see
"v0.7: timestamps change to milliseconds" below. The listening was done on
the same code with the delay supplied as a property; v0.7 itself, with the
delay built in and no property set, was then booted and seen to change over
6 s into a stream with no pause from the AirPods.

Two things were wrong. Each one alone was enough for silence.

## 1. No media packet ever left the phone

Every run before v0.2 negotiated the codec, started the stream, encoded
audio, and then the stack threw each packet away:

```
A2DP_GetPacketTimestamp: unsupported codec id 0x8001004cff
GetNextSourceDataPacket: unsupported codec type (255)
```

`A2DP_GetPacketTimestamp` (0x8ef590) is not a vendor dispatcher. It compares
the codec ID against a fixed list (SBC, AAC, aptX, aptX HD, LDAC, Opus, aptX
Adaptive) and returns false for anything else, and its only caller,
`bta_av_co_audio_source_data_path` (0x5dc530), then frees the packet. Every
earlier capture in `the development captures (not published)` that has encoder output has one of those
lines per packet and none has a packet reaching `avdt_ad_write_req`. All the
SBR, bit rate and pacing tuning in the earlier notes was done with nothing on
the air. The module now hooks the function.

## 2. The AirPods rejected the RTP timestamps

With packets flowing, the AirPods still played nothing: an AVRCP PAUSE 2.9 s
after every START, and if the stream was kept going anyway the bud stopped
taking data, dropped the link with a supervision timeout and came back on a
new BLE address, which is a reboot.

The stream carried millisecond timestamps, as macOS and the USBPods firmware
send. Switching to an ordinary 48 kHz sample count, +1440 per three-frame
packet, fixed it: no pause, clean audio.

Why the millisecond stamps fail here and not from USBPods is **not known**.
The first explanation was regularity: this stack ticks the encoder every
30 ms so exactly that every stamp was the previous plus 30, 162 out of 162,
where USBPods' stamps wander between 21 and 35 and macOS's between 29 and 33.
That was tested later and is wrong: stamps taken from the real clock with
random jitter, steps of 28 to 32, are rejected in exactly the same way, a
PAUSE 2.9 s after START (`persist.aaceld.rtp_ms=true`, v0.6). The likeliest
reading is that the AirPods expect sample units from this phone because of
something settled before the stream starts, so a clock that advances 30 per
packet looks 48 times too slow to them. What that something is has not been
found. One millisecond-stamped run that directly followed a good session was
not rejected.

Later the same day: the rejection belongs to the start of a stream, not to
the unit. A stream that starts on the sample count and changes to millisecond
stamps once it is running, the stamps carrying on from the last one, is
accepted. Changed by hand 8 s after START three times and 48 s into music
once, and by the module itself 6 s after START three times: no pause, playback
carried on, nothing heard at the change. The USBPods firmware notes that the
buds spend about the first 3 s after START locking onto a stream; that is
the window in which they reject millisecond stamps from this phone. Since
v0.7 the module does the change-over on every stream, 6 s in
(`persist.aaceld.rtp_ms_after`); flipping `persist.aaceld.rtp_ms` does it on
the running one. `tools/test/ts_switch.sh` runs the test. Delays shorter than
6 s have not been tried.

## What was ruled out on the way

The pause was chased through everything else first. Each of these was tested
by itself and made no difference:

| Suspect | How it was tested |
|---|---|
| Encoder bitstream | This phone's FDK and the one in USBPods-Pico2W give byte-identical output; the phone's captured frames decode cleanly |
| SBR on or off | Both rejected with millisecond stamps, both play with sample stamps |
| Bytes altered below the host stack | A Raspberry Pi Pico running a test receiver (not published) received 529 packets over the air, all byte-identical to the phone's HCI capture, none lost |
| Wrong endpoint | SET_CONFIGURATION, OPEN and START address the AirPods' SEID 3 |
| RTP SSRC, marker bit | SSRC zeroed in v0.2; the marker was never set |
| Stream content | A stream of pure digital silence was rejected too |
| Delay Reporting in the stream setup | Removed with `persist.aaceld.delay_report=false` |
| Device ID record | Presented an Apple-style record through `bluetooth.device_id.*` |
| AACP handshake | The Lite USBPods firmware has none and the same AirPods play it |
| Hands-Free link | Enabled on the Pico; disabled on the phone |
| AVRCP play status | Announced PLAYING from the Pico, in the phone's order |
| Ear detection | Wear state checked: still paused when in-ear |
| Link policy, sniff, role | Neither is used by the AirPods in the working Pico session |

## Fixed in the same pass

- `setPeerCodecCapabilities` returned the status-style zero where the stack
  wants a bool, so every connection logged "cannot update peer codec
  capabilities for AAC-ELD". `setCodecConfig` really is status-style.
- The RTP SSRC went out as 0x000000FF; it is zeroed, as macOS sends.
- Packets are assembled across ticks so each carries exactly three frames.
- The peak bit rate bounds every frame at 320 bytes in CBR too. Without it CBR
  frames reached 470.
- Encoder properties are re-read at every stream start.
- Defaults are VBR mode 5 without SBR, which is what macOS's stream looks
  like: frames starting 0x85 and 0x86, never above 320 bytes.

## v0.5: settings on the headset's page, and offload left alone

- **Hardware offload is no longer switched off.** Up to v0.4 the module set
  `persist.bluetooth.a2dp_offload.disabled`, on the belief that the stack only
  encodes in software with offload off. It chooses per codec: with offload on,
  a codec the DSP cannot encode gets the software path by itself. Tested from
  a clean boot with offload on, AAC-ELD plays as before. This matters beyond
  tidiness, because with offload off this phone cannot play a standard codec
  at all: the audio patch to the headset fails with -22 and the A2DP stream
  never starts. That is the "plain AAC plays from the speaker" seen earlier,
  and it would have hit every other headset too. Now AAC to the same AirPods
  streams normally, on the DSP.
- **AAC-ELD was never in the selectable codec list.** The object layout table
  had four members one slot off, so the module wrote its selectable capability
  into the user config. Nothing needed the list until Settings had to ask for
  the codec by type. Fixed; see `docs/notes/object-layout.md`.
- **Two settings**, `persist.bluetooth.aaceld.enabled` and
  `persist.bluetooth.aaceld.bitrate`, shown by `label/` as a switch and a
  drop-down on the headset's page. The switch moves a connected headset
  between AAC-ELD and AAC in about a second without reconnecting; the bit
  rate changes on the running encoder without a gap. Checked: off and on while
  connected, off and on across a Bluetooth restart, audio after each, and
  Xiaomi's own AAC switch toggled in between.
- **SBR and VBR measured**, in `module/README.md`: variable rate without SBR
  stays the default. SBR costs 30 to 40 % more encoder CPU for 1 to 5 % fewer
  bits; variable rate costs about the same CPU as constant 256 and sends 15
  to 40 % fewer bits.
- The stack no longer logs a line per media packet.

## v0.6: volume

Two separate things, both found from "the voice moves to the left".

- **"Keep volume consistent" has to stay on.** That switch is AVRCP absolute
  volume. With it off, Xiaomi sets the headset to full volume and attenuates
  the music on the phone, 31 to 37 dB at a normal listening level. Offloaded
  codecs take that at 24 or 32 bit. The software path is 16 bit and so is
  FDK's input, so the encoder was being fed music at -55 to -65 dBFS, quiet
  passages at -72 to -79, which the headset then amplifies back up. Heard as
  clearly worse sound. `label/` now refuses to turn the switch off while
  AAC-ELD is the active codec and turns it on if it finds it off.
- **A volume change while playing left the buds at different levels.**
  With absolute volume on, a test tone identical in both channels moved to
  the left after a volume step. Measured: the PCM entering the encoder is
  identical in both channels before and after
  (`persist.aaceld.pcm_stats=true`), the encoder does not favour a channel
  even 65 dB down (offline model against the same FDK), the phone sends one
  ordinary `SetAbsoluteVolume` per step which the AirPods accept with the
  same value, and the media stream is undisturbed, 785 of 785 packets in
  step. So the difference arose inside the AirPods. Plain AAC to the same
  AirPods does not do it, and neither does USBPods, which sends the same
  AVRCP volume command but millisecond timestamps (the owner's report).
  Switching AAC-ELD off and on cleared it. Fixed in v0.7.

## v0.7: timestamps change to milliseconds

The bud imbalance came from the sample-count timestamps. The buds
synchronise on the RTP timestamps and Apple's are milliseconds; v0.3 to v0.6
counted samples for the whole stream because the AirPods reject millisecond
stamps from this phone at the start of a stream (section 2).

A stream now starts on the sample count and changes to millisecond stamps
6 s after its first packet, carrying on from the last stamp. With that:

- The AirPods keep playing through the change and nothing is heard at it.
- On two streams that had changed over by themselves, the owner made four
  and seven volume steps while music played (the log has every
  `SetAbsoluteVolume` after the change-over line) and reported the buds
  stay balanced. Before, one or two steps were enough to move the image.

What this leaves:

- A volume change in the first 6 s of a stream is still on the sample count.
  Whether it can leave the imbalance, and whether the change-over then
  clears it, has not been tried.
- The delay is the only value tried. The rejection at the start is over by
  about 3 s, so something shorter may work; a delay that is too short pauses
  the music, which is worse than the six seconds.
- Why the start of the stream is different from USBPods', which sends
  millisecond stamps from its first packet to the same AirPods, is still not
  known.
- Only AirPods 5 have been tried.

## v0.8: 288 and 320 kbps

Constant 288 and 320 kbps, above the 256 kbps the headset advertises, tried
at the owner's request by changing the rate of music that was playing.
AirPods 5 took both: about a minute at 288 (361 byte frames) and half a
minute at 320 (401 byte frames), no pause from the headset, every L2CAP
write accepted, transmit queue never above zero. Three frames of that size
do not fit one 986 byte packet, so the module sends two per packet at these
rates, 50 packets a second. SET_CONFIGURATION still says 256000.

In that trial the stack was ticking every 30 ms, so the two-frame packets
left in an uneven one, one, two pattern. v0.8 asks the stack for a 20 ms
tick when a stream starts at one of these rates.

Not known: whether they sound better than 256, how they hold up at range,
and what they cost in battery.

## v0.9: a maximum bit rate and a separate variable-rate switch

Asked for by the owner after hearing 320 kbps. The single bit-rate setting
(0 for "Automatic", otherwise a constant rate) became two:
`persist.bluetooth.aaceld.bitrate` is now the maximum bit rate, and
`persist.bluetooth.aaceld.vbr` says whether the rate is variable under it or
constant at it. Variable rate never goes above 256 kbps whatever the
maximum. Unset, the two give what "Automatic" was. `label/` shows them as a
switch and a slider with four stops: 192, 256, 288 and 320 kbps.

Checked on the phone, on one stream of the test melody with the settings
changed while it played, no pause from the AirPods and no congestion:

| Variable | Maximum | Encoder | On the air | Largest frame | Frames per packet |
|---|---|---|---|---|---|
| off | 288 | constant 288 | 288 kbps | 360 bytes | 2 |
| on | 288 | variable, bound 256 | 160 kbps | 319 bytes | 3 |
| on | 192 | variable, bound 192 | 159 kbps | 240 bytes | 3 |
| off | 192 | constant 192 | 192 kbps | 310 bytes | 3 |
| off | 320 | constant 320 | 320 kbps | 400 bytes | 2 |
| on | 320 | variable, bound 256 | 160 kbps | 317 bytes | 3 |

A stream that starts at 288 or 320 gets a 20 ms tick from the stack and its
two-frame packets leave evenly; the change to millisecond timestamps 6 s in
works at that cadence too. On the page, tapping the switch and the slider
stops writes the two properties, read back after each tap.

## v0.10: no longer tied to one build

Asked for by the owner ahead of a Xiaomi Bluetooth update. The hardcoded
address table, the hardcoded object offsets and the build-ID check are gone;
`module/README.md` describes what replaced them. On the development build:

```
found all 35 functions by name ...; encoder interval is in milliseconds
measured: codec_config_ 0x60, codec_user_config_ 0x108, no codec_capability_
installed 19 hooks, all succeeded
layout: priority 0x58 config 0x60 local 0x98 selectable 0xd0 user 0x108 audio 0x140
        ota 0x178 peer capability 0x1a5 peer config 0x1d2, 45 bytes each
ready: AAC-ELD takes the Opus slot, codec index 6
```

Every figure equals the hand-derived one, start-up takes about 130 ms, the
AirPods negotiated AAC-ELD and the test melody played with the timestamp
change-over as before. Checked from a reboot and again after killing the
Bluetooth process. Not checked: any other build. The start-up marker and the
pass-through path for a build that does not fit have not been exercised,
because this build fits.

### "Bluetooth version: Update" in Settings is not the stack

The update the owner then installed from the Bluetooth settings page turned
out not to touch the stack. That row belongs to `BluetoothCloudControlTools`
in the Settings app and downloads Xiaomi's headset plugin for Settings, a
dynamically loaded split under
`/data/user_de/0/com.android.settings/app_qigsaw/` (version 1.95 here).
`libbluetooth_jni.so` was byte-identical afterwards and no package or APEX
had changed. The rows `label/` adds were still on the headset's page with
the new plugin. So v0.10 has still only met the one stack build.

## v0.11: checked against HyperOS 4.0.6.0 (Android 17) without flashing it

The owner supplied a ROM package for the same phone, `OS4.0.6.0`, Android 17.
Its Bluetooth library was taken out of the package and looked at on the Mac
(`tools/rom/`: the super image's partition table, a small EROFS reader
with LZ4, then `debugfs` for the APEX payload). Build ID
`e2911fd00d152b418728edd5b3742931`, `Bluetooth@CP2A.260605.016`; a copy is kept
outside this repository.

What that showed:

- **v0.10 would have stayed out.** Four vendor dispatchers it required are
  gone from this stack: `A2DP_VendorAdjustCodec` and `A2DP_VendorGetBitRate`
  are removed, `A2DP_VendorCodecName` and `A2DP_VendorCodecIndexStr` are
  folded into the generic functions. Everything else, 32 names, is there
  with the same signatures. v0.11 makes those four optional and hooks the
  generic name functions where the vendor ones are missing.
  `tools/check_lib.py` now reports "would start" for both libraries.
- **The object layout is the same**, read from the disassembly:
  `codec_config_` at 0x60, `codec_user_config_` at 0x108, OTA config at
  0x178, 45 bytes. But `getCodecConfig()` now logs the codec's name before
  it copies. The layout probe used to fill the dummy object with its pattern
  from 0x30, name included; it got away with that here only because of what
  the pattern's bytes happen to mean to `std::string`. v0.11 leaves
  everything ahead of 0x58 zero and moves the pattern down only if needed.
- **The rest of what the module leans on is still there**: the Opus codec
  class and `persist.bluetooth.opus.enabled`, `A2DP_GetPacketTimestamp`, the
  stock AAC encoder's interface with the same callback signatures and a
  millisecond interval, the FDK encoder, `avdt_ad_write_req`, and the same
  set of offload-or-software functions (`IsCodecOffloadingEnabled` is no
  longer a separate function).
- **The Settings app** in the ROM has every class, method, field and
  preference key `label/` uses, by name.

None of this is a run. Not known until it is flashed: whether the probe and
the hooks behave on that stack, whether a codec the DSP cannot encode still
gets the software path, whether the AirPods negotiate, and whether Zygisk
and the Xposed framework work on Android 17 at all. v0.11 itself has only
been built and checked against the two libraries, not booted.

## Add-on 1.6: the rows on Xiaomi's own AirPods page too

Found by the owner with AirPods Pro: AirPods that Xiaomi has in its database
do not get the stock headset page but one drawn by the headset plugin, and
the rows were only on the stock page. `label/` 1.6 adds them there as well
(`label/README.md`, "Two pages, same rows"). Checked on the phone with those
AirPods Pro: the three rows appear under the plugin's AAC switch; tapping the
variable-rate switch and a slider stop writes the properties; the AAC-ELD
switch moves the headset between AAC-ELD and AAC from that page.

The same session answered why "Dimensional audio" on that page will not
switch on: it is unavailable whenever AAC-ELD is the active codec. With
AAC-ELD the audio service logs "can spatialize media 5.1: false on device
bt_a2dp" and disables the effect; a second after switching to AAC it logs
"true" and enables it. Seen three times in a row, in both directions. The
audio policy has one spatializer output, in the primary audio module with
the offloaded Bluetooth device behind it; the software Bluetooth path that
AAC-ELD uses is a different module. Routing the spatializer there has not
been tried.

## Dimensional audio and AAC-ELD

Looked into at the owner's request: can the phone's spatial effect be made
to work while AAC-ELD is the active codec? Not with anything this project
can reasonably change. What the audio policy shows
(`dumpsys media.audio_policy`, taken with AAC-ELD active):

- The phone has one spatializer output, the mix port `spatial_out`, in the
  **primary** audio module. Its Bluetooth route ends at that module's own
  `bt_a2dp_out` device, which stands for the DSP's hardware encoder and
  lists the formats it can encode: SBC, AAC, aptX, aptX HD, LDAC and so on.
- Which Bluetooth device the framework attaches is decided by the codec the
  stack reports. AAC-ELD sits in the Opus slot, the DSP cannot encode it,
  so the headset is attached as `BT A2DP Out` of the separate **bluetooth**
  audio module, the software path, and `bt_a2dp_out` stays unconnected.
- The only route to `BT A2DP Out` is from that module's single mix port,
  `a2dp output`, which can be opened once. There is no spatializer output
  there, so "can spatialize" is false for the headset.

To bridge it the bluetooth audio module would need a spatializer output of
its own, and a way to play it alongside its ordinary output into the one
software A2DP stream. That module is part of the vendor audio service, not
of the Bluetooth stack or of anything a Zygisk module reaches, and its
single stream is where a second output would have to be mixed in. Changing
the vendor audio service and the audio policy is a different project with a
real chance of leaving the phone without sound, and it has not been
attempted.

What was done instead: on Xiaomi's AirPods page the module now says why the
switch will not turn on (`label/` 1.7). The AAC-ELD switch on the same page
is the way to choose: off gives AAC with Dimensional audio, on gives
AAC-ELD without it.

## v0.12 and v0.13: out of range and back

Reported by the owner: walk out of range, come back, and the AirPods stay
silent for a minute or more.

The cause was this module's own handling of a link that stops taking audio.
From v0.2 it stopped encoding once six packets were waiting and dropped the
PCM instead, to keep the Apple frame numbers consecutive. So the packets
already queued sat there for the whole outage and went out when the link
came back, however old, followed by frames whose timestamps had jumped by
the length of the outage with not one frame missing. The owner's macOS study
describes the behaviour that recovers at once as the opposite: the encoder
never pauses, anything stale is discarded, and the AirPods conceal the gap.

v0.12 does that. The encoder carries on at real time and the stack does what
it does for every other codec: it lets its transmit queue fill to 27 packets
and throws it away, again and again, so nothing older than about half a
second is ever waiting.

Walk test, AirPods Pro, constant 320 kbps, logged on the phone:

| Outage | Lasted | Stack emptied its queue | Packets made meanwhile |
|---|---|---|---|
| 20:12:57 | 4.3 s | 8 times | 266 |
| 20:13:03 | 4.3 s | 7 times | 264 |
| 20:13:09 | 0.8 s | once | 92 |
| 20:13:21 | 15.5 s | 15 times | 825 |
| 20:20:24 | 5.6 s | 9 times | 331 |
| 20:20:32 | 2.9 s | 4 times | 194 |

The stream was never suspended or restarted, the headset never pressed
pause, and after each outage the first packet on the air carried a timestamp
and an Apple frame number that had both advanced by the real time gone by.
The owner's report: "pretty good". What was not measured is how long the
sound took to come back in seconds.

Two things in the same builds that are off and unproven:

- `persist.aaceld.rtp_seq_gaps`: makes the RTP sequence number show the
  discarded packets too, as macOS's does. v0.12 shipped a broken version of
  it switched on, which numbered packets from 1 again at every pause and
  resume; the AirPods discarded everything as old and there was no sound
  until it was switched off. v0.13 has it rebuilt on top of the stack's own
  continuing count, and off by default. The walk test passed without it.
- `persist.aaceld.recover_kick_ms`: an AVDTP suspend and start after an
  outage, the recovery in the USBPods notes. Never run.

## v0.14: running on HyperOS 4.0.6.0 (Android 17)

The owner flashed the ROM that v0.11 was checked against on paper. v0.13
started on it exactly as predicted (every name found, same layout, "ready")
and then did not work: AirPods connected on AAC, and switching AAC-ELD on
from their page sent music to the phone speaker. Three things in this stack
differ from the Android 16 one, and v0.14 has one hook for each.

1. **The slot ranks below AAC.** The Opus codec is made with priority 1501
   here (AAC 2001), from a resource with no property behind it; on Android 16
   it was 8001. The Opus source codec's constructor is hooked and handed 8001
   (`persist.aaceld.priority`, 0 leaves the stack's figure).
2. **No audio session after a change between AAC and AAC-ELD.** This build
   keeps the audio session across a codec change ("UpdateCodecConfig: ignore
   ending session", then "Reconfig done - Ignore calling startSession(), just
   update codec to audio HAL") whenever the AIDL audio HAL is in use. That
   holds between two codecs the DSP encodes. Going from AAC (DSP) to AAC-ELD
   (encoded in the stack) the codec setup ends the hardware session, moves to
   the software one and starts nothing. The audio HAL then answers
   "session_type=A2DP_SOFTWARE_ENCODING_DATAPATH is not ready", the audio
   policy fails with -38 ("handleDeviceConfigChange failed for A2DP device
   ... codec=AUDIO_FORMAT_OPUS"), removes the headset as an output, and music
   plays on the speaker. `btif_a2dp_source_setup_codec` is hooked: when the
   kind of session is different after it than before, the module calls the
   stack's own `bluetooth::audio::a2dp::start_session()`
   (`persist.aaceld.session_start`). On a fresh connection the stack's own
   start follows and finds the session already started; in v0.14 as shipped
   it did not follow, which is the bug v0.15 fixes below. The Android 16 build
   does not have the function under this name and restarts the session
   itself.
3. **Opus is disabled at every ordinary stream start.** Android keeps Opus
   for low-latency mode only. Here `A2dpStreamCallbacks::StartStream` calls
   `btif_av_source_set_low_latency_codec(false)`, which sends a codec
   preference "Opus, priority -1" when Opus is the codec in use. The first
   build of v0.14 showed it: AirPods connected on AAC-ELD at priority 8001,
   and five seconds later the first stream start disabled the slot and they
   were reconfigured to AAC. That function is hooked and does nothing while
   the module is in. Android 16 asks the Bluetooth app instead
   (`invoke_switch_codec_cb`), which never did this there; it is hooked the
   same way where it exists.

Checked on the phone after a reboot: AirPods Pro connect on AAC-ELD at
priority 8001 with music routed to them; AAC-ELD off on the page gives AAC,
on gives AAC-ELD again, with the route on the headset each time and "audio
session changed to DSP / software encoding: starting it" in the log; a
14-second silent stream ran through the change to millisecond timestamps
with no pause from the headset. Listening on this ROM is the owner's to
report.

The errors `A2DP_ParseInfoOpus: invalid codec ie length 16` and
`A2DP_VendorGetTrack...Opus: cannot decode codec information` that this build
logs at each codec setup are the stack's Opus-specific helpers being shown
Apple's element. They are not what broke the route: they are still logged
now that the stream plays.

### v0.14 against the HyperOS 3 stack, without that ROM on the phone

The phone is on HyperOS 4 now, so v0.14 cannot be played on HyperOS 3 any
more. What changed for that build since v0.13, which was played on it for
days, is small: two more hooks go in (the Opus constructor, where 8001 is
handed 8001, and `invoke_switch_codec_cb`, which is swallowed); the session
start hook does not, because that build has no function of that name. The
encoder and everything on the stream path are the same code as v0.13.

`tools/harness/` checks the rest for real. It loads a copy of a stack
library into a scratch root process on the phone (the library's own
dependencies come from the phone's Bluetooth APEX; Bluetooth itself is not
touched), runs the module's start-up on it, and then calls through the
hooks on objects the stack builds itself:

```
tools/harness/run.sh os3 work/os3/libbluetooth_jni.so
tools/harness/run.sh os4 work/os4/libbluetooth_jni.so
```

| | HyperOS 3 library | HyperOS 4 library |
|---|---|---|
| names found | 43 of 45 | 40 of 45 |
| layout | priority 0x58, config 0x60, user 0x108, ota 0x178, 45 bytes | the same |
| hooks installed | 21, all succeeded | 20, all succeeded |
| codec index asked of the stack | 6 | 6 |
| low-latency request, both values | swallowed | swallowed |
| stack's Opus constructor called with 1501 | object holds 8001 | object holds 8001 |
| stack-built object given Apple's capability | configures `... 00 80 00 84 00 83 e8 00` | the same |

The HyperOS 4 column is the control: it matches line for line what the
module logs inside the real Bluetooth process on the phone. What the harness
does not do is stream: no AVDTP, no audio HAL, no headset.

## v0.15: reconnecting on HyperOS 4

v0.14 lasted one evening. The owner moved the AirPods to a Mac and connected
them to the phone again; Bluetooth settings then read "Connected | Active
(call audio)" with the AAC-ELD badge, and music played on the phone speaker.
The audio service had refused the headset 110 ms after it connected ("APM
failed to make available A2DP device ... error=1"), and tapping the device
did nothing, because the Bluetooth app already held it as the active media
device ("setActiveDevice: current is ... no changed"). Switching "Media
audio" off and on from the AirPods' page reproduced it.

The cause was the hook v0.14 put on `btif_a2dp_source_setup_codec`. The
function returns whether the setup worked (its body ends `and w0, w20, #1`)
and its caller on a connection, `btif_a2dp_source_start_session_delayed`,
acts on that: `tbz w0, #0` jumps past its own `start_session()` and past
fulfilling the promise the Bluetooth app waits on. The hook was declared
`void`. What the caller read in `w0` was whatever the hook's last call had
left there, and on the path where the kind of session had not changed that
was the answer to "is the session DSP encoded?". With AAC-ELD selected the
answer is no, so the stack was told the setup had failed and started no
audio session. The recorded log of a failing reconnection ends:

```
setup_codec: is_audio_share_mode false
IsCodecOffloadingEnabled: software codec=CodecConfiguration{codecType: OPUS, ...
UpdateAudioConfig: BluetoothAudioHal session has not started
is_hal_offloading
btif_av_source_set_active_device: check pending play cmd
```

with no `start_session` between the last two lines. The first connection
after Bluetooth starts never showed it. The audio HAL interface begins on
the hardware side, the first setup for AAC-ELD moves it to software
("Switching BluetoothAudio HAL to Software"), and for that case the hook
starts the session itself. Every later connection in the same Bluetooth
process finds the interface on the software side already, so nothing
changes and the wrong value went back. A connection while AAC was selected
got "yes" and worked, which is why only AAC-ELD lost its sound. The same
hook's result is ignored on the codec-change path it was written for, so
the switch on the page kept working and hid the mistake.

v0.15 declares the hook `bool` and returns what the stack's function
returned, on every path, and does nothing more when the setup failed. The
other two hooks v0.14 added were checked for the same mistake:
`btif_av_source_set_low_latency_codec` (HyperOS 4) and
`invoke_switch_codec_cb` (HyperOS 3) are each called from one place,
`A2dpStreamCallbacks::StartStream`, in the library that has them, and `w0`
is overwritten straight after the call.

Checked on the phone, HyperOS 4.0.6.0, after a reboot:

- First connection: "Switching BluetoothAudio HAL to Software", the hook's
  "audio session changed to software encoding: starting it", then the
  stack's own `start_session`, which finds the session started
  ("StartSession: session started already") and goes on to
  `set_remote_delay` and the codec report. "A2DP sink device ... now
  available", music routed to the headset, and the owner's own music then
  ran at 320 kbps, 50 packets a second, queue 0.
- "Media audio" off and on: no change of session kind, the stack's own
  `start_session`, "startSession - SessionType=A2DP_SOFTWARE_ENCODING_
  DATAPATH" in the audio HAL, the headset available again and the route back
  on it. The same steps on v0.14 left the route on the speaker.
- The 20 s silent file after that reconnection: 666 packets in and out in
  13 s, through the change to millisecond timestamps.
- `tools/harness/run.sh` for both libraries: PASS, same figures as in
  the v0.14 table above.

Not rerun on v0.15: the AAC-ELD switch on the page (that path does not read
the hook's result), a reconnection after the headset was taken by another
device (it goes through the same `btif_a2dp_source_restart_session` as the
"Media audio" switch, and is what failed on v0.14), and Bluetooth off and
on.

What let this through: v0.14 was tested from a reboot and by switching the
codec, never by disconnecting and connecting again, and the harness cannot
see it because it does not run the audio session code. A reconnection in
the same Bluetooth process belongs in the checks for any build that touches
the connection path.

### What has been verified on which system (v0.15)

| | HyperOS 3 (Android 16) | HyperOS 4.0.6.0 (Android 17) |
|---|---|---|
| module start-up: names, layout, hooks | v0.15 in the harness; v0.10 to v0.13 on the phone | v0.15 on the phone and in the harness |
| AirPods connect on AAC-ELD by default | v0.13 on the phone | v0.15 on the phone, from a reboot |
| AAC-ELD off and on from the page, sound stays on the headset | v0.13 on the phone | v0.14 on the phone, both directions; not rerun on v0.15 |
| stream runs through the change to millisecond timestamps | v0.13 on the phone | v0.15 on the phone |
| 320 kbps constant rate, real music | v0.13 on the phone | v0.14: the owner's own listening, 8 minutes, 24082 packets, none failed or queued; v0.15: the first minute after the reboot, same rate |
| 192 / 256 / 288 kbps, variable rate | v0.13 on the phone | not run on this system |
| out of range and back | v0.13 on the phone | not run on this system |
| volume change keeps both buds level | v0.13 on the phone | not run on this system |
| media disconnected and connected again without restarting Bluetooth | v0.13 on the phone | broken in v0.14; v0.15 on the phone ("Media audio" off and on) |
| Bluetooth off and on, reconnect | v0.13 on the phone | not run on this system |
| the rows on the stock page (AirPods the phone does not know) | add-on 1.7 on the phone | not looked at; the plugin page has them |

For HyperOS 3 "on the phone" always means v0.13 or earlier: v0.14 and v0.15
differ there by the two hooks described above and have only been through the
harness. The hook v0.15 corrects is not installed on that build, which has
no function of that name. For HyperOS 4 the rows marked "not run" use the same encoder and
packet code that is shown working at 320 kbps; they are untested, not known
to be broken.

## What is not done

- **A headset that really speaks Opus.** The module raises the Opus slot to
  priority 8001 on HyperOS 4 and stops the stack from disabling Opus outside
  low-latency mode. For AirPods that is the point; an Opus headset (Pixel
  Buds Pro) would now get Opus where stock HyperOS 4 gives it something
  else. Not tried. `persist.aaceld.priority=0` puts the stack's ranking back.
- **Other headsets with the module installed.** Not played. Since v0.5 they
  are on the stock offload path, and the one standard codec that was checked,
  AAC to the AirPods with AAC-ELD switched off, streams normally. LDAC, LHDC
  and SBC headsets have not been tried.
- **Per-headset settings.** The switch and the bit rate are phone-wide.
- **Battery.** Encoder CPU time and bit rate were measured; drain was not.
- **Long sessions.** A ten-minute stream with six outages in it ran
  through (v0.12). Hours-long sessions have not been watched.
- **Codec name.** The badge in the Bluetooth device list reads "AAC-ELD"
  with `label/` installed. Developer options still say Opus, and choosing AAC
  or SBC there overrides AAC-ELD until the headset reconnects.
- The phone still has a bond for the test sink, "FakePods ELD". Forget it in
  Bluetooth settings.

## Testing

```bash
AACELD_MEDIA_ID=560 tools/test/run_test.sh mylabel 15
```

Restarts Bluetooth, waits for the headset and for music to be routed to it,
plays that MediaStore audio file and prints what went on the air and how the
headset reacted. It will not play if music is not routed to the headset, and
it stops playback when it finishes. `tools/test/make_test_tone.py` makes a
recognisable test melody and says how to get its id.

```bash
AACELD_MEDIA_ID=560 tools/test/rate_sweep.sh mylabel 10
```

Plays the same way but steps the bit-rate setting while playing (automatic,
256, 192, 128 kbps by default) and prints the average bit rate and encoder
CPU time for each step. `AACELD_PROPS="persist.aaceld.sbr=1"` adds SBR for
the run.

For anything deeper, `persist.bluetooth.btsnooplogmode full` gives an HCI
capture, which `tools/avdtp.py` decodes. Delete captures once read: they
contain link keys. The packet validator, the event timeline and the Pico
test receiver used during development are not published here.

## Reverting completely

```bash
adb shell su -c '/data/adb/ksud module uninstall aaceld_a2dp'
```

Then reboot. The uninstall script clears every property the module reads. One
change lives outside the module: `com.android.bluetooth` had `umount_modules`
turned off in its KernelSU app profile so Zygisk would leave the process
alone.
