# Third-party code

Everything in this folder keeps its own licence. None of it is covered by
this repository's GPL-2.0 licence.

| Folder | What | From | Licence |
|---|---|---|---|
| `shadowhook/` | Inline hook engine, version 2.0.1, the native source only | [bytedance/android-inline-hook](https://github.com/bytedance/android-inline-hook), `shadowhook/src/main/cpp` | MIT, see `shadowhook/LICENSE` |
| `shadowhook/third_party/xdl/` | xDL, symbol lookup including `.gnu_debugdata` | bundled with ShadowHook | MIT, see its `LICENSE` |
| `shadowhook/third_party/lss/` | linux-syscall-support | bundled with ShadowHook | BSD 3-Clause, see its `LICENSE` |
| `shadowhook/third_party/bsd/` | BSD `queue.h` and `tree.h` | bundled with ShadowHook | BSD, in the file headers |
| `zygisk/zygisk.hpp` | The Zygisk module API header | [topjohnwu/zygisk-module-sample](https://github.com/topjohnwu/zygisk-module-sample) | Permissive, in the file header |

Outside this folder, `module/META-INF/com/google/android/update-binary` is
the installer stub that every Magisk-format module carries, from the
[Magisk](https://github.com/topjohnwu/Magisk) project.

## Changes made to ShadowHook

The copy here is not pristine. Three changes were needed for it to work
inside the Bluetooth process of the development phone, and they are already
applied. `shadowhook-patches/` describes each one: what was changed, in which
file, and the crash that led to it.

| File | Change |
|---|---|
| `1-linker-optional.txt` | `shadowhook_init` no longer fails when the dynamic linker's private symbols cannot be found. They serve a feature this project does not use |
| `2-no-island.txt` | Hooks are installed without branch islands, which the engine placed in memory it could not write |
| `3-fresh-trampoline-page.txt` | Every trampoline gets its own page, because a page that has executed code could no longer be written |

On another phone these may be unnecessary or insufficient; see
`docs/TROUBLESHOOTING.md`, "Bluetooth crashes at start".

The module is linked statically against this engine on purpose: a Zygisk
module is loaded from a place where the target process cannot resolve further
shared libraries.
