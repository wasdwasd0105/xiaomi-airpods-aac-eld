# AAC-ELD in Bluetooth settings

A small Xposed module for `com.android.settings` that makes HyperOS's
Bluetooth settings aware of the AAC-ELD stack module. It does two things.
It needs an Xposed-compatible framework; on this phone that is Vector.

## The codec badge

HyperOS shows the active codec as a small badge beside a connected headset.
The text comes from a table compiled into the Settings app,
`BluetoothDevicePreference.getCodecName(int)`, and the Opus slot the stack
module borrows (codec type 6) is not in it, so the badge is hidden.

The module hooks that method. When the lookup would return "UNKNOWN" and the
headset's active A2DP codec is type 6 **and** carries the mark the stack
module writes into `codecSpecific1` (0x004C8001, Apple's vendor and codec
ID), it returns "AAC-ELD". A real Opus session has no mark and is left alone.
The codec type Settings passes on to its own dialogs is not touched.

## The headset's page

A headset that offers AAC-ELD gets three more rows under Xiaomi's own codec
switch, in the Properties section of its page:

| Row | What it does |
|---|---|
| **AAC-ELD** switch | Writes `persist.bluetooth.aaceld.enabled` and asks the stack to choose a codec again, so the headset moves between AAC-ELD and AAC within a second, without reconnecting |
| **Variable bit rate** switch | Writes `persist.bluetooth.aaceld.vbr`. On: variable rate up to the maximum, never above 256 kbps. Off: constant rate at the maximum |
| **Max bit rate** slider | Writes `persist.bluetooth.aaceld.bitrate`. Four stops, 192, 256, 288 and 320 kbps, labelled under the bar; the value is in the row's title, with a note when variable rate holds it to 256 |

The slider is Xiaomi's own seek-bar row, the class its Bluetooth volume
slider extends. That row is only a title and a bar reaching the bottom of the
card, so the module hooks its `onBindViewHolder` and, for this one row, adds
the stop labels and some room below.

### Two pages, same rows

Xiaomi draws a headset's page in one of two places, and the rows go on
both. A headset it does not know (AirPods 5 at the time of writing) gets the
stock page, `DeviceProfilesSettings`. AirPods it has in its database get a
page of their own, with noise-control buttons and the like, from the headset
plugin: a split that Settings downloads ("Bluetooth version: Update") and
loads into a class loader of its own. There the page is
`plugin.settings.java.airpods.MiuiAirpodsFragment` and its codec switch is
managed by `AirPodsCodec`.

The plugin's classes do not exist when Settings starts, so the module hooks
`Instrumentation.newActivity`, and installs its plugin hooks the first time
an activity from `plugin.settings.java.` is created, in that activity's
class loader. The rows are placed right after the plugin's own AAC switch;
the plugin gives its rows explicit positions and a row without one lands at
the top of the card. The other plugin page, `MiuiHeadsetFragment`, for
Xiaomi's own headsets, is not touched.

On that page "Dimensional audio" will not switch on while AAC-ELD is the
active codec, and the module says so in a toast when it is tapped. The
reason is in the phone's audio routing, not in the page; see
`docs/notes/development-log.md`, "Dimensional audio and AAC-ELD".

Both bit-rate rows take effect within a second while playing and need
module v0.9. 288 and 320 kbps are above what the headset advertises.

The settings are the stack module's and are phone-wide; `module/README.md`
describes them and which bit rate setting to prefer. The page only shows the
rows for a headset whose codec list contains the marked slot, which the stack
module publishes even while AAC-ELD is switched off; for a headset that is not
connected, the answer from its last connection is remembered.

Three details of how this fits beside Xiaomi's own switch:

- **Making the stack choose again.** A codec preference request only changes
  codec when the requested one ends up with a strictly higher priority than
  the one in use, and Xiaomi's switch always asks with the highest value, one
  million. So the AAC-ELD switch asks with one more than the highest priority
  the headset's codecs currently hold. Turning it off asks for AAC, or for SBC
  if Xiaomi's AAC switch is off.
- **Xiaomi's AAC switch does not push the headset off AAC-ELD.** While
  AAC-ELD is on and in use, a request from that page for AAC or SBC is held
  back. Xiaomi still records its switch, and it decides between AAC and SBC
  once AAC-ELD is turned off. Requests from anywhere else, developer options
  included, go through.
- **Xiaomi's AAC switch keeps its setting.** The page rewrites its record of
  that switch from whatever codec is active, so with AAC-ELD active it would
  turn itself off, and the Bluetooth app would then force SBC as soon as
  AAC-ELD was switched off. While AAC-ELD is the active codec that rewrite is
  skipped.

- **"Keep volume consistent" stays on with AAC-ELD.** That switch is AVRCP
  absolute volume. With it off the phone turns the music down itself before
  the encoder, on a 16-bit path, and the sound is clearly worse (see
  `docs/notes/development-log.md`). While AAC-ELD is the active codec the page refuses to
  turn it off, with a short message saying why, and turns it on if it finds
  it off when the page opens or AAC-ELD is switched on.

## Languages

English and Simplified Chinese. The phone's language decides: any Chinese
locale gets Chinese, everything else English.

| Text | Where it is |
|---|---|
| The rows on the headset's page and the two short messages | `src/io/aaceld/label/DevicePage.java`, picked by `Locale.getDefault()`. They cannot be Android string resources: the add-on's code runs inside the Settings app, where its own resources are not loaded |
| The app's name and its description in the Xposed manager | `res/values/strings.xml` and `res/values-zh/strings.xml` |
| The names of Xiaomi's own switches in those messages ("Keep volume consistent", "Dimensional audio") | Not translated here: the row's own title is read from the page, so it is always in the phone's language |

To add a language, add a branch beside each `zh ?` in `DevicePage.java` and a
`res/values-<language>/strings.xml`.

## Build and install

```bash
label/build.sh
```

```bash
adb install -r label/aaceld-label.apk
```

```bash
adb shell su -c '/data/adb/modules/zygisk_vector/cli modules enable io.aaceld.label'
```

```bash
adb shell su -c '/data/adb/modules/zygisk_vector/cli scope set io.aaceld.label com.android.settings/0'
```

```bash
adb shell am force-stop com.android.settings
```

The build uses `javac`, `d8`, `aapt2` and `apksigner` directly, with
hand-written stand-ins for the Xposed API in `stubs/` that are compiled
against but not packaged. `res/` holds only the app's name and its
description for the Xposed manager, in English and Chinese. `signing.jks` is a throwaway self-signed key that
the script creates on first run and keeps so rebuilds can update the app.

## Removing it

```bash
adb uninstall io.aaceld.label
```

The two properties stay as they were set. With the switch left off, AAC-ELD
stays off until `persist.bluetooth.aaceld.enabled` is set back to true.

## Limits

- Tied to this Settings build. It relies on `BluetoothDevicePreference`,
  `DeviceProfilesSettings` and `CachedBluetoothDevice` keeping the names of
  the methods and fields it uses (`getCodecName`, `addPreferencesForSpecialCodec`,
  `updateCodecStatus`, `onPreferenceChange`, `sendBroadcastEnableOrDisable`,
  `setSpecificCodecStatus`, `mBluetoothA2dp`, `mCachedDevice`,
  `mCodecContainer`). If a system update renames them, that
  hook does not apply and the badge or the rows are simply absent.
- Needs stack module v0.5 or later for the rows. Earlier versions never
  listed AAC-ELD as selectable, so the rows would only show while AAC-ELD is
  the active codec and the switch could not turn it back on.
- Developer options still list the slot as Opus.
