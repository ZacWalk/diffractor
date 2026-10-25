---
description: UI-thread ownership, invalidation, handle safety, and naming for views and controls.
applyTo: 'src/view_*.cpp, src/view_*.h, src/ui_*.cpp, src/ui_*.h'
---

# Views, controls, and layout

Owning documents: [design](../../docs/design.md) for what the user sees,
[selection controls](../../docs/selection-controls.md) for the selection panel,
[implementation](../../docs/implementation.md#view-invalidation) for the invalidation contract.

## UI thread

Views, controls, hosts, controllers and `view_element` instances are UI-thread-owned; their
mutable state is read and written only on the UI thread. No filesystem, database, decoding,
indexing, hashing, querying, thumbnail, map or network work happens here — request it.

Paint functions never access SQLite and never scan files, directly or through a helper. If a paint
path needs data it does not have, it asks for Source work and draws what it has.

## Hit testing

A controller's `bounds()` is its own region, never the element narrowed by what covers it. Clipping
it there drops the click and the hover highlight on the clipped-away part. What the host may cache
the answer for is `hit_test_context`: test every higher-precedence region through `ctx.occluded()`
so recording it cannot be forgotten, and never restate the same rectangles afterwards as exclusion
arithmetic. See [implementation](../../docs/implementation.md#hit-testing).

## Absent handles
A `ui::frame_ptr` member that can be unset is reached **only** through its accessor, which answers
`ui::no_frame()` rather than null. Callers do not test it. A raw `_frame->` on such a member
outside that accessor is a lint failure.

**Existence is not visibility.** Never bind a handle's lifetime to whether the thing is on screen.
A hidden panel still populates, counts, ticks and invalidates: attach it unconditionally and gate
only the repaint. Diffractor 1.27.0 shipped a startup crash that was exactly this conflation.

## Naming is closed

[design](../../docs/design.md) closes the vocabulary and source must match it. A **view** is one of
the named top-level presentations; there are exactly two **modes** (zoom mode, Slideshow);
everything else the user chooses is a **presentation choice**. "Edit mode", "tag mode" and
"items mode" name nothing. **Fullscreen** is one word.

## Layout and relayout

Relayout from a resize, toolbar wrap, or presentation change preserves orientation in each pane:
the focused visible item keeps its screen position where possible, otherwise the content nearest
the viewport centre, with proportional scroll as the last fallback.

A control the user is actively manipulating keeps ownership of its value across result refreshes.
Refreshes it triggered coalesce, discard superseded results, and must not recreate, reposition, or
write back to the captured control.

## Testing without a window

`view_state` runs headless with `null_state_strategy`, `null_async_strategy` and a null
`view_host_base_ptr` — see `browsing_fixture` in [test_app.cpp](../../src/test_app.cpp). Where a
decision is trapped inside a method that also performs it, **extract the decision** rather than
building a window; that is how zoom, playback advance, the address-box session and the accelerator
table became testable.

## Checks

`/test:*layout*`, `/test:*scroll*`, `/test:*selection*`, `/test:*zoom*`, `/test:*command*`.
