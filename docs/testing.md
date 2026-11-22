# Diffractor Testing

This document owns the test taxonomy: which subject file owns which test, what the runner enforces,
fixtures, and what is tested without a window. Build commands are the [README](../README.md#building)'s.

## The suite

The tests are compiled into the application and run from its command line; there is no separate test
binary or framework.

```powershell
& .\exe\diffractor64-d.exe /test 2>&1 | Out-Host             # x64 Debug, everything
& .\exe\diffractor64-d.exe /test:*search* 2>&1 | Out-Host    # one subject
& .\exe\diffractor64-d.exe "/test:Should rename file case" 2>&1 | Out-Host
```

`/test:` takes a wildcard matched case-insensitively against the test name. Piping makes PowerShell wait
for this GUI-subsystem executable; read `$LASTEXITCODE` straight after. The console lists every case and
the pass, fail and total counts. On Linux a leading `/` starts a path, so the switch is `--test`.

`.\dd.ps1 test` is the complete gate, failing if any part fails: `tools/lint_repo.ps1`;
[test_release_tools.ps1](../tools/test_release_tools.ps1) (SymStore discovery and publishing failures,
against temporary fixtures, never a real store); the `--label` checks
([test_headless_cli.ps1](../tools/test_headless_cli.ps1), [test_dd_headless.py](../tools/test_dd_headless.py));
a Release build when `exe\diffractor64.exe` is missing or older than `src/`; `/test`; `/validate-po`;
[test_check_translations.py](../tools/test_check_translations.py) and `check_translations.py`; and the
agent session [test_mcp_session.py](../tools/test_mcp_session.py) drives
([agents](agents.md#9-testing-and-diagnostics)), which runs alone given the program's path. `.\dd.ps1 bean`
runs it with the temporary files on the developer's network share.

`--label headless`, the workspace-wide name for a run with no visible UI, selects that same gate, as does
any regex matching the label case-sensitively; an invalid regex, one matching nothing, or any other option
fails before a build. On Linux `./dd test` runs the embedded suite alone and accepts the same label.

The decoder robustness sweeps and the closest-location lookup register only under `#ifndef _DEBUG`, so a
Windows Debug run leaves them out; [Linux](linux.md#current-support) runs them in both configurations.

`check_translations.py` also checks that `app_text` registers libdesktop's shared messages, read from the
sibling `..\libdesktop` or the one copy fetched under `tmp`, or else from the `services.h` passed with
`--platform-text-header`; its tests prove that omitted, duplicated, unknown and wrongly typed shared
messages fail. Outside the gate, `python tools\test_generate_icon.py` holds the logo to `tools/logo.html`,
desktop and Store artwork to the same vectors, tile margins, and placement that keeps alpha; the
[README](../README.md#icon-artwork) regenerates and `--check`s the artwork.

## Taxonomy

One file per subject, matching the `src/` prefixes in [implementation](implementation.md#source-organization). A test lives with the code it constrains, never with the reason it was written.

| File | Registration | Owns |
|---|---|---|
| [test_util.cpp](../src/test_util.cpp) | `register_util_tests` | `util*`, `crypto*`: strings, wildcards, versions, natural compare, interning, path identity and paths moved with their folders, cancellation tokens, result scopes, hashes and perceptual hashes, base64, JSON, kd-trees, top-N ranking, task queues, memory-mapped files |
| [test_text.cpp](../src/test_text.cpp) | `register_text_tests` | `app_text`, `util_spell`: catalogs, plural forms, translated month names, spell checking |
| [test_render.cpp](../src/test_render.cpp) | `register_render_tests` | `render*`, `util_simd`: software blends, YUV conversion, area downscaling, colour adjustment, surface transforms, alpha animation, decode budgets, the globe and panorama rasterisers and the map tiles the globe drapes |
| [test_files.cpp](../src/test_files.cpp) | `register_files_tests` | `files*`: codecs, format detection, container parsing, folder art and `.nfo` files, the write path (staging, replacement, originals, collisions, rollback), decoder robustness |
| [test_metadata.cpp](../src/test_metadata.cpp) | `register_metadata_tests` | `metadata*`, `model_property`, `model_dates`, `model_tags`: EXIF, IPTC, XMP, ICC, per-format metadata scanning, the date pack and its authority table, the panorama declaration, sidecars, tag sets, property presentation, face names written into their regions |
| [test_media_edit.cpp](../src/test_media_edit.cpp) | `register_media_edit_tests` | The metadata write path and the image editing model: in-place and staged edits, Windows shell tags, orientation, crop, perspective, temperature, a wide-gamut photo's colours, and the metadata a lossless video or sound edit keeps |
| [test_faces.cpp](../src/test_faces.cpp) | `register_faces_tests` | `model_faces*`, `model_db_faces`, `view_faces*`: face regions files name; pictures handed to libmug in every orientation, packed or planar, reduced, with nobody in them, or in formats libmug cannot read (libmug's own suite holds its networks); a video's faces read from its key frames; face hashes, records and the store; the scan and its share of the disk; face terms; the face book, its database and CSV - a later release's database, two windows, a failed write, marks following their pictures; suggestions and the overview; the Faces view's pages. See [faces](faces.md) |
| [test_av.cpp](../src/test_av.cpp) | `register_av_tests` | `av*`: probing, seeking, read-ahead bounds, hover preview, audio buffers, the visualizer, SID tunes and where they end, tracker modules, the Commodore 64 the media pane runs and its keyboard, subtitles, session lifetime, a picture's matrix, chroma siting and fields, Edit's lossless copy, a video's key frames read and decoded alone for face search, and an MP4's index moved before its media |
| [test_index.cpp](../src/test_index.cpp) | `register_index_tests` | `model_index`, `model_db`, `model_postings`, `model_search_index`: indexing, the inverted and trigram indexes, the search index and its agreement with a scan, the database, the thumbnail pipeline, duplicates and presence, stale-result discarding, hydration; face records restamped across a metadata write and none written once face search was turned off since, regrouping only on change, an import's face suggestions, the upgrade to people named in pictures, and face-book names found through the search index |
| [test_search.cpp](../src/test_search.cpp) | `register_search_tests` | `model_search`, `model_related`, `app_match`: query parsing, term matching, person terms, scopes, negation, exclusion, related items, prediction |
| [test_locations.cpp](../src/test_locations.cpp) | `register_location_tests` | `model_locations`, `model_visits`, `model_explore`, `ui_map*`, `ui_globe`, `model_tile_cache`: the gazetteer, place naming, map areas and geometry, the globe's projection, framing and zoom, visits, the timeline, Explore's counts, trips and clusters, the map tile cache. See [locations](locations.md) |
| [test_movie.cpp](../src/test_movie.cpp) | `register_movie_tests` | `model_movie`: the Movie document - output geometry and YouTube Shorts, crossfade and duration arithmetic, when captions show and how loud each sound plays, timeline edits and undo, entering Movie with a timeline, the `.dfv` project file. See [movie](movie.md) |
| [test_view.cpp](../src/test_view.cpp) | `register_view_tests` | `view*`, `ui*`, `model_zoom`: the zoom model, the panorama camera and its field-of-view ladder, the item selector, the scroller, flex layout, tile geometry, detail rows, the task list's run highlight and scrolling keys, Explore's timeline, keyboard reach and hit testing among drawn elements, the media stack, the SID and tracker monitors and Movie's audio-clip frames, the Commodore 64 listing, cartridge and screen, a picture's people and the face actions on it, the Faces toolbar, text editing. Its scroller, flex and hit-test tests repeat libdesktop's `ui` suite through the application's element tree |
| [test_app.cpp](../src/test_app.cpp) | `register_app_tests` | `app*`: selection and command enablement, Tab order and the keys a focused control keeps, settings and the Options view, rename, import, sync and convert planning, external tools, history, the safe start and crash records, an agent session's options and its tools' arguments; the Faces commands, status text and sidebar button, face marks an import moves, and face-book names predicted in the address bar |
| [test_platform_win.cpp](../src/test_platform_win.cpp) | `register_platform_tests` | `platform_win*`: extended paths, DXGI device loss, adapter classification and texture budgets, the decode device's adapter, texture recycling, the GPU recovery session, the font stack, the registry store, the shell data object, the window chrome. The only test file that may include system headers |

[test.h](../src/test.h) holds the assertion helpers, [test_fixtures.h](../src/test_fixtures.h) and
[test_fixtures.cpp](../src/test_fixtures.cpp) the shared fixtures, and [test_runner.cpp](../src/test_runner.cpp)
the console runner; none contains a test.

### Placing a test

Ask what the test constrains, not what prompted it.

- A regression goes in the file that owns the behavior, with the issue as a comment above its registration. There is no regressions file.
- A test that needs two subjects belongs to the one whose contract it would falsify: a metadata value read through the AV probe is a metadata test; the probe's byte budget is an AV test.
- A helper used in one file stays `static` there; one used from two files moves to `test_fixtures.h` as `inline`.

## What the runner enforces

- **A test that asserts nothing fails.** Assertions are counted per test, so a test disabled by an early return or an `#if` cannot report PASS forever.
- **A duplicate test name fails**, compared case-insensitively: two registrations under one name make `/test:<name>` ambiguous and a failure unattributable.
- **A filter matching no test fails**, so a filter that misses cannot pass in silence.
- **A temporary file that cannot be deleted at the end fails the run.**
- **Animations a test leaves are cleared after it.** Nothing steps them without a window, so one left running would carry what it animates into every later test.
- Failures name the source file and line of the failing assertion.

## What a test must not do

- **Persist user state.** A test that appends to a real dictionary, settings key or collection passes once, then fails, and edits a file the user owns. Write to `test_temps` instead.
- **Depend on another test.** One `shared_test_context` carries the loaded index, and one gazetteer is loaded per run, because loading either per test is not affordable; nothing else carries between tests.
- **Depend on ordering, wall-clock timing, or the machine's fonts and DPI.** Layout tests measure through stub measure contexts for this reason. Ordering includes the order a folder lists in: NTFS lists names sorted and ext4 does not, and a Linux build of a checkout under WSL's `/mnt/c` reaches `exe/` through links that list as NTFS does, so only CI shows the dependency; the record tests sort what they collect (`scan_parity::add_folder` in [test_files.cpp](../src/test_files.cpp)). The processor matters where a codec picks its kernels at run time: dav1d decodes a damaged stream differently with each set, so the HEIF sweep pins its SSE4.1 kernels.
- **Add files to `exe/test`.** That folder is indexed recursively and its file count is asserted by `expected_cached_item_count` and every search-count test. Fixtures loaded by path go in `exe/test/excluded1`, which indexing skips.

## Proving a test is worth having

A test must be able to fail for the defect it claims to cover. Break the targeted behavior, confirm the
test fails, then restore it. Record a non-obvious discriminating case beside the test.

## Fixtures

`exe/test` is the indexed collection every index and search test resolves against. Its `excluded1`
(excluded by path) and `excluded2` (by the wildcard `exclud*2`) hold fixtures loaded by path. Beside it,
outside the count, `exe/test-formats` is the format corpus the record tests scan and `exe/test-generated`
keeps files an encoder wrote once, since it writes other bytes on other platforms. Temporary files come from
`test_temps`, deleted at the end of the run; `/test-temp:<path>` moves them, to exercise the write path on a
network share.

The face detection tests' photographs of people are libfacedetection's own samples, read from the archive
libmug fetched (`DIFFRACTOR_FACEDETECTION_SOURCE_DIR`), so the repository keeps none; the agent session's
faces sessions find the same samples in a build tree under `tmp` and are skipped where none has them.
Pictures with nobody in them are ordinary fixtures.

Passing a tracker module's path with `/test:*requested*tracker*fixture*` adds a read-only playback check of
that file: loading, seeking, PCM, pattern snapshots and media information. The suite itself uses generated
tracker and SID fixtures.

## Testing behavior without a window

`view_state` runs headless: construct it with `null_state_strategy`, `null_async_strategy`, an `index_state`
and a player (`make_test_player`, or none), open a search over the fixture collection with a **null**
`view_host_base_ptr`, and drive the entry points the commands do.
`browsing_fixture` in [test_fixtures.h](../src/test_fixtures.h) does this; selection, filtering, grouping,
sibling navigation and the browsing sequence are covered through it.

Where a decision is trapped inside a method that also performs it, extract the decision rather than building
a window. These are pure functions for that reason:

| Function | Owns |
|---|---|
| `should_resume_at` / `position_to_save` ([av_player.h](../src/av_player.h)) | the resume window and what a close saves |
| `calc_playback_advance` ([util_interfaces.h](../src/util_interfaces.h)) | slideshow, repeat and continue-with-next, and how they interact |
| `search_edit_session` ([app_search.h](../src/app_search.h)) | the address box editing session: draft, preview, two-stage Escape, Tab, Enter |
| `make_search_auto_complete` ([app_search.h](../src/app_search.h)) | the address bar's completion results: content, counts, ranking, de-duplication, the cap, latest-wins |
| `default_keyboard_accelerators` ([app_commands.h](../src/app_commands.h)) | the shipped key bindings, so a collision can be seen |

A test that drives the completion strategy runs the pass on a real worker thread, as the shipped code does:
`index_state::auto_complete_words` asserts it is not on the UI thread. `run_completion_worker` in
[test_app.cpp](../src/test_app.cpp) models the hop with a `deferred_async_strategy`, which also lets the
latest-wins test publish two passes out of order; running the pass inline trips the assertion.

## The sweep

The unit suite and the session test check what is written down; [sweep_mcp.py](../tools/sweep_mcp.py) presses
every button. It starts a session out of sight, allowed to write, on a collection and profile it makes and
throws away. In each state it reaches - Items with nothing, one photo and everything selected; a photo, a tune
and a document full screen; Explore; Faces; each task - it runs each command, presses each row of each
command's menu and each control, link and part `ui_controls` lists, and puts the window back between presses,
checking each time that the process lives, the window settles and still draws, the log shows no new error, and
a dialog or menu that opened is described, then cancelled.

Without `launch`, what would open another program, folder or web page is skipped and listed; what acts outside
the window - the desktop background, printing, the clipboard, the network - and the sidebar's drive rows, which
browse outside the fixture collection, are not pressed. Putting the window back restores what it shows, not the
Back and Forward history, so a menu row an earlier press disabled - Forward, once something new has opened - is
skipped and listed. It ends with `coverage`: what ran, what was used, and what never was, with why.

```powershell
python tools\sweep_mcp.py exe\diffractor64.exe
```

## Where this lives

The taxonomy table above routes tests. The runner and its supporting pieces:

| Piece | Source |
|---|---|
| Assertion helpers and the failure type | [test.h](../src/test.h) |
| Shared fixtures, index building, the shared gazetteer, the null AV host, `browsing_fixture` | [test_fixtures.h](../src/test_fixtures.h), [test_fixtures.cpp](../src/test_fixtures.cpp) |
| Registration and the console runner | [test_runner.h](../src/test_runner.h), [test_runner.cpp](../src/test_runner.cpp) |
| The `/test`, `/test-temp:` and `/validate-po` switches | [app_command_line.h](../src/app_command_line.h), read by `command_line_t::parse` and run by `app_frame::run_console_mode` in [app.cpp](../src/app.cpp); [app_validate_po.cpp](../src/app_validate_po.cpp) |
| The gate and its label | `Invoke-Tests` and `Assert-HeadlessTestSelection` in [dd.ps1](../dd.ps1); `cmd_test` in [tools/dd.py](../tools/dd.py) |
| Translation checks | [check_translations.py](../tools/check_translations.py), [test_check_translations.py](../tools/test_check_translations.py) |

Build commands are the [README](../README.md#building)'s.
