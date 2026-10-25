# Diffractor Codebase

Diffractor is a C++20 Windows media organizer with a custom Direct2D/Direct3D UI, SQLite index, and local file/metadata processing.

## Primary design drivers

1. **Fast, lightweight performance:** start and respond quickly, scale to large collections, bound resource use, and keep expensive work off the UI thread.
2. **A clear user mental model:** make scope, contents, target, effect, and recovery predictable; avoid hidden state and context-dependent surprises.

Both are product requirements. Make tradeoffs explicit and validate behavior and performance where affected.

## Start here: route the task before reading anything

Find the row matching the concrete behavior, symbol, or file. Read that document's named section,
open the listed source, and run the listed check. Do not read unrelated documents for context.

| If the task is about | Read | Code | Narrowest check |
|---|---|---|---|
| Query parsing, results, address box, autocomplete | [design](docs/design.md#navigation-and-search) | [model_search.cpp](src/model_search.cpp), [model_tokenizer.h](src/model_tokenizer.h), [app_search.h](src/app_search.h), [model_index_summary.cpp](src/model_index_summary.cpp) | `/test:*search*` |
| Indexing, the database, thumbnails, hydration | [implementation](docs/implementation.md#index-search-and-database) | [model_index.cpp](src/model_index.cpp), [model_db.cpp](src/model_db.cpp), [model_postings.h](src/model_postings.h) | `/test:*index*`, `/test:*thumbnail*` |
| Collection membership, duplicates, presence | [collections](docs/collections.md) | [model_index.cpp](src/model_index.cpp), [model_index_duplicates.cpp](src/model_index_duplicates.cpp), [app_settings.cpp](src/app_settings.cpp), [model_related.h](src/model_related.h) | `/test:*duplicate*`, `/test:*presence*` |
| Zoom, pan, the navigator, panorama projection | [zoom](docs/zoom.md) | [model_zoom.h](src/model_zoom.h), [view_media.h](src/view_media.h), [ui_panorama.h](src/ui_panorama.h) | `/test:*zoom*`, `/test:*panorama*` |
| Places, distance, map, visits, tiles | [locations](docs/locations.md) | [model_locations.cpp](src/model_locations.cpp), [model_visits.cpp](src/model_visits.cpp), [ui_map_common.h](src/ui_map_common.h), [ui_globe.h](src/ui_globe.h), [model_tile_cache.cpp](src/model_tile_cache.cpp) | `/test:*location*`, `/test:*visit*`, `/test:*globe*` |
| Reading or writing a media file, staging, sidecars, rollback | [file I/O](docs/file-io.md) | [files_core.cpp](src/files_core.cpp), [app_util.cpp](src/app_util.cpp) | `/test:*sidecar*`, `/test:*collision*` |
| Which tag a property is read from or written to | [metadata](docs/metadata.md) | [metadata_exif.cpp](src/metadata_exif.cpp), [metadata_xmp.cpp](src/metadata_xmp.cpp), [metadata_iptc.cpp](src/metadata_iptc.cpp), [model_property.cpp](src/model_property.cpp) | `/test:*metadata*`, `/test:*tag*` |
| Drawing, backends, device loss, colour, the video pipeline | [rendering](docs/rendering.md) | libdesktop's [platform_win_d3d11.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/render/platform_win_d3d11.cpp), [platform_win_software.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/render/platform_win_software.cpp), libdesktop's [render_surface.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/render/render_surface.cpp), [av_format.cpp](src/av_format.cpp) | `/test:*surface*`, `/test:*colour*`, `/test:*video*`, `/test:*audio*` |
| The selection panel: form, content, order, density | [selection controls](docs/selection-controls.md) | [view_items.cpp](src/view_items.cpp), [ui_elements.h](src/ui_elements.h), [ui_flex.cpp](src/ui_flex.cpp) | `/test:*selection*`, `/test:*layout*` |
| Tile geometry, scrolling, list layout | [design](docs/design.md#application-structure) | [view_items.h](src/view_items.h), [view_list.h](src/view_list.h), [ui_flex.cpp](src/ui_flex.cpp) | `/test:*layout*`, `/test:*scroll*` |
| Command availability, targeting, keyboard | [design](docs/design.md#command-availability) | [app_commands.cpp](src/app_commands.cpp), [app_commands.h](src/app_commands.h) | `/test:*command*`, `/test:*selection*` |
| A guided operation: Rename, Import, Sync, Convert, Metadata, Date | [design](docs/design.md#guided-operations) | [view_rename.cpp](src/view_rename.cpp), [view_import.cpp](src/view_import.cpp), [view_sync.cpp](src/view_sync.cpp), [view_batch.cpp](src/view_batch.cpp) | `/test:*rename*`, `/test:*import*`, `/test:*sync*` |
| A crash report, dump, or symbolization | [crash investigation](docs/crash.md) | [app_toolbar.cpp](src/app_toolbar.cpp), [util_crash_files_db.h](src/util_crash_files_db.h) | see [crash](docs/crash.md) |
| Where a test belongs, or why the suite is arranged this way | [testing](docs/testing.md) | `src/test_*.cpp` | `.\dd.ps1 test` |
| Translated text, catalogs, plural forms | [testing](docs/testing.md) | [app_text.cpp](src/app_text.cpp), `exe/languages` | `.\dd.ps1 test` |
| Upgrading or patching a vendored dependency | [third-party](docs/third-party.md) | `third-party/`, `cmake/vendored` | `.\dd.ps1 test` |
| Portability, platform-boundary debt, the Linux build | [Linux port](docs/linux.md) | libdesktop's [platform.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform.h), [platform_render.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform_render.h), libdesktop's [platform_linux_ui.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/render/platform_linux_ui.cpp), [platform_linux_app.cpp](src/platform_linux_app.cpp) | `.github/workflows/linux.yml` |

`/test:` filters match the test **name**, case-insensitively, not the file. Every filter above is verified to match at least one test; a filter matching nothing is a failure, not a silent pass. Run them as `.\exe\diffractor64-d.exe /test:*zoom* | Out-Host`.

## Information ownership

GitHub issues and releases own work, status, discussion, follow-up, and release history. Source owns exact APIs, enums, and file lists. Every other subject has exactly one owning document:

| Document | Owns |
|---|---|
| [design](docs/design.md) | Durable user concepts and behavior; the scope/contents/target/effect ontology; view, mode, and presentation naming |
| [implementation](docs/implementation.md) | Architecture, data flow, threading, ownership, invalidation, index/search internals |
| [crash investigation](docs/crash.md) | Crash report contents, symbol store and debugger setup, dump symbolization, attributing a fault to a cause |
| [testing](docs/testing.md) | The test taxonomy: which subject file owns which test, what the runner enforces, fixtures, and headless test boundaries |
| [collections](docs/collections.md) | The collection: membership, what it earns, and its edge |
| [locations](docs/locations.md) | Places, location search, distance, map-driven queries, visits |
| [metadata](docs/metadata.md) | Property-to-tag mapping across XMP, EXIF, IPTC, and container tags |
| [file I/O](docs/file-io.md) | Read and write paths: decode ladder, staging, patching, sidecars, backups, rollback, failure contract |
| [rendering](docs/rendering.md) | Backends and parity, device/swap-chain, frame and resize lifecycle, batching, text, video pipeline |
| [zoom](docs/zoom.md) | The zoom model, laws, rendering tiers, input, and navigator |
| [selection controls](docs/selection-controls.md) | The selection panel: form classification, content, ordering, density, responsive behavior |
| [movie](docs/movie.md) | Assembling photos and videos into one video: the timeline document, trims and transitions, the project file, and video encoding |
| [third-party](docs/third-party.md) | Vendored dependencies, the outbound licence they constrain, upgrade procedure, integration patches |
| [Linux port](docs/linux.md) | Current Linux support, the platform boundary, unsupported surfaces, and build behavior |
| [README](README.md) | Product overview, build prerequisites, command line, and the licence Diffractor is released under |
Move information to its owner; link rather than duplicate volatile detail. `tools/lint_repo.ps1` fails when a document has no owner row, when a link does not resolve, or when a `src/` path a document names no longer exists.

Each document carries a **Where this lives** section naming the source that implements it. Those anchors are routing, not API documentation: they name files and stable symbols, never signatures or enumerations, which remain owned by the source.

## Source directory map

Start from the concrete failing behavior, symbol, test, or file, then route to the owning prefix in `src/`. Do not scan unrelated modules to build general context. Every file states its own scope in a `// Purpose:` comment on its first lines; read that before reading the file.

- `app*`: application coordination; `view*`/`ui*`: views, controls, layout, drawing.
- `model*`: state, index, search, SQLite, properties; `files*`/`metadata*`: I/O, codecs, EXIF/IPTC/XMP/ICC.
- `av*`: audio/video; `render*`: surfaces/color/transforms; `platform_win_d3d11.cpp`/`platform_win_software.cpp`: draw backends.
- `platform*`: the app's own Windows and Linux glue - its side of FFmpeg's hardware decoding, its resource script, its Linux entry point; `util*`: the app's own helpers; `test*`: tests. The foundation (`df::`, `str::`) and the OS services (`platform.h`) are libdesktop's system module, and the desktop UI - `ui.h`, `platform_render.h`, the window layer and the renderers - its render module, all included as `libdesktop/<file>.h` - see [cmake/DiffractorLibdesktop.cmake](cmake/DiffractorLibdesktop.cmake).

Editing a file under `src/` also loads the matching rules in `.github/instructions/`, which carry the threading, rendering, view, test, and platform detail for that area at the point it applies.

Four recurring workflows are `.github/prompts/` slash commands rather than procedures to reconstruct: `/change-behavior`, `/add-test`, `/triage-crash`, and `/upgrade-dependency`.

## Working rules

- Build and validate with `.\dd.ps1 test`, which runs `tools/lint_repo.ps1`, the unit tests, and the translation checks. CMake is the only description of the build on both platforms; `tools/dd.py` drives it. App source belongs in `src/`; temporary output belongs in `tmp/`.
- System, OS, Windows API, and other platform-specific implementation code MUST exist only in `platform*.*` files or in libdesktop. Other modules use platform abstractions.
- Keep `// Purpose:` comments accurate in modified `src/*.h` and `src/*.cpp` files.
- Preserve [product design](docs/design.md), especially targeting, recovery, navigation restoration, and preview-before-run.

## Mandatory pre-flight validation

Before generating or modifying C++ code, an AI agent MUST declare a change class and output exactly the fields that class requires. Placeholders are invalid. Do not output both schemas.

**Change class `User-visible behavior`** — the change alters what a user can observe: scope, contents, target, effect, presentation, or recovery. Populate from the closed [design ontology](docs/design.md#user-mental-model).

```json
{
	"Change class": "User-visible behavior",
	"Scope": "Indexed collection | Folder | Recursive folder | External folder | Search | All scopes",
	"Contents": "What visible or derived contents the change reads or changes",
	"Target": "Focused item | Singular displayed item | Complete visibly selected set | Visible items",
	"Effect": "The exact behavior, affected count or class, destinations/collisions when applicable, retained originals, and recovery"
}
```

`All scopes` is earned, not default: use it only when the behavior is provably identical in every concrete scope, and say why in the `Contents` field. If the change belongs to one concrete scope, name that scope.

**Change class `Internal`** — the change alters no observable behavior: rendering backends, threading and ownership, indexing internals, codecs, build configuration, tests, or refactors. Naming a user scope or target here is a gate violation.

```json
{
	"Change class": "Internal",
	"Component": "The owning src/ prefix and the specific files or subsystem",
	"Invariant": "The correctness, ownership, or performance property the change must preserve or restore",
	"Effect": "The exact implementation change and its blast radius",
	"Observable behavior": "Unchanged, plus the check that proves it"
}
```

If an `Internal` change turns out to shift observable behavior, stop and restate it as `User-visible behavior` before continuing.

The most common misclassification is calling a rendering or performance change `Internal` when it alters what the user sees. If the pixels, the timing, or a command's availability differ, it is `User-visible behavior` regardless of which layer changed.

Before editing, explicitly confirm both [implementation boundaries](docs/implementation.md):

1. UI-thread execution remains disjoint from I/O, decoding, database access, and network use.
2. Paint invalidation remains disjoint from Source work; no Paint-layer function contains or calls SQLite access or file scanning.

This is a gate. If a `User-visible behavior` change cannot map unambiguously to the schema and enums, HALT and ask the user; never invent, alias, or infer a state, and never reclassify it as `Internal` to avoid the question.

## Iteration and loop protocol

1. After each substantive edit, run the narrowest relevant check before editing again: the filter from the routing table, otherwise a focused compile/lint. `pwsh -File tools/lint_repo.ps1` is the cheapest check in the repository and catches boundary violations a passing test cannot see.
2. Cap one failing check/hypothesis at three correction attempts. After the third failure, HALT and report the hypothesis, attempts, exact failure, and request guidance; cosmetic changes do not reset the count.
3. If an edit breaks unrelated modules, remove only the agent's causal changes before changing approach. Preserve user work; never use blanket `git checkout`, `git reset`, or file replacement.
4. Once the owner, falsifiable hypothesis, and discriminating check are known, edit and validate; stop broad exploration.

## Strict code anti-patterns

Rules marked **[lint]** are enforced by `tools/lint_repo.ps1` and fail `.\dd.ps1 test`. The rest need judgement and are review rules; a lint that guessed at them would produce false positives and be turned off.

- **Threads: [lint]** no application `std::thread`; use `async_strategy` or an existing queue. Direct threads are limited to platform queue/thread implementation and focused concurrency tests.
- **Locks:** no I/O, SQLite, decoding, callbacks, sorting, or large/unbounded allocations under an index lock.
- **UI thread:** no filesystem, database, decoding, indexing, hashing, querying, thumbnail, map, or network work.
- **Invalidation:** Paint functions never access SQLite or scan files, directly or indirectly; request Source work.
- **Platform: [lint]** system headers and Windows handle types appear only in `platform*` files.
- **Database: [lint]** `sqlite3_*` appears only in `model_db*` and `model_tile_cache*`.
- **Dependencies:** fetched at the pins in `cmake/dd-dependencies.json` and never patched; change
  behavior in owned wrappers or build configuration, and a version by moving its pin.
- **Cache keys:** a key must carry every input the cached value depends on. Invalidation is only as good as the identity it is keyed on, and a key that omits an input silently serves one caller's value to another.
- **Argument order: [lint]** one call must not both capture a variable by copy, for a lambda, and `std::move` that same variable into another argument. Function arguments are indeterminately sequenced and MSVC evaluates them right to left, so the move empties the variable before the capture copies it and the lambda is left holding an emptied value. Give the lambda its own statement, so the capture is sequenced before the move. The fault surfaces wherever the callback is finally invoked, not at the call that built it, which is why this is linted rather than left to review. Moving the captured copy inside the lambda's own body, and an init-capture such as `[f = std::move(f)]`, are the correct forms and are not reported.
- **Absent handles: [lint]** a `ui::frame_ptr` member that can be unset is reached only through an accessor that never returns null, and callers do not test it. `view_host::frame()`, and the `frame()` accessor on any `ui::frame_host`, answer `ui::no_frame()` when there is no window; a raw `_frame->` on such a member outside that accessor is the defect. `ui::control_frame_ptr` has no stand-in, so those members are instead either guarded at every use or safe by construction — text alone cannot tell the two apart in a file holding both, so `ui_dialog.h` is reviewed rather than linted. Either way, a member guarded at some call sites and dereferenced at others is this bug already, whether or not it has crashed yet.
- **Existence is not visibility:** never bind a handle's lifetime to whether the thing is on screen. A hidden panel still populates, counts, ticks and invalidates, so attach it unconditionally and gate only the repaint.
- **Animation:** alpha fades are disabled in CPU software rendering mode (`ui::animations_enabled`, which the renderer sets); never bypass the gate or animate alpha outside `ui::animate_alpha`.
- **Exceptions:** every `catch` propagates, logs, returns a bounded error, or implements a documented bounded fallback; no unexplained swallowing.

## Detailed rules

Path-scoped rules live in [.github/instructions](.github/instructions): threading, views,
rendering, platform boundaries, tests, and documentation. [Implementation](docs/implementation.md)
owns architecture and synchronized types; source owns exact APIs and enums.

## Post-flight checklist

Before completion, verify every applicable item:

- [ ] `pwsh -File tools/lint_repo.ps1` passes.
- [ ] Narrowest relevant tests pass; run `.\dd.ps1 test` for shared behavior or release completion.
- [ ] Modified source retains accurate `// Purpose:` comments.
- [ ] A modified document's **Where this lives** anchors still name files that exist and implement what the section describes.
- [ ] The diff is task-only and preserves user work.
- [ ] The final response lists checks, outcomes, and unavailable/skipped checks accurately.