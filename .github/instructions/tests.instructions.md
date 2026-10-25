---
description: Where a test belongs, what the runner enforces, and what a test may not do.
applyTo: 'src/test_*.cpp, src/test_*.h, src/test.h'
---

# Tests

Owning document: [testing](../../docs/testing.md). Read its taxonomy table before adding a file.

## Placement

Ask what the test **constrains**, not what prompted it.

- A regression goes in the file that owns the behavior, with the issue as a comment above its
  registration: `// Issue #177 - search term negation`. There is no regressions file. Organising by
  provenance means a subject-scoped run silently misses coverage.
- A test needing two subjects belongs to the one whose contract it would falsify.
- A helper used in one file stays `static` there; used from two, it moves to `test_fixtures.h` as
  `inline`.

## The runner enforces

- A test that asserts nothing **fails** — assertions are counted, so a test disabled by an early
  return cannot report PASS forever.
- A duplicate test name **fails**.
- A filter matching no test **fails**. Silence used to mean the filter missed.

Release registers exactly six more tests than Debug (five decoder fuzz sweeps and the closest
-location lookup are `#ifndef _DEBUG`). Debug `N` and Release `N + 6` is correct; any other gap
means something was skipped.

## A test must not

- **Persist user state.** Never append to a real dictionary, settings key or collection. Use
  `_temps`.
- **Depend on another test.** One `shared_test_context` carries the loaded index and gazetteer
  because loading those per test is not affordable. Nothing else carries between tests.
- **Depend on ordering, wall-clock timing, or the machine's fonts and DPI.** Layout tests measure
  through a stub measure context for this reason.
- **Add files to `exe/test`.** That folder is indexed recursively and its file count is asserted by
  `expected_cached_item_count` and by every search-count test. Fixtures loaded by path go in
  `exe/test/excluded1`.

## Prove it can fail

A test that cannot fail is worse than no test: it reports coverage that does not exist. Break the
behavior, confirm the test fails, restore. Record the negative case in a comment where it is not
obvious. Assert the property, not a bound that a systematic error could still satisfy — an
unbiased residual catches truncation that a tolerance check cannot.

A test driving the completion strategy must run its pass on a real worker thread:
`index_state::auto_complete_words` asserts it is not on the UI thread. `run_completion_worker`
models the hop with a `deferred_async_strategy`; running it inline trips the assertion.

## Checks

`.\dd.ps1 test` runs the lint, the suite, and the translation checks. Narrow first with
`.\exe\diffractor64-d.exe /test:*subject* | Out-Host`.
