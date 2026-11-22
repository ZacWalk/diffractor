# Linux Port

This document owns Diffractor's Linux support: what runs, the platform boundary, what is unavailable,
and how it builds. [Implementation](implementation.md) owns architecture and [Design](design.md) user
behavior; libdesktop's
[Linux backend](https://github.com/ZacWalk/libdesktop/blob/main/docs/platform.md#the-linux-backend)
owns the platform services beneath it.

## Current Support

The application builds with CMake and GCC and runs on libdesktop's Linux window layer: SDL3 windows on
Wayland or X11, drawn by the software renderer, with Pango setting the text. Frames, controls, menus and
dialogs are the ones Windows shows, since libdesktop draws them itself on both platforms, and the picture
keeps the software renderer's contract ([rendering](rendering.md#backends)). Indexing, search, metadata,
image and media decode, face search, collections, locations and file operations share Windows' model and
tests. The embedded suite runs headless in Debug and Release; `_DEBUG` is an MSVC definition, so Linux runs
the Release-only tests (`#ifndef _DEBUG`) in both. An agent session without a display gets SDL's offscreen
windows.

The Linux build is not distributed: it has no audio output, GPU backend or hardware video decode, and
much of the desktop integration is unavailable.

### Desktop icon

On X11 the window carries the embedded `app-icon.bmp`. Wayland finds icons through the application ID
instead: CMake writes a `diffractor.desktop` launcher into the build directory, naming that build's binary
and `src/Res/app.png`. Copy it to `$XDG_DATA_HOME/applications/diffractor.desktop` (normally
`~/.local/share/applications/`), keeping its file name and the checkout's paths; the name matches the
window's application ID, `diffractor`, which libdesktop makes from the registered name in lower case. It is
a launcher for a local build, not a distribution package.

## Platform Boundary

Portable code depends on libdesktop's
[platform.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform.h) for
operating-system services, and on its
[render.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/render.h) and
[ui.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/ui.h) for the window layer.
Operating-system headers, handles and system calls stay in libdesktop and Diffractor's own `platform*`
files, apart from the entry points in [app.cpp](../src/app.cpp), `wWinMain` and `main`, which both call
`platform::run`. Repository lint enforces what source text shows of this boundary and allows OS headers in
any `platform*` file; only `platform_win_av.cpp`, the screen recorder `platform_win_record.cpp` and the
Windows platform tests use them. [platform_linux_app.cpp](../src/platform_linux_app.cpp) is built in their
place: it answers that there is no hardware video decode and nothing to record the window with, and
registers no platform tests.

Portable code expresses the intended operation rather than mirroring a Win32 API shape. A Linux stand-in
may support headless tests, but never reports success for a user-visible operation it did not perform.

Path identity follows the filesystem: case-insensitive on Windows, case-sensitive on Linux. Case-folded
comparison suits media types, tags and other vocabulary, not path-keyed containers. Property ratios and
search terms are read with `_snscanf_s` (in `model_property.cpp` and `model_search.cpp`), which libdesktop
shims on Linux and which can read damaged text differently there; dates, and the numbers in a photo's
metadata, parse by one rule on both platforms - libdesktop's and libfiles'.

## Unavailable On Linux

A service libdesktop's Linux backend does not implement answers as unavailable, and a capability query
that answers false keeps its command out of the interface; libdesktop's
[list](https://github.com/ZacWalk/libdesktop/blob/main/docs/platform.md#the-linux-backend) is the complete
one. In Diffractor that means no:

- drag and drop, or files and images on the clipboard - text works;
- GPU backend, audio output, or hardware video decode;
- network, so no map tiles beyond the built-in map, update check, crash or Support report, or spelling
  dictionary download;
- movie writer, so no Movie Render and no recording of the window for an agent or a tutorial;
- Recycle Bin, so Delete is permanent;
- Import from a phone or camera connected as a device rather than a drive;
- folder pickers, opening a file or link in another program or the file browser, external tools,
  printing, the desktop background, disc burning or sharing;
- Windows Shell features such as associations, properties, Explorer thumbnails, device eject and taskbar
  progress;
- update, installer, Store or desktop packaging workflows.

## Build

CMake is the only build description. Linux builds the same fetched dependencies, upstream FFmpeg through
its own configure and make, and libfiles, libdesktop, lib64 and libmug at their pins - or sibling
`../libfiles`, `../libdesktop`, `../lib64` and `../libmug` checkouts. The binary stays in the build
directory, with the runtime fixtures, catalogs, dictionaries and location data linked beside it.

```bash
bash ./dd setup    # install the packages the build needs; uses sudo
bash ./dd build    # Debug; --config Release for the other
bash ./dd test     # the embedded suite, as CI runs it
bash ./dd run
```

`./dd` runs `dd.ps1` under PowerShell, or `tools/dd.py` without it; it is checked in without an
executable bit, hence `bash`. The build goes to `tmp/linux-build/`, or - for a checkout on a Windows drive,
where a tree is slow and FetchContent cannot rename the directories it extracts - to
`~/.cache/diffractor/<checkout path>/linux-build` (`$XDG_CACHE_HOME` where set); `./dd info` names it. A
leading `/` starts a path on Linux, so switches take `-` or `--` there.
[linux.yml](../.github/workflows/linux.yml) is authoritative for the compiler and packages, and `./dd setup`
installs the same window-layer list.

From Windows, `.\dd.ps1 linux` builds Release in WSL and starts it on WSLg's display (`--config Debug`
builds the other; further arguments reach Diffractor). It stops first if a package the build needs is
missing - `./dd setup` inside WSL installs them - or if there is no display. FFmpeg arrives as an
upstream archive, so a Windows checkout's CRLF line endings never reach its `configure`.

### One build description

Repository lint rejects tracked `.sln`, `.vcxproj` and `.vcxproj.filters` files, so no second build
description can drift from CMake. A fetched dependency's own build system never builds the application;
Diffractor's CMake modules decide how each joins the build, FFmpeg's configure and make on Linux among
them.

## Where this lives

- libdesktop's [platform.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform.h)
  and [render.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/render.h): the
  abstraction required from each platform.
- libdesktop's [Linux backend](https://github.com/ZacWalk/libdesktop/tree/main/src/platform/linux),
  [Linux window layer](https://github.com/ZacWalk/libdesktop/blob/main/src/render/linux/window_device.cpp)
  and [platform_linux_app.cpp](../src/platform_linux_app.cpp): the Linux implementation.
- libdesktop's [dialect.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/core/dialect.h):
  cross-platform compatibility helpers, `_snscanf_s` among them.
- [CMakeLists.txt](../CMakeLists.txt) and the `cmake/` modules: the build description, the Linux sources,
  the runtime links and the `.desktop` launcher.
- [dd](../dd), [dd.ps1](../dd.ps1) and [tools/dd.py](../tools/dd.py): the drivers - `setup`, `linux` and
  the build directory.
- [linux.yml](../.github/workflows/linux.yml): the Debug and Release builds and suites CI runs.
