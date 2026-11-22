# Diffractor Implementation

This document owns architecture, source organization, data flow, threading, ownership, invalidation, and
index, search and database internals. [Design](design.md) owns observable behavior; the subject documents
in the [ownership table](../AGENTS.md#information-ownership) own theirs. Source owns exact APIs and enums.

## System Shape

Diffractor is a C++20 media organizer on libdesktop's platform, render and UI libraries. SQLite holds a
rebuildable index and thumbnail cache; media files and sidecars stay authoritative. libfiles scans every
file, reads and writes its metadata, and decodes and encodes most still formats. FFmpeg decodes audio,
video and the stills libfiles is not built to decode here - GIF and TIFF among them - and copies a video's
or sound file's packets into a new file for Edit's lossless edits. lib64 emulates the Commodore 64 that
plays SID tunes and runs disks, tapes, cartridges and programs. libmug finds faces and makes each a vector
for [face search](faces.md). Diffractor's own `platform*` files hold only what is the application's: its
side of hardware decoding, the screen recorder, its resource identifiers and its Linux stand-ins.

## Source Organization

| Prefix | Responsibility |
| --- | --- |
| `app*` | Process coordination: the frame, commands, settings, workers, text and translations, sidebar, toolbars and bars, dialogs, agent sessions, and the headless command-line tools |
| `model*` | State, items, index, search, database, properties and dates, locations, visits and Explore, the Movie document, and faces |
| `view*`, `ui*` | Views and task views, controls, layout, input, and drawing, on libdesktop's element tree |
| `files*`, `metadata*` | Diffractor's side of libfiles: format detection, scanning, decode and encode, EXIF/IPTC/XMP/ICC |
| `media_edit.cpp` | A photo's edits applied to its pixels |
| `av*` | Audio and video sessions, playback, subtitles, SID tunes, tracker modules, the running Commodore 64, and the visualizer |
| `render*` | Software rasterisers for the globes, the sidebar's charts and panoramas; the draw backends are libdesktop's |
| `crypto*` | Hashes and checksums, including the perceptual image hash |
| `platform*` | The application's own Windows and Linux glue: FFmpeg hardware decoding, the screen recorder, resource identifiers and Linux stand-ins |
| `util*` | Shared value types and bounded helpers |
| `test*` | Embedded unit and regression tests, arranged by [testing](testing.md) |
| `pch.h` | The precompiled header: libdesktop's UI umbrella and the headers every file uses |

The boundaries the lint enforces - platform code in `platform*` files and libdesktop, `sqlite3_*` in the
database owners, a `// Purpose:` comment in every file - are AGENTS.md's
[anti-patterns](../AGENTS.md#strict-code-anti-patterns).

## State And Views

`view_state` owns scope and query, results, grouping, selection, focus, display state, navigation
history, playback and the active view; `app_frame` coordinates windows, commands, workers and
invalidation. Items is the windowed browser; fullscreen reuses the persistent media renderer without
replacing the logical browsing state.

The window holds three frames and draws only the background between them: the title and status bars
(`bar_frame`, on the CPU) and the view frame (`view_frame`, on the GPU where it can). The view frame
composes the view, the task view's panel - whose controls are elements of its tree - and the selector
strip into one surface, and a dialog's map widget shares its dialog's surface the same way: no native
child window or device, but local input, focus and coordinates. Menus, tooltips and autocomplete lists
are separate popup windows. Full screen and a zoomed picture give the view frame the whole window.

Each view declares what the title and status bars carry for it (`view_base::bars`). `app_frame` builds
one set of toolbar controls per distinct declaration before the first layout, updates them with command
state, and lays out only the active view's, the status bar's trailing set first so a short bar gives up
its leading one. The status bar paints the status text and totals from in-memory state; the selection
summary is Items' Open button's label. A view never creates, positions or updates a toolbar.

Views, controls, hosts, controllers and `item_element` are UI-owned. A host may outlive its native window,
so `view_host::frame()` answers `ui::no_frame()` when unattached. Visibility gates painting, not attachment
or model updates.

Rebuilt elements keep what the user made of them. The selection panel's file head keeps its text
selection and document control (`ui_document`, over libdesktop's document panel); a table remade with the
panel keeps its sizes and sort on the display by name (`keep_table_choices`); a detail group's live in
`view_state` by group key, which `item_group::sort` orders by while its headings show; the task review
(`review_list`) keeps its rows, sort keys and selection (libdesktop's `list_selection`). Faces' panes keep
their selection by what each face shows and each group's key; Movie's selection is the project's, saved
with it and never empty. Headed tables draw their headings through libdesktop's `table_headings.h`, and
rubber bands and drags - in Items, Faces, Explore's weeks, the selector strip and Movie's timeline - use
its `is_drag` and `edge_scroll`.

## Hit Testing

One pointer position produces one controller through a single ordered walk of libdesktop's
[element tree](https://github.com/ZacWalk/libdesktop/blob/main/docs/ui.md#the-element-tree); precedence is
the order of the walk, and nothing restates it afterwards. A controller's `bounds()` is its own region -
what it paints over, and where a release must land - never narrowed by what covers it, or it drops clicks
and hover on the covered part. A release reaches the controller under the pointer whether or not it saw
the press (the press that closed a menu went to the menu), so a controller acts on a release only after
its own press.

`hit_test_context`, threaded through the walk and owned by `view_host`, bounds how far an answer travels:
a higher-precedence region tested and passed over is cut out, so reaching it tests again, and since
recording is a side effect of testing, no region is tested but forgotten. The host caches
`bounds() ∩ stability()`; an empty result re-tests on every move.

Tooltip anchors and hover regions start in the owning frame's client coordinates; the popup anchor is
converted to screen coordinates through that frame exactly once, and the hover region stays
client-relative. Native child-window bounds are parent-relative, not a desktop origin. A view host opens a
menu at its bounds converted through its frame's `client_to_screen`; a right-click, which arrives as a
screen point, reaches the view by the view surface's screen origin.

A bubble shows once the pointer has dwelt for its target's delay (`bubble_delay`; half a second for help),
observed after input and queued UI work in monotonic milliseconds (`pointer_dwell`). The dwell allows four
logical pixels of jitter round where it started, not round the last sample, so slow drift restarts it;
sidebar and scroll-bar entries follow the pointer instead. Idle ticks complete a dwell without another
mouse event, and refreshed content keeps its target's timer.

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

UI execution is disjoint from file I/O, decoding, database access, indexing, hashing, queries, maps and
network work. `async_strategy` ([model.h](../src/model.h)) owns the named queues (`async_queue`) and the
database, tile-database, location and media-preview queues; `state_strategy` carries `view_state`'s
notifications to the frame. Tests substitute null or deferred strategies.
[app_workers.cpp](../src/app_workers.cpp) gives each queue its threads, several only on first use.

Long work an agent starts - a Movie render, the duplicate report - runs on `agent`, a thread of its own, so
the window's `work` queue never waits behind it ([agents](agents.md)); a running Commodore 64 holds
`emulator`. Face search's scan reads pictures in order on `faces` and decodes, detects and embeds them on
`faces_work` ([faces](faces.md#32-the-scan)); its overview of people and suggestions runs on
`faces_overview`, so a pass never holds back the scan, and `faces_work` threads take pieces of a pass's
comparisons when free ([faces](faces.md#7-recognizing-people)). The presence, sidebar, prediction and
summary passes share one thread, `index_compute`; the search index is read on its own.

Workers consume moved values or immutable snapshots and publish detached results through `queue_ui`.
Publication checks lifetime and currency: path, scope, generation, request identity or content revision as
appropriate. A `weak_ptr` crosses a queue only as an opaque lifetime token, not locked until execution is
back on the UI queue, and a UI-owned object a worker might release last travels as a `ui_owned_ptr`, which
hands the final reference back to the UI thread.

No I/O, callback, sorting, decoding or unbounded allocation runs under an index lock. Stale work is
cancelled or superseded, and completion releases every claim even when the result is discarded.

### SQLite Connection Ownership

SQLite is built in multi-thread mode (`SQLITE_THREADSAFE=2`), so each connection has one owning queue. The
index and the face database (`diffractor-people.db`, which the index's `database` opens) use the database
queue; `map-tiles-cache.db` uses the tile-database queue. `database` and `tile_cache_db` assert their owning
thread in Debug. A new connection requires its own documented execution owner.

### Synchronized Types

Mutable objects have one owning context unless listed here. Besides execution primitives, app command
coordination, media sessions and the lock a hardware-decoded picture lends the renderer, these are the
approved synchronized objects; atomic reference counting synchronizes lifetime only. Adding one requires
naming every owning context, the protected invariant, and why moved results or immutable publication are
insufficient.

- **Index records.** `item_element` is entirely UI-owned. `index_file_item`'s atomic metadata pointer
  publishes complete immutable snapshots, never mutated once published. For `index_folder_item`, the index
  worker publishes replacement nodes through `index_items` while scan and database workers update per-file
  atomics in place; a content revision lets replacement reject a node built before an in-place update,
  since cloning a folder for every metadata update would copy large vectors and put scanning back under
  publication contention. A search revision rises with every in-place update except to values derived from
  an item's bytes - duplicates, checksums, picture hashes - so the search index can tell captured contents
  that changed from a derived value it reads live; one counter cannot serve both, as replacement must still
  reject a node built before a derived update.
- **Indexing progress** is an immutable snapshot the index worker publishes atomically, total and
  remaining read together; routing each paint through the index queue, or a UI callback per scanned file,
  would block Paint on Source work or flood invalidation.
- **Scan admission** (`scan_io_gate`): the file and face scans share a gate for disk work that counts
  waiting file indexers as well as running ones, so face reads cannot keep the index waiting; a face read
  and its queued decode share one permit, since a decoder can fall back to reading the file
  ([faces](faces.md#32-the-scan)). No index lock or admission mutex is held during I/O.
- **The visible thumbnail plan**, a queue of scan requests the UI thread replaces and extends and the
  thumbnailing thread takes from, is under one short lock that also counts the reads waiting on decode
  threads and the requests taken but incomplete, so the listing regroups once when the last completes.
  Each item's loading claim belongs to one request - planned, being scanned or finished - and replacing the
  plan returns the requests not yet reached, releasing their claims at once, which a batch moved to a
  worker by value could not.
- **Map tiles.** Each tile's entry - surface, whether the view shows it and its rank, fetch claim, retry
  time - has a lock of its own; the tiles waiting for the network are a list under another, always taken
  first. A fetch rereads its entry before each step - store, network, decode - so a tile the view has left
  is let go and its claim released, where a request moved by value would download every tile a zoom passed
  through. The entries' map changes only on the UI thread, and the globe's bands read it while the UI
  thread waits for them ([locations](locations.md#tile-cache)).
- **Cancellation versions.** `view_state`'s `_visits_generation` and the address box's `_count_version`
  are atomic only so a worker can see through `core::cancel_token` that its pass was superseded; the
  location worker still publishes a complete `visit_timeline` to the UI thread, and a superseded count
  publishes nothing. A prediction's face icons, loaded on the database and thumbnail-decode queues, share
  the count's version: they stop on a newer edit or a closed popup, and only the current list receives the
  crops, without rebuilding its rows.
- **The search index** is one immutable snapshot: built on its own queue with no lock held, swapped by
  `index_state` under the summary lock, copied by readers under that lock and evaluated with none. One
  updated in place would need a lock across every evaluation that indexing writers then wait on.
- **Process-wide values.** A translation, with a plural text's extra forms, is an atomic pointer into
  storage `app_text_t` only appends to, so a worker formatting text across a language switch reads the old
  string or the new, never one being rewritten; the strings are read at too many call sites to hand each
  worker a snapshot. The file-group spellings `parse_file_group` accepts are an immutable table a language
  switch copies, extends and swaps whole. `df::last_loaded_path` is an atomic value decoding workers write
  and the crash handler reads, since a handler waiting on a lock the faulting thread held would hang.
- **Native music renders** are shared by the player's read thread and the UI. A render lock owns the SID
  emulator or tracker decoder and the PCM cache; a short data lock publishes complete chip or pattern
  snapshots and channel-mix requests, since the UI never calls a live decoder, which must continue from its
  preceding audio block. A tracker snapshot carries the patterns either side of the one playing, formatted
  once on the render thread, and how far through its row its moment is; libopenmpt places a module only at
  a row's start, so a seek plays on from there unheard to the sample it asked for.
- **Movie's SID renders.** An audio clip's readers - its frames, drawn on the render queue or the render's
  worker, and the decoders reading its sound - share one render per tune of each file through a map of weak
  references under a lock of its own: a tune only plays forward, so a reader with its own render would
  emulate it again from its start. A clip's frames are drawn into a display and monitor made for it, with a
  software draw context whose text engine the drawing worker made ([movie](movie.md#8-ownership-and-limits)).
- **A running Commodore 64** (`c64_session`): the emulator queue's worker alone touches the machine. A
  short frame lock publishes each finished picture whole - the worker writes two surfaces in turn and
  replaces, rather than reuses, one the UI still holds - and a short input lock hands key changes the other
  way; the joystick, its port, the volume, RESTORE, the status and the stop request are atomics. Immutable
  publication alone cannot carry input into a machine that runs on between pictures.
- **Faces.** The face store has one writer, and readers see only what it has published; the face book
  takes a reader-writer lock and publishes its edits in commit order ([faces](faces.md#52-the-face-store)).

## Index, Search, And Database

Startup loads cached records, validates collection roots, scans stale files, queues database writes,
and rebuilds summaries, duplicate predictions, presence, and search candidates. Cached results may
publish before extraction completes.

The index keeps the metadata of every file type whose traits include `cache_metadata`: photos, video,
audio, and Commodore 64 disk, tape, cartridge and program images, described by their listing. Other files
are known by name, size and dates alone. `db_metadata_version` in [model_db.cpp](../src/model_db.cpp)
names what a cached record holds. Every version since 3 states in `metadata_changes` which kinds of file it
changed, and a test holds that list to the current version, so an upgrade asks only those kinds to be read
again while their old rows answer meanwhile: never a full re-index by omission, and never a re-read that
loses the face records rows carry.

A cached record keeps what a file states, not what Diffractor makes of it, so a later release that
corrects a judgment applies it to every row as it loads, with nothing read again: a face region is kept as
the file places it, with the dimensions it was measured on, and turned upright - or found stale - as it is
unpacked ([faces](faces.md#22-where-a-region-is)); a file's colour is kept as its ICC profile's name and the
H.273 primaries and transfer it names, from which its profile and HDR are read as the record loads; a
found face keeps its measures, and its quality is judged from them as the record is read
([faces](faces.md#44-quality)). A record a release adds describes its own shape - a version, and lengths a
reader steps over - so a later release can add a field without the collection being read again.

The `item_properties.properties` blob stores versioned binary property records, whose format and private
IDs live in [model_db_pack.h](../src/model_db_pack.h) and [model_property.h](../src/model_property.h).
Private cache IDs are separate from the public property registry, so opaque values such as the face cache
never become EXIF/XMP fields or UI metadata. Metadata-only writes keep unknown records from the item's
previous blob; binary-property edits keep all other records byte for byte.

Everything in the index can be made again by reading files, so it is deleted and rebuilt freely. What the
user tells Diffractor about faces cannot, and is kept in a database of its own beside it
([faces](faces.md#64-the-face-database)), which resetting the index leaves alone.

Search parses into exact matchers and conservative candidate prefilters; negation and OR never create a
prefilter that could omit an exact result. Query generations prevent stale publication. Folder-scoped
searches validate their filesystem scope; index-wide searches answer from cached records, through the
[search index](#search-index) where it is current. In-app file operations report every changed source and
destination folder, so search refresh does not depend on a live watcher.

The summary keeps complete tag histograms and word counts, and counts each `@` word - a media type, a kind
such as `@raw` or `@animated`, a flag such as `@duplicates` - by the items it finds, so the vocabulary
offers a word only where it finds something. Tag-companion hints for typeahead are bounded per item: an item
with more distinct tags than the companion budget contributes only its first bounded set, and nothing else
is capped, so companion scores never depend on folder hash order and counts, summaries and exact tag
matching stay complete.

Duplicate prediction narrows by cheap metadata and CRC, then requests perceptual hashes only for
unresolved candidates; every attempted hash reaches a terminal state. Duplicate search, related items and
presence consume one duplicate relation, so they cannot disagree about identity
([collections](collections.md#7-redundancy-one-relation-three-questions)). Related-item collection scores
the index against one anchor and keeps bounded best results per relation; counting and display share the
collector and its stable path tie-breaks.

### Search Index

The search index is an immutable in-memory reverse index of the collection, read from the cached records
rather than the database, so it needs no schema change and no re-index. Items are numbered in path order.
Each distinct name, free-text value, formatted value, split part and tag part maps to the items holding it,
and a trigram index over the distinct text values suggests which of them hold a free-text term. The names
of the people items name are kept apart, each asked once whether a free-text term begins one of its words,
and the names the face book gives a picture's faces are asked of the matcher's own snapshot, as
`match_term` asks it. A search's terms become item sets combined by `combine_search_terms`, the steps the
matcher applies to one item, and the matcher's own predicate confirms every suggestion. Rating and label
terms are answered once per group of items sharing the value; date terms compare each day once, through the
`date_term_keys_of` and `date_term_found` the matcher uses too; a term no table can answer, such as a size
comparison, is answered by `match_term` over each item's captured metadata. An answer from the index is
therefore the answer a scan gives, and the index tests compare the two over representative searches.

Each folder entry keeps the folder node and search revision it was read from; a folder changed since is
scanned on top of the snapshot. Duplicates, checksums, picture hashes and presence are always read live, so
recording them leaves a folder current. A search uses the snapshot while at least half the collection is
unchanged, and otherwise scans. Formatted values depend on the language and number format, and date keys on
the time zone and the day; when one changes, those terms are answered item by item until the next read, and
a new day reads the collection again.

Reading never delays indexing or a search. It runs on its own thread (`index_search_single`), not the
`index_compute` thread the summary and prediction passes share, a debounced second after them, and only once
changes since the last read pass a tolerance that grows with the collection or leave the snapshot one no
search would use. It does not start while indexing or scanning runs and abandons when either starts; a
snapshot too stale to use is then dropped, so it stops holding replaced records. Nothing is locked while
reading or evaluating: publication swaps a pointer under the summary lock and releases the old snapshot
after it. A collection whose snapshot would pass 1 GiB keeps none and scans. Each snapshot published asks
the sidebar to count again. The performance report's Search index rows show how many searches it answered,
how long reads took and the memory they hold.

Sidebar rows and address-box predictions are counted through `search_counter`, which lists the collection
once for a whole pass and shares each term's items, and the items through each presence requirement,
among the pass's searches; a count reads only the folders the snapshot cannot answer for. A sidebar pass
counts from the snapshot only while few items have changed since it was read, and otherwise by scanning.
Prediction counts are budgeted per keystroke and stopped by the next. Where the snapshot cannot answer
cheaply - there is none yet, too many items changed, the search walks a folder tree or is a related search -
a prediction's count is left out rather than scanned, and a folder search with a term is never counted,
since that would read the folder's unread files on every keystroke.

### Thumbnail Pipeline

Layout dimensions, encoded thumbnail, decoded near-viewport surface and GPU texture are separate
representations; SQLite stores the bounded encoded form. Visible items query it in batches before a
source scan, and an item is not scanned while its query is unanswered; cloud-only files request provider
thumbnails without hydrating source media.

A view hands its visible working set to the plan in the order it wants the thumbnails: what the viewport
shows, centre first, then the band beyond the edge it is scrolling towards, then the band it is leaving.
Each scroll step replaces the plan, and so does each answer from the database, in the order the view last
asked for; what the old plan held but had not started is planned again in the new order, so nothing waits
behind rows already scrolled past.

One thread, the thumbnailing thread (`scan_displayed_items`), reads every source the plan asks for, a file
at a time in plan order. A still is read whole and handed to the `thumbnail_decode` threads, which scan,
decode, scale and encode it while the next file is read; video, audio and RAW, which read as they decode,
are scanned on the thumbnailing thread itself. Reads waiting for a decoder are bounded in number and bytes,
and reading stops until one is free. A HEIF's own thumbnail item is used without decoding the whole
picture. A single thumbnail asked for outside a plan - for a tooltip, the sidebar or a dialog - and the
rescan of a modified file read on their own queues.

Encoded and decoded retention is viewport-aware and bounded. Eviction re-arms the cheaper database path
and keeps layout dimensions. Claims are taken when a request is made and released on every completion path;
publication and texture staging use separate generations. A thumbnail the database holds is decoded into
the surface its tile draws on the `thumbnail_stage` threads: a folder opening asks for hundreds at once, and
the render queue's placeholder for the picture opened next must not wait behind them. A scan that decoded
the surface its thumbnail was encoded from hands it to the item in the
form staging would produce, so the tile draws without the thumbnail being decoded again; a hovered video's
frame is drawn from the surface it was scaled to in the same way.

## Files, Media, And Rendering

[File I/O](file-io.md) owns progressive image display and writes, [Metadata](metadata.md) tag mapping,
and [Rendering](rendering.md) image budgets, video presentation, subtitles and graphics fallbacks; the
draw backends and device lifetime are libdesktop's. FFmpeg sessions decode off the UI thread and publish
generation-checked frames; graphics upload stays UI-owned. Long operations execute immutable reviewed
plans: each item records success, failure, skip or cancellation, and cancellation stops future work
without claiming to undo completed effects.

A native tune - a SID tune or a tracker module - reaches the player through [av_pcm.cpp](../src/av_pcm.cpp),
a shared WAV stream that is never written, so the session's clock, seeking, pausing and end are the
ordinary ones. The stream describes the original tune at the decoder boundary, so sessions, metadata and
monitors never expose the generated WAV's codec or bit rate. [av_sid.cpp](../src/av_sid.cpp) runs the tune
in lib64's Commodore 64 as the decoder's read thread asks for samples, and keeps each PAL frame's SID
registers beside its samples for the SID monitor ([ui_sid.cpp](../src/ui_sid.cpp)) to read on the UI
thread. Reads are kept short and the WAV demuxer asks for small packets, so a muted voice is heard to stop
within half a second.

A SID tune's length is not in its file, so the stream is declared with room for any tune and the render
holds where the tune ends within it: three minutes until a `work` worker's `find_sid_tune_end` says
otherwise. That runs the tune for up to thirty minutes of its time in a second machine with its sound off -
nearly two hundred times faster than it plays - each frame run to its own cycle boundary, since lib64's
`exec` counts from wherever its last run stopped and frames asked for one at a time drift against the play
routine's interrupt. Each frame keeps each voice's gate, waveform and nearest note, and an envelope followed
from the ADSR registers, a gate written more than once in a frame taken as a note started again. A tune ends
when nothing has sounded for twenty seconds, at the moment its last note died away; or when a window of
frames repeats and carries on repeating, each voice matching a frame either side of a period before with no
more than a tenth missed over any five seconds, for a whole period and a minute more. The end is then where
the repeat began, taken back over a first pass that matches at least four fifths, and where the tune fell
silent if a second of silence leads into it. A sound held unchanged for a minute repeats on the shortest
period. A scan that reaches thirty minutes takes the shortest period it kept finding whose last two whole
passes each match the one before at least 85%. Results are remembered per file, by its content, and tune.
A repeating tune fades out over three seconds as it starts again, and samples past the end are silence, so
the session never learns of it: `display_state_t::media_end` gives the transport the render's end, and the
tick ends the item there. What the user hears is [design](design.md#selection-and-viewing)'s.

[av_tracker.cpp](../src/av_tracker.cpp) renders MOD, XM, S3M, IT and the other supported tracker formats
in libopenmpt, not through its FFmpeg demuxer, for every decoder entry point including metadata and audio
extraction. Stereo PCM and monitor snapshots keep four seconds, earlier seeks using libopenmpt's own; output
stays 48 kHz floating point, keeping mixing headroom until the player's gain; title, artist, encoder,
comment and date reach the index. The comment is the module's song message alone - where there is none,
libopenmpt stands the sample and instrument names in, and those are the bank panel's to show. Pattern data
is published immutably with the audio position that produced it.

The SID monitor is drawn as the audio visualizer is, so a playing tune does not rebuild the view's scene for
every frame. Its waveforms, meters, lamps and the register grid's highlight are flat rectangles in a
`render::vertices` the scene draws by reference: the monitor places them when it draws
(`sid_monitor_motion`), and `update_for_present` brings them up to the session's position before the frame
presents the scene again. The values, notes and register contents beside them are text, which only
rebuilding the scene redraws: ten times a second while they change, and not at all once the monitor has not
been drawn for two seconds. For other audio the visualizer marks the presented position
(`av_session::mark_presented`).

[av_c64.cpp](../src/av_c64.cpp) runs what a Commodore 64 listing's row names - a disk's or a tape's program,
a cartridge, a program file - on the emulator queue's worker. lib64's autostart types the load and the `RUN`
as a user would; the boot and the load run unpaced and silent, then a PAL frame at a time, paced by the
sound device, or by the clock without one. `update_for_present` uploads each newer picture to the texture
[ui_c64.cpp](../src/ui_c64.cpp) draws, so the scene is presented again rather than rebuilt; starting,
failing and the first picture rebuild it. Keys reach the machine through `view_state::c64_key_down` ahead of
the application's own, and are let go when polling finds them up, since key-ups are not delivered.

Subtitles ride the playing session ([av_subtitle.cpp](../src/av_subtitle.cpp)). The read thread decodes the
chosen embedded stream's packets as it demuxes them, or a sidecar whole when the session opens, into events
on the presentation timeline. It rasterises the lines near the playhead and releases them once behind, so
memory follows the playhead rather than the film; the UI thread only uploads and draws finished bitmaps
([rendering](rendering.md#subtitles)). A seek drops an embedded stream's earlier events by generation; a
sidecar's are kept. The choice of track belongs to the display, so a change of audio track keeps it.

## Persistence And Network

Settings use libdesktop's settings store. The index's SQLite data - properties, thumbnails, import
history and web cache - and the map tiles are rebuildable; the face database is not
([faces](faces.md#64-the-face-database)). User metadata belongs in media files or sidecars.

Update checks, crash and Support reports, spelling dictionaries and map tiles are separate network
capabilities over libdesktop's web client. Application settings decide whether each is used, and
`df::network_allowed`, which an agent session clears unless allowed `network`, withholds them all.
Feature-use and performance counters are relaxed aggregate diagnostics and never control behavior.

## Crash-Loop Protection

Graphics crash guards disable only the risky capability whose durable marker survived a crash
([Rendering](rendering.md#resilience)). `crash_files_db` records the files the faulting thread had open
during a scan, thumbnail read or picture hash - or every open file when that thread had none, as in Windows'
recovery callback, which runs on its own - and skips them for the rest of the release line (the major
version) during unattended indexing, thumbnailing and duplicate hashing. Explicit user opens are still
attempted.

Two unsettled starts in a row make the next a safe start: it resets presentation settings and turns off
GPU rendering and hardware decode before the first window, so that window can explain the recovery.
Collection roots and user metadata are retained. Only the process holding the startup scope counts its
start, so overlapping launches cannot add up to a history that never happened, and an agent session counts
none and leaves the graphics crash guards alone. Crash report contents and symbolization are owned by
[crash.md](crash.md).

## Where this lives

- [app.cpp](../src/app.cpp), [app.h](../src/app.h) and [app_workers.cpp](../src/app_workers.cpp):
  process, frames, queues and their threads, and invalidation; [app_toolbar.cpp](../src/app_toolbar.cpp):
  the bars built from each view's declaration, how they are arranged, and the tooltip dwell.
- [model.cpp](../src/model.cpp), [model.h](../src/model.h) - `async_strategy`, `state_strategy`,
  `ui_owned_ptr` - [model_items.cpp](../src/model_items.cpp) and [model_items.h](../src/model_items.h):
  view and item state; [util_interfaces.h](../src/util_interfaces.h): `async_queue`.
- [model_index.cpp](../src/model_index.cpp), [model_search.cpp](../src/model_search.cpp),
  [model_postings.h](../src/model_postings.h) and [model_db.cpp](../src/model_db.cpp): index, search,
  candidate sets and persistence; [model_search_index.cpp](../src/model_search_index.cpp): the search
  index snapshot and its evaluation.
- [model_index_duplicates.cpp](../src/model_index_duplicates.cpp): duplicate prediction and presence;
  [model_index_summary.cpp](../src/model_index_summary.cpp): the summary, its vocabulary and
  auto-complete; [crypto.h](../src/crypto.h): checksums and the perceptual image hash.
- [model_tile_cache.cpp](../src/model_tile_cache.cpp): the separately owned map tile database;
  [model_db_faces.cpp](../src/model_db_faces.cpp): the face database, which keeps the user's face book
  apart from the rebuildable index ([faces](faces.md#64-the-face-database));
  [model_faces_search.cpp](../src/model_faces_search.cpp): the face scan and its admission gate beside the
  file scan.
- [av_pcm.cpp](../src/av_pcm.cpp): the WAV stream native tunes play through; [av_sid.cpp](../src/av_sid.cpp)
  and [ui_sid.cpp](../src/ui_sid.cpp): SID tunes played through lib64, how long each lasts, and the SID
  monitor; [av_tracker.cpp](../src/av_tracker.cpp): tracker modules through libopenmpt.
- [av_c64.cpp](../src/av_c64.cpp) and [ui_c64.cpp](../src/ui_c64.cpp): the Commodore 64 run in the
  media pane, its keyboard, the listing whose rows start it, and its screen.
- [av_subtitle.cpp](../src/av_subtitle.cpp): subtitles - styling, placement, the events a session
  keeps, and the choice of track.
- [ui_view.h](../src/ui_view.h), [ui_elements.h](../src/ui_elements.h) and
  [ui_controllers.h](../src/ui_controllers.h): view framework, on libdesktop's element tree
  ([ui/elements.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/ui/elements.h)).
- [ui_document.h](../src/ui_document.h), [model_document.h](../src/model_document.h) and
  [view_list.h](../src/view_list.h): the document panel adapters and task-table policy over
  libdesktop's document views, table elements and shared column layout.
- [app_mcp.cpp](../src/app_mcp.cpp), [app_mcp_tools.cpp](../src/app_mcp_tools.cpp),
  [app_mcp_ui.cpp](../src/app_mcp_ui.cpp), [app_mcp_controls.cpp](../src/app_mcp_controls.cpp) and
  [app_mcp_faces.cpp](../src/app_mcp_faces.cpp): the agent session and its tools, which [agents](agents.md)
  owns.
- [util_crash_files_db.h](../src/util_crash_files_db.h): media crash-loop protection;
  [app_settings.h](../src/app_settings.h) and [app_settings.cpp](../src/app_settings.cpp): when a start is
  safe, and the settings it resets.
- libdesktop's [core.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/core.h):
  shared vocabulary and utilities; [platform.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform.h):
  operating-system services, tasks and the application loop;
  [render.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/render.h): draw and
  measure contexts, rendering backends and textures; [ui.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/ui.h):
  frames, the element tree, controls, menus and text views.
