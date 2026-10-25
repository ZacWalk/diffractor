---
description: Thread ownership, worker publication, and lock discipline for the model and worker layers.
applyTo: 'src/model_*.cpp, src/model_*.h, src/model.cpp, src/model.h, src/app_workers.cpp, src/app_util.cpp, src/util_interfaces.h'
---

# Threading and result publication

Owning document: [implementation](../../docs/implementation.md#async-execution). This file repeats
only what is decided at the moment of editing these files.

## Before adding a field

Mutable objects have **one owning execution context**. `docs/implementation.md` carries the
synchronized-type registry — the closed list of approved exceptions. Adding an atomic, mutex,
event or synchronized mutable field to a type outside that list requires first documenting, in
that registry, the owning threads, the protected invariant, and why moved values or an immutable
snapshot on one context is insufficient. If you cannot write those three sentences, the field is
the wrong fix.

`df::item_element` is entirely UI-owned, including its metadata snapshot and playback position.
`df::index_file_item` is a synchronized index record whose atomic metadata pointer publishes
**complete** payloads only: clone, finish every field, then replace the pointer.

## Worker contract

- Workers consume moved values or immutable snapshots and produce detached results.
- A background callback must not capture `this`, a raw pointer, a reference, or an owning
  `shared_ptr` to a UI-owned object. A `weak_ptr` may cross a queue only as an opaque lifetime
  token — do not lock or dereference it until execution is back on `queue_ui`.
- Apply results on the owning context in one bounded batch. Never publish a partially
  initialized object.
- **Lifetime is not currency.** A successful `weak_ptr::lock()` says the object still exists; it
  says nothing about whether this result is still the one wanted. Check the generation,
  request ID, scope, path, size, status, or CRC as well. A missing currency check is the classic
  stale-result bug here, and it presents as correct data attached to the wrong item.
- `const_pointer_cast` to publish a worker result is a lint failure, not a workaround.

## Locks

Never hold an index lock across I/O, SQLite, decoding, a callback, a sort, or a large or
unbounded allocation. Prefer cancelling and replacing stale work over letting it accumulate.

## SQLite

Two connections exist, each pinned to its own thread for life: the index database on
`queue_database`, and the tile store on `queue_tile_db`. Do not call a database method from any
other context, and do not add a third connection without a thread of its own. `sqlite3_*` outside
`model_db*` and `model_tile_cache*` is a lint failure.

## Checks

`/test:*index*`, `/test:*thumbnail*`, `/test:*search*`, then `pwsh -File tools/lint_repo.ps1`.
A new ownership migration should add a stale-result test *before* the synchronization field is
removed, so the test is proven against the old behavior.
