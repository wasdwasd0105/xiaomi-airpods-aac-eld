# What the AirPods actually advertise

Captured on this phone with full HCI logging on 9 November 2026, while the
AirPods reconnected. The capture settles the question the rest of this work
depended on: whether these AirPods offer AAC-ELD to a non-Apple source at all.

They do.

## Endpoints

The AVDTP DISCOVER response lists three sink endpoints, and
GET_ALL_CAPABILITIES on each gives:

| SEID | Codec | Also offered |
|---|---|---|
| 1 | SBC | Media Transport, Delay Reporting |
| 2 | MPEG-2,4 AAC | Media Transport, Delay Reporting |
| 3 | **Apple AAC-ELD**, vendor 0x4C codec 0x8001 | Media Transport, Delay Reporting |

AAC-ELD sits on its own endpoint. Android currently configures SEID 2 and
ignores SEID 3, because an unrecognised vendor codec is dropped silently during
capability parsing rather than reported.

## The AAC-ELD capability element, byte for byte

```
07 10 00 ff 4c 00 00 00 01 80 00 80 00 8c 00 83 e8 00
```

| Field | Value | Meaning |
|---|---|---|
| service category | 0x07 | Media Codec |
| LOSC | 16 | 14 bytes of codec specific info follow the two type bytes |
| media type | 0x00 | Audio |
| codec type | 0xFF | non-A2DP, so a vendor codec |
| vendor id | 0x0000004C | Apple, little endian |
| codec id | 0x8001 | AAC-ELD, little endian |
| object type | 0x0080 | AAC-ELD |
| freq and channels | 0x008C | 48 kHz only; mono and stereo both offered |
| reserved | 0x00 | |
| VBR | set | |
| max bit rate | 256000 | |

This matches the prediction in `aac-eld-apple.md` exactly, including the LOSC,
the field order and the endianness mix. Nothing in the reference notes needed
correcting on this point.

## What it changed in the implementation

One thing. The capability advertises VBR as supported, and the macOS
SET_CONFIGURATION recorded in the reference notes also carries the VBR bit set,
`83 E8 00`. The module originally cleared that bit because its encoder runs CBR.
Those are separate things: the bit is a negotiation field, the encoder's rate
control is not. The module now defaults `persist.aaceld.vbr` to true so the
configuration it sends matches the one configuration known to be accepted, while
the encoder still runs CBR. Set the property to false to try the other way.

## What this does not yet prove

That the AirPods accept our SET_CONFIGURATION on SEID 3, and that audio plays.
Both need the module live, which needs a reboot.

A useful detail for that test: the capability offers **48 kHz only**. Android
currently negotiates standard AAC at 44.1 kHz, because the stack's own AAC codec
advertises only 44100 locally. The ELD path has no such limit, so a successful
session should show 48 kHz.

## Reproducing the capture

```bash
adb shell su -c 'setprop persist.bluetooth.btsnooplogmode full'
```

Restart Bluetooth, let the headset reconnect, then pull
`/data/misc/bluetooth/logs/btsnoop_hci_*.log` and run the parser inline in this
repository's history, or open it in Wireshark. Turn it back off afterwards:

```bash
adb shell su -c 'setprop persist.bluetooth.btsnooplogmode disabled'
```

Full captures contain all Bluetooth traffic for the session, so they are worth
deleting once read. The decoded copy above is the part that mattered.
