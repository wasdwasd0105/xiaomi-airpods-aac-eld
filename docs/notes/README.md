# Working notes

These are the notes written while the module was being built on one phone, a
Xiaomi 17 Pro Max, kept as they were. They are the record of how each fact in
the other documents was established and of what was tried and ruled out.

Read [../HOW-IT-WORKS.md](../HOW-IT-WORKS.md) and [../PORTING.md](../PORTING.md)
first. Come here when you want the evidence behind a statement, or to see
how a particular problem was tracked down.

| File | What it is |
|---|---|
| [development-log.md](development-log.md) | Every version from the first silent stream to v0.15: what was wrong, how it was found, what was checked and what was not |
| [software-encoder-path.md](software-encoder-path.md) | The first investigation: where the stack encodes in software, that its FDK-AAC can do ELD, which functions needed to learn about the codec. Written before the module ran; section 8 says what happened when it did |
| [object-layout.md](object-layout.md) | The stack's codec object, field by field, as read from the disassembly of one build. The module now measures this itself |
| [airpods-capability-capture.md](airpods-capability-capture.md) | The capture that showed AirPods offer AAC-ELD to a non-Apple phone, decoded byte by byte |
| [baseline.md](baseline.md) | The development phone and its settings before anything was changed |

Things to know when reading them:

- **Addresses** such as `0x8ef590` are positions inside one build of
  `libbluetooth_jni.so` (build ID `ea12c5b9edc07b7e826bd2aaec5a5f5b`,
  HyperOS 3). They are different in every other build. The function names
  next to them are what carries over.
- **"The owner"** is the person whose phone it is and who did the listening.
- **Files that are mentioned but not in this repository**: the Bluetooth
  captures (they contain link keys), copies of the phone's libraries and the
  ROM (they are the phone maker's), and a test receiver built on a Raspberry
  Pi Pico. `aac-eld-apple.md` is the codec description in the
  [USBPods-Pico2W](https://github.com/wasdwasd0105/USBPods-Pico2W) project.
- Earlier statements are sometimes corrected further down the same file.
  The later one is the one that holds.
