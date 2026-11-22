# Crash investigation

This document owns turning a Diffractor crash report into an attributed defect: what a report contains,
where symbols come from, how to make a minidump resolve, and how to tell the faulting frame from the
cause. What the code does belongs to [implementation.md](implementation.md) and the owning subject
document.

## 1. What a report contains

libdesktop's exception filter calls `app_frame::crash` ([app.cpp](../src/app.cpp)), which posts a
`crash.zip` to diffractor.com under the subject `Diffractor CRASH report`, beside form fields carrying
`calc_app_info` - the block the logo's tooltip shows with debug info on. The zip holds:

- `Diffractor-<version>-<build>-<yyyymmdd>-<hhmmss>.dmp` (UTC) — a user minidump.
- `diffractor.log` — the crashing session.
- `diffractor.previous.log` — the session before it.

**Support** (`send_info`, [app_dialogs.cpp](../src/app_dialogs.cpp)) posts the same block and logs to the
same endpoint as `logs.zip` under `Diffractor LOG`, with no dump. Read the subject first: a `LOG` report is
a user asking for help, not a fault.

The dump is the part most likely to be missing: writing it needs `dbghelp` and a writable temp file, and
without either the logs go alone, the log saying `no minidump was written - reporting logs only`. A report
with no `.dmp` is therefore a real crash, not a truncated upload. Both temp files are deleted once the
attempt ends, so nothing is left on the user's machine to ask for.

One thread claims the handler (`df::handling_crash`), so a report never duplicates a second fault inside
the first. Before anything else, and whether or not it reports, the handler adds the files the faulting
thread had open to the skip list ([crash-loop protection](implementation.md#crash-loop-protection)). No
report comes from the Store build, which compiles the filter out (`WINSTORE`) - its crashes reach Partner
Center's health reports through the `.appxsym` symbols in the upload `.\dd.ps1 store` builds - nor with
`setting.send_crash_dumps` off, from a run with `-profile:`, or from an agent session without `network`
(`app_frame::apply_run_settings`). When Windows ends a hung or crashing process, in either build, its
recovery callback (`save_recovery_state`) logs `*** recover callback ***` and `Open file <type> in <stage>`
for each open file, and adds them to the skip list.

## 2. Read the logs before the dump

The log is cheaper than the dump and often decides the answer on its own. Each line holds the thread ID,
milliseconds since the first line, and its context - usually the function that logged it; paths show the
account name as `%user%`. Take:

- **Lines 1-2** — version, build, OS, bit-ness, `debug`/`release`, core count.
- **Startup block** — settings backend, GPU adapter and vendor/device IDs, whether the D3D11 device was
  created, `p010`/`nv12` support, image budgets.
- **Timeline** — compare the last line's time with `Process Uptime` in the dump: a long gap means the
  fault has no logged neighbour and the log is only context.
- **The tail** — `Add file type to crash list <type>`, then `*** CRASH ***`, plus `Last file type opened`
  and `Rendering function` (from `render::rendering_func`, maintained by `render::scope_rendering_func`)
  when they were set.
- **`skipping <n> file(s) that crashed a previous run of this release`** — files earlier crashes listed,
  left unscanned in this session.
- **The previous log** — if it shows the same anomaly, the crash is reproducible on that machine and the
  anomaly is a lead rather than a coincidence.

A log that reaches 4 MiB says `*** log truncated ...` and then keeps only `main` and `perf` lines, so its
crash tail is missing. Anything a `__try`/`__except` or `catch` swallowed appears here and nowhere else; a
swallowed fault minutes before the crash is a suspect, not a footnote.

## 3. Symbols

`dd.ps1`'s `Add-ToSymbolStore` publishes with
`symstore add /r /f <file> /s c:\code\symbols /t Diffractor /compress`, run from the Windows SDK's
`Windows Kits\10\Debuggers\x64`: its **Debugging Tools for Windows**, without which desktop and Store
packaging fail before changing the version (`Get-SymStore`). Without administrator rights, deploy an SDK
administrative image under `%LOCALAPPDATA%\Microsoft\WindowsSDK\Debuggers\<SDK version>`, keeping that
layout; the newest version is used when there is no machine-installed copy. The 32- and 64-bit desktop
binaries and the Store `diffractor.exe` are published with their PDBs, so:

- The store is `c:\code\symbols`, indexed by `<name>\<key>\<name>.ex_` / `.pd_`.
- Entries are **cab-compressed**, so they must be `expand`ed before a debugger can use them as an image.

The console debugger is the WinDbg store app, not the Windows Kits install:

```powershell
$cdb = Join-Path (Get-AppxPackage *WinDbg*).InstallLocation 'amd64\cdb.exe'
```

## 4. The minidump trap

A user minidump does not contain the executable's PE header. `dbghelp` therefore
cannot read the debug directory, cannot find the PDB, and reports
`Image header paged out` / `(no symbols)`. A symbol path alone — `-y srv*...` — is not
enough, and pointing `-i` at the symbol server does not help either.

The image has to be on disk, uncompressed, and matched:

```powershell
<# Step 1: module range end minus start gives the image size. #>
& $cdb -z <dump> -c "lm m diffractor*; q"

<# Step 2: store keys are TimeDateStamp plus size; filter by the size suffix. #>
Get-ChildItem c:\code\symbols\diffractor64.exe | Where-Object Name -like '*30a3000'

<# Step 3: expand the image and PDB side by side. #>
expand c:\code\symbols\diffractor64.exe\<key>\diffractor64.ex_ tmp\bin\diffractor64.exe
expand c:\code\symbols\diffractor64.pdb\<guid+age>\diffractor64.pd_ tmp\bin\diffractor64.pdb

<# Step 4: -i supplies the image and -y supplies symbols. #>
& $cdb -z <dump> `
  -y "<checkout>\tmp\bin;srv*c:\code\symbols*https://msdl.microsoft.com/download/symbols" `
  -i "<checkout>\tmp\bin" `
  -c ".lines -e; .reload /f; .ecxr; kv; q"
```

**Several store entries share one size suffix** — one per build that day. Pick by the
leading `TimeDateStamp`, or try each: the wrong image says `image header does not
match memory image header` and then `mismatched pdb`. The matching PDB key is the one
whose folder timestamp is minutes after the image's; `lmvm diffractor64` prints the
image `Timestamp:` once the right one loads, which confirms the pair.

Run cdb from a `.ps1` invoked as `pwsh -NoProfile -File`. An inline
`pwsh -Command "...$var..."` loses its variables to the outer shell.

## 5. Local debug builds

A crash you can reproduce is quickest to read live. `.\dd.ps1 vs` builds Debug x64 and opens
`exe\diffractor64-d.exe` in the Visual Studio debugger; F5 starts it, and the crash stops at the
faulting line with the Call Stack window (Ctrl+Alt+C) showing how it got there. `--config Release`
debugs the Release build instead, `--no-build` skips the build, `--dry-run` prints what it would do,
and any other argument is passed to Diffractor.

A dump from `exe\diffractor64-d.exe` usually has no stored symbols, and the local PDB
has been relinked since. Force the mismatched PDB on:

```
.reload /f /i diffractor64-d.exe=<base>,<size>
```

The result is approximate: treat function names as a strong hint and line numbers as weaker evidence.
Verify the answer against current source before acting on it.

## 6. Reading the fault

In order, and stopping as soon as the answer is unambiguous:

- `.ecxr` — switch to the stored exception context. Everything else is meaningless
  without it. Read the faulting instruction and the register it dereferenced; `rax=0`
  with `mov rax,[rax]` is a null virtual call, `0xcccccccc` is uninitialised debug
  memory, a plausible-looking-but-wrong pointer is a lifetime or corruption bug.
- `kv` — the stack. Frames below an unresolved module are unreliable, because x64
  unwinding needs that module's unwind info.
- `!analyze -v` — the bucket, `Failure.Exception.Code`, and `Timeline.Process.Start.DeltaSec`.
- `dps <rsp> <stack-top>` — a raw scan of the stack region. When `kv` dies at a
  system DLL this recovers the app frames above it, which is usually what identifies
  the feature involved.
- `~*k` — the other threads, to see what work was in flight.
- `?? setting.<field>` — globals read straight out of the dump. The fastest way to
  confirm which configuration produced the crash.

## 7. Attribution

**The faulting frame is not always the culprit.** Two cases recur:

- **Heap corruption** (`!analyze` bucket `HEAP_CORRUPTION`, original exception
  `0xC0000374`). The fault is in `RtlpFreeHeapInternal` on some innocent free, often
  inside a system DLL. The corrupter ran earlier and left no frame. Go back to the log
  and look for a swallowed fault, a third-party or driver call, or an operation that
  writes a buffer — and check whether the previous session shows the same thing.
- **Deliberate crashes.** A Debug build's `debug_action` tool ([agents](agents.md)) crashes on purpose:
  `crash` writes through a null pointer in `app_frame::run_test_action`, and `send-crash-report` sends a
  report whose "dump" is the fixture `Test.jpg`. A crash with `run_console_tests` on its stack and an
  uptime of seconds is a `/test` console run, not a user session.

An attributed crash needs three things that agree: the faulting instruction, a
mechanism in our code that produces it, and something in the log or the dump's globals
that says this configuration took that path. Two out of three is a hypothesis.

## Where this lives

| Crash-handling subject | Source |
|---|---|
| Report contents and posting | `app_frame::crash` and `calc_app_info` in [app.cpp](../src/app.cpp); `send_info` in [app_dialogs.cpp](../src/app_dialogs.cpp) |
| Exception filter, minidump writing, and restart and recovery registration | libdesktop's [process.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/platform/win/process.cpp) |
| The open-file list a fault records, and its bounds | [util_crash_files_db.h](../src/util_crash_files_db.h); `app_frame::save_recovery_state` in [app.cpp](../src/app.cpp) |
| The graphics crash guard and hardware-acceleration fallback | libdesktop's [settings.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/platform/win/settings.cpp) keeps the guards; `apply_gpu_crash_guard` in [app.cpp](../src/app.cpp) falls back from them |
| The safe start after two unsettled launches | `app_frame::load_settings` in [app.cpp](../src/app.cpp), `decide_startup` in [app_settings.h](../src/app_settings.h), `reset_presentation` in [app_settings.cpp](../src/app_settings.cpp) |
| Diagnostic log and session counters | libdesktop's [core/log.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/core/log.h), [core/util.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/core/util.h) and [platform/services.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform/services.h); [app_perf.h](../src/app_perf.h) |
| SDK symbol-store discovery and publishing | `Get-SymStore` and `Add-ToSymbolStore` in [dd.ps1](../dd.ps1), checked by [test_release_tools.ps1](../tools/test_release_tools.ps1) |
| Debugging a reproducible crash in Visual Studio (`dd.ps1 vs`) | `Open-VisualStudio` in [dd.ps1](../dd.ps1) |
| Deliberate crashes in a Debug build | `app_frame::run_test_action` in [app_commands.cpp](../src/app_commands.cpp) |
