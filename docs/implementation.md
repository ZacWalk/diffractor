# Diffractor Implementation

This document owns architecture, execution ownership, invalidation, index/search/database flow, and
recovery boundaries. [Design](design.md) owns observable behavior; specialized documents own file I/O,
metadata, rendering, locations, faces, zoom, and selection controls. Source owns exact APIs and enums.

## System Shape

Diffractor is a C++20 media organizer on libdesktop's platform, render and UI libraries. SQLite
stores a rebuildable index and thumbnail cache; media files and sidecars remain authoritative.
libfiles scans every file, reads and writes its metadata, and decodes and encodes most still formats;
FFmpeg decodes audio and video, and the stills libfiles is not built to decode here, GIF and TIFF
among them; lib64 emulates the Commodore 64, which plays SID tunes and runs disks, tapes, cartridges
and programs. Diffractor's `platform*` files hold only what is the application's own: its side of
hardware decoding, its resource identifiers and its Linux hooks.

## Source Organization

| Prefix | Responsibility |
| --- | --- |
| `app*` | Process coordination: the frame, commands, settings, workers, text and translations, sidebar, toolbars and bars, dialogs, and the headless command-line tools |
| `model*` | State, items, index, search, database, properties and dates, locations and visits, the Movie document, face detection and search |
| `view*`, `ui*` | Views and task views, controls, layout, input, and drawing, on libdesktop's element tree |
| `files*`, `metadata*` | Diffractor's side of libfiles: format detection, scanning, decode and encode, EXIF/IPTC/XMP/ICC |
| `media_edit.cpp` | A photo's edits applied to its pixels |
| `av*` | Audio and video sessions, playback, subtitles, SID tunes, the running Commodore 64, and the visualizer |
| `render*` | Software rasterisers for the sidebar's globe and charts and for panoramas; the draw backends are libdesktop's |
| `crypto*` | Hashes and checksums, including the perceptual image hash |
| `platform*` | The application's own Windows and Linux glue: FFmpeg hardware decoding, resource identifiers and Linux hooks |
| `util*` | Shared value types and bounded helpers |
| `test*` | Embedded unit and regression tests, arranged by [testing](testing.md) |
| `pch.h` | The precompiled header: libdesktop's UI umbrella and the headers every file uses |

The boundaries the lint enforces - platform code in `platform*` files and libdesktop, `sqlite3_*` in
the database owners, a `// Purpose:` comment in every file - are AGENTS.md's
[anti-patterns](../AGENTS.md#strict-code-anti-patterns).

## State And Views

`view_state` owns scope/query, results, grouping, selection, focus, display state, navigation history,
playback, and active view. `app_frame` coordinates windows, commands, workers, and invalidation.
Items is the windowed browser; fullscreen reuses the persistent media renderer without replacing the
logical browsing state.

The window holds three surfaces, each a frame of its own, and draws nothing itself but the line of
background round them: the title bar and the status bar (`bar_frame`, drawn on the CPU) and between them
the view frame (`view_frame`, drawn on the GPU where it can). The view frame holds the view, the task
view's panel beside it - a region of the view frame, whose controls are elements of its tree, drawn,
hit and focused with it - and the selector strip along its foot, composed into the same surface.
Dialog map widgets likewise share their dialog's surface. Neither creates a native child window or
rendering device; their embedded frames retain local input, focus and coordinates. Menus, tooltips
and autocomplete lists remain separate popup windows. Full screen and a zoomed picture give the view
frame the whole window.

Each view declares what the frame's title bar and status bar carry for it (`view_base::bars`): command
identifiers, and the few bar controls that are not buttons, for the title bar and for each end of the
status bar. `app_frame` builds one set of toolbar controls per distinct declaration before the first
layout, updates them with the rest of command state, and lays out only the active view's sets, the
status bar's trailing set first so a short bar gives up its leading one; the status text and totals
are painted by the status bar from in-memory state, and the selection summary is the label of Items'
Open button. A view therefore never creates, positions or updates a toolbar.

Views, controls, hosts, controllers, and `item_element` are UI-owned. A host may outlive its native
window; `view_host::frame()` therefore returns `ui::no_frame()` when unattached. Visibility gates
painting, not attachment or model updates.

The selection panel's decoded file head owns its text selection and its formatted document control,
so rebuilding the surrounding elements does not replace a captured control or reset table widths.
`ui_document` places text, dumps and the shared Markdown and CSV views in libdesktop's flex document
panel. Element tables belong to libdesktop too; task lists retain application sorting, progress and
row identities while using the same column sizing and resize gesture as document tables. A table of
the selection panel is made again with the panel, so what the user made of it - sizes and sort - is
kept on the display by the table's name (`keep_table_choices`). A detail group's headings are drawn by
`item_group` itself, over its own rows; their sort and sizes live in `view_state` by group key, and
`item_group::sort` orders a group by them while its headings show.

## Hit Testing

One pointer position produces one controller through a single ordered walk. Precedence is the order
of the walk; nothing restates it afterwards.

A controller's `bounds()` is its own region: what it paints over, and what a release must land in
for the gesture to count. It is never narrowed to describe what covers it — a controller clipped
that way drops clicks and hover highlights on the part that was clipped away. A release reaches the
controller under the pointer whether or not that controller saw the press - the press that closed a
menu went to the menu - so a controller acts on a release only after its own press.

How far that answer travels is `hit_test_context`, threaded through the walk and owned by
`view_host`. A region tested at higher precedence and passed over is cut out of it, so the pointer
reaching that region forces a fresh test. Recording is a side effect of testing, which is what stops
a new tool from being hit tested but forgotten. The host caches `bounds() ∩ stability()`; an empty
result means "re-test on every move", which is what a controller standing in for whatever is under
the pointer wants.

Tooltip anchors and their hover regions start in the owning frame's client coordinates. The popup
anchor is converted to screen coordinates through that frame exactly once; the hover region stays
client-relative. Native child-window bounds are parent-relative, not a desktop origin. Menus take the
same path both ways: a view host opens one at its bounds converted through its frame's
`client_to_screen`, and a right-click, which arrives as a screen point, reaches the view by the view
surface's screen origin.

Bubble presentation observes the pointer after input and queued UI work, using monotonic
milliseconds. A half-second dwell allows four logical pixels of jitter around its starting point,
not around the previous sample, so slow cumulative movement also restarts it. Regular idle ticks
complete the dwell without another mouse event; content refreshes keep the same target's timer.

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
are execution primitives, app command coordination, index records/caches, media sessions, and the
lock a hardware-decoded picture lends the renderer. Atomic reference counting synchronizes lifetime
only.

`item_element` remains entirely UI-owned. `index_file_item` is a synchronized index record whose
atomic metadata pointer publishes complete immutable snapshots; published payloads are never mutated.
`index_folder_item` is a synchronized index record: the index worker publishes replacement nodes
through `index_items`, scan/database workers update per-file atomics in place, and a content revision
lets folder replacement reject a node built before an in-place update. The revision protects the
folder contents as a set of per-file metadata/search-presence/cache fields; immutable replacement
alone is insufficient because scanning a whole folder for each individual metadata update would
clone large folder vectors and move file scanning back under publication contention. A second
counter, the search revision, rises with every in-place update except those to what is derived from an
item's bytes - duplicates, checksums and picture hashes - so the search index can tell a folder whose
captured contents changed from one that only gained a derived value it reads live; one counter cannot
serve both, because replacement must still reject a node built before a derived update.

Indexing progress is an immutable snapshot owned by the index worker and atomically published for
UI/sidebar readers; paired total/remaining counters must be read together, and handing every paint
call through the index queue or posting a UI callback for each scanned file would either block Paint
on Source work or create unbounded invalidation traffic.

`view_state` owns the visit timeline on the UI thread, while `_visits_generation` is atomic only so
the location worker can observe superseded visit derivations through `core::cancel_token`; the worker
still publishes a complete detached `visit_timeline` back to the UI thread before the UI-owned value
is replaced. The address box's `_count_version` is atomic for the same reason: each keystroke raises
it on the UI thread, and the auto-complete worker observes it through `core::cancel_token` to stop
counting a superseded pass, which then publishes nothing.

The search index is an index cache published as one immutable snapshot. Its own queue builds it with
no lock held, `index_state` swaps the pointer under the summary lock, and readers copy the pointer
under the same lock and evaluate with none held. An index updated in place would need a lock across
every evaluation that indexing writers then wait on; a snapshot plus the per-folder revisions it
records keeps readers and indexing apart.

Three process-wide values are also published to every thread. A translation - and the extra plural
forms of a plural text - is an atomic pointer into storage `app_text_t` only ever appends to: the UI
thread publishes on a language switch, and a worker formatting text across the switch reads the old
string or the new one, never one being rewritten. The strings are read at too many call sites to hand
each worker a snapshot. The file-group spellings `parse_file_group` accepts are an immutable table
that a language switch copies, extends and swaps whole, for the same readers. `df::last_loaded_path`
is a lock-free atomic value that decoding workers write and the crash handler reads, because a
handler that waited on a lock held by the faulting thread would hang the report it exists to make.

`index_state` owns face consent, the face content revision, and the immutable face snapshot.
Detection owns its engine on one worker; a separate grouping worker builds detached snapshots and
swaps the published pointer under the face lock. Groups are derived and never persisted, so a
discarded pass leaves nothing to reconcile. Consent changes cancel analysis. Content may advance
during a grouping snapshot: obsolete source rows are removed before publication and a final
catch-up replaces the partial projection. Picture captions additionally check the source face-set
identity. The lock protects publication and feature transitions, not clustering, projection,
sorting, or database access.

Adding synchronization requires naming every owning context, the protected invariant, and why moved
results or immutable publication are insufficient.

Native music renders are shared by the player's read thread and the UI. A render lock owns the SID
emulator or tracker decoder and PCM cache; a separate, short data lock publishes complete chip or
pattern snapshots and channel-mix requests. The UI never calls a live decoder: immutable publication
alone cannot drive a decoder that must continue from its preceding audio block. A tracker snapshot
carries the patterns either side of the one playing, formatted once on the render thread, and how far
through its row its moment is, measured from the snapshots around it; libopenmpt places a module only at
the start of a row, so a seek plays on from there unheard to the sample it asked for.

Movie shares one SID render per tune of each file among the readers of an audio clip - its frames, drawn on the
render queue or the render's worker, and the decoders that read its sound - through a map of weak
references under a lock of its own. The render's locks already serialise its readers, and a tune only
plays forward: a reader that had its own render would emulate the whole tune again from its start.
The frames are drawn into a `display_state_t` and a monitor element made for that one clip, with a
software draw context whose text engine the drawing worker made; no view holds any of them, and the one
worker drawing a frame owns them all, so the UI thread's ownership of displays and elements does not
reach them.

A running Commodore 64 (`c64_session`) is shared by the emulator queue's worker and the UI thread. The
worker alone touches the emulator. A short frame lock publishes each finished picture whole - the worker
writes two surfaces in turn and replaces, rather than reuses, one the UI still holds - and a short input
lock hands key changes the other way; the joystick, its port, the volume, RESTORE, the status and the stop
request are atomics. Immutable publication alone cannot carry input into a machine that keeps running
between the pictures it publishes.

## Index, Search, And Database

Startup loads cached records, validates collection roots, scans stale files, queues database writes,
and rebuilds summaries, duplicate predictions, presence, and search candidates. Cached results may
publish before extraction completes.

The `item_properties.properties` blob stores versioned binary property records. Private cache IDs
are separate from the public media-property registry, so opaque values such as the face cache never
become EXIF/XMP fields or UI metadata. Metadata-only writes preserve unknown records from the same
item's previous blob; binary-property edits preserve all other records byte for byte. The format and
private IDs live in [model_db_pack.h](../src/model_db_pack.h) and
[model_property.h](../src/model_property.h).

Search parses into exact matchers and conservative candidate prefilters. Negation and OR never create
a prefilter that could omit an exact result. Query generations prevent stale result publication.
Folder-scoped searches validate their filesystem scope; index-wide searches answer from cached
records, through the [search index](#search-index) where it is current. In-app file operations report
every changed source and destination folder so search refresh does not depend on a live watcher.

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

### Search Index

The search index is an immutable in-memory reverse index of the collection, read from the cached
records rather than the database, so it needs no schema change and no re-index. Items are numbered in
path order. Each distinct name, free-text value, formatted value, split part, and tag part maps to the
items holding it, and a trigram index over the distinct text values suggests which of them hold a
free-text term. A search's terms become item sets combined by `combine_search_terms`, the steps the
matcher applies to one item. The matcher's own predicate confirms every suggestion. Rating and label
terms read nothing of an item but that property, so items are grouped by its value and `match_term`
answers once per group. Date terms compare only the day each of an item's date keys falls on, so
`date_term_keys_of` and `date_term_found`, which the matcher uses too, decide them with each day
compared once. A term no table can answer, such as a size comparison, is answered by `match_term` over
each item's captured metadata. An answer from the index is therefore the answer a scan gives, and the
index tests compare the two over representative searches.

Each folder entry keeps the folder node and search revision it was read from. A folder changed since
is scanned on top of the snapshot. Duplicates, checksums, picture hashes and presence are always read
live, so recording them leaves a folder current. A search uses the snapshot while at least half the
collection is unchanged and otherwise scans, as it would without one. Formatted values depend on the
language and number format, and date keys on the time zone and, through what counts as a valid date,
the day; when one changes, those terms are answered item by item until the next read, and maintenance
reads the collection again on a new day.

Reading never delays indexing or a search. It runs on its own `search_index` queue, never
`index_compute`, a debounced second after the summary and duplicate-prediction passes, and only once
changes since the last read pass a tolerance that grows with the collection, or leave the snapshot one
no search would use. It does not start while indexing or scanning runs and abandons when either
starts; a snapshot too stale to use is then dropped so it stops holding replaced records. Nothing is
locked while reading or evaluating: publication swaps a pointer under the summary lock and releases the
old snapshot after it. A collection whose snapshot would pass a 1 GiB ceiling keeps none and scans.
Each snapshot published asks the sidebar to count again, since it may have counted by scanning. The
performance report's Search index rows show how many searches it answered and how long reads took and
how much memory they hold.

Sidebar rows and address-box predictions are counted through `search_counter`, which lists the
collection once for a whole pass. Its evaluator keeps each term's items and the items through each
presence requirement for the pass, so the searches of a pass share them, and a count reads only the
folders the snapshot cannot answer for. A sidebar pass counts from the snapshot only while few items
have changed since it was read, since every row would read each of them; past that it counts by
scanning. Prediction counts are budgeted per keystroke and stopped by the next one. When the
snapshot cannot answer cheaply - there is none yet, too many items changed, the search walks a folder
tree, or it is a related search - the count is left out rather than scanned, and a folder search with a
term is never counted, since that would read the folder's unread files on every keystroke.

### Thumbnail Pipeline

Layout dimensions, encoded thumbnail, decoded near-viewport surface, and GPU texture are separate
representations. SQLite stores the bounded encoded form. Visible items query it in batches before a
source scan; cloud-only files request provider thumbnails without hydrating source media.

Encoded and decoded retention is viewport-aware and bounded. Eviction re-arms the cheaper database
path and preserves layout dimensions. Claims are taken before queueing and released on every
completion path; publication and texture staging use separate generations.

## Files, Media, And Rendering

[File I/O](file-io.md) owns progressive image display and writes, [Metadata](metadata.md) tag
mapping, and [Rendering](rendering.md) image budgets, video presentation and graphics fallbacks; the
draw backends and device lifetime are libdesktop's. FFmpeg sessions decode off the UI thread and
publish generation-checked frames; graphics upload remains UI-owned.

A native tune reaches the player through [av_pcm.cpp](../src/av_pcm.cpp), a shared WAV stream that is
never written. [av_sid.cpp](../src/av_sid.cpp)
runs the tune in lib64's Commodore 64 as the decoder's read thread asks for samples, so the
session's clock, seeking, pausing and end are the ordinary ones. The render keeps each PAL frame's SID
registers beside its samples for the SID monitor ([ui_sid.cpp](../src/ui_sid.cpp)) to read on the UI
thread. Reads are kept short and the WAV demuxer is asked for small packets, so a muted voice is heard
to stop within half a second. The shared stream describes the original tune at the decoder boundary,
so sessions, metadata and monitors never expose the generated WAV's codec or bit rate. For other audio
the visualizer marks the presented position; native monitors use `update_for_present`
(`av_session::mark_presented`).

A SID tune's length is not in its file, so the stream is declared with room for any tune and the render
holds where the tune ends within it: three minutes until a `work` worker's `find_sid_tune_end` says
otherwise. That runs the tune in a second machine with its sound off - well over a hundred times faster
than it plays - until nothing sounds for three seconds, or until a window of frames, each reduced to its
voices' gates, waveforms and nearest notes, repeats and carries on repeating for a whole period and
fifteen seconds more; results are remembered per file and tune. Samples past the end are silence, so
the session never learns of it: `display_state_t::media_end` gives the transport the render's end, and
the tick ends the item there.

[av_tracker.cpp](../src/av_tracker.cpp) renders MOD, XM, S3M, IT and the other supported tracker
formats directly in libopenmpt, rather than its FFmpeg demuxer. All decoder entry points use the same
native bridge, including metadata and audio extraction. Stereo PCM and monitor snapshots retain only
four seconds; earlier seeks use libopenmpt's native seeking instead of retaining a whole song in
memory. Tracker output stays at 48 kHz floating point, preserving mixing headroom until the player's
gain is applied; title, artist, encoder, comment and date tags still reach the index. The comment is
the module's song message alone: where a module has none, libopenmpt stands its sample and instrument
names in for one, and those are the bank panel's to show. Pattern data is
published immutably with the audio position that produced it.

The monitor is drawn as the audio visualizer is, so a playing tune does not rebuild the view's scene for
every frame. Its waveforms, meters, lamps and the register grid's highlight are flat rectangles in a
`render::vertices` the scene draws by reference: the monitor places them when it draws (`sid_monitor_motion`),
and `update_for_present` brings them up to the session's position, after which the frame presents the
scene again. The values, notes and register contents beside them are text, which only rebuilding the
scene redraws, so they are redrawn ten times a second while they change - and not at all once the monitor
has not been drawn for two seconds, as when the view showing it has gone.

[av_c64.cpp](../src/av_c64.cpp) runs what a Commodore 64 listing's row names - a disk's or a tape's
program, a cartridge, a program file - in lib64's Commodore 64 on the emulator queue's worker. lib64's
autostart types the load and the `RUN` as a user would; the worker runs the boot and the load unpaced and
silent, then a PAL frame at a time, paced by writing its samples to the sound device, or by the clock
without one. `update_for_present` uploads each newer picture to the texture
[ui_c64.cpp](../src/ui_c64.cpp) draws, so a running machine presents its scene again rather than
rebuilding it; starting, failing and the first picture rebuild it. Keys reach the machine through
`view_state::c64_key_down` ahead of the application's own, and are let go when polling finds them up,
since key-ups are not delivered.

Subtitles ride the playing session ([av_subtitle.cpp](../src/av_subtitle.cpp)). The read thread decodes
the chosen embedded stream's packets as it demuxes them, or a sidecar whole when the session opens, into
events on the presentation timeline. It rasterises the lines near the playhead - text through
libdesktop's styled-text renderer, pictures through their palettes - and releases them once behind, so
memory follows the playhead rather than the film. The UI thread only uploads finished bitmaps and draws
them over the picture, scaled from the video's pixels; `update_for_present` rebuilds the scene once when
what is on show changes, with or without a new frame, since a replay would present the lines it was
recorded with. A seek drops the stream's earlier events by generation; a sidecar's are kept. The choice
of track belongs to the display, so a change of audio track keeps it.

Long operations execute immutable reviewed plans. Each item records success, failure, skip, or
cancellation. Cancellation stops future work and does not claim to undo completed effects.

## Persistence And Network

Settings use libdesktop's settings store. SQLite data, thumbnails, import history, web cache, and map
tiles are rebuildable. User metadata belongs in media or sidecars.

Updates, crash reports, dictionaries, maps, and location lookup are separate network capabilities.
Transport is libdesktop's web client; application settings decide whether a capability is used.
Feature-use and performance counters are relaxed aggregate diagnostics and never control behavior.

## Crash-Loop Protection

Graphics crash guards disable only the risky capability whose durable marker survived a crash
([Rendering](rendering.md#resilience)). `crash_files_db` records files open on a faulting scan or
picture-hash thread and skips them for the current release line during unattended indexing and
duplicate hashing. Explicit user opens are still
attempted.

Two unsettled starts in a row make the next a safe start: it resets presentation settings and turns
off GPU rendering and hardware decode before the first window, so that window can explain the
recovery. Collection roots and user metadata are retained. Crash report contents and symbolization
are owned by [crash.md](crash.md).

## Where this lives

- [app.cpp](../src/app.cpp), [app.h](../src/app.h), and [app_workers.cpp](../src/app_workers.cpp):
  process, frame, queues, and invalidation; [app_toolbar.cpp](../src/app_toolbar.cpp): the bars built
  from each view's declaration and how they are arranged.
- [model.cpp](../src/model.cpp), [model.h](../src/model.h),
  [model_items.cpp](../src/model_items.cpp), and [model_items.h](../src/model_items.h): view and item
  state.
- [model_index.cpp](../src/model_index.cpp), [model_search.cpp](../src/model_search.cpp),
  [model_postings.h](../src/model_postings.h), and [model_db.cpp](../src/model_db.cpp): index, search,
  candidate sets, and persistence; [model_search_index.cpp](../src/model_search_index.cpp): the
  search index snapshot and its evaluation.
- [model_index_duplicates.cpp](../src/model_index_duplicates.cpp): duplicate prediction and presence;
  [model_index_summary.cpp](../src/model_index_summary.cpp): the summary, its vocabulary, and
  auto-complete; [crypto.h](../src/crypto.h): checksums and the perceptual image hash.
- [model_tile_cache.cpp](../src/model_tile_cache.cpp): separately owned map tile database.
- [av_sid.cpp](../src/av_sid.cpp) and [ui_sid.cpp](../src/ui_sid.cpp): SID tunes played through
  lib64, how long each lasts, and the SID monitor.
- [av_c64.cpp](../src/av_c64.cpp) and [ui_c64.cpp](../src/ui_c64.cpp): the Commodore 64 run in the
  media pane, its keyboard, the listing whose rows start it, and its screen.
- [av_subtitle.cpp](../src/av_subtitle.cpp): subtitles - styling, placement, the events a session
  keeps, and the choice of track.
- [ui_view.h](../src/ui_view.h), [ui_elements.h](../src/ui_elements.h), and
  [ui_controllers.h](../src/ui_controllers.h): view framework, on libdesktop's element tree
  ([ui/elements.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/ui/elements.h)).
- [ui_document.h](../src/ui_document.h), [model_document.h](../src/model_document.h), and
  [view_list.h](../src/view_list.h): the document panel adapters and task-table policy over
  libdesktop's document views, table elements and shared column layout.
- [util_crash_files_db.h](../src/util_crash_files_db.h): media crash-loop protection;
  [app_settings.cpp](../src/app_settings.cpp): the safe start's settings.
- libdesktop's [core.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/core.h):
  shared vocabulary and utilities; [platform.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform.h):
  operating-system services, tasks and the application loop;
  [render.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/render.h): draw and
  measure contexts, rendering backends and textures; [ui.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/ui.h):
  frames, the element tree, controls, menus and text views.
