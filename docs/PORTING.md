# Porting guide: making this work on another Android phone

This project has been run on one phone, a Xiaomi 17 Pro Max, on two system
versions. The author cannot test other phones. This guide is for the person,
or the AI agent, who wants it on theirs.

It is written to be followed from the top, one step at a time. Every step has

- a command to run,
- what you should see if all is well,
- where to go if you see something else.

You do not need to know the Bluetooth stack beforehand. You do need a rooted
phone, a computer with `adb`, and patience for a few reboots.

Three documents go with this one:

- [HOW-IT-WORKS.md](HOW-IT-WORKS.md): what the module does and what goes on
  the air. Read section 1 of it first if you have ten minutes.
- [TROUBLESHOOTING.md](TROUBLESHOOTING.md): a list of symptoms, each with its
  cause and fix. Every problem met while building this is in it.
- [notes/](notes/): the original working notes, in the order things were
  found. Long, but they show how each problem was tracked down.

## Rules for whoever does the port (read these first)

These are short because each one cost a day when it was broken.

1. **One change at a time, then test.** If two things change and it breaks,
   you do not know which one did it.
2. **Never replace or edit files of the Bluetooth APEX or `/vendor`.** The
   module only hooks functions in memory. Uninstalling it and rebooting
   always gets you back to stock. Keep it that way.
3. **Never switch A2DP hardware offload off for the whole phone**
   (`persist.bluetooth.a2dp_offload.disabled`). It is not needed, and on the
   development phone it broke every other headset.
4. **Start the log capture before the event.** A busy phone overwrites its
   log in a minute or two. Run `tools/watch.sh file.log` first, then reboot or
   connect.
5. **Before playing a test sound, check where music is routed** (step 8).
   If it is not routed to the headset the sound comes out of the phone
   speaker.
6. **A first connection after a reboot proves little.** Also disconnect and
   connect again without restarting Bluetooth (step 10). One version of this
   module passed every test except that one.
7. **Read a function's return type from the disassembly before hooking it.**
   The mangled name does not say what a function returns. Three bugs in this
   project's history were a hook returning the wrong kind of value (section
   "Writing a hook" below).
8. **Bluetooth capture files (`btsnoop_hci.log`) contain your link keys.**
   Never upload them, never commit them, delete them when read.
9. **The stack library is the phone maker's file.** Pull it to look at it;
   do not put it in a repository or attach it to an issue.
10. If you are an AI agent: ask the human before rebooting the phone, before
    playing sound, and before driving the phone's screen. They may be using it.

## Words used in this guide

| Word | Meaning |
|---|---|
| the stack | Android's Bluetooth implementation. Its native code is one library, `libbluetooth_jni.so`, loaded into the process `com.android.bluetooth` |
| the module | this project's Zygisk library, loaded into that process |
| hook | replacing the start of a function in memory so that calls to it reach the module first. The module can answer itself or pass the call on to the original |
| symbol, name | the name of a function as stored in the library. C++ names are stored *mangled*: `_Z10osi_mallocm` is `osi_malloc(unsigned long)` |
| mini debug symbols | a compressed table of function names that Android keeps inside stripped system libraries, in a section called `.gnu_debugdata`. It is why the module can find functions by name |
| codec element | the few bytes in which a headset says "I can do this codec with these options". Apple's AAC-ELD element is 17 bytes |
| codec slot, codec index | the stack keeps one object per codec it knows, numbered. The module borrows the slot of Opus, a codec no AirPod uses |
| A2DP offload | the phone's audio chip (DSP) encodes the audio instead of the stack. The other way is the *software path*: the stack encodes in its own process. AAC-ELD needs the software path |
| audio session | the connection between Android's audio system and the stack over which sound samples arrive. No session, no sound to the headset |
| SEP, SEID | a stream endpoint on the headset and its number. AirPods have one endpoint per codec |

## What must be true of the phone

Check these before spending time. The commands for each are in the steps
below; this is the list.

| Requirement | Why | If it is not so |
|---|---|---|
| 64-bit ARM (`arm64-v8a`) | The module is built for it | Stop. 32-bit is not supported |
| Root with Zygisk (Magisk, KernelSU or APatch with a Zygisk implementation) | That is how the module gets into the Bluetooth process | Stop, or root the phone first |
| The stack is AOSP's (`libbluetooth_jni.so` exists) | The module hooks AOSP's functions | A phone with a different stack needs a different project |
| The library has mini debug symbols | Functions are found by name | Hard. See "No symbols at all" |
| The stack has the Opus source codec (it does on Android 16 and 17, the two versions this ran on; step 2 tells you for yours) | Its slot is borrowed | Another unused vendor codec's slot could be borrowed instead; that is new code |
| FDK-AAC with the ELD encoder is inside the library | It is the encoder | Stop, or bundle an encoder, which is new code and a licence question |
| The stack can encode in software while offload is on | AAC-ELD cannot be encoded by the DSP | See [TROUBLESHOOTING.md](TROUBLESHOOTING.md), "music plays from the phone speaker" |
| The headset offers Apple's AAC-ELD to this phone | Nothing to negotiate otherwise | AirPods 5 and AirPods Pro do. Check with step 7 |

## The work, in one picture

```
 computer                               phone
 ────────                               ─────
 1 pull the library         <──  libbluetooth_jni.so
 2 check the names (check_lib.py)
 3 fix what is missing (source edits)
 4 build (./build.sh)
 5 harness: start-up in a test process ──>  nothing touched
 6 install, reboot, read the start-up log
 7 connect the headset, read the negotiation
 8 check the audio route
 9 play, read the packet log
 10 the checks that catch what a first connection hides
```

Set up once on the computer:

```bash
pip install capstone
```

and have the Android NDK installed (Android Studio's SDK manager, or a
download from Google) with `ANDROID_NDK` pointing at it if it is not in the
default place. `adb devices` must list the phone.

---

## Step 1. Get the library off the phone

Bluetooth must be switched on.

```bash
tools/pull_lib.sh
```

You should see the phone's build fingerprint, where the library was, and
`saved work/libbluetooth_jni.so`.

- "libbluetooth_jni.so not found": find it by hand with
  `adb shell su -c 'find /apex /system -name "libbluetooth*.so"'`. If there
  is a `libbluetooth_jni.so` somewhere else, `adb pull` it to
  `work/libbluetooth_jni.so`. If there is none, this phone's stack is not
  the one this project hooks.
- You have a ROM package and not the phone yet: the library can be taken out
  of the package. `tools/rom/` has a reader for the super image's partition
  table (`lp.py`) and for EROFS (`erofs.py`, `erofs_z.py`); the library is in
  the `system` partition under `/system/apex/com.android.bt*.apex` (an APEX is
  a zip with `apex_payload.img` inside, an ext4 or EROFS image).
  [notes/development-log.md](notes/development-log.md), "v0.11", did exactly
  this. Treat a downloaded ROM as untrusted: read it, never run anything
  from it.

## Step 2. Check the names

```bash
tools/check_lib.py work/libbluetooth_jni.so
```

The last lines are what matter. There are four possible results.

**Result A: all there.**

```
required 32 of 32, optional 10 of 15
AAC-ELD encoder inside the library: yes (FDKaacEnc_Transform_Real_Eld is there)
encoder interval: milliseconds
verdict: the module would start on this library
```

Go to step 4. The optional names that are absent are listed; the table in
"The names, one by one" says what each absence costs. Come back to it if
something misbehaves later.

**Result B: `REQUIRED, MISSING` lines.** Go to step 3.

**Result C: `no .gnu_debugdata section`.** Go to "No symbols at all".

**Result D: `AAC-ELD encoder inside the library: NOT FOUND`.** The stack has
no encoder for this codec. Check with
`grep -i -c 'aacEnc' work/libbluetooth_jni.so.mangled.txt` after
`tools/symbols.py work/libbluetooth_jni.so`. If `aacEncOpen` is there but
`FDKaacEnc_Transform_Real_Eld` is not, the FDK was built without ELD. Stop
here unless you want to bundle an encoder.

## Step 3. Fix a missing required name

Do this for each `REQUIRED, MISSING` line. First make the symbol files, which
the other tools use:

```bash
tools/symbols.py work/libbluetooth_jni.so
```

Under each missing name, `check_lib.py` prints the names this library has
that look like it (`this library has: ...`). There are three cases.

### Case 1: same function, different parameters

Example: the module wants `_Z10osi_mallocm` and the library has
`_Z10osi_mallocj`. Read both in plain form:

```bash
grep ' osi_malloc' work/libbluetooth_jni.so.demangled.txt
```

If the parameters mean the same thing (same count, same order, same kinds),
replace the mangled name in the `kNames` table in
[src/target.cpp](../src/target.cpp) with the one this library has. If a
parameter was added, removed or changed kind, the module's code for that
function has to change too: find the function's use in
[src/hooks.cpp](../src/hooks.cpp) (the hook is called `h_<Name>`, the pointer
to the original `o_<Name>`) and make both match the new parameters. Then read
"Writing a hook" below and check the return type as well.

### Case 2: the function is gone, another does its job

Example from Android 17: `A2DP_VendorCodecName` no longer exists; the
general `A2DP_CodecName` handles vendor codecs itself. The module hooks the
general one where the vendor one is missing.

To find the replacement, look at who used to call the missing function in
AOSP's source for the Android version of the phone
(`packages/modules/Bluetooth/system/stack/a2dp/` on
[cs.android.com](https://cs.android.com)), or ask the library:

```bash
grep -i 'codecname' work/libbluetooth_jni.so.demangled.txt
```

Then add the new name to `kNames` and a hook for it, following the pattern
of `h_CodecName` in `src/hooks.cpp`.

### Case 3: nothing like it exists

The compiler may have merged the function into its callers (inlining), or the
maker removed the feature. Look the name up in the table "The names, one by
one": the last column says what to do for each.

After any change, run `tools/check_lib.py` again until the verdict is "would
start", then go to step 4.

## Step 4. Build

```bash
./build.sh
```

You should see `packed AAC-ELD-AirPods-<version>.zip`.

- "Android NDK not found": install it and set `ANDROID_NDK`.
- A compile error in `src/`: it is in something you changed in step 3.
- "Settings add-on not built": harmless for a port. The add-on is for
  Xiaomi's Settings app only; see "The Settings add-on" below.

## Step 5. Try the start-up in a test process (safe, optional, worth it)

This loads your copy of the library into a throw-away root process on the
phone and runs the module's start-up on it: name lookup, layout measurement,
all hooks. Bluetooth itself is not touched, so a crash here costs nothing.

```bash
tools/harness/run.sh mine work/libbluetooth_jni.so
```

You should see:

```
ok    loaded /data/local/tmp/aaceld_harness/mine/libbluetooth_jni.so
ok    name lookup
ok    layout measured and hooks installed: priority 0x58 config 0x60 user 0x108 ota 0x178, codec index 6
ok    low-latency requests are swallowed
ok    the stack's Opus constructor ends up with priority 8001
ok    a stack-built object accepts Apple's capability and configures AAC-ELD
PASS
```

The numbers after "layout" may differ on your phone. That is fine: they are
measured, not assumed.

- `cannot load ... library "libsomething.so" not found`: the library needs
  another one that is not on the search path. Find it
  (`adb shell su -c 'find /apex /system /vendor -name libsomething.so'`) and
  add its folder to `LP=` in `tools/harness/run.sh`.
- `FAIL` at "layout measured": go to [TROUBLESHOOTING.md](TROUBLESHOOTING.md),
  "the codec object could not be measured".
- A crash (`exit 139` or similar): the lines printed just before it say how
  far it got. The module log under "--- module log" ends with the last thing
  it tried, for example `hooking A2DP_VendorInitCodecConfig at 0x...`. Go to
  [TROUBLESHOOTING.md](TROUBLESHOOTING.md), "Bluetooth crashes at start".

What the harness cannot tell you: anything about connecting, the audio
session, or sound. It does not start Bluetooth.

## Step 6. Install and read the start-up log

Start the log capture first, in a second terminal, and leave it running
through steps 6 to 9:

```bash
tools/watch.sh build/first-run.log
```

Install:

```bash
adb push AAC-ELD-AirPods-*.zip /sdcard/Download/
```

Flash the zip in your root manager (Magisk, KernelSU or APatch) and reboot.
The installer prints what it found and what is left to do by hand; do those
things.

After the reboot the capture should show, in this order:

| Line | Meaning | If it is missing |
|---|---|---|
| `starting, dry run off` | The module is inside the Bluetooth process | Zygisk did not load it. [TROUBLESHOOTING.md](TROUBLESHOOTING.md), "no AacEldA2dp lines at all" |
| `...libbluetooth_jni.so, build ID ...: not a build this module has been run on; everything is found by name and checked before use` | It found the library | `libbluetooth_jni.so never appeared`: Bluetooth is off, or the library has another name |
| `found every function it needs by name (N of 45, the rest optional)` | Step 2 passed on the phone too | `not in this build: <name>`: back to step 3 with that name |
| `measured: codec_config_ 0x.., codec_user_config_ 0x.., no/with codec_capability_` | First half of the layout | "the codec object could not be measured" |
| `hooking <name> at 0x...` (about 20 of them) | Each hook going in | The last one printed is the one that failed or crashed |
| `installed N hooks, all succeeded` | | `some failed`: the line above it names the hook |
| `layout: priority 0x.. config 0x.. ... N bytes each` | Second half of the layout | "the codec object could not be measured" |
| `ready: AAC-ELD takes the Opus slot, codec index N` | Start-up is complete | `the stack does not know an Opus source codec` |

If Bluetooth crashes during start-up, the module notices at the next start
and stays out (`the last start on this build did not get through start-up`),
so the phone keeps working with stock Bluetooth. To let it try again after
you have changed something:

```bash
adb shell su -c 'resetprop -p --delete persist.bluetooth.aaceld.probe'
```

Want to look before anything is patched? Set
`adb shell su -c 'setprop persist.aaceld.dryrun true'` and restart
Bluetooth: the module then only finds the functions and logs their addresses.
Set it back to `false` afterwards.

## Step 7. Connect the headset and read the negotiation

Connect the AirPods. The capture should show:

```
hit: setPeerCodecCapabilities for AAC-ELD
hit: setCodecConfig(is_capability=1) accepted AAC-ELD: 48 kHz stereo, 256000 bps, VBR on
hit: VendorGetEncoderInterface -> our encoder
encoder init: peer MTU 1011, rtp clock samples then milliseconds, marker cleared, ssrc zeroed
AAC-ELD encoder up: 48 kHz stereo, frame 480, ...
```

and this command should name the Opus slot with the module's mark:

```bash
adb shell dumpsys bluetooth_manager | grep -m1 'mCodecConfig:'
```

```
mCodecConfig: {codecName:Opus,mCodecType:6,mCodecPriority:8001,...,mCodecSpecific1:5013505,...
```

`5013505` is 0x4C8001, Apple's vendor and codec number. That mark is how to
tell AAC-ELD from real Opus; both show as "Opus" here.

| What you see instead | Go to, in [TROUBLESHOOTING.md](TROUBLESHOOTING.md) |
|---|---|
| No `hit: setPeerCodecCapabilities for AAC-ELD` | "the headset does not offer AAC-ELD" |
| That line is there but `codecName:AAC` | "connects on AAC although the module is ready" |
| AAC-ELD at first, AAC a few seconds later | "falls back to AAC a few seconds after connecting" |
| `cannot set source codec` or `cannot update peer codec capabilities` in the stack's log | the entries with those names |

## Step 8. Check the audio route

```bash
adb shell dumpsys audio | grep -A8 '^- STREAM_MUSIC' | grep Devices
```

- `Devices: bt_a2dp(80)`: music goes to the headset. Go to step 9.
- `Devices: speaker(2)`: the audio system refused or lost the headset. Do
  **not** play anything yet. Go to [TROUBLESHOOTING.md](TROUBLESHOOTING.md),
  "music plays from the phone speaker".

## Step 9. Play something and read the packet log

Play any music. The capture should show the first frames, the first packets
exactly as they leave the phone, and after a while a statistics line:

```
frame 1: seq=1 au_len=6 hdr=b0 01 10 06 au=14 a0 02 a8 00 a0
packet 1: rtp_ts=0 frames=3 payload=30 queue=0
wire 1: len=42 pt=96 marker=0->0 seq=1 ts=0 ssrc=000000ff->00000000 hdr=b0 01 10 06 au=14 a0 02 a8
rtp clock now counts milliseconds (persist.aaceld.rtp_ms_after), after packet 200, last stamp 286560
stats: 1000 packets 3000 frames in 30000 ms (33.3 pkt/s, 180 kbps), ... queue max 0, ... l2cap ok/congested/failed 1000/0/0
```

And you should hear it.

| What you see or hear | Go to, in [TROUBLESHOOTING.md](TROUBLESHOOTING.md) |
|---|---|
| `frame` and `packet` lines but no `wire` lines | "packets are made but none is sent" |
| `wire` lines, silence, music pauses by itself after about 3 seconds | "the headset pauses the stream after three seconds" |
| Sound, then the headset disconnects | "the headset disconnects while playing" |
| Sound is quiet, noisy or thin | "the sound is clearly worse than AAC" |
| `queue max` far above 0, or `failed` not 0, while the phone is next to the headset | "the headset disconnects while playing" |

## Step 10. The checks that catch what a first connection hides

Do all of these before calling the port working. Each one has broken at
least once on the development phone while everything above looked fine.

| # | Do this | Pass if |
|---|---|---|
| 1 | Reboot, let the headset connect | Step 7 and step 8 pass |
| 2 | **Disconnect the headset's media and connect it again, without switching Bluetooth off.** In Bluetooth settings open the headset's page and switch "Media audio" off and on; or connect the AirPods to another device and then back | Step 8 passes again, and music plays |
| 3 | Switch AAC-ELD off and on while connected: `adb shell su -c 'setprop persist.bluetooth.aaceld.enabled false'`, then disconnect and connect the headset; then `true` and again | Off: `codecName:AAC` and music plays. On: AAC-ELD again and music plays |
| 4 | Switch Bluetooth off and on | Step 7 and step 8 pass |
| 5 | Pause for a minute, then resume | Music resumes |
| 6 | After the "rtp clock now counts milliseconds" line, change the volume a few steps while music plays | Both buds stay equally loud |
| 7 | Walk out of range until the music stops, walk back | Music comes back within a few seconds by itself |
| 8 | Connect a different headset (any codec) | It works as it did without the module |
| 9 | Take a phone call on the headset, then play music again | Music returns to the headset |

Number 2 is the one most likely to fail on a new phone, because it is where
phone makers' stacks differ most (who starts the audio session, and when).

When all pass: please report it (section "Reporting a port" at the end).

---

## When the stack behaves differently: how to find out why

Steps 7 to 10 are where a new phone shows that its stack does something the
two known ones do not. The method that found every such difference so far is
the same four moves. Use it; do not guess.

**1. Get the log of the moment it goes wrong.** Start `tools/watch.sh`, do the
thing, stop. If the filtered log is not enough, take everything from the
Bluetooth process:

```bash
adb logcat -v time --pid=$(adb shell pidof com.android.bluetooth) > build/bt.log
```

**2. Find the stack's own line that states the decision.** The stack logs
nearly every decision in words. Read the log around the failure and look for
a sentence, not a number. The three differences found on HyperOS 4 each had
one:

| The line in the log | What it gave away |
|---|---|
| `codec: Opus priority: 1501` | The slot ranks below AAC (2001), so AAC wins |
| `Reconfig done - Ignore calling startSession(), just update codec to audio HAL` | After a codec change this stack does not restart the audio session |
| `SetCodecUserConfig: ... Opus ... priority: -1` five seconds after connecting | Something disables Opus when a stream starts |

**3. Find the code that printed it.**

```bash
tools/xref.py work/libbluetooth_jni.so 'Peer {} : Reconfig done - Ignore' 60 30
```

Give it the *beginning* of the message as stored: without the
`file.cc:123 function:` prefix that the log adds, and with `{}` where the log
shows a value. The output names the function and shows the instructions
around the message. The calls above the message are the conditions that led
to it; the calls below are what the stack does next. To read a whole
function, or to see who calls one:

```bash
tools/a64.py work/libbluetooth_jni.so fn btif_a2dp_source_start_session_delayed 200
tools/a64.py work/libbluetooth_jni.so calls btif_a2dp_source_setup_codec
```

You do not need to understand every instruction. Read the names after `bl`
(a call) and the branches right after a call: `tbz w0, #0, <address>` means
"if the call returned false, jump".

Compare with AOSP's source for the same file when the fork's code is
unclear: the function names are the same and the source has comments.

**4. Change the smallest thing.** Prefer, in this order:

1. a property the stack already reads (search the library for `persist.` near
   the code: `strings work/libbluetooth_jni.so | grep persist.bluetooth`);
2. a hook that changes one argument or one return value (the priority hook
   passes 8001 where the stack passed 1501);
3. a hook that adds one call (the session hook calls the stack's own
   `start_session()` when the stack skipped it);
4. a hook that swallows one call (the low-latency hook).

Make the new name **optional** in `kNames` (third field `false`) so the
module still starts on stacks that do not have it, and gate the behaviour
with a property so it can be switched off without a rebuild.

### The differences met so far

Use this as a map of where stacks differ. Details and log lines are in
[notes/development-log.md](notes/development-log.md), "v0.14" and "v0.15".

| | Android 16 (HyperOS 3) | Android 17 (HyperOS 4) | How the module copes |
|---|---|---|---|
| Opus slot priority | 8001, above AAC | 1501, below AAC | Hook on the Opus codec's constructor sets it (`persist.aaceld.priority`) |
| Opus outside low-latency mode | Bluetooth app is asked (`invoke_switch_codec_cb`), does nothing | Stack disables Opus at each stream start (`btif_av_source_set_low_latency_codec`) | Both calls are swallowed |
| Audio session after a codec change | Stack ends and restarts it | Stack keeps it; nothing starts the software session when moving from a DSP codec | Hook on `btif_a2dp_source_setup_codec` starts it (`persist.aaceld.session_start`) |
| Four vendor helper functions (`A2DP_VendorCodecName`, `...CodecIndexStr`, `...GetBitRate`, `...AdjustCodec`) | present | gone or merged into general ones | Optional; general ones hooked instead |
| `getCodecConfig()` | copies | logs the codec's name first | Layout probe keeps the name area zero |
| Codec object layout | config 0x60, user 0x108, 45-byte elements | the same | Measured at start-up either way |

Things known to differ in plain AOSP and likely on other makers' phones, and
already handled by measuring: `codec_capability_` exists (one more 56-byte
block), the codec element arrays are 20 bytes and not 45, and the encoder
interval is counted in microseconds. None of these has been run.

---

## Writing a hook

A hook is three things in [src/hooks.cpp](../src/hooks.cpp) and one line in
[src/target.cpp](../src/target.cpp):

```cpp
// 1. a pointer to the original, with the function's exact type
bool (*o_Thing)(const uint8_t*) = nullptr;

// 2. the replacement, with the same type
bool h_Thing(const uint8_t* codec_info) {
  if (ours(codec_info)) return true;      // Apple's element: answer here
  return o_Thing(codec_info);             // anything else: the stack's own answer
}

// 3. in hooks_install(): INSTALL(Thing_field, Thing);   or INSTALL_IF_PRESENT
```

```cpp
// in kNames, src/target.cpp, plus a field in TargetOffsets, src/target.h
{&TargetOffsets::Thing_field, "_Z5ThingPKh", true},   // false = optional
```

The checklist. Every item is here because skipping it caused a bug.

1. **Copy the mangled name from the symbol file**, do not type it:
   `grep 'Thing' work/libbluetooth_jni.so.mangled.txt`.
2. **The parameters** are in the name: read the demangled form.
3. **The return type is not in the name. Read it from the code:**

   ```bash
   tools/a64.py work/libbluetooth_jni.so fn Thing 120
   ```

   Look at what is put in `w0` (32-bit) or `x0` (64-bit) just before each
   `ret`:
   - nothing is put there: `void`
   - `and w0, w?, #1` or `cset w0, ...`: `bool`
   - `mov w0, w?` with small numbers like 0, 1, 2 on different paths: a
     status or an enum; find out which value means success
   - a pointer loaded into `x0`: a pointer

   Then look at what the callers do with it:

   ```bash
   tools/a64.py work/libbluetooth_jni.so calls Thing
   ```

   `tbz w0, #0, ...` or `cbz w0, ...` right after the call means the caller
   uses the result. If no caller looks at `w0`, the type matters less, but
   declare it correctly anyway.
4. **Return the original's result on every path** where you did not decide
   the answer yourself. Declare the hook with the true return type and write
   `return o_Thing(...)`, or keep the result in a variable and return it.
5. **Pass everything through while the module is not ready**
   (`g_ready` is false) and for anything that is not Apple's codec
   (`ours(...)` is false). Other headsets must behave as if the module were
   not there.
6. **Do not log on every packet.** Log the first few, or on change.
7. **The function must be long enough to hook.** The hook overwrites the
   first five or so instructions. A normal function starting with
   `paciasp` / `stp x29, x30, ...` is fine. A two-instruction function is
   not: hook its caller instead.

The three return-type bugs, for the record:

| Function | Was declared | Really returns | What happened |
|---|---|---|---|
| `A2dpCodecConfigOpusBase::setPeerCodecCapabilities` | status, 0 = good | `bool`, true = good | "cannot update peer codec capabilities" on every connection; codec never selectable |
| `avdt_ad_write_req` | `bool` | 0 failed, 1 sent, 2 congested | "congested" became "sent"; the stack kept writing and the headset dropped the link |
| `btif_a2dp_source_setup_codec` (Android 17) | `void` | `bool` | The stack read a leftover value as "setup failed" and skipped starting the audio session on every reconnection |

---

## The names, one by one

"Hooked" means the module replaces it; "called" means the module only uses
it. The mangled forms are in `kNames` in [src/target.cpp](../src/target.cpp).

### Required

| Function | Use | What it is for | If your library does not have it |
|---|---|---|---|
| `aacEncOpen`, `aacEncClose`, `aacEncEncode`, `aacEncoder_SetParam`, `aacEncInfo` | called | The FDK-AAC encoder that the stack carries for plain AAC | No encoder. See result D in step 2 |
| `osi_malloc`, `osi_free` | called | Packets handed to the stack must come from the stack's own allocator, because the stack frees them | Search for the allocator the stock AAC encoder uses: `tools/a64.py LIB fn a2dp_aac_send_frames` and read the first calls |
| `osi_property_get_bool`, `osi_property_get_int32` | called | Reading settings | Could be replaced by `__system_property_get` from libc in the module |
| `std::recursive_mutex::lock`, `unlock` | called | Locking the codec object the way the stack does | Present wherever libc++ is linked in; if the names differ (`__ndk1`), update them |
| `A2DP_IsVendorSourceCodecValid`, `A2DP_IsVendorPeerSourceCodecValid`, `A2DP_IsVendorSinkCodecSupported` | hooked | "Is this vendor codec element one we know?" The module answers yes for Apple's | These are AOSP names. If one is gone, find what the stock code calls on a vendor element: `grep 'IsVendor.*Valid\|IsCodecValid' LIB.demangled.txt` |
| `A2DP_VendorCodecTypeEquals` | hooked | "Are these two elements the same codec?" | As above |
| `A2DP_VendorSourceCodecIndex`, `A2DP_VendorSinkCodecIndex` | hooked, first also called | Which slot a codec element belongs to. The module answers "the Opus slot" for Apple's, and asks it once which slot Opus has | Essential. Without the index the stack does not know the codec |
| `A2DP_VendorInitCodecConfig` | hooked | Writes the phone's own offer for a slot. The module writes Apple's element for the Opus slot, which is what makes the phone show an AAC-ELD endpoint | Essential |
| `A2DP_VendorUsesRtpHeader`, `A2DP_VendorBuildCodecHeader` | hooked | Packet framing questions; answers are "yes, RTP" and "no extra header" | Essential for packets to be built right |
| `A2DP_VendorGetEncoderInterface` | hooked | Hands the stack the module's encoder | Essential |
| `A2DP_VendorCodecInfoString` | looked up only | Not hooked and not called at present | Safe to make optional: change `true` to `false` in `kNames` |
| `A2DP_VendorGetSinkTrackChannelType` | hooked | Channel layout question, from the side of the stack that receives audio | Making it optional should cost nothing for sending; not tried |
| `A2DP_GetPacketTimestamp` | hooked | **The one that decides whether any audio leaves the phone.** It has a fixed list of codecs and makes the stack throw away every packet of any other | If it is gone, find where the stack reads the timestamp out of a finished packet: `tools/xref.py LIB 'unsupported codec'` |
| `A2dpCodecConfigOpusBase::setCodecConfig`, `setPeerCodecCapabilities` | hooked | The negotiation: accepting the headset's offer and filling in the borrowed codec object | No Opus class means no slot to borrow. Another vendor codec class with the same two methods could be used; change the two names and the element in `hooks_install()` that asks for the slot's index |
| `avdt_ad_write_req` | hooked | The last point where a finished packet can be seen and corrected (marker bit, SSRC) and where the link's back-pressure is read | If its parameters changed, update the hook. If gone, find the function that hands media packets to L2CAP |
| `A2dpCodecConfig::getCodecConfig`, `getCodecUserConfig`, `copyOutOtaCodecConfig` | called | Used only to measure where things are inside the codec object | All three are AOSP. Without them the layout must be read from the disassembly and written into `g_layout` by hand |
| `a2dp_aac_encoder_init`, `a2dp_aac_send_frames` | name check only | Their exact names prove the encoder interface still has the shape the module fills in | If their parameters changed, the encoder interface changed: compare `tA2DP_ENCODER_INTERFACE` in AOSP's `a2dp_codec_api.h` for your Android version with `A2dpEncoderInterface` in `src/target.h` |
| `a2dp_aac_get_encoder_interval_ms` or `..._us` | name check only | Tells whether the encoder interval is in milliseconds or microseconds | One of the two must exist |

### Optional

| Function | Use | Without it |
|---|---|---|
| `A2DP_VendorGetBitRate`, `A2DP_VendorAdjustCodec` | hooked | Nothing lost |
| `A2DP_VendorCodecName`, `A2DP_VendorCodecIndexStr`, else `A2DP_CodecName`, `A2DP_CodecIndexStr` | hooked | The stack's own logs and dumps call the codec "Opus" or "Unsupported codec" |
| `A2dpCodecConfigOpusSource` constructor | hooked | The slot keeps the stack's priority. If that is below AAC's the headset connects on AAC |
| `btif_a2dp_source_setup_codec`, `bluetooth::audio::a2dp::is_hal_offloading`, `...::start_session` | first hooked, others called | On a stack that keeps the audio session across codec changes: music on the phone speaker after switching between AAC and AAC-ELD |
| `btif_av_source_set_low_latency_codec` or `invoke_switch_codec_cb` | hooked | On a stack that disables Opus outside low-latency mode: falls back to AAC when the first stream starts |
| `btif_av_stream_suspend`, `btif_av_stream_start` | called | `persist.aaceld.recover_kick_ms` does nothing (it is off by default and untested) |
| `A2dpCodecs::setCodecUserConfig` | not used | Nothing |

---

## No symbols at all

If the library has no `.gnu_debugdata` and no symbol table, the module cannot
find anything by name. This has not been done by this project since its
first versions, but those worked exactly this way, so it is possible:

1. Every function still has to be found, by hand, once per build. The
   stack's log messages are the handles: most functions log their own name.
   `tools/xref.py LIB 'A2DP_GetPacketTimestamp'` style searches find the
   message; the function containing it is the one you want. Functions that
   log nothing are found through their callers and callees.
2. Put the addresses in a table keyed by the library's build ID, and fill
   `kOffsets` from that table instead of from names (in `target_resolve()`,
   [src/target.cpp](../src/target.cpp)). Refuse any build ID that is not in
   the table. The build ID is printed by `tools/check_lib.py`.
3. Everything after that (measuring the layout, hooking) is unchanged.

A table like that breaks with every system update. If the same maker ships a
build *with* symbols for a close version, use it to learn what each function
looks like first.

## Things outside the stack library

These are not part of the port of the stack module, but they will come up.

- **Zygisk must be allowed into the Bluetooth process.** On KernelSU,
  "Umount modules" must be off for the Bluetooth app; on Magisk it must not
  be on the DenyList. The installer tries to arrange the first and tells you
  about both.
- **`persist.bluetooth.opus.enabled` must be true**, or the stack disables
  the slot. The module's `post-fs-data.sh` sets it at every boot.
- **The phone maker's Bluetooth app may pin a codec.** Xiaomi's remembers a
  codec per headset and asks for it with priority 1000000, above everything.
  If your phone's Settings has a codec switch for the headset, it may fight
  the module. See [TROUBLESHOOTING.md](TROUBLESHOOTING.md), "connects on AAC
  although the module is ready".
- **Absolute volume must be on** (the setting is called "Keep volume
  consistent" on Xiaomi, "Disable absolute volume" in developer options
  elsewhere, where it must be *off*). Without it the phone turns the music
  down before the encoder, on a 16-bit path, and it sounds bad.
- **Spatial audio** on the phone may only work with DSP-encoded codecs.

### The Settings add-on

`label/` is an Xposed module for **Xiaomi's** Settings app. It adds the
"AAC-ELD" badge and three rows to the headset's page. It hooks classes of
that one app by name and will do nothing on another maker's Settings. On
other phones, skip it and use the properties:

| Property | Default | Meaning |
|---|---|---|
| `persist.bluetooth.aaceld.enabled` | true | false: the stack declines AAC-ELD and the headset gets AAC. Applies at the next connection |
| `persist.bluetooth.aaceld.vbr` | true | true: variable bit rate, as macOS sends. false: constant |
| `persist.bluetooth.aaceld.bitrate` | 256000 | Maximum bit rate in bit/s, 64000 to 320000 |

Set them with `adb shell su -c 'setprop <name> <value>'`. The full list is in
[../module/README.md](../module/README.md).

## Reporting a port

If you got it working on another phone or system version, or got stuck, an
issue with the following helps the next person:

- the phone, and `ro.build.fingerprint` (it is in `work/phone.txt`);
- the output of `tools/check_lib.py`;
- the start-up lines from `adb logcat -s AacEldA2dp` (up to "ready");
- which of the ten checks in step 10 pass;
- any source change you needed, as a patch or pull request. Keep new names
  optional and new behaviour behind a property, so the phones that already
  work keep working.

Do **not** attach: the library, a ROM, a `btsnoop` capture, or a full bug
report. `tools/watch.sh file.log` hides Bluetooth addresses in what it
saves; other logs do not.
