# How it works

Two parts: what the module does inside the phone, and what goes over the air.
[PORTING.md](PORTING.md) uses the first; anyone writing another sender for
this codec wants the second.

## 1. Inside the phone

### The problem

AirPods offer three codecs to any phone: SBC, AAC, and Apple's own variant of
AAC-ELD, a low-delay codec. Android's Bluetooth stack reads the third offer,
does not recognise the vendor and codec numbers in it, and drops it without a
word. So an Android phone always ends up on AAC.

Three things are missing in the stack for that codec: recognising the offer,
negotiating it, and an encoder. The first two are a handful of small
functions that answer questions about a codec. The third is already there:
the stack carries Fraunhofer's FDK-AAC encoder for plain AAC, and that
encoder can also produce AAC-ELD.

### The approach

Nothing on disk is changed. A Zygisk module is loaded into the Bluetooth
process (`com.android.bluetooth`) when it starts, and replaces the beginning
of about twenty functions in memory ("hooks"). For Apple's codec the module
answers; for everything else the call goes on to the original function, so
other headsets see a stock phone.

```
 music app
    │ PCM, 48 kHz 16-bit stereo
    ▼
 Android audio system ──(audio session, software path)──┐
                                                        ▼
 ┌──────────────── com.android.bluetooth ────────────────────────────┐
 │  libbluetooth_jni.so (the stack)                                  │
 │                                                                   │
 │   codec questions ──hook──▶ module: "yes, that is AAC-ELD,        │
 │   (valid? which slot?        it lives in the Opus slot"           │
 │    how to frame?)                                                 │
 │                                                                   │
 │   negotiation on the ──hook──▶ module: accept 48 kHz stereo,      │
 │   Opus codec object            write Apple's element              │
 │                                                                   │
 │   encoder interface ──hook──▶ module's encoder ──calls──▶ FDK-AAC │
 │                                  │ frames + Apple headers         │
 │                                  ▼                                │
 │   stack's packet queue ──▶ AVDTP adds the RTP header              │
 │                                  │                                │
 │   avdt_ad_write_req ──hook──▶ module: clear marker, zero SSRC     │
 └──────────────────────────────────│────────────────────────────────┘
                                    ▼
                                 AirPods
```

### Start-up, step by step

All of this is in [src/main.cpp](../src/main.cpp),
[src/target.cpp](../src/target.cpp) and `hooks_install()` in
[src/hooks.cpp](../src/hooks.cpp). Each step logs a line; if a step fails the
module stops and the stack is left as it was.

1. **Attach.** Zygisk loads the module into every app process as it starts.
   The module stays only in `com.android.bluetooth` and unloads itself from
   the rest.
2. **Wait for the library.** `libbluetooth_jni.so` is loaded a little after
   the process starts; the module polls for it.
3. **Leave a marker.** The property `persist.bluetooth.aaceld.probe` is set
   to the library's build ID until start-up is over. If the process crashes
   in between, the next start sees the marker and stays out. A build the
   module cannot cope with costs one Bluetooth restart, not a crash loop.
4. **Find the functions by name.** System libraries are stripped, but Android
   keeps a compressed table of function names inside them (the
   `.gnu_debugdata` section). The module looks up 30 names it must have and
   up to 15 more it can use. The names are C++ mangled names, which include
   the parameter types, so a function whose parameters changed is simply
   "not found". Two more names are only checked for existence: they prove the
   encoder interface has the shape the module fills in.
5. **Measure the codec object, part one.** The stack keeps one C++ object per
   codec, and the module must write into the borrowed one: the chosen
   configuration, the capabilities, the bytes sent to the headset. Where
   those fields are differs between Android versions and makers. So the
   module builds a dummy object in which every word holds its own offset,
   calls the stack's own `getCodecConfig()` and `getCodecUserConfig()` on it,
   and reads from the result where they copied from.
6. **Install the hooks.**
7. **Ask which slot Opus has**, by giving the stack Opus's own element.
8. **Measure the codec object, part two.** The byte arrays for the on-air
   elements are found by putting Apple's element where the array should be
   and asking `copyOutOtaCodecConfig()` for it back. This needs the hooks,
   because the stack only copies an element it considers valid.
9. **Ready.** From here the hooks answer for Apple's codec.

### Why the Opus slot

The stack has a fixed table of codec objects. Adding one would mean building
a C++ object with a virtual table at run time. Opus already has an object and
a place in the table, is switched off by default, and no AirPod speaks it. So
the module switches Opus on (`persist.bluetooth.opus.enabled`), and hooks the
two methods through which that object negotiates. When the headset's offer is
Apple's element the module does the negotiation itself; a real Opus offer
still goes to the original code.

The cost: Android's developer options and `dumpsys` call the codec "Opus".
To tell the two apart, every configuration the module writes carries
`0x004C8001` (Apple's vendor and codec number) in the field
`codec_specific_1`. Java code sees it as `getCodecSpecific1()`.

### Negotiation

When a headset connects, the stack reads each of its endpoints' offers.

- `A2DP_IsVendor...Valid`, `A2DP_VendorSourceCodecIndex`: the module says the
  element is valid and belongs to the Opus slot.
- `setPeerCodecCapabilities` (hooked): records the offer as selectable.
- `setCodecConfig` (hooked): checks the offer has AAC-ELD, 48 kHz and stereo
  or mono; writes the configuration into the codec object; produces the
  element to send. If `persist.bluetooth.aaceld.enabled` is false it declines
  instead, and the stack moves on to AAC.
- The stack then sends SET_CONFIGURATION to the headset's AAC-ELD endpoint.

The stack picks the codec with the highest priority among those both sides
have. The slot must rank above AAC for this to be automatic; where the
stack's default puts it below, the module raises it
(`persist.aaceld.priority`).

### The encoder

The stack drives software encoders through a table of eight functions
(`A2dpEncoderInterface` in [src/target.h](../src/target.h)). The module
supplies its own table, in [src/eld_encoder.cpp](../src/eld_encoder.cpp):

- `encoder_init`: opens FDK-AAC for AAC-ELD, 48 kHz, 480 samples per frame.
- `get_encoder_interval_ms`: 30, so the stack calls every 30 ms.
- `send_frames`: on each call, reads PCM through the stack's callback,
  encodes as many 10 ms frames as are due, puts Apple's four-byte header in
  front of each, collects three frames into a packet and hands it to the
  stack's queue.

The stack's AVDTP layer adds the RTP header and sends the packet. The module
hooks the last function on that path, `avdt_ad_write_req`, to correct two
header fields (below) and to log what really leaves the phone.

### One function that is easy to miss

`A2DP_GetPacketTimestamp` reads the timestamp back out of each finished
packet. It has a fixed list of codecs. For any other it returns false, and
its caller then frees the packet. Without a hook there, everything appears to
work (the codec is negotiated, the stream starts, the encoder runs) and not
one packet is sent.

### Hardware offload is left alone

Most phones encode Bluetooth audio in the audio chip (DSP), not in the
stack. The stack decides per codec: one the DSP does not know is encoded in
the stack's own process, the *software path*, and the rest stay on the DSP.
The module relies on that and changes nothing about offload.

Moving between a DSP codec and a software codec means moving between two
kinds of audio session. On some stacks nobody starts the new session after a
codec change; the module has a hook for that
(`persist.aaceld.session_start`). This is the area most likely to differ on
another phone.

## 2. On the air

Everything here was measured against AirPods 5 and AirPods Pro, and agrees
with the description of the codec in the
[USBPods-Pico2W](https://github.com/wasdwasd0105/USBPods-Pico2W) project
(`aac-eld-apple.md` there), which this work started from.

### The headset's offer

AirPods list three stream endpoints. The third carries, as its Media Codec
capability:

```
10 00 ff 4c 00 00 00 01 80 00 80 00 8c 00 83 e8 00
```

| Bytes | Value | Meaning |
|---|---|---|
| `10` | 16 | length of what follows |
| `00` | | media type: audio |
| `ff` | | codec type: vendor specific |
| `4c 00 00 00` | 0x0000004C | vendor: Apple (little endian) |
| `01 80` | 0x8001 | codec: AAC-ELD (little endian) |
| `00 80` | 0x0080 | object type bitmap: AAC-ELD (big endian) |
| `00 8c` | 0x008C | top 12 bits 0x008: 48 kHz only. Low 4 bits 0xC: mono (8) and stereo (4) |
| `00` | | reserved |
| `83 e8 00` | 0x83E800 | top bit: variable bit rate supported. Rest: 256000, the maximum bit rate |

### What the phone answers

SET_CONFIGURATION to that endpoint, with the same element narrowed to one
choice each:

```
10 00 ff 4c 00 00 00 01 80 00 80 00 84 00 83 e8 00
```

Stereo only (`84`), the variable-rate bit set, 256000. macOS sends the same.
The bit rate here is a ceiling that is stated once; the encoder may run
lower, and the module keeps 256000 here whatever the bit-rate setting is, so
that the setting can change without renegotiating.

### A media packet

```
┌──────────── RTP header, 12 bytes ────────────┐┌─ frame ─┐┌─ frame ─┐┌─ frame ─┐
 80 60 SSSS TTTTTTTT 00000000                    H H H H AU  H H H H AU  H H H H AU
```

RTP header:

| Field | Value |
|---|---|
| version, flags | `80` |
| marker bit | 0 |
| payload type | 96 |
| sequence number | counts packets |
| timestamp | see below |
| SSRC | 0 |

Android's stack would set the marker bit and an SSRC of 0x000000FF for a
codec in this slot. macOS sends zero for both, so the module clears them.

Each frame is a four-byte header and one raw AAC-ELD access unit (no ADTS or
LATM wrapper):

| Byte | Content |
|---|---|
| 0 | `0xB0` + the top 4 bits of the frame counter |
| 1 | the low 8 bits of the frame counter |
| 2 | `0x10` + the top 4 bits of the access unit's length |
| 3 | the low 8 bits of the length |

The frame counter is 12 bits, goes up by one per frame across packets, and
wraps at 4096. The length is at most 2047 bytes. In the log:
`hdr=b0 01 10 06` is frame 1, 6 bytes long.

### The audio inside

| Property | Value |
|---|---|
| Codec | MPEG-4 ER AAC ELD (audio object type 39), no SBR |
| Sample rate, channels | 48 kHz, stereo |
| Frame | 480 samples, 10 ms |
| Frames per packet | 3 (30 ms). At 288 and 320 kbps: 2, because three no longer fit one packet |
| Bit rate | variable, with no frame over 320 bytes (what 256 kbps allows), which is what macOS's stream looks like; or constant |

AirPods play only the 480-sample frame length, not the 512-sample one.
Constant 288 and 320 kbps are above the advertised maximum; AirPods 5 and
AirPods Pro play them anyway.

### The RTP timestamp: the part that is not obvious

Apple's own senders count **milliseconds** in the RTP timestamp, not audio
samples: +30 for a 30 ms packet, with a few milliseconds of natural jitter.

From this phone, a stream that *starts* on millisecond stamps is rejected:
the AirPods send an AVRCP pause about 2.9 seconds after the stream starts. A
stream that counts samples (+1440 per three-frame packet at 48 kHz) plays.
Why the start is different from Apple's own senders is not known.

But a stream that stays on the sample count has a defect: after a volume
change while playing, the two buds can end up at different loudness. The
buds synchronise on the timestamps.

What works, and what the module does: start every stream on the sample
count, and six seconds in, switch to millisecond stamps, continuing from the
last value so the timestamp never jumps. The AirPods play through the switch
with nothing audible, and volume changes after it keep the buds level.

```
packet:      1     2     3    ...   200  │  201    202    203
timestamp:   0   1440  2880   ...  286560│ 286590 286621 286650
             └──── samples, +1440 ───────┘ └── milliseconds, about +30 ──┘
```

Settings: `persist.aaceld.rtp_ms_after` (milliseconds before the switch,
default 6000; 0 never switches), `persist.aaceld.rtp_ms_jitter_us`.

### When the link cannot keep up

Out of range, the stack's packet queue fills. The module keeps encoding in
real time and lets the stack throw away what it cannot send, as it does for
every other codec. Frame counters and timestamps therefore advance by the
real time that passed, and the AirPods resume by themselves when the link is
back. An earlier version stopped encoding and kept the queued packets; the
AirPods then stayed silent for a minute or more after the link returned.

### What else was tried and made no difference

Removing Delay Reporting from the stream setup, presenting an Apple-style
Device ID record, SBR on or off, and several link settings: none of them
changed whether the AirPods play. The table is in
[notes/development-log.md](notes/development-log.md), "What was ruled out on
the way".

## 3. Where to read more

| Topic | File |
|---|---|
| Every setting | [../module/README.md](../module/README.md) |
| The codec object's fields, as found by hand on one build | [notes/object-layout.md](notes/object-layout.md) |
| How the software path was found on the development phone | [notes/software-encoder-path.md](notes/software-encoder-path.md) |
| The capture of the AirPods' offer | [notes/airpods-capability-capture.md](notes/airpods-capability-capture.md) |
| Each bug, in the order it was met | [notes/development-log.md](notes/development-log.md) |
