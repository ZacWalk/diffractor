# Linux Port

This document owns the current Linux support boundary: what builds, which platform services exist,
and which product surfaces are unavailable. [Implementation](implementation.md) owns architecture;
[Rendering](rendering.md) owns backend parity; [Design](design.md) owns cross-platform user behavior.

## Current Support

The portable core and vendored dependencies build with CMake and GCC on Linux, and the embedded test
suite runs headlessly in Debug and Release. Indexing, search, metadata parsing, image and media decode,
collections, locations, and core file operations use the same model and tests as Windows.

Linux does not yet produce the interactive Diffractor application. There is no native window or
message loop, text stack, renderer present target, audio output, or complete desktop integration.
The Linux target is therefore a portability and headless-test target, not a distributable product.

## Platform Boundary

Portable application code depends on libdesktop's [platform.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform.h) -
the OS services of its system module, which also carries their Linux backend and
[documents it](https://github.com/ZacWalk/libdesktop/blob/main/docs/system.md#the-linux-backend) - and on
libdesktop's [platform_render.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform_render.h) for what the desktop UI layer implements: icon glyphs,
movie writing, audio output and the rendering probes. Operating-system headers, handles, and direct system calls
remain in libdesktop and in the application's own `platform*` files - its side of FFmpeg's hardware
decoding and its entry points. Repository lint enforces the parts
of this boundary visible from source text. It allows OS headers in any `platform*` file, though
only `platform_win_av.cpp` and the Windows platform tests still need them. XMP rationals, property
ratios and search terms are still read with `_snscanf_s` (in `metadata_xmp.cpp`,
`model_property.cpp` and `model_search.cpp`), which libdesktop shims on Linux and which can read
damaged text differently there; dates already parse by one rule on both platforms.

The Linux implementation currently provides:

| Area | Implementation |
| --- | --- |
| Process, synchronization, locale, known folders | libdesktop's `platform_linux.cpp` |
| Files, mapping, enumeration, paths | libdesktop's `platform_linux_files.cpp` |
| Copy, move, delete, and shell stand-ins | libdesktop's `platform_linux_desktop.cpp` |
| INI settings | libdesktop's `platform_linux_settings.cpp` |
| The desktop UI's stand-ins: theme colours, key codes, UI-thread identity, icon glyphs, movie writing and audio output | libdesktop's `platform_linux_ui.cpp` |
| Entry point, the absent hardware video decode, and the empty platform test registration | `platform_linux_app.cpp` |

Path identity follows the filesystem: case-insensitive on Windows and case-sensitive on Linux.
Case-folded comparisons remain appropriate for media types, tags, and other vocabulary, but not for
path-keyed containers.
Filesystem creation time is read from birth-time metadata when available; an absent birth time
stays absent. Directory moves require atomic non-replacing rename support and report failure rather
than emulating it with an existence check followed by a replacing rename.
Startup selects the UTF-8 character locale for library filename conversions, independently of the
shell's locale. A failed file operation always supplies a nonempty diagnostic, including collisions
reported by an atomic move.

## Unsupported Surfaces

The current Linux platform has no implementation for:

- native windows, input dispatch, dialogs, menus, clipboard, drag/drop, or monitor/DPI integration;
- font discovery, shaping, fallback, and glyph rasterization;
- a GPU backend or a native present target for the portable software rasterizer;
- audio output and hardware video presentation;
- Windows Shell features such as associations, properties, Explorer thumbnails, device eject, the
  share sheet, and taskbar progress;
- update, installer, Store, and desktop packaging workflows.

Portable code must express the intended operation rather than mirror a Win32 API shape. A Linux
stand-in may support headless tests, but it must not report success for a user-visible operation it
did not perform.

## Rendering Path

The platform-independent software rasterizer already produces bounded BGRA tiles. A Linux UI needs a
window-owned buffer and present target plus the text stack described above. Rendering parity remains
defined by [rendering.md](rendering.md); Linux does not get a reduced visual contract merely because
its first backend is software.

## Build

CMake is the only application build description. Linux builds vendored dependencies by default and
upstream FFmpeg as a required dependency, and fetches libfiles and libdesktop - or builds sibling
`../libfiles` and `../libdesktop` checkouts. Runtime fixtures, languages, dictionaries, and location data are staged beside the test
binary.

Typical commands are:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/diffractor /test
```

CI definitions remain authoritative for compiler versions and installed system prerequisites.

Build from a Linux checkout. A Windows checkout with `core.autocrlf` set, built from WSL, gives
FFmpeg's `configure` CRLF line endings, which `/bin/sh` rejects as "No such file or directory".

### Retiring MSBuild

The application solution and project files were removed after CMake reached parity. Repository lint
rejects tracked `.sln` and application `.vcxproj` files so a second build description cannot drift
back in. Vendored forks may retain their own upstream project files; Diffractor's CMake modules decide
how they join the application build.

## Where this lives

- libdesktop's [platform.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform.h) and
  [platform_render.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform_render.h): abstraction required from each platform.
- libdesktop's [Linux backend](https://github.com/ZacWalk/libdesktop/tree/main/src/system) and
  [platform_linux_ui.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/render/platform_linux_ui.cpp) and
  [platform_linux_app.cpp](../src/platform_linux_app.cpp): current Linux implementation.
- libdesktop's [platform_compat.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform_compat.h): cross-platform
  compatibility helpers.
- libdesktop's [render_software.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/render_software.h) and
  [render_software.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/render/render_software.cpp): portable CPU rasterizer.
- [CMakeLists.txt](../CMakeLists.txt), `cmake/`, [dd.ps1](../dd.ps1), and
  [tools/dd.py](../tools/dd.py): build description and drivers.