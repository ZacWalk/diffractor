# Diffractor Product Design

This document owns durable user concepts and observable behavior. Specialized documents own the
details of collections, locations, zoom, selection controls, metadata, file I/O, and Movie
mixer. [Implementation](implementation.md) owns architecture; GitHub owns plans and issue status.

## Primary Design Drivers

1. **Fast, lightweight performance.** Start and respond quickly, scale to large collections, work
   progressively, and bound CPU, memory, storage, and network use.
2. **A clear user mental model.** Make scope, contents, command target, effect, and recovery
   predictable. Hidden targets and silent state changes are defects.

Neither driver overrides the other. Performance changes preserve behavior, and clearer workflows
must not introduce avoidable blocking or unbounded work.

## Product Promise

Diffractor is a private Windows organizer for photos, video, and audio on storage the user controls.
It finds, views, compares, describes, edits, and manages media without making its rebuildable index
the authority for user files or metadata.

## User Mental Model

Every user-visible interaction states four facts:

| Field | Closed values or meaning |
| --- | --- |
| Scope | Indexed collection, Folder, Recursive folder, External folder, Search, or justified All scopes |
| Contents | Query, filters, grouping, sorting, and visible/hidden counts |
| Target | Focused item, Singular displayed item, Complete visibly selected set, or Visible items |
| Effect | Action, affected count, destination/collisions, retained originals, and recovery |

`All scopes` is used only when behavior is provably identical in every concrete scope. Internal work
has no user scope or target. A behavior that cannot be expressed with this ontology requires product
clarification rather than a new alias or implicit state.

The target is always visible. Focus is the keyboard cursor and range anchor; selection is the
complete batch target; a displayed or pinned item does not silently enlarge it.

### Command Availability

A command is offered when its effect is visible on the current surface and its target qualifies.
Ineligible target commands retain a concrete disabled reason, which a toolbar button's bubble shows
beside the dimmed command; a menu dims the command without it. A submenu reflects the eligibility of
its entries. A platform capability that does not exist is absent rather than permanently disabled.
Configured external tools in the Open menu declare a singular `{item-path}` invocation, so they are
offered only when exactly one file is selected. Multi-file selections omit those entries; association
handlers that accept the whole selected set remain available when the selection qualifies.

Commands affecting list contents or presentation belong to Items. Commands affecting rendered media
belong to Items and Fullscreen. Task commands belong to their task view. Keyboard, toolbar, menu,
context-menu, and accessibility entry points share the same eligibility, target, confirmation, and
effect. The Items and media context menus open with the commonest commands on their target - Cut,
Copy, Rename and Delete, and Paste in Items - as a row of buttons, each named under its icon, which
are not listed again below.

## Vocabulary

- **Scope** supplies candidate items; a **query** narrows them; filters determine **visible items**.
- **Focus** is navigation state; **selection** is the complete visible batch target.
- **Pin** visibly holds one selected item while focus moves for comparison.
- **Preview** is non-committing presentation; **Analyze** computes a plan; **Run** executes the
  validated reviewed plan.
- **Presence** compares one file with collection membership and likely copies.
- **Relation** says why another collection item is connected: copy, album, series, capture time, or
  capture place.

### Naming Views, Modes, And Presentation Choices

Top-level views are Items, Fullscreen, Edit, Movie mixer, Tags, Locate, Rename, Convert, Metadata,
Date, Import, Sync, and Options. “Media view” names the internal Fullscreen renderer, not another user
view.

The user-entered modes are zoom mode and a running Slideshow. Inspect zoom is a gesture, not a mode.
Grouping, sorting, density, verbose metadata, and navigator display are presentation choices.
Singular, Comparison, and Selection summary are panel forms selected by the current selection.

## Application Structure

Items is the windowed browser: grouped thumbnails or details, optional media/metadata preview, and
independent list/preview scrolling. Fullscreen reuses the same logical scope, query, filters,
grouping, sorting, focus, selection, and media state. Leaving it restores Items without rebuilding
the browsing context.

Every windowed view sits between a title bar and a status bar, and neither bar scrolls. Fullscreen
and zoom mode hide both. In Items the title bar is the navigation bar: the address box between the
history and folder buttons. A task view's title bar names the task and offers Close beside the window
buttons. The status bar states the view's status and ends with the view's own commands,
right-aligned: the item list's controls in Items, and the task's Analyze, Run and the like in a task
view. In Items it starts under the sidebar with one toolbar: the Options cog, beside the overview of
the collection Options opens on; Tools and Playback options, as icons; then Open, labelled with the
selection's count and size, which hovering breaks down by type, and with its own name when nothing is
selected. Open, Tools and Playback drop down the same menus they do elsewhere. A short bar gives up its
leading entries before the view's commands. A running task shows its progress there, and Cancel
replaces its other commands until the run ends.

Task views expose their parameters beside the work surface and return to Items on task Close. Edit,
Locate, Tags, and Metadata can show a selector strip containing only items the task can act on.
Fullscreen is unavailable from a task view because it would hide the task's target and controls.

The item list owns its filters, recursion, grouping, sorting, density, and size controls. The filter
box, thumbnail size, media type filters, recursion, totals, and grouping sit in the status bar, so
they stay in reach however far the list scrolls; density is chosen per group. Totals and grouping are
separate affordances. Optional query-specific rows head the results and are navigation/presentation
only: they do not change grouping, sorting, focus, or selection. A captured control retains its value
and position while its result refresh is in flight.

Thumbnail rows share a height and preserve each image's aspect. A short trailing row remains short;
the layout does not crop images merely to fill width. Relayout preserves the focused visible item or
the content nearest the viewport center when possible.

The selection panel and responsive behavior are owned by [selection-controls.md](selection-controls.md).
Wheel and pinch routing are owned by [zoom.md](zoom.md#input).

## Options

Options is one page holding every setting the user can change, in the sections a user looks for, each
under a shaded header that names it and says what it covers, so the sections stand apart without a rule
between them:

1. **Your collection**: the folders it holds, what each holds, and index maintenance. First, because it is
   the first thing to set up.
2. **Viewing and playback**: how photos, videos and music show and play, and subtitles.
3. **Sidebar**: what it shows, and the favorite searches and tags it lists.
4. **Editing and saving**: the confirmations before a change, backup copies, and the quality files are
   saved at.
5. **General**: the language, help, hidden files, updates, and advanced settings.

It fills the window, each section's cards in as many columns as fit, between a title bar with Close and
a status bar ending with Help, Cancel, and Save options and close. One command opens it, Options, with
the cog icon and F6, wherever it is offered: the Options menu, the sidebar's context menu, the cog that
starts the Items status bar, and the sidebar's collection overview. It opens at the top, on the
collection. The one exception is the sidebar's own Customise sidebar link, which opens the page at its
Sidebar section.

The page edits a copy. Save options and close commits what the page edits and nothing else, and
returns to Items, as a task's Close does; Cancel discards the copy. Close, Escape, and closing
Diffractor discard it too when it holds no change, and otherwise ask first: save the changes, close
without them, or stay on the page. A change put back is no change. Media playing
when the page opened is paused while it shows and resumes after. Saving reads the collection again
only when its folders or what they hold changed. Index maintenance acts at once, behind its own
confirmation, because it acts on the index rather than on a setting.

Options has no item target, as the owner decided for it: settings apply everywhere, and no scope or
selection narrows them.

| Field | Options |
| --- | --- |
| Scope | All scopes: no scope narrows a setting |
| Contents | Application settings |
| Target | None: application settings |
| Effect | Saved settings; no files change. Recovery is opening Options again |

## Navigation And Search

The address box accepts folders and indexed search terms for text, metadata, dates, locations, media
types, ratings, labels, tags, duplicates, comparisons, negation, and Boolean expressions.
When Diffractor writes a query back to text, literal operands are quoted whenever the tokenizer
would otherwise read their characters as syntax, and numeric operands preserve the predicate value
rather than a rounded display label unless the property is explicitly bucketed.
An unavailable absolute path remains a folder scope and reaches the unavailable-location recovery
dialog; it does not become a collection-wide text search. A server named alone, such as `\\server`, is a
folder scope too: it opens on the shares the server offers.

Address editing keeps a committed value, a typed draft, and an optional completion preview. Up/Down
preview completions without replacing the draft. Tab accepts into the draft; Enter commits; Escape
first restores the draft from a preview, then restores the address captured when editing began.

Each prediction shows how many items choosing it lists, and of two suggestions of one kind, the one
that finds more ranks higher. A suggestion drawn from the collection's own words, tags, places, or
properties is not offered when it would find nothing. What was typed is always offered first, and
recent searches keep their order. A count of none, or one not ready in time, shows nothing; the list
never waits for a count.

History entries retain query and selection. Back and Forward restore rather than rewrite them, and
move the history position only after the target navigation is accepted; a refused or unavailable
target leaves the current entry and the next Back/Forward target unchanged. Navigating after Back
removes the forward branch. Parent broadens one narrowing at a time. Sibling folder commands remain
at the same level and are unavailable at their ends.

Navigation from a map, chart, breakdown, summary, or group header changes the query only. Grouping,
sorting, and filters remain user-owned. Filtering removes hidden items from selection; removing the
filter does not reselect them.

A sidebar row's total is the number of items clicking it lists. Hovering a row also says how many of
the items shown it holds, when the index can say. A tag or label row that would list nothing is not
shown; rating rows always are, because together they are one scale. Predictions and sidebar rows
have no item target, as the owner decided: they navigate, and their counts describe what navigating
lists.

## Selection And Viewing

Plain click selects one item; Ctrl toggles; Shift extends from the focus anchor; Ctrl+Shift combines
both. Empty-space clicks and empty selection rectangles leave selection unchanged. Right-clicking an
unselected item selects it before opening a batch-oriented menu. Select All targets visible items,
except while the read-only document a non-media file shows in the selection panel has the keyboard:
from a press on it until a press lands anywhere else, Select All selects its text, Copy copies the
text selected in it, and Cut does nothing rather than cut the file being read. With no text
selected, Copy and Cut keep their item targets
([selection controls](selection-controls.md#other-singular-files)).

One selected media item previews, two compatible files compare, and other selections summarize.
Opening media preserves selection. Selection, focus, pin, hover, and errors remain visually distinct.

Play controls the displayed audio/video. Slideshow walks visible photo/video/audio items from the
focused item, holds photos for the configured delay, plays timed media to completion, and stops when
no eligible next item exists. Repeat controls sequence ends; normal continuation and Slideshow are
separate choices. Resume stores playback position in the rebuildable index and never changes media.
Seeking while paused updates the displayed frame, progress bar, and elapsed time without resuming
playback or requiring a hover preview.

A Commodore 64 SID tune plays as audio, emulated rather than decoded. The file does not say how long a
tune lasts, so each plays for three minutes, fading out over the last three seconds, and always from
its start. In place of the visualizer, the Media view and the selection panel show the SID monitor:
each voice's waveform, note, envelope and ADSR, the chip's output, filter and volume, and the registers
the tune writes. Its controls start another of the file's tunes and mute or solo a voice; the tune
runs on underneath, so a voice comes back mid-note. Where space is short the monitor is drawn smaller,
down to three quarters of its size, before it gives up its register grid, then the filter, then the
output, and finally its heading and ADSR row; it is centred in whatever space it leaves over.

Video thumbnail preview is latest-wins, does not hydrate offline files, and may publish the last
successful frame as the versioned SQLite thumbnail.

## Films, Series, And Music

A film shows as its title and year, and an episode as its number and title (`S01E02 · Title`), on
tiles and in details, which add Episode and Year columns. Grouping by album or show orders a show by
season and episode and an album by disc and track. What makes a video a film or an episode, and what
each collection folder holds, belong to [metadata.md](metadata.md#films-episodes-and-music).

`@movie` (or `@film`) and `@episode` (or `@tv`) find films and episodes; `@inprogress` (or `@watching`)
finds media started and not finished, and a tile shows how far its media has played. When an episode
ends with Continue with the next item on, or in a Slideshow, play goes on to the next episode of its
show among the visible items - by season, number and air date - before the next visible item.

Subtitles show over a playing video: its own text and picture tracks, and subtitle files beside it
(`Movie.srt`, `Movie.en.srt`, `Movie.en.forced.srt`). Only a language, or a flag - forced, SDH, CC - after
the video's name pairs a subtitle with it, so another film's `Alien.Resurrection.srt` is never `Alien.mkv`'s;
renamed, moved or imported with its video, a subtitle keeps what it adds (`New.en.forced.srt`). Options
say when they show by themselves - when
the speech is in another language (the default), always, or off - and in which language, by default the
one Diffractor is shown in. A forced track translates only foreign speech, so it shows even when
subtitles are off. The playback menu's Subtitles section offers Off and every track and file, and a
choice there holds for the item until another is shown. Text keeps its track's bold, italic, color,
alignment and position, and lines rise clear of the media controls while those show.

## Collection Presence

Presence distinguishes direct membership from possible-copy evidence:

- **In collection** means this file path belongs to a collection folder.
- **Possible copy**, **Possible newer copy**, and **Possible older copy** compare an outside file with
  likely collection copies and state uncertainty explicitly.
- **Outside collection; no possible copy identified** is a completed absence of current evidence,
  not proof that no copy exists.
- **Checking presence** is incomplete work and cannot publish an absence result.

Presence supports browsing and import decisions. It never chooses a keeper, recommends deletion, or
adds a candidate to the command target. [Collections](collections.md) owns duplicate evidence parity.

## Related Items

Related items searches the indexed collection from one focused anchor and changes no files. Each
result appears under its strongest relation only, in this order: possible copy, same film, same album,
same series, nearby capture time, nearby capture place. Each group is bounded and ordered by closeness.

Same film gathers other files of the anchor's title and year: another release, or one file kept in two
folders. It is evidence to compare by size and resolution beside the anchor, not a copy claim, and like
every relation it never chooses which file to keep.

Copy identity is shared with duplicate search and Presence. Re-encoded, resized, or rotated images
may match through perceptual evidence; crops, bursts sharing only a timestamp, and low-information
images are not asserted as copies. Related results never become targets automatically.

## Metadata, Editing, And Files

Metadata persists in media or sidecars; the index is a cache. [Metadata](metadata.md) owns format and
tag support, and [File I/O](file-io.md) owns write safety and read-back.

Edit holds photo adjustments as a draft. Save updates the file; Save As creates and opens a new file;
leaving a changed draft offers Save, Don’t Save, and Cancel. Reset changes only the draft.
A file Diffractor cannot write, such as a RAW, saves as a JPEG beside it under a name of its own
(`<name>-edit.jpg`, numbered past names already taken), never as the same name with `.jpg`, which
beside a RAW is the camera's own JPEG; any existing file a save with pixel changes replaces gets its
original backup when that option is on.
If source pixels are still loading, unreadable, or refused as too large, Edit shows that bounded
state and withholds pixel operations rather than editing a placeholder.
Adjustment rows move labels above their controls when a narrow pane cannot otherwise retain a
usable slider and numeric field; resizing never changes an adjustment's value.

Metadata and Tags operate on the complete visibly selected set. Optional metadata fields change only
when selected for the run. Date adjustment applies one shift while preserving relative offsets;
items without a known date receive the chosen start. Rotation changes selected originals and is
always reviewed for multiple items. Convert/Resize creates output and retains sources; its size limit
reduces what is larger than the limit and never enlarges what is not, as its review shows. Share hands
the complete visibly selected set to an app chosen in the system share sheet, or to an email draft
where the system has none. Files its options leave unchanged go as themselves; converted, resized,
or zipped copies are staged in a temporary folder and kept until the next share, because the
receiving app may read them later. Sources are never changed.

Collisions use Replace, Skip, Auto-rename, or Block Run. Two plan rows claiming one destination are
a collision. Delete distinguishes recoverable Recycle from confirmed Permanent delete. A location
without a usable recycle facility never presents permanent deletion as Recycle; when a requested
Recycle would instead destroy an item, Windows owns the mandatory permanent-delete confirmation
before that item is removed. If the user has enabled Windows' Recycle Bin delete confirmation,
Windows may also confirm ordinary recycles.

## Guided Operations

Import, Sync, Convert, Metadata, Date, and batch Rename follow one state grammar:

1. Scope and destination.
2. Complete visible target and count.
3. Options, collision policy, and recovery.
4. Analyze a snapshot.
5. Review actions, overwrites, skips, and permanent effects.
6. Revalidate and Run the approved snapshot.
7. Retain per-item Succeeded, Failed, Skipped, and Canceled results.

A finished run's status states what it did, then why any rows did nothing; a run that reports neither
leaves the review's status.

The review list's columns fit their content, shrinking to the view when they cannot all fit. The table
starts at the view's left edge and its last column takes the rest of the width, so the table always
runs from edge to edge. Dragging the divider after a column resizes it, no narrower than a column can
be, while the columns before it stay put and the last column takes up or makes the room. Escape
abandons the drag, and a double-click fits the column to its content. Sizes last until the task is
opened again.

Changing scope, target, options, destination, or relevant files invalidates the analysis. Cancellation
stops future work and reports partial completion without claiming rollback. Import history is not
duplicate proof; Sync names both endpoints and direction. Convert carries the reviewed collision
decision through final publication: a destination that appears or changes after review is refused
rather than overwritten. Sync applies full-path collection exclusions to corresponding relative
folders on both compared trees.

## System States And Consistency

Surfaces distinguish loading, searching, empty folder, no results, filtered results, unavailable
scope, indexing, offline, download required, unsupported, and decode failed. Each state names its
scope and next action; blank content is not an error message.
After bounded image-load retries fail, Items and Fullscreen show the decode failure in a drawable
media area even when image dimensions are unknown, rather than a blank area or a thumbnail presented
as the loaded image. A changed source can be loaded again.

Long work shows operation, current item, completed/total counts, and Cancel. The taskbar button shows
the same progress, and flashes when work ends while another application is in front. Results and
actionable errors remain afterward. Progressive results apply only to the requesting scope and
generation. Visible state converges after source changes without unrelated input. [Implementation](implementation.md#view-invalidation)
owns the invalidation ordering.

Animation carries no meaning and completes immediately in software rendering or when the system
disables client-area animation. Durable preferences persist; transient focus, selection, pin, zoom,
and operation progress do not.

Unattended indexing skips media attributed to a prior crash for the current release line; an explicit
user open is still attempted. Repeated unsettled launches reset presentation and disable hardware
acceleration while retaining collection and user data.

Network capabilities independently disclose trigger, transmitted data, recipient, purpose, and
disable control. Aggregate feature-use diagnostics contain no stable user ID or file information.

## Where this lives

- [model.cpp](../src/model.cpp), [model.h](../src/model.h), and [model_display.h](../src/model_display.h):
  scope, navigation, selection, display, and view transitions.
- [app_commands.cpp](../src/app_commands.cpp) and [app_commands.h](../src/app_commands.h): command
  availability, targeting, and keyboard behavior.
- [app_search.h](../src/app_search.h), [model_search.cpp](../src/model_search.cpp), and
  [model_tokenizer.h](../src/model_tokenizer.h): address editing, parsing, and matching.
- [view_items.cpp](../src/view_items.cpp), [view_items.h](../src/view_items.h), and
  [view_media.h](../src/view_media.h): Items, selection presentation, and Fullscreen.
- [app.cpp](../src/app.cpp) and [app_toolbar.cpp](../src/app_toolbar.cpp): the title bar and status
  bar, and the commands each view declares for them in [ui_view.h](../src/ui_view.h).
- [view_import.cpp](../src/view_import.cpp), [view_sync.cpp](../src/view_sync.cpp),
  [view_rename.cpp](../src/view_rename.cpp), and [view_batch.cpp](../src/view_batch.cpp): guided
  operations, and [view_list.h](../src/view_list.h) the review list they share.
- [app_util.cpp](../src/app_util.cpp) and libdesktop's [platform.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform.h): file effects, collisions,
  shared copies, and the recycle and share capabilities.
- [app_settings.cpp](../src/app_settings.cpp): durable preferences and diagnostics settings, and
  [view_options.cpp](../src/view_options.cpp) the Options view that edits them.