---
description: The platform boundary - what may live here, and what must not leak out.
applyTo: 'src/platform*.cpp, src/platform*.h'
---

# The platform boundary

Owning documents: [implementation](../../docs/implementation.md) for the boundary,
[Linux port](../../docs/linux.md) for the portability assessment and the current debt.

## This is the only place system code exists

System headers, Windows API calls and OS handle types (`HWND`, `LRESULT`, `WPARAM`, `LPARAM`,
`<windows.h>`, `d3d11`, `dxgi`, `dwrite`, `shlobj`, `wincodec`) appear **only** in `platform*`
files. The lint enforces this. `test_platform_win.cpp` is the single test file permitted system
headers, and keeping those tests there is what lets every other test file stay free of them.

## Adding to the abstraction

When portable code needs a system capability, add it to libdesktop's
[platform.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform.h) - a change made in libdesktop, under its own
AGENTS.md - as an intention, not as a thin wrapper over one API. The Linux side must be able to
implement it honestly. A function named for a Win32 call has already leaked the boundary even
though it compiles. A service only the window layer can implement is declared in
libdesktop's [platform_render.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform_render.h) instead, and implemented by libdesktop-render's
`platform_win_*` backends and its `platform_linux_ui.cpp` - a change made in libdesktop too. What
stays here is the application's own: its side of FFmpeg's hardware decoding
(`platform_win_av.cpp`), its resource script and its Linux entry point (`platform_linux_app.cpp`).

Every new entry point needs a Linux counterpart or an explicit stub, and
[linux.md](../../docs/linux.md) records which. The `platform_linux_*_stubs.cpp` files are
alternatives to their real implementations, never built alongside them.

## Capability is answered at run time

A command the platform cannot perform is **absent, not dimmed** — and that question is asked of the
running system, not fixed at build time, so the same build offers a command on one machine and not
another. `can_recycle` is the model: it asks about the path in front of it.

## Checks

`/test:*path*` and the Windows suite via `.\dd.ps1 test`; the Linux build is
`.github/workflows/linux.yml`. Adding a `platform.h` entry point without a Linux counterpart breaks
that workflow, not the local build, so state it in the summary when you cannot run it.
