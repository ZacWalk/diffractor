# Diffractor Codebase

Diffractor is a C++20 photo, video and audio organizer for Windows, with a Linux build that runs but does not yet ship. Its interface is libdesktop's, drawn with Direct3D 11 or the software renderer; SQLite holds a rebuildable index; libfiles reads and writes media files and their metadata, and FFmpeg decodes audio and video.

## Primary design drivers

1. **Fast, lightweight performance:** start and respond quickly, scale to large collections, bound resource use, and keep expensive work off the UI thread.
2. **A clear [user mental model](docs/design.md#user-mental-model):** make scope, contents, target, effect, and recovery predictable; avoid hidden state and context-dependent surprises.

Both are product requirements. Make tradeoffs explicit and validate behavior and performance where affected.

## Start here: route the task before reading anything

Find the row matching the concrete behavior, symbol, or file. Read that document's named section,
open the listed source, and run the listed check. Do not read unrelated documents for context.

| If the task is about | Read | Code | Narrowest check |
|---|---|---|---|
| Query parsing, results, address box, autocomplete | [design](docs/design.md#navigation-and-search) | [model_search.cpp](src/model_search.cpp), [model_tokenizer.h](src/model_tokenizer.h), [app_search.h](src/app_search.h), [model_index_summary.cpp](src/model_index_summary.cpp) | `/test:*search*` |
| Indexing, the database, thumbnails, hydration | [implementation](docs/implementation.md#index-search-and-database) | [model_index.cpp](src/model_index.cpp), [model_db.cpp](src/model_db.cpp), [model_postings.h](src/model_postings.h), [model_search_index.cpp](src/model_search_index.cpp) | `/test:*index*`, `/test:*thumbnail*` |
| Collection membership, duplicates, presence | [collections](docs/collections.md) | [model_index.cpp](src/model_index.cpp), [model_index_duplicates.cpp](src/model_index_duplicates.cpp), [app_settings.cpp](src/app_settings.cpp), [model_related.h](src/model_related.h) | `/test:*duplicate*`, `/test:*presence*` |
| Zoom, pan, the navigator, panorama projection | [zoom](docs/zoom.md) | [model_zoom.h](src/model_zoom.h), [view_media.h](src/view_media.h), [ui_panorama.h](src/ui_panorama.h) | `/test:*zoom*`, `/test:*panorama*` |
| Places, distance, map, visits, tiles | [locations](docs/locations.md) | [model_locations.cpp](src/model_locations.cpp), [model_visits.cpp](src/model_visits.cpp), [ui_map_common.h](src/ui_map_common.h), [ui_globe.h](src/ui_globe.h), [model_tile_cache.cpp](src/model_tile_cache.cpp) | `/test:*location*`, `/test:*visit*`, `/test:*globe*` |
| Reading or writing a media file, staging, sidecars, rollback | [file I/O](docs/file-io.md) | [files.cpp](src/files.cpp), [app_util.cpp](src/app_util.cpp) | `/test:*sidecar*`, `/test:*collision*` |
| Which tag a property is read from or written to | [metadata](docs/metadata.md) | [files.cpp](src/files.cpp)'s `apply_properties`, [metadata.cpp](src/metadata.cpp), [model_property.cpp](src/model_property.cpp) | `/test:*metadata*`, `/test:*tag*` |
| Image budgets, colour, the video pipeline and HDR, subtitles, device loss and graphics fallbacks | [rendering](docs/rendering.md); the draw backends are libdesktop's [render.md](https://github.com/ZacWalk/libdesktop/blob/main/docs/render.md) | [av_format.cpp](src/av_format.cpp), [platform_win_av.cpp](src/platform_win_av.cpp), [av_subtitle.cpp](src/av_subtitle.cpp), [model.cpp](src/model.cpp)'s `texture_state`; libdesktop's [d3d11.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/render/win/d3d11.cpp), [software_context.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/render/software_context.cpp) and [surface.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/core/surface.cpp) | `/test:*surface*`, `/test:*colour*`, `/test:*video*`, `/test:*audio*` |
| The selection panel: form, content, order, density | [selection controls](docs/selection-controls.md) | [view_items.cpp](src/view_items.cpp), [ui_controls.h](src/ui_controls.h), libdesktop's [ui/elements.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/ui/elements.h) and [flex.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/ui/flex.cpp) | `/test:*selection*`, `/test:*layout*` |
| Tile geometry, scrolling, list layout | [design](docs/design.md#application-structure) | [view_items.h](src/view_items.h), [view_list.h](src/view_list.h), libdesktop's [flex.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/ui/flex.cpp) | `/test:*layout*`, `/test:*scroll*` |
| Command availability, targeting, keyboard | [design](docs/design.md#command-availability) | [app_commands.cpp](src/app_commands.cpp), [app_commands.h](src/app_commands.h) | `/test:*command*`, `/test:*selection*` |
| A guided operation: Rename, Import, Sync, Convert, Metadata, Date | [design](docs/design.md#guided-operations) | [view_rename.cpp](src/view_rename.cpp), [view_import.cpp](src/view_import.cpp), [view_sync.cpp](src/view_sync.cpp), [view_batch.cpp](src/view_batch.cpp) | `/test:*rename*`, `/test:*import*`, `/test:*sync*` |
| Movie mixer: the timeline, project files, rendering, checking and rendering without the window | [movie](docs/movie.md) | [model_movie.cpp](src/model_movie.cpp), [view_movie.cpp](src/view_movie.cpp), [view_movie_audio.cpp](src/view_movie_audio.cpp), [app_movie_batch.cpp](src/app_movie_batch.cpp) | `/test:*movie*` |
| An agent session over MCP, its tools, `-tool:`, screenshots, recording and tutorial scripts | [agents](docs/agents.md) | [app_mcp.cpp](src/app_mcp.cpp), [app_mcp_tools.cpp](src/app_mcp_tools.cpp), [app_mcp_ui.cpp](src/app_mcp_ui.cpp), [app_mcp_controls.cpp](src/app_mcp_controls.cpp), [app_tutorial.cpp](src/app_tutorial.cpp) | `/test:*agent*`, `/test:*tool call*`, `python tools/test_mcp_session.py exe/diffractor64-d.exe`, `python tools/sweep_mcp.py exe/diffractor64.exe` |
| Face detection: the detector, the plane it reads, upright faces and their order | [faces](docs/faces.md) | [model_faces.cpp](src/model_faces.cpp), [model_faces_detect.cpp](src/model_faces_detect.cpp), [facedetection.cmake](cmake/vendored/facedetection.cmake) | `/test:*detect*face*` |
| A crash report, dump, or symbolization | [crash investigation](docs/crash.md) | [app.cpp](src/app.cpp)'s `app_frame::crash`, libdesktop's [process.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/platform/win/process.cpp), [util_crash_files_db.h](src/util_crash_files_db.h) | see [crash](docs/crash.md) |
| Where a test belongs, or why the suite is arranged this way | [testing](docs/testing.md) | `src/test_*.cpp` | `.\dd.ps1 test` |
| Translated text, catalogs, plural forms | [testing](docs/testing.md) | [app_text.cpp](src/app_text.cpp), [app_validate_po.cpp](src/app_validate_po.cpp), `exe/languages` | `/test:*plural*`, `/validate-po`, `python tools/check_translations.py` |
| Upgrading a fetched dependency or moving a pin | [third-party](docs/third-party.md) | [dd-dependencies.json](cmake/dd-dependencies.json), `cmake/vendored`, `cmake/Diffractor*.cmake` | `.\dd.ps1 test` |
| Portability, the platform boundary, the Linux build | [Linux port](docs/linux.md) | libdesktop's [platform.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform.h), [render.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/render.h), libdesktop's [Linux backend](https://github.com/ZacWalk/libdesktop/tree/main/src/platform/linux), [platform_linux_app.cpp](src/platform_linux_app.cpp) | `./dd build`, then `./dd test`, on Linux or in WSL, as `.github/workflows/linux.yml` runs them |

`/test:` filters match the test **name**, case-insensitively, not the file. Every filter above is verified to match; a filter matching nothing is a failure, not a silent pass. Build the Debug binary with `python tools\dd.py build`, then run a filter as `.\exe\diffractor64-d.exe /test:*zoom* | Out-Host`; the pipe is what makes PowerShell wait for the exit code.

## Information ownership

GitHub issues and releases own work, status, discussion, follow-up, and release history. Source owns exact APIs, enums, and file lists. Every other subject has exactly one owning document:

| Document | Owns |
|---|---|
| [design](docs/design.md) | Durable user concepts and behavior: the user mental model, targeting and command availability, and view, mode, and presentation naming |
| [implementation](docs/implementation.md) | Architecture and source organization, data flow, threading, ownership, invalidation, index/search internals |
| [crash investigation](docs/crash.md) | Crash report contents, symbol store and debugger setup, dump symbolization, attributing a fault to a cause |
| [testing](docs/testing.md) | The test taxonomy: which subject file owns which test, what the runner enforces, fixtures, and headless test boundaries |
| [collections](docs/collections.md) | The collection: membership, what it earns, and its edge |
| [locations](docs/locations.md) | Places, location search, distance, map-driven queries, visits |
| [metadata](docs/metadata.md) | Property-to-tag mapping across XMP, EXIF, IPTC, and container tags |
| [file I/O](docs/file-io.md) | Read and write paths: decode ladder, staging, patching, sidecars, backups, rollback, failure contract |
| [rendering](docs/rendering.md) | What Diffractor draws through libdesktop's renderer: image budgets, presentation, video decode and hardware decode, HDR, subtitles, its own software rasterisers, and graphics fallbacks |
| [zoom](docs/zoom.md) | The zoom model, laws, rendering tiers, input, and navigator |
| [selection controls](docs/selection-controls.md) | The selection panel: form classification, content, ordering, density, responsive behavior |
| [movie](docs/movie.md) | Assembling photos, videos, audio files and screens of text into one video: the timeline document, trims, captions and crossfades, the `.dfv` project file, video encoding, and checking and rendering projects without the window |
| [agents](docs/agents.md) | Agent sessions over MCP: starting one, what it may do, its window, long work and dialogs, the tools, tutorial videos, screenshots, and one tool called from the command line |
| [faces](docs/faces.md) | Face detection: what the detector finds in a picture, the input it reads, and the upright space and stable order it reports faces in |
| [third-party](docs/third-party.md) | Fetched dependencies and their pins, the outbound licence they constrain, the upgrade procedure, and the integration constraints an upgrade must keep |
| [Linux port](docs/linux.md) | Current Linux support, the platform boundary, unsupported surfaces, and build behavior |
| [README](README.md) | Product overview, build prerequisites, command line, and the licence Diffractor is released under |

Move information to its owner; link rather than duplicate volatile detail. `tools/lint_repo.ps1` fails when a document has no owner row, when a link does not resolve, or when a `src/` path a document names no longer exists.

Each document carries a **Where this lives** section naming the source that implements it. Those anchors are routing, not API documentation: they name files and stable symbols, never signatures or enumerations, which remain owned by the source.

## Source directory map

Start from the concrete failing behavior, symbol, test, or file, then route to the owning prefix in `src/`. Do not scan unrelated modules to build general context. Every file states its own scope in a `// Purpose:` comment on its first lines; read that before reading the file.

- `app*`: application coordination; `view*`/`ui*`: views, controls, layout, drawing.
- `model*`: state, index, search, SQLite, properties, face detection; `files*`/`metadata*`: I/O, codecs, EXIF/IPTC/XMP/ICC; `media_edit.cpp`: a photo's edits applied to its pixels.
- `av*`: audio/video; `render*`: software rasterisers for the sidebar's charts and globe and for panoramas. The draw backends and surfaces are libdesktop's render module.
- `platform*`: the app's own Windows and Linux glue - its side of FFmpeg's hardware decoding, its resource script and its Linux hooks; `crypto*`: hashes and checksums, including the perceptual image hash; `util*`: the app's own helpers; `test*`: tests.

Diffractor's `df::` and `str::` are its own types. Three sibling libraries supply the rest, each pinned in its own `cmake/` module:

- **libdesktop** ([DiffractorLibdesktop.cmake](cmake/DiffractorLibdesktop.cmake)): the `core::` vocabulary in `libdesktop/core.h`; OS services and the application loop in `platform::`/`libdesktop/platform.h`; drawing backends and counters in `render::`/`libdesktop/render.h`; frames, controls, menus and text views in `ui::`/`libdesktop/ui.h`, with headless fakes in `libdesktop/ui/test_support.h` and syntax highlighters in `libdesktop/ui/syntax.h`.
- **libfiles** ([DiffractorLibfiles.cmake](cmake/DiffractorLibfiles.cmake)): scanning, decoding and encoding stills, scanning audio and video containers, and reading and writing EXIF, IPTC and XMP. `files.cpp` and `metadata.cpp` are Diffractor's side of it.
- **lib64** ([DiffractorLib64.cmake](cmake/DiffractorLib64.cmake)): the Commodore 64 emulator. It plays SID tunes, through `av_sid.cpp`, and runs a disk's, tape's, cartridge's or program file's program in the media pane, through `av_c64.cpp`.

A sibling checkout at `..\libdesktop`, `..\libfiles` or `..\lib64` is built instead of the pin whenever it exists, so a local edit there reaches this build at once while CI builds the pin. A change to one of them is made in that repository, under its own AGENTS.md.

Editing a file under `src/` also loads the matching rules in [.github/instructions](.github/instructions), which carry the threading, rendering, view, test, and platform detail for that area at the point it applies; editing a document loads the documentation rules.

Four recurring workflows are `.github/prompts/` slash commands rather than procedures to reconstruct: `/change-behavior`, `/add-test`, `/triage-crash`, and `/upgrade-dependency`.

## Working rules

- Build and validate with `.\dd.ps1 test`: it runs `tools/lint_repo.ps1` and the release-tool checks, rebuilds `exe\diffractor64.exe` (Release) when a source file under `src/` is newer than it, then runs the unit tests and the translation checks. It does not look at the sibling libraries, so after editing one, build first with `python tools\dd.py build --config Release`.
- Build with `tools/dd.py`: `python tools\dd.py build` for Debug (`exe\diffractor64-d.exe`), `--config Release` for Release. `.\dd.ps1 build` also bumps the build number in `src/app.cpp`, `installer/diff.nsi` and `exe/AppxManifest.xml`; use it only to cut a build. CMake is the only description of the build on both platforms. App source belongs in `src/`; temporary output belongs in `tmp/`.
- Style: C and C++ follow the repository's `.clang-format` (tabs, Allman braces, 120 columns); `.\dd.ps1` has no format command, so run the `clang-format` Visual Studio installs in `VC\Tools\Llvm\x64\bin`. `.clang-format-ignore` lists the generated files it skips, and hand-aligned tables sit between `// clang-format off` and `// clang-format on`. Diffractor's own code builds without a warning at `/W4` and at GCC's `-Wall`, in Debug and Release - fix a warning rather than suppress it - while vendored targets build at `/W0` (`-w`). `.clang-tidy` holds the static-analysis checks; where a finding is wrong, a `// NOLINTNEXTLINE(check)` says why.
- System, OS, Windows API, and other platform-specific implementation code MUST exist only in `platform*.*` files or in libdesktop; the one exception is the entry points, `wWinMain` and `main`, in `app.cpp`. Other modules use platform abstractions.
- Keep `// Purpose:` comments accurate in modified `src/*.h` and `src/*.cpp` files.
- Preserve [product design](docs/design.md), especially targeting, recovery, navigation restoration, and preview-before-run.

## Behavior changes

A change is a behavior change when anything a user can observe differs: what a view shows, a command's target, availability or effect, recovery, timing, or the pixels. A rendering or performance change is one whenever it does that, whichever layer it is made in. Before editing one, read the owning document from the routing table and keep [design](docs/design.md)'s rules for targets, preview-before-run, collisions and recovery; update the owning document in the same change. Where design.md does not decide a behavior, ask the owner rather than inventing a state, an alias or a target. `/change-behavior` walks through it.

## Iteration and loop protocol

1. After each substantive edit, run the narrowest relevant check before editing again: the filter from the routing table, otherwise a focused compile/lint. `pwsh -File tools/lint_repo.ps1` is the cheapest check in the repository and catches boundary violations a passing test cannot see.
2. Cap one failing check/hypothesis at three correction attempts. After the third failure, HALT and report the hypothesis, attempts, exact failure, and request guidance; cosmetic changes do not reset the count.
3. If an edit breaks unrelated modules, remove only the agent's causal changes before changing approach. Preserve user work; never use blanket `git checkout`, `git reset`, or file replacement.
4. Once the owner, falsifiable hypothesis, and discriminating check are known, edit and validate; stop broad exploration.

## Strict code anti-patterns

Rules marked **[lint]** are enforced by `tools/lint_repo.ps1` and fail `.\dd.ps1 test`; the rest are review rules.

- **Threads: [lint]** no application `std::thread`; use `async_strategy` or an existing queue. Direct threads are limited to platform queue/thread implementation and focused concurrency tests.
- **Thread ownership: [lint]** a worker's result is published whole - clone it, finish every field, then replace the pointer; never `const_pointer_cast` to write through a shared one.
- **Locks:** no I/O, SQLite, decoding, callbacks, sorting, or large/unbounded allocations under an index lock.
- **UI thread:** no filesystem, database, decoding, indexing, hashing, querying, thumbnail, map, or network work.
- **Invalidation:** Paint functions never access SQLite or scan files, directly or indirectly; request Source work.
- **Platform: [lint]** Windows headers and handle types appear only in `platform*` files and `test_platform_win.cpp`, apart from the entry points in `app.cpp`. The lint catches the common ones, not every one.
- **Database: [lint]** `sqlite3_*` appears only in `model_db*`, `model_tile_cache*` and tests.
- **Dependencies:** third-party archives are fetched at the pins in `cmake/dd-dependencies.json`, and libdesktop, libfiles and lib64 at the revisions in their `cmake/Diffractor*.cmake` modules; none is patched. Change behavior in owned wrappers or build configuration, and a version by moving its pin.
- **Cache keys:** a key carries every input the cached value depends on; one that omits an input silently serves one caller's value to another.
- **Argument order: [lint]** one call must not both capture a variable by copy, for a lambda, and `std::move` that same variable into another argument. Argument evaluation order is unspecified - MSVC goes right to left - so the lambda can capture an emptied value, and the fault surfaces only when the callback runs. Give the lambda its own statement. Moving inside the lambda's body, or an init-capture such as `[f = std::move(f)]`, is correct and not reported.
- **Absent handles: [lint]** a `ui::frame_ptr` member that can be unset is reached only through its `frame()` accessor, which answers `ui::no_frame()` when there is no window - `view_host::frame()` and every `ui::frame_host` do - and callers do not test it; a raw `_frame->` on such a member is the defect. A dialog's own frame is never unset and is used directly; `ui_dialog.h` holds both kinds under the name `_frame`, so it is reviewed rather than linted. A member guarded at some call sites and dereferenced at others is already this bug.
- **One definition: [lint]** a class is defined in one source file. Two same-named classes outside anonymous namespaces are one class to the linker, which keeps one vtable for both, so objects of one silently run the other's code. A class used only in its own file goes in an anonymous namespace.
- **Existence is not visibility:** never bind a handle's lifetime to whether the thing is on screen. A hidden panel still populates, counts, ticks and invalidates, so attach it unconditionally and gate only the repaint.
- **Animation:** alpha fades are disabled in CPU software rendering mode (`render::animations_enabled`, which the renderer sets); never bypass the gate or animate alpha outside `ui::animate_alpha`.
- **Exceptions:** every `catch` propagates, logs, returns a bounded error, or implements a documented bounded fallback; no unexplained swallowing.

## Post-flight checklist

Before completion, verify every applicable item:

- [ ] `pwsh -File tools/lint_repo.ps1` passes.
- [ ] Narrowest relevant tests pass; run `.\dd.ps1 test` for shared behavior or release completion.
- [ ] Modified source retains accurate `// Purpose:` comments.
- [ ] A behavior change is described in its owning document, and a modified document's **Where this lives** anchors still name files that exist and implement what the section describes.
- [ ] The diff is task-only and preserves user work.
- [ ] The final response lists checks, outcomes, and unavailable/skipped checks accurately.