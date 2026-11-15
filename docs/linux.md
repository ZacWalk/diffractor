# Linux Port

This document owns Diffractor's Linux support: what runs, the platform boundary, what is unavailable,
and how it builds. [Implementation](implementation.md) owns architecture and [Design](design.md) user
behavior; libdesktop's
[Linux backend](https://github.com/ZacWalk/libdesktop/blob/main/docs/platform.md#the-linux-backend)
owns the platform services beneath it.

## Current Support

The application builds with CMake and GCC and runs on libdesktop's Linux window layer: SDL3 windows on
Wayland or X11, drawn by the software renderer, with Pango setting the text. Its frames, controls,
menus and dialogs are the ones Windows shows, since libdesktop draws them itself on both platforms,
and the picture is held to the same contract as Windows' software renderer
([rendering](rendering.md#backends)). Indexing, search, metadata, image and media decode,
collections, locations and file operations share Windows' model and tests, and the embedded suite runs
headless in Debug and Release. `_DEBUG` is an MSVC definition, so Linux runs the Release-only tests
(`#ifndef _DEBUG`) in both configurations.

The Linux build is not distributed: it has no audio output, GPU backend or hardware video decode, and
much of the desktop integration is unavailable.

### Desktop icon

The window carries the generated, embedded `app-icon.bmp` on X11. Wayland resolves
icons through the application ID instead: CMake writes a `diffractor.desktop`
launcher in the build directory, referring to that build and `src/Res/app.png`.
Copy it to `$XDG_DATA_HOME/applications/diffractor.desktop` (normally
`~/.local/share/applications/diffractor.desktop`), keeping its filename and the
checkout paths intact. Its ID matches libdesktop's default `diffractor` window ID.
This is a local-build launcher, not a Linux distribution package.

## Platform Boundary

Portable application code depends on libdesktop's
[platform.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform.h) for
operating-system services, and on its
[render.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/render.h) and
[ui.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/ui.h) for what the window
layer implements. Operating-system headers, handles and direct system calls stay in libdesktop and in
Diffractor's own `platform*` files; [app.cpp](../src/app.cpp) owns the entry points, `wWinMain` and
`main`, which both call `platform::run`. Repository lint enforces the parts of this boundary visible
in source text. It allows OS headers in any `platform*` file, though only `platform_win_av.cpp`, the
Debug build's screen recorder `platform_win_record.cpp` and the Windows platform tests need them.
[platform_linux_app.cpp](../src/platform_linux_app.cpp) is built in place of `platform_win_av.cpp`,
`platform_win_record.cpp` and `test_platform_win.cpp`: it answers that there is no hardware video decode
and, in a Debug build, nothing to record the window with, and registers no platform tests.

Portable code expresses the intended operation rather than mirroring a Win32 API shape. A Linux
stand-in may support headless tests, but it never reports success for a user-visible operation it did
not perform.

Path identity follows the filesystem: case-insensitive on Windows and case-sensitive on Linux.
Case-folded comparisons suit media types, tags and other vocabulary, but not path-keyed containers.
Property ratios and search terms are read with `_snscanf_s` (in `model_property.cpp` and
`model_search.cpp`), which libdesktop shims on Linux and which can read damaged text differently
there; dates, and the numbers in a photo's metadata, parse by one rule on both platforms -
libdesktop's and libfiles'.

## Unavailable On Linux

A service libdesktop's Linux backend does not implement answers as unavailable, and a capability
query that answers false keeps its command out of the interface; libdesktop's
[list](https://github.com/ZacWalk/libdesktop/blob/main/docs/platform.md#the-linux-backend) is the
complete one. In Diffractor that means no:

- drag and drop, or files and images on the clipboard - text works;
- GPU backend, audio output, or hardware video decode;
- Movie Render, which needs the platform's movie writer;
- Windows Shell features such as associations, properties, Explorer thumbnails, device eject, the
  share sheet and taskbar progress;
- update, installer, Store or desktop packaging workflows.

## Build

CMake is the only build description. Linux builds the same fetched dependencies, upstream FFmpeg
through its own configure and make, and libfiles, libdesktop and lib64 at their pins - or sibling
`../libfiles`, `../libdesktop` and `../lib64` checkouts. Runtime fixtures, languages, dictionaries
and location data are linked beside the binary.

```bash
bash ./dd setup    # install the packages the build needs; uses sudo
bash ./dd build    # Debug; --config Release for the other
bash ./dd test     # the embedded suite, as CI runs it
bash ./dd run
```

`./dd` runs `dd.ps1` under PowerShell, or `tools/dd.py` without it; it is checked in without an
executable bit, hence `bash`. The build goes to `tmp/linux-build/`, or to
`~/.cache/diffractor/<checkout path>/linux-build` for a checkout on a Windows drive, where a tree is
slow and FetchContent cannot rename the directories it extracts; `./dd info` names it. A leading `/`
starts a path on Linux, so switches take `-` or `--` there.
[linux.yml](../.github/workflows/linux.yml) is authoritative for the compiler and the packages
installed, and `./dd setup` installs the same window-layer list.

From Windows, `.\dd.ps1 linux` builds Release in WSL and starts it on WSLg's display (`--config Debug`
builds the other; further arguments reach Diffractor). It stops first if a package the build needs is
missing - `./dd setup` inside WSL installs them - or if there is no display. FFmpeg arrives as an
upstream archive, so a Windows checkout's CRLF line endings never reach its `configure`.

### One build description

Repository lint rejects tracked `.sln` and `.vcxproj` files, so a second build description cannot
drift from CMake. A fetched dependency's own build system is never used for the application;
Diffractor's CMake modules decide how each joins the build, FFmpeg's configure and make on Linux
among them.

## Where this lives

- libdesktop's [platform.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform.h)
  and [render.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/render.h): the
  abstraction required from each platform.
- libdesktop's [Linux backend](https://github.com/ZacWalk/libdesktop/tree/main/src/platform/linux),
  [Linux window layer](https://github.com/ZacWalk/libdesktop/blob/main/src/render/linux/window_device.cpp)
  and [platform_linux_app.cpp](../src/platform_linux_app.cpp): the Linux implementation.
- libdesktop's [dialect.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/core/dialect.h):
  cross-platform compatibility helpers, `_snscanf_s` among them.
- [CMakeLists.txt](../CMakeLists.txt), `cmake/`, [dd](../dd), [dd.ps1](../dd.ps1), and
  [tools/dd.py](../tools/dd.py): the build description and drivers.
