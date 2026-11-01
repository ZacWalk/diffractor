# Diffractor Implementation

This document owns architecture, execution ownership, invalidation, index/search/database flow, and
recovery boundaries. [Design](design.md) owns observable behavior; specialized documents own file I/O,
metadata, rendering, locations, zoom, and selection controls. Source owns exact APIs and enums.

## System Shape

Diffractor is a C++20 media organizer with a custom retained UI. SQLite stores a rebuildable index and
thumbnail cache. Media files and sidecars remain authoritative. libfiles reads still images and
audio/video containers; FFmpeg decodes audio and video; format libraries decode still images;
platform modules contain operating-system integration.

## Source Organization

| Prefix | Responsibility |
| --- | --- |
| `app*` | Process coordination, commands, settings, workers, text, sidebar, toolbar |
| `model*` | State, items, index, search, database, properties, locations, movie model |
| `view*`, `ui*` | Views, controls, layout, input, and backend-neutral drawing |
| `files*`, `metadata*` | Format dispatch, decode/encode, scanning, EXIF/IPTC/XMP/ICC |
| `av*`, `render*` | Audio/video sessions, surfaces, color, transforms, software rendering |
| `platform*` | Files, threads, windows, graphics, audio, settings, network, shell |
| `util*` | Shared value types and bounded helpers |
| `test*` | Embedded unit and regression tests |

System APIs and Windows types stay in `platform*`; SQLite calls stay in database owners. Keep each
source file's `// Purpose:` comment accurate when modifying it.

## State And Views

`view_state` owns scope/query, results, grouping, selection, focus, display state, navigation history,
playback, and active view. `app_frame` coordinates windows, commands, workers, and invalidation.
Items is the windowed browser; fullscreen reuses the persistent media renderer without replacing the
logical browsing state.

Each view declares what the frame's title bar and status bar carry for it (`view_base::bars`): command
identifiers, and the few bar controls that are not buttons, for the title bar and for each end of the
status bar. `app_frame` builds one set of native controls per distinct declaration before the first
layout, updates them with the rest of command state, and lays out only the active view's sets, the
status bar's trailing set first so a short bar gives up its leading one; the status text and totals
are painted by the frame itself from in-memory state, and the selection summary is the label of Items'
Open button. A view therefore never creates, positions or updates a toolbar.

Views, controls, hosts, controllers, and `item_element` are UI-owned. A host may outlive its native
window; `view_host::frame()` therefore returns `ui::no_frame()` when unattached. Visibility gates
painting, not attachment or model updates.

## Hit Testing

One pointer position produces one controller through a single ordered walk. Precedence is the order
of the walk; nothing restates it afterwards.

A controller's `bounds()` is its own region: what it paints over, and what a release must land in
for the gesture to count. It is never narrowed to describe what covers it — a controller clipped
that way drops clicks and hover highlights on the part that was clipped away.

How far that answer travels is `hit_test_context`, threaded through the walk and owned by
`view_host`. A region tested at higher precedence and passed over is cut out of it, so the pointer
reaching that region forces a fresh test. Recording is a side effect of testing, which is what stops
a new tool from being hit tested but forgotten. The host caches `bounds() ∩ stability()`; an empty
result means "re-test on every move", which is what a controller standing in for whatever is under
the pointer wants.

## View Invalidation

`view_invalid` is a coalesced request to make derived state current, not an event log. Producers add
flags from any owning context; `app_frame::complete_pending_events` drains them on the UI thread.
Handlers are idempotent and read current state.

Invalidation proceeds in dependency order:

1. Source work refreshes index, query, metadata, presence, predictions, and summaries.
2. Derived work rebuilds groups, selection/media elements, command state, and sidebar data.
3. Geometry lays out views and rebuilds interaction controllers.
4. Paint invalidates frames or bounds only after state and geometry are current.

Source and Paint are disjoint. Paint never reads SQLite or scans files; it requests Source work.
Producers request the narrowest upstream layer made stale, and each handler owns its unconditional
downstream invalidation.

## Async Execution

UI execution is disjoint from file I/O, decoding, database access, indexing, hashing, queries, maps,
and network work. `async_strategy` owns named queues; `state_strategy` coordinates model-to-UI work.
Tests substitute null or deferred strategies.

Workers consume moved values or immutable snapshots and publish detached results through `queue_ui`.
Publication checks lifetime and currency: path, scope, generation, request identity, or content
revision as appropriate. A `weak_ptr` may cross a queue only as an opaque lifetime token and is not
locked until execution returns to the UI queue.

No I/O, callback, sorting, decoding, or unbounded allocation runs under an index lock. Stale work is
cancelled or superseded, and completion releases every claim even when the result is discarded.

### SQLite Connection Ownership

SQLite is built in multi-thread mode, so each connection has one owning queue. The media index uses
the database queue; `map-tiles-cache.db` uses the tile-database queue. Methods assert their owning
thread in Debug. A new connection requires its own documented execution owner.

### Synchronized Types

Mutable objects have one owning context unless documented otherwise. Approved synchronized objects
are execution primitives, app command coordination, index records/caches, media sessions, and the D3D
shared-texture handoff. Atomic reference counting synchronizes lifetime only.

`item_element` remains entirely UI-owned. `index_file_item` is a synchronized index record whose
atomic metadata pointer publishes complete immutable snapshots; published payloads are never mutated.
`index_folder_item` is a synchronized index record: the index worker publishes replacement nodes
through `index_items`, scan/database workers update per-file atomics in place, and a content revision
lets folder replacement reject a node built before an in-place update. The revision protects the
folder contents as a set of per-file metadata/search-presence/cache fields; immutable replacement
alone is insufficient because scanning a whole folder for each individual metadata update would
clone large folder vectors and move file scanning back under publication contention.
Indexing progress is an immutable snapshot owned by the index worker and atomically published for
UI/sidebar readers; paired total/remaining counters must be read together, and handing every paint
call through the index queue or posting a UI callback for each scanned file would either block Paint
on Source work or create unbounded invalidation traffic.
`view_state` owns the visit timeline on the UI thread, while `_visits_generation` is atomic only so
the location worker can observe superseded visit derivations through `df::cancel_token`; the worker
still publishes a complete detached `visit_timeline` back to the UI thread before the UI-owned value
is replaced.

Three process-wide values are also published to every thread. A translation - and the extra plural
forms of a plural text - is an atomic pointer into storage `app_text_t` only ever appends to: the UI
thread publishes on a language switch, and a worker formatting text across the switch reads the old
string or the new one, never one being rewritten. The strings are read at too many call sites to hand
each worker a snapshot. The file-group spellings `parse_file_group` accepts are an immutable table
that a language switch copies, extends and swaps whole, for the same readers. `df::last_loaded_path`
is a lock-free atomic value that decoding workers write and the crash handler reads, because a
handler that waited on a lock held by the faulting thread would hang the report it exists to make.

Adding synchronization requires naming every owning context, the protected invariant, and why moved
results or immutable publication are insufficient.

## Index, Search, And Database

Startup loads cached records, validates collection roots, scans stale files, queues database writes,
and rebuilds summaries, duplicate predictions, presence, and search candidates. Cached results may
publish before extraction completes.

Search parses into exact matchers and conservative candidate prefilters. Negation and OR never create
a prefilter that could omit an exact result. Query generations prevent stale result publication.
Folder-scoped searches validate their filesystem scope; index-wide searches answer from cached
records. In-app file operations report every changed source and destination folder so search refresh
does not depend on a live watcher.

The summary keeps authoritative tag histograms and word counts for every indexed tag. Tag-companion
recommendations are auxiliary typeahead hints and are bounded per pathological item: when one item
contains more distinct tags than the companion budget can cover, only that item's first bounded set
contributes companion pairs. Normal collections are not globally capped, so their companion scores do
not depend on folder hash order. Search counts, tag summaries, and exact tag matching remain complete;
only recommendation breadth for a single pathological item is traded for bounded per-item CPU.

Duplicate prediction narrows by cheap metadata and CRC, then requests perceptual hashes only for
unresolved candidates. Every attempted hash reaches a terminal state. Duplicate search, related
items, and presence consume the same duplicate relation so they cannot disagree about identity.

Related-item collection scores the index against one anchor and keeps bounded best results per
relation. Counting and display share the same collector and stable path tie-breaks.

### Thumbnail Pipeline

Layout dimensions, encoded thumbnail, decoded near-viewport surface, and GPU texture are separate
representations. SQLite stores the bounded encoded form. Visible items query it in batches before a
source scan; cloud-only files request provider thumbnails without hydrating source media.

Encoded and decoded retention is viewport-aware and bounded. Eviction re-arms the cheaper database
path and preserves layout dimensions. Claims are taken before queueing and released on every
completion path; publication and texture staging use separate generations.

## Files, Media, And Rendering

[File I/O](file-io.md) owns progressive image display and writes. [Metadata](metadata.md) owns tag
mapping. [Rendering](rendering.md) owns draw backends and device lifetime. FFmpeg sessions decode off
the UI thread and publish generation-checked frames; graphics upload remains UI-owned.

A SID tune reaches the player as a WAV file that is never written: [av_sid.cpp](../src/av_sid.cpp)
runs the tune in lib8bit's Commodore 64 as the decoder's read thread asks for samples, so the
session's clock, seeking, pausing and end are the ordinary ones. The render keeps each PAL frame's SID
registers beside its samples for the SID monitor ([ui_sid.cpp](../src/ui_sid.cpp)) to read on the UI
thread. Reads are kept short and the WAV demuxer is asked for small packets, so a muted voice is heard
to stop within half a second. The session's own info describes the WAV, so the display swaps in the
tune's (`sid_tune_render::describe`). For other audio the visualizer marks the presented position; for
a SID tune, which has no visualizer, `update_for_present` marks it (`av_session::mark_presented`).

The monitor is drawn as the audio visualizer is, so a playing tune does not rebuild the view's scene for
every frame. Its waveforms, meters, lamps and the register grid's highlight are flat rectangles in a
`ui::vertices` the scene draws by reference: the monitor places them when it draws (`sid_monitor_motion`),
and `update_for_present` brings them up to the session's position, after which the frame presents the
scene again. The values, notes and register contents beside them are text, which only rebuilding the
scene redraws, so they are redrawn ten times a second while they change - and not at all once the monitor
has not been drawn for two seconds, as when the view showing it has gone.

Subtitles ride the playing session ([av_subtitle.cpp](../src/av_subtitle.cpp)). The read thread decodes
the chosen embedded stream's packets as it demuxes them, or a sidecar whole when the session opens, into
events on the presentation timeline. It rasterises the lines near the playhead - text through
libdesktop's styled-text renderer, pictures through their palettes - and releases them once behind, so
memory follows the playhead rather than the film. The UI thread only uploads finished bitmaps and draws
them over the picture, scaled from the video's pixels; `update_for_present` rebuilds the scene once when
what is on show changes, with or without a new frame, since a replay would present the lines it was
recorded with. A seek drops the stream's earlier events by generation; a
sidecar's are kept. The choice of track belongs to the display, so a change of audio track keeps it.

Long operations execute immutable reviewed plans. Each item records success, failure, skip, or
cancellation. Cancellation stops future work and does not claim to undo completed effects.

## Persistence And Network

Settings use the platform settings store. SQLite data, thumbnails, import history, web cache, and map
tiles are rebuildable. User metadata belongs in media or sidecars.
New Windows INI settings files explicitly select Unicode before the first property is written;
opening an existing settings file never recreates it.

Updates, crash reports, dictionaries, maps, and location lookup are separate network capabilities.
Transport stays in platform/web modules; application settings decide whether a capability is used.
Feature-use and performance counters are relaxed aggregate diagnostics and never control behavior.
Server browsing requests share names and types without security descriptors; an enumeration failure
does not publish a partial listing as complete.

## Crash-Loop Protection

Graphics crash guards disable only the risky capability whose durable marker survived a crash.
`crash_files_db` records files open on a faulting scan thread and skips them for the current release
line during unattended indexing. Explicit user opens are still attempted.

Repeated unsettled starts reset presentation settings and disable GPU, hardware decode, and direct YUV
until a working window can explain recovery. Collection roots and user metadata are retained. Crash
report contents and symbolization are owned by [crash.md](crash.md).

## Build And Validation

CMake is the only build description and `dd` is the repository gateway. Run `.\dd.ps1 test` for
repository lint, build, embedded tests, and translation validation. Focused tests use
`.\exe\diffractor64-d.exe /test:*name*`. [Testing](testing.md) owns test placement and constraints;
[Linux](linux.md#retiring-msbuild) owns the cross-platform build boundary.

## Where this lives

- [app.cpp](../src/app.cpp), [app.h](../src/app.h), and [app_workers.cpp](../src/app_workers.cpp):
  process, frame, queues, and invalidation; [app_toolbar.cpp](../src/app_toolbar.cpp): the bars built
  from each view's declaration and how they are arranged.
- [model.cpp](../src/model.cpp), [model.h](../src/model.h),
  [model_items.cpp](../src/model_items.cpp), and [model_items.h](../src/model_items.h): view and item
  state.
- [model_index.cpp](../src/model_index.cpp), [model_search.cpp](../src/model_search.cpp),
  [model_postings.h](../src/model_postings.h), and [model_db.cpp](../src/model_db.cpp): index, search,
  candidate sets, and persistence.
- [model_index_duplicates.cpp](../src/model_index_duplicates.cpp): duplicate prediction and presence;
  [model_index_summary.cpp](../src/model_index_summary.cpp): the summary, its vocabulary, and
  auto-complete.
- [model_tile_cache.cpp](../src/model_tile_cache.cpp): separately owned map tile database.
- libdesktop's [platform.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform.h): operating-system abstraction;
  libdesktop's [platform_render.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform_render.h): the platform services the window layer implements.
- [av_sid.cpp](../src/av_sid.cpp) and [ui_sid.cpp](../src/ui_sid.cpp): SID tunes played through
  lib8bit, and the SID monitor.
- [av_subtitle.cpp](../src/av_subtitle.cpp): subtitles - styling, placement, the events a session
  keeps, and the choice of track.
- [ui_view.h](../src/ui_view.h), [ui_elements.h](../src/ui_elements.h), and
  [ui_controllers.h](../src/ui_controllers.h): view framework.
- [util_crash_files_db.h](../src/util_crash_files_db.h): media crash-loop protection.
