# Diffractor Testing

This document owns the test taxonomy: which subject owns which test, where a new test belongs, and what the suite is allowed to assume. It does not restate build commands, which belong to [implementation](implementation.md#build-and-validation).

## The suite

Tests are compiled into the application and run from the command line. There is no separate test binary and no external framework.

```powershell
.\exe\diffractor64-d.exe /test             # x64 Debug, everything
.\exe\diffractor64-d.exe /test:*search*    # one subject
.\exe\diffractor64-d.exe "/test:Should rename file case"
```

`/test:` takes a wildcard matched against the test name, case-insensitively. A filter that matches nothing is a failure, not a silent pass.

Release registers exactly six more tests than Debug: five decoder fuzz sweeps and the closest-location lookup are `#ifndef _DEBUG`. Debug `N` and Release `N + 6` is correct; any other gap means something was skipped.

The gateway also runs [test_release_tools.ps1](../tools/test_release_tools.ps1):
SDK-only SymStore discovery, missing prerequisites, argument forwarding and
nonzero exit handling. It uses temporary fixtures and never writes to a real
symbol store.

Translation registration checks include `app_text`'s composed libdesktop
messages. [test_check_translations.py](../tools/test_check_translations.py)
proves that omitted, duplicated, unknown and wrongly typed shared messages
still fail. For an explicitly overridden libdesktop source, pass its
`include/libdesktop/platform.h` to `check_translations.py --platform-text-header`.

## Taxonomy

One file per subject, matching the `src/` prefixes in [implementation](implementation.md#source-organization). A test lives with the code it constrains, never with the reason it was written.

| File | Registration | Owns |
|---|---|---|
| [test_util.cpp](../src/test_util.cpp) | `register_util_tests` | `util*`, `crypto*`: strings, wildcards, versions, natural compare, interning, cancellation tokens, result scopes, hashes and perceptual hashes, base64, JSON, kd-trees, top-N ranking, task queues, memory-mapped files |
| [test_text.cpp](../src/test_text.cpp) | `register_text_tests` | `app_text`, `util_spell`: catalogs, plural forms, translated month names, spell checking |
| [test_render.cpp](../src/test_render.cpp) | `register_render_tests` | `render*`, `util_simd`: software blends, YUV conversion, area downscaling, colour adjustment, surface transforms, alpha animation, decode budgets, the globe and panorama rasterisers |
| [test_files.cpp](../src/test_files.cpp) | `register_files_tests` | `files*`: codec decode and encode, format detection, container parsing, the write path (staging, replacement, originals, collisions, rollback), decoder robustness |
| [test_metadata.cpp](../src/test_metadata.cpp) | `register_metadata_tests` | `metadata*`, `model_property`, `model_dates`, `model_tags`: EXIF, IPTC, XMP, ICC, per-format metadata scanning, the date pack and its authority table, the panorama declaration, sidecars, tag sets, property presentation |
| [test_media_edit.cpp](../src/test_media_edit.cpp) | `register_media_edit_tests` | The metadata write path and the image editing model: in-place and staged edits, Windows shell tags, orientation, crop, perspective, temperature |
| [test_av.cpp](../src/test_av.cpp) | `register_av_tests` | `av*`: probing, seeking, read-ahead bounds, hover preview, audio buffers, the visualizer, session lifetime |
| [test_index.cpp](../src/test_index.cpp) | `register_index_tests` | `model_index`, `model_db`, `model_postings`: indexing, the inverted and trigram indexes, the database, the thumbnail pipeline, duplicates and presence, stale-result discarding, hydration |
| [test_search.cpp](../src/test_search.cpp) | `register_search_tests` | `model_search`, `model_related`, `app_match`: query parsing, term matching, scopes, negation, exclusion, related items, prediction |
| [test_locations.cpp](../src/test_locations.cpp) | `register_location_tests` | `model_locations`, `model_visits`, `ui_map*`, `ui_globe`, `model_tile_cache`: the gazetteer, place naming, map areas and geometry, the globe's projection and framing, visits, the timeline, the map tile cache. See [locations](locations.md) |
| [test_movie.cpp](../src/test_movie.cpp) | `register_movie_tests` | `model_movie`: the Movie document -- derived output geometry, transition and duration arithmetic, timeline edits and undo, what entering Movie does with the timeline it holds, the OpenTimelineIO project file, the Movie Maker reader, finding a moved source among same-named files. See [movie](movie.md) |
| [test_view.cpp](../src/test_view.cpp) | `register_view_tests` | `view*`, `ui*`, `model_zoom`, `ui_flex`: the zoom model, the panorama camera and its field-of-view ladder, the item selector, the view scroller, flex layout, tile geometry, detail rows, the task-list run highlight, text editing |
| [test_app.cpp](../src/test_app.cpp) | `register_app_tests` | `app*`: selection and command enablement, settings persistence, rename, import, sync and convert planning, external tools, history, the crash guard |
| [test_platform_win.cpp](../src/test_platform_win.cpp) | `register_platform_tests` | `platform_win*`: extended paths, DXGI device loss, adapter classification and texture budgets, the decode device's adapter, texture recycling, the font stack, the registry store, the shell data object, control painting. The only file that may include system headers |

[test.h](../src/test.h) holds the assertion helpers, [test_fixtures.h](../src/test_fixtures.h) / [test_fixtures.cpp](../src/test_fixtures.cpp) the shared fixtures, and [test_runner.cpp](../src/test_runner.cpp) the console runner. None of them contains a test.

### Placing a test

Ask what the test constrains, not what prompted it.

- A regression goes in the file that owns the behavior, with the issue recorded as a comment above its registration. There is no separate regressions file.
- A test that needs two subjects belongs to the one whose contract it would falsify. A metadata value read through the AV probe is a metadata test; the probe's byte budget is an AV test.
- A helper used by tests in one file stays `static` in that file. A helper used from two files moves to `test_fixtures.h` as `inline`.

## What the runner enforces

- **A test that asserts nothing fails.** Assertions are counted per test, so a test disabled by an early return or an `#if` cannot report PASS forever.
- **A duplicate test name fails.** Two registrations under one name make `/test:<name>` ambiguous and a failure report unattributable.
- **A filter matching no test fails.** Silence used to mean the filter missed, not that the tests passed.
- Failures are reported with the source file and line of the failing assertion.

## What a test must not do

- **Persist user state.** A test that appends to a real dictionary, settings key or collection passes once and then fails, and it edits a file the user owns. Write to `_temps` instead.
- **Depend on another test.** Tests share one `shared_test_context` for the loaded index and gazetteer, because loading those per test is not affordable, but nothing else may carry between them.
- **Depend on ordering, wall-clock timing, or the machine's fonts and DPI.** Layout tests measure through a stub measure context for this reason. Ordering includes the order a folder lists in: NTFS lists names sorted and ext4 does not, and a Linux build from a checkout under WSL's `/mnt/c` reaches `exe/` through links that list as NTFS does, so only CI shows the dependency. The record tests sort what they collect (`scan_parity::add_folder` in [test_files.cpp](../src/test_files.cpp)). The processor matters too, where a codec picks its kernels when it runs: dav1d decodes a damaged stream differently with each set, so the HEIF record pins its SSE4.1 kernels.
- **Add files to `exe/test`.** That folder is indexed recursively and its file count is asserted by `expected_cached_item_count` and by every search-count test. Fixtures loaded directly by path go in `exe/test/excluded1`, which indexing skips.

## Proving a test is worth having

A test must be able to fail for the defect it claims to cover. Break the targeted behavior, confirm the
test fails, then restore it. Record a non-obvious discriminating case beside the test.

## Fixtures

`exe/test` is the indexed collection every index and search test resolves against; its item count is fixed. `exe/test/excluded1` and `excluded2` are excluded from indexing and hold fixtures loaded by path. Temporary files come from `_temps`, which deletes them at the end of the run; `/test-temp:<path>` redirects them so the write path can be exercised against a network share.

The gazetteer is loaded once per run and shared, because a 24 MB index per test context is not affordable.

## Testing behavior without a window

`view_state` runs headless. Construct it with `null_state_strategy`, `null_async_strategy` and a **null** `view_host_base_ptr`, open a search over the fixture collection, and drive the same entry points the commands do. `browsing_fixture` in [test_app.cpp](../src/test_app.cpp) does exactly this, and it is how selection, filtering, grouping, sibling navigation and the browsing sequence are covered.

Where a decision is trapped inside a method that also performs it, extract the decision rather than building a window. Four rules are pure functions for this reason, and each is worth more than the code it replaced:

| Function | Owns |
|---|---|
| `should_resume_at` / `position_to_save` ([av_player.h](../src/av_player.h)) | the resume window and what a close saves |
| `calc_playback_advance` ([util_interfaces.h](../src/util_interfaces.h)) | slideshow, repeat and continue-with-next, and how they interact |
| `search_edit_session` ([app_search.h](../src/app_search.h)) | the address box editing session: draft, preview, two-stage Escape, Tab, Enter |
| `make_search_auto_complete` ([app_search.h](../src/app_search.h)) | the address bar's completion results: content, ranking, de-duplication, the cap, latest-wins |
| `default_keyboard_accelerators` ([app_commands.h](../src/app_commands.h)) | the shipped key bindings, so a collision can be seen |

A test that drives the completion strategy must run the pass on a real worker thread: `index_state::auto_complete_words` asserts it is not on the UI thread, which is the contract the shipped code keeps. `run_completion_worker` in [test_app.cpp](../src/test_app.cpp) models the hop with a `deferred_async_strategy`; running it inline trips the assertion rather than passing. That is also what makes the latest-wins test possible, because it can publish two passes out of order.

## Where this lives

The taxonomy table above is the routing for tests. The runner and its supporting pieces are:

| Piece | Source |
|---|---|
| Assertion helpers and the failure type | [test.h](../src/test.h) |
| Shared fixtures, index building, the shared gazetteer, the null AV host | [test_fixtures.h](../src/test_fixtures.h), [test_fixtures.cpp](../src/test_fixtures.cpp) |
| Registration and the console runner | [test_runner.h](../src/test_runner.h), [test_runner.cpp](../src/test_runner.cpp) |
| The `/test`, `/test-temp:` and `/validate-po` entry points | [app_command_line.h](../src/app_command_line.h), [app_validate_po.cpp](../src/app_validate_po.cpp) |

Build orchestration and the repository-wide validation command are owned by
[implementation.md](implementation.md#build-and-validation).
