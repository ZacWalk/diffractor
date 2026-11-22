---
description: The platform boundary - what may live here, and what must not leak out.
applyTo: 'src/platform*.cpp, src/platform*.h'
---

# The platform boundary

Owning documents: [implementation](../../docs/implementation.md) for the boundary,
[Linux port](../../docs/linux.md) for what Linux supports.

## This is the only place system code exists

System headers, Windows API calls and OS handle types (`HWND`, `LRESULT`, `WPARAM`, `LPARAM`,
`<windows.h>`, `d3d11`, `dxgi`, `dwrite`, `shlobj`, `shellapi`, `wincodec`) appear **only** in `platform*`
files, apart from the entry points in `app.cpp`. The lint enforces this. `test_platform_win.cpp` is
the single test file permitted system headers, and keeping those tests there is what lets every other
test file stay free of them.

## Adding to the abstraction

When portable code needs a system capability, add it to libdesktop's
[libdesktop/platform.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform.h) - a
change made in libdesktop, under its own AGENTS.md - as an intention the Linux side can implement
honestly, not as a thin wrapper over one API: a function named for a Win32 call has leaked the boundary
even though it compiles. A service only the window layer can implement is declared in libdesktop's
[render.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/render.h) or
[ui.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/ui.h) instead, and implemented
by its render backends (`src/render/win`, `src/render/linux`), also in libdesktop. What stays here is the
application's own: its side of FFmpeg's hardware decoding (`platform_win_av.cpp`), the screen recorder
(`platform_win_record.cpp`), its resource script and Linux glue (`platform_linux_app.cpp`).
`src/app.cpp` owns the Windows `wWinMain` and Linux `main`, and both call `platform::run`.

Every new platform capability needs a Linux implementation or an explicit unavailable answer.
libdesktop's [Linux backend](https://github.com/ZacWalk/libdesktop/blob/main/docs/platform.md#the-linux-backend)
lists what it answers as unavailable, and [linux.md](../../docs/linux.md) what that removes from
Diffractor. `platform_linux_app.cpp` supplies the Linux hardware-decode, screen-recorder and platform-test
answers; it is built instead of the Windows-only `platform_win_av.cpp`, `platform_win_record.cpp` and
`test_platform_win.cpp`.

## Capability is answered at run time

A command the platform cannot perform is **absent, not dimmed** — and that question is asked of the
running system, not fixed at build time, so the same build offers a command on one machine and not
another. `can_recycle` is the model: it asks about the path in front of it.

## Checks

On Windows, build Debug with `python tools\dd.py build`, run
`.\exe\diffractor64-d.exe /test:*path* | Out-Host`, then the Windows suite with `.\dd.ps1 test`. On
Linux or in WSL, `./dd build` then `./dd test` (as `bash ./dd`, since the launcher has no executable
bit) is the Linux check, as `.github/workflows/linux.yml` runs it. A new `platform::` capability
without a Linux counterpart breaks the Linux build, not the Windows one, so say so in the summary
when you could not run it.
