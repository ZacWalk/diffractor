# Diffractor Product Design

This document owns durable user concepts and observable behavior. The subject documents in
[AGENTS.md's ownership table](../AGENTS.md#information-ownership) own their details,
[implementation](implementation.md) owns architecture, and GitHub owns plans and issue status.

## Primary Design Drivers

1. **Fast, lightweight performance.** Start and respond quickly, scale to large collections, work
   progressively, and bound CPU, memory, storage, and network use.
2. **A clear user mental model.** Make scope, contents, command target, effect, and recovery
   predictable. Hidden targets and silent state changes are defects.

Neither driver overrides the other. Performance changes preserve behavior, and clearer workflows
must not introduce avoidable blocking or unbounded work.

## Product Promise

Diffractor is a private organizer for photos, video, and audio on storage the user controls. It is
released for Windows; a [Linux](linux.md) build runs but is not distributed. It finds, views,
compares, describes, edits, and manages media without making its rebuildable index the authority for
user files or metadata.

## User Mental Model

Every user-visible interaction states four facts:

| Field | Meaning |
| --- | --- |
| Scope | Where the items come from: the indexed collection, a folder, a folder with its subfolders, a folder outside the collection, or a search |
| Contents | Query, filters, grouping, sorting, and visible/hidden counts |
| Target | What a command acts on: the focused item, the one displayed item, the complete visibly selected set, or the visible items; Options, predictions and sidebar rows have none |
| Effect | Action, affected count, destination/collisions, retained originals, and recovery |

A behavior holds for every scope only when it is provably the same in each. A behavior this document
does not decide needs the owner's decision, not a new alias or implicit state.

The target is always visible. Focus is the keyboard cursor and range anchor; selection is the
complete batch target; a displayed or pinned item does not silently enlarge it.

### Command Availability

A command is offered when its effect is visible on the current surface and its target qualifies. An
ineligible command keeps a concrete disabled reason, which a toolbar button's bubble shows; a menu
just dims it, and a submenu reflects its entries. A platform capability that does not exist is absent,
not disabled. Configured external tools in the Open menu take one `{item-path}`, so they are offered
only when exactly one file is selected; association handlers that accept the whole selected set stay
available.

Commands affecting list contents or presentation belong to Items, those affecting rendered media to
Items and Fullscreen, and task commands to their task view. Keyboard, toolbar, menu, context-menu and
accessibility entry points share one eligibility, target, confirmation and effect.

A right-click opens a menu about what is under the pointer, as the view names it:

- In Items, an item offers the items' menu. The listing away from its items - between them, on a
  group's title or the scroll bar - offers its own: Paste and New folder, then where to go, what to
  select, how it groups and shows, and Refresh, and nothing acting on a selection the pointer is not on.
- The media pane's picture offers the media menu; the details beside it offer the item's metadata, to
  edit or list in full, its grade and its file, not playing or zooming.
- A sidebar row offers what a click does and, for a folder or a drive, show it in the file browser, copy
  its path or show its properties; then the sidebar's own commands, Customise among them, and the
  window's. Elsewhere on the sidebar offers just those.
- The media view and Movie's timeline have menus of their own; every other view, Movie away from its
  timeline included, offers its status bar's commands, then Close.
- The frame offers its own menu on the title and status bars, the splitter between Items' panes, and
  beyond the view; the panel beside a view and the strip under it offer none beyond their controls' own.

The Items and media menus open with a row of named icon buttons for the commonest commands on their
target - Cut, Copy, Rename and Delete, and Paste in Items; Paste and New folder on the listing's menu -
which are not listed again below.

### Links, Buttons And Hints

In a dialog or a view's panel, a link goes somewhere - a folder, a person's pictures, another settings
page, help - and a button does something - apply tags, hide every face under a heading, delete face
data, dismiss the dialog. Choose by effect, not importance: a secondary action is still a button, and a
destination in running text still a link. Label each with its destination or action.

A hover bubble explains the control under the pointer; it is not another place to press it. It reads
from the heading through the facts to the hint, explanations sharing a left edge, though a preview
image may be centred and numeric columns right-aligned for comparison. The key or gesture hint comes
last, in the theme's hint blue, without an underline; warnings and reasons a command cannot run are
explanation, not hints. A sidebar button shows its toolbar counterpart's shortcut; a face thumbnail or
map location that opens a narrower search does not borrow the view's shortcut. Exit shows Ctrl+F4 only
where that key exits, in normal Items.

A dialog's decision goes at its foot, after the fields and any warning, not among the choices in its
body; the same decision keeps the same order across views, and its buttons stay in reach when the body
scrolls. A task panel identifies the target before its settings, and its status bar holds the task's
main command. A bar short of room prefers whole controls and a More menu to clipped icons.

### Keyboard

F4 opens Explore and Shift+F4 shows or hides the sidebar; F6 opens Options; F8 opens Faces and Ctrl+F8
Convert or Resize; Ctrl+F2 names the one face of the picture or video shown or selected
([faces](faces.md#93-faces-on-a-picture)). A shortcut is available exactly when its command is. Ctrl+F4
closes the current view, respecting its unsaved changes, or leaves Fullscreen; in windowed Items it first
leaves a slideshow or zoom, and in normal Items it closes the application. F1 is About Diffractor, and
Ctrl+F1 the keyboard reference.

Tab moves the keyboard round the window, and Shift+Tab back, in reading order: the title bar's controls;
Items' sidebar; the view's own content; the links of Items' listing - headers, group titles and footer;
the media pane, or the panels over a full-screen picture; the selector strip; each control and link of a
task's panel; then the status bar's controls. A toolbar, a radio group and a group of drawn parts - the
sidebar's rows and calendar months, the media pane's links and buttons, a rating's stars - are one stop
each, whose parts the arrows move through: Left and Right in order, Up and Down to the nearest row, Home
and End to either end. Enter or Space presses what the keyboard is on, as a click does, and a ring shows
where it is wherever nothing else does. Tab into an edit selects its text, so typing replaces it.

Lists, grids and strips - Items, a task's review, Faces' people and faces, the selector strip and Movie's
timeline - share their keys: the arrows move the cursor, Page Up and Page Down by what shows, Home and End
to either end; the cursor selects what it reaches, Shift extends from the anchor, Ctrl+Shift adds the run,
Ctrl moves the cursor alone, and Ctrl+Space toggles the entry under it (Space in Faces, which plays
nothing). In a task's review the ring goes round the cursor's row. The strip picks on Enter or Space,
since what it picks is the task's target. In Items the cursor is the focused item, Page Up and Page Down go
to the previous or next group, and Z toggles zoom mode; the media view steps through the items, Ctrl
toggling each one it reaches.

A focused control keeps its own keys: Enter presses a button, Space works any control, and the arrows move
along a toolbar, slider or radio group. Enter elsewhere in a task view runs the task. Escape takes the
keyboard back to the view from a bar, a group of parts or the strip, and in a task's panel closes the
task. Apps and Shift+F10 open, beside what has the keyboard, the menu a right-click there opens.

Tab passes over what is only pointed into - a picture, a seek bar, the sidebar's type pie and globe,
Explore's globe; pictures zoom and media plays by keys of their own. A zoomed or compared picture has the
keyboard alone, and a running Commodore 64 keeps every key, Tab included, until Escape stops it. A pointer
press takes the keyboard off a drawn part, but a press on the strip or a toolbar leaves it where it was. A
group rebuilt under the keyboard - the sidebar counting again - keeps it on the nearest part; a part its
own press removes hands the keyboard back to the view.

## Vocabulary

- **Scope** supplies candidate items; a **query** narrows them; filters determine **visible items**.
- **Focus** is navigation state; **selection** is the complete visible batch target.
- **Pin** visibly holds one selected item while focus moves for comparison.
- **Preview** is non-committing presentation; **Analyze** computes a plan; **Run** executes the
  validated reviewed plan.
- **Presence** compares one file with collection membership and likely copies.
- **Relation** says why another collection item is connected: copy, film, album, series, capture time,
  or capture place.

### Naming Views, Modes, And Presentation Choices

Top-level views are Items, Fullscreen, Edit, Movie mixer, Tags, Locate, Rename, Convert, Metadata,
Date, Import, Sync, Explore, Faces, and Options. “Media view” names the internal Fullscreen renderer, not
another user view.

The user-entered modes are zoom mode and a running Slideshow. Inspect zoom is a gesture, not a mode.
Grouping, sorting, density, verbose metadata, and navigator display are presentation choices.
Singular, Comparison, and Selection summary are panel forms selected by the current selection.

## Application Structure

Items is the windowed browser: grouped thumbnails or details, optional media/metadata preview, and
independent list/preview scrolling. Fullscreen reuses the same logical scope, query, filters,
grouping, sorting, focus, selection, and media state. Leaving it restores Items without rebuilding
the browsing context.

Every windowed view sits between a title bar and a status bar, neither of which scrolls; Fullscreen and
zoom mode hide both. In Items the title bar is the navigation bar, the address box between the history
and folder buttons; a task view's names the task and offers Close beside the window buttons. The status
bar states the view's status and ends, right-aligned, with the view's own commands: the item list's
controls in Items, a task's Analyze, Run and the like. In Items it starts under the sidebar with one
toolbar: the Options cog, beside the collection overview Options opens on; Tools, Playback options and
[Recent changes](#recent-changes) as icons; then Open, followed by the selection's count and size, which
its bubble breaks down by type. Open, Tools and Playback drop down their usual menus. A short bar gives
up its leading entries first; then the view's commands give way from their leading end into a More
button, so the last - a task's Run, Save or Render - stays, and [Faces](faces.md#9-the-faces-view) keeps
its filter and face-size slider at the leading end. A running task shows its progress there, with Cancel
in place of its other commands; its panel and strip take no input meanwhile, nor Movie's timeline edits,
so the run works on what was reviewed.

Task views put their parameters beside the work surface and return to Items on Close; Fullscreen is
unavailable from them, since it would hide the task's target and controls. Edit, Locate, Tags and
Metadata can show a selector strip of only the items the task can act on. Edit's Save and open next and
Locate's Add location and open next step through the strip's items, never to one it leaves out, and the
strip scrolls to show where they land; when its set shrinks - Locate, showing only items with no
location, hides each one it places - the current item keeps its place on screen.

A blue line on a scroll bar marks each selected item - on the strip's, Items' and Movie's timeline's -
where the thumb's centre is while that item is mid-view, so a mark is under the thumb just while its
item shows; pressing the strip's bar at a mark brings its item into the strip. Items' and Faces' scroll
bars are banded by group, each band named after its group where it is tall enough: a name wraps onto
more lines where its band has the height, and otherwise ends in an ellipsis.

Task panels open with an icon, a heading and a short explanation of what the work changes, then the
selection or source, then the settings; Rename puts its template and collision choice before its
examples, so a repeated task does not start with a page of help. Options heads each section the same
way, and within a panel each group of settings - Edit's colour and trim, Movie's clip, Tags' favourite,
common and removable tags - and each card of Options has the same section title.

The item list owns its filters, recursion, grouping, sorting, density and size controls. The filter box,
thumbnail size, media type filters, recursion, totals and grouping sit in the status bar, in reach
however far the list scrolls; density is chosen per group, and totals and grouping are separate
affordances. Optional query-specific rows head the results and only navigate or present: they change no
grouping, sorting, focus or selection. A captured control keeps its value and position while its
result refresh is in flight.

Thumbnail rows share a height and preserve each image's aspect; a short trailing row stays short rather
than cropping images to fill the width. Relayout preserves the focused visible item, or the content
nearest the viewport centre. Saved playback progress is not drawn over thumbnails; playback resumes from
it and its transport shows it.

The selection panel and responsive behavior are owned by [selection-controls.md](selection-controls.md).
Wheel and pinch routing are owned by [zoom.md](zoom.md#input).

## Options

Options is one page holding every setting the user can change, in sections each under a shaded header
naming it and saying what it covers:

1. **Your collection**: the folders it holds, what each holds, and index maintenance - first, as the
   first thing to set up.
2. **Viewing and playback**: how photos, videos and music show and play, and subtitles.
3. **Sidebar**: what it shows, and the favorite searches and tags it lists.
4. **Editing and saving**: the confirmations before a change, backup copies, and the quality files are
   saved at.
5. **General**: the language, help, hidden files, updates, and advanced settings.

It fills the window, each section's cards in as many columns as fit, between a title bar with Close and
a status bar ending with Help, Cancel, and Save options and close. One command, Options (the cog, F6),
opens it at the top from the Options menu, the sidebar's context menu, the cog starting Items' status
bar and the sidebar's collection overview; only the sidebar's Customise sidebar link opens it at its
Sidebar section.

The page edits a copy. Save options and close commits only what the page edits and returns to Items;
Cancel discards the copy. Close, Escape, closing Diffractor and opening another folder or search discard
it too when it holds no change - a change put back is none - and otherwise ask: save the changes, close
without them, or stay. Media playing when the page opened pauses while it shows and resumes after.
Saving reads the collection again only when its folders or what they hold changed. Index maintenance
acts at once, behind its own confirmation, since it acts on the index rather than on a setting.

On Windows, **Reuse existing window when opening files**, on by default and applied when saved, sends a
file opened from Explorer or the command line to the first eligible window of its profile, which opens
the file's folder, clears listing filters and the comparison pin, selects and previews the file in
Items, and comes forward; if that window closes, another takes its place within a few seconds. A window
refuses a file it could take only by interrupting - one showing a dialog, running a command or task, or
in Edit or Options with changes to ask about - and a refused or unanswered handoff opens a new window
instead. Requests arriving together open the latest file, not a combined selection. A launch without a
file, of a folder, with `-new-window`, headless, or with explicit startup overrides stays independent,
and Debug and Release windows are kept apart.

| Field | Options |
| --- | --- |
| Scope | Every scope: no scope narrows a setting |
| Contents | Application settings |
| Target | None: application settings |
| Effect | Saved settings; no files change. Recovery is opening Options again |

## Navigation And Search

The address box accepts folders and indexed search terms for text, metadata, dates, locations, media
types, ratings, labels, tags, duplicates, comparisons, negation, and Boolean expressions. A query
written back to text quotes literal operands the tokenizer would otherwise read as syntax, and keeps a
numeric operand's predicate value rather than a rounded display label unless the property is bucketed.
An unavailable absolute path stays a folder scope and reaches the unavailable-location recovery dialog
rather than becoming a collection-wide text search. A server named alone, such as `\\server`, is a folder
scope too, opening on the shares it offers.

Address editing keeps a committed value, a typed draft, and an optional completion preview. Up/Down
preview completions without replacing the draft. Tab accepts a previewed completion into the draft, and
with none previewed moves on, as from any field; Enter commits; Escape first restores the draft from a
preview, then the address captured when editing began.

Each prediction shows how many items choosing it lists, and of two suggestions of one kind the one
finding more ranks higher. A suggestion drawn from the collection's own words, tags, places or
properties is not offered when it would find nothing. What was typed is always offered first, and recent
searches keep their order. A count of none, or one not ready in time, shows nothing; the list never
waits for a count.

After a search term, a space offers refinements drawn from the matching items - tags, media kinds,
ratings, labels, cameras, lenses and albums - which the next term's first letters narrow: `#holiday cam`
can offer `#holiday camera:"Canon EOS R5"` if that tag and camera occur together. Tab accepts the
combination without opening it, so another term can follow.

`@` asks what kind of item to find. `@photo`, `@video`, `@audio`, `@archive` and `@commodore` (or
`@c64`) name a media type. `@raw`, `@hdr` (PQ, HLG, or a photo carrying a gain map), `@animated`,
`@transparent`, `@portrait`, `@landscape`, `@square`, `@wide` (shown at least twice as wide as tall,
declared a panorama or not) and `@slowmo` (100 frames a second or more) ask how a picture or video was
made, a shape being the picture as shown, turned by its orientation; `@panorama` asks for the panoramas
files declare, and `@duplicates` for items the collection holds more than once. An `@` word naming none
of these is searched for as typed, so a mistyped one never widens the search to everything, and a word
typed alone is also offered as the `@` words it begins that find something. A Commodore disk or tape is
found by the name it gives itself and by the names of the files it lists.

History entries keep query and selection. Back and Forward restore them, and move only once the target
navigation is accepted, so a refused or unavailable target leaves the history as it was; navigating
after Back drops the forward branch. Explore and Faces are steps too: opening one adds it after the
listing beneath, Back from what it opened returns to it as left, and Back from it returns to that
listing; Back leads their top-right toolbar, which has no Forward, and closing either leaves the history
on the listing Items shows. A task view is no step, so Back and Forward - toolbar, key or mouse button -
do nothing there. Parent broadens one narrowing at a time; the sibling folder commands stay at the same
level and are unavailable at either end of it.

Navigation from a map, chart, breakdown, summary, or group header changes the query only. Grouping,
sorting, and filters remain user-owned. Filtering removes hidden items from selection; removing the
filter does not reselect them.

Who, where and when are ways into the collection wherever they show: in an item's panel each person the
picture names, its place and its dates link to the items sharing them, and Explore shows the collection
by where and when, Faces by who. Show in Explore, on the item's menus and after its place in the panel,
opens Explore with its globe facing where the item was taken, close enough to see its neighbours, and its
timeline at the item's week, marked as the keyboard marks one. Each is a step of the history, so Back
retraces the way taken.

Bubbles wait for the pointer to rest on their target, tolerating small jitter: 500 ms for command help,
the logo and the status bar; 200 ms for face previews in Faces and on a picture, whose action buttons
still wait 500 ms; not at all for map markers, Explore's timeline, and the sidebar's calendar, file-type
chart and face previews. Sidebar rows wait 150 ms, and the Items and Faces scroll-bar previews 50 ms, on
entry, then follow the pointer without waiting again. Refreshing a visible bubble does not hide it.

A sidebar row's total is the number of items clicking it lists, and its bubble says how many of the
items shown it holds, when the index can say. A tag or label row that would list nothing is not shown;
rating rows always are, being one scale. Predictions and sidebar rows have no item target: they
navigate, and their counts describe what navigating lists.

The sidebar's history calendar has narrow year columns, latest on the right, and month rows from
December at the back to January at the front, unlabelled but for five-year landmarks where the text
fits. It is as wide as the sidebar, whose globe and pie keep one size, so a wider sidebar shows more
years, not wider ones; earlier years fill any width left - ordinary years before a start worked out
from the collection, empty ones before a start year typed in Options. With no start year it works back
from this year by capture date, leaving out a thin or isolated tail of dates likely to be wrong, and
offers at least eight years; the start year brings in genuine older photos it leaves out. It opens at
the most recent years. A horizontal drag or the wheel pans it, a drag never selects a month, and Escape
restores where the drag began. A month's bubble, its arrow on that month, gives the date, counts and a
representative picture, and follows the pointer between months even with the sidebar scrolled.
Clicking a month searches its capture dates, or modified dates with Ctrl. Refreshes and relayout keep
the user's pan, and navigating to an offscreen month brings it into view.

### Explore

Explore (F4), on the navigation toolbar, in the Navigate menu and behind the button in the corner of the
sidebar's globe, which it expands, shows the whole collection at once: where it was taken on a globe to
the left, when on a timeline to the right. Every part of either half is a search of the whole collection,
which a click opens in Items at once, changing the query only, as the sidebar does. Explore acts on no
selection; Close and Escape return to Items. Its globe is [locations](locations.md#explores-globe)', as is
what makes a [trip](locations.md#trips-and-a-years-places). Explore stays as it was left while Diffractor
runs - the globe's facing and zoom, and the day at the top of the timeline, even as the collection
changes - so Back from what it opened, or opening it again, finds it as it was.

The timeline reads from today back, under an introduction saying a date or trip opens its items, which
scrolls away with the dates and is omitted in a short window. Each year opens with a header that stays
at the top while the year scrolls under it: its number, its count, and chips naming the places it was
spent away from home. Each day is a bar growing with the items taken that day, coloured by kind as the
sidebar's pie is; alternate weeks are shaded, each with its count beside it; each month is named beside
its days, over the picture the sidebar calendar shows for it; and empty months, and empty years, fold
into one row saying so. A trip is a bracket beside its days, labelled with its place, dates and count.
The scroll bar is a rail naming the years.

Clicking a week's days lists the week; dragging down the bars takes in a run of weeks, scrolling near
either end, and lists them on release, and Escape abandons the drag. A month's name or picture lists the
month, a year's header the year, a chip that place in that year, and a trip its place in its days.
Counts are by capture date as the capture-date search reads it, so each is what clicking lists. Bubbles
give a time's dates, its photos, videos and other items, its trips and its picture, and a place's name,
dates and count.

Pointing at one half marks the other. A time lights the globe's clusters holding its items - only its
place's, for a trip or a chip - and a globe showing none of them turns to them once the pointer has
rested on the time for 500 ms, unless zoomed in close. A cluster lights the days of its items and marks
the scroll bar where they lie, however far off. The keyboard walks the weeks from the first in sight - Up
and Down a week, Page Up and Page Down a page, Home and End to either end, Enter or Space to list the
week - and plus and minus zoom the globe.

### Faces

Faces (F8), on the navigation toolbar, in the Navigate menu and in the sidebar, shows the collection's
people in three panes: People (including Unknown and Hidden), faces, and the controls for face search
and the face data. Naming a face is evidence for suggestions; rejecting a person and hiding a face are
separate decisions. [Faces](faces.md#9-the-faces-view) owns the view.

## Selection And Viewing

A click selects one item; Ctrl toggles, Shift extends from the focus anchor, and Ctrl+Shift does both.
Clicks and rubber bands over empty space leave the selection unchanged. In Items, Faces, Explore's
timeline, the selector strip and Movie a press becomes a drag - a rubber band, a move, a run of weeks -
once the pointer has moved the same few pixels; a band stays one until let go, and a band or drag held
near either end of a list carries it along at one pace, faster the nearer the end. Right-clicking an
unselected item selects it before opening a batch-oriented menu. The click that closes a menu is the
menu's: its release selects nothing. Select All targets visible items, except while the read-only
document a non-media file shows in the selection panel has the keyboard - from a press on it until a
press anywhere else - when Select All selects its text, Copy copies the text selected in it, and Cut
does nothing rather than cut the file being read; with no text selected, Copy and Cut keep their item
targets ([selection controls](selection-controls.md#other-singular-files)).

One selected media item previews, two compatible files compare, and other selections summarize; opening
media keeps the selection. Selection, focus, pin, hover and errors stay visually distinct, and every
list, grid and strip - Items' tiles and rows, a task's review, Faces' faces and groups, the selector
strip, Movie's timeline - draws them alike: the selection in the selection colour, muted while the
keyboard is in another pane, window or bar so it never reads as a hover; the item under the pointer
lighter, marked at once rather than faded in; the keyboard's place in a ring; a rubber band in one
translucent blue.

Ctrl and drag draws a region on a picture, in Items or Fullscreen and at any zoom, though not over a
projected panorama. The region belongs to that picture, held in its own pixels, so a resize, a zoom or
an index update leaves it over the same part of the picture. Dragging inside it moves it, a new Ctrl and
drag replaces it, and a press that hardly moves keeps it. Its three buttons close it, zoom to it
([zoom](zoom.md#scale-and-pan)), or open Edit with it as the crop; Escape clears it before anything else
Escape does.

Play controls the displayed audio or video and is remembered as Autoplay, the choice the Playback menu
and Options show: pausing turns it off, so the next file opens paused, and playing turns it on, so the
next plays. Slideshow walks the visible photos, videos and audio from the focused item, holding photos
for the configured delay and playing timed media to completion, and stops where no eligible item
follows; continuing to the next item and Slideshow are separate choices, and Repeat decides what
happens at a sequence's end. Resume keeps the playback position in the rebuildable index, never in the
media. Seeking while paused updates the frame, progress bar and elapsed time without resuming playback.

Playback and a Slideshow go on while a menu is open or a command runs, but the sequence waits - an item
ending meanwhile holds at its end until the menu or command has gone - since both act on the selection.
A command that changes or removes the playing file - Delete, Rename, Move, a metadata write - stops it
and lets go of the file first.

A Commodore 64 SID tune plays as audio, emulated rather than decoded, always from its start. Its file
does not say how long it lasts, so a silent copy is run far faster than it plays: a tune that falls
silent ends as its last note dies away, and one that starts again from where it first repeated plays
through once and fades out over three seconds as it starts again; a pause, or a phrase played over and
over before the tune moves on, is not its end. Until that run has answered, and for a tune it cannot
place within thirty minutes of play, a tune plays for three minutes, fading out over the last three
seconds. In place of the visualizer, the media view and the selection panel show the SID monitor - each
voice's waveform, note, envelope and ADSR, the chip's output, filter and volume, and the registers the
tune writes - as panels of the media stack (tunes, voices, output, filter, register grid), with the
selection panel under them naming the tune. Its controls start another of the file's tunes and mute or
solo a voice, which comes back mid-note since the tune runs on underneath. Voice panels wrap with the
width; where two parts fit side by side at full size, the output stands over the filter beside the
register grid, and where the grid has given way the output and filter share a row. Short of room, the
monitor is drawn smaller, down to three quarters of its size, before it gives up parts
([selection controls](selection-controls.md#presentation)). Play and seeking are the ordinary media
controls, whose time labels move below the track in a narrow transport rather than seeking being hidden.

Tracker modules play through libopenmpt with their own duration, seeking, pause and sequence end, on the
same transport and stack. Their monitor is one panel: where the module is - order, pattern, row, tempo
and speed, as much as fits - over the pattern under the playhead, coloured as a tracker colours it and
centred when narrower than the panel. Channel headers - number, level, mute and solo - and row numbers
stay in view as the rows scroll. Every channel a played pattern uses is shown; as the panel narrows each
cell gives up its volume, then its effect, then its instrument, and only channels too many for their
notes alone are stepped through with arrows. Following playback, the pattern scrolls smoothly through a
bar across its middle, the patterns before and after fainter above and below; paused, the playing row
rests on the bar, and scrolled by hand the pattern stands still until Follow playback resumes it. The
module's sample and instrument names are a panel below the selection panel, with how the patterns play
each - how many notes, between which, on how many channels - and its description carries the song
message alone.

Other audio shows the visualizer as a panel of the stack: its cover art, if any, over the bars
([rendering](rendering.md#backends) says how each renderer draws them), which keep at least half the
panel's height, the art giving way first. A file whose only picture is not cover art - an icon, or a
store's drawing of its waveform ([metadata](metadata.md#audio-and-video-metadata)) - shows the bars
alone, and a cover still on its way leaves its place empty rather than grey.

A Commodore 64 disk (D64, D71, D81), tape (T64) or program file (PRG, P00) shows its listing in the
C64's blue where media shows, above the item's panel. Clicking a program's row - any row of a tape -
runs it on an emulated Commodore 64, and a program file also has Start under its listing. A cartridge
(CRT) is drawn as Commodore's own: a black shell in the original's proportions, standing as it goes in,
with its grip ridges, its title on a silver label and the board's gold contacts, and Start under it. The
machine starts a program as a user would - a disk's with `LOAD"name",8,1` and `RUN`, the boot and load
hurried - then runs at its own speed with sound through the chosen device, its picture in the listing's
place, as large as fits and at a whole multiple of its pixels where that loses little. While the view
has the keyboard, the arrows and Space are a joystick in port 2, or port 1 once its button chooses it,
Space doubling as the space bar; the C64's other keys are where its keyboard has them, Pause or F9 being
RUN/STOP and Page Up RESTORE, while Alt combinations stay the application's. Back or Escape stops the
machine and shows the listing again, as selecting another item does, and the screen saver waits while it
runs. Fullscreen shows a running machine or a cartridge above the item's panel but leaves the listing to
Items, so a machine stopped there leaves just the panel; a task view or Options shows neither, so
opening one stops the machine. In the item list a disk is drawn as a floppy and a cartridge as one.

Video thumbnail preview follows the pointer: a hover waiting behind another is replaced by it, one
already decoding stops at the frame it has reached once another waits, and every frame decoded is shown,
so a moving pointer shows key frames and a resting one the frame near its position. The frame the
pointer rests on is saved as the versioned SQLite thumbnail. Preview does not hydrate offline files.

## Films, Series, And Music

A film shows as its title and year, and an episode as its number and title (`S01E02 · Title`), on
tiles and in details, which add Episode and Year columns. Grouping by album or show orders a show by
season and episode and an album by disc and track. What makes a video a film or an episode, and what
each collection folder holds, belong to [metadata.md](metadata.md#films-episodes-and-music).

`@movie` (or `@film`) and `@episode` (or `@tv`) find films and episodes, and `@inprogress` (or
`@watching`) media started and not finished. When an episode ends with Continue with the next item on,
or in a Slideshow, play goes on to the next episode of its show among the visible items - by season,
number and air date - before the next visible item.

Subtitles show over a playing video from its own text and picture tracks and from subtitle files beside
it (`Movie.srt`, `Movie.en.srt`, `Movie.en.forced.srt`). Only a language, or a flag - forced, SDH, CC -
after the video's name pairs a file with it, so another film's `Alien.Resurrection.srt` is never
`Alien.mkv`'s, and a subtitle renamed, moved or imported with its video keeps what it adds
(`New.en.forced.srt`). Options say when subtitles show by themselves - for speech in another language
(the default), always, or never - and in which language, by default Diffractor's own. A forced track,
translating only foreign speech, shows even with subtitles off. The playback menu's Subtitles section
offers Off and every track and file, and a choice there holds for the item until another is shown. Text
keeps its track's bold, italic, colour, alignment and position, and lines rise clear of the media
controls ([rendering](rendering.md#subtitles)).

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

Edit holds photo adjustments as a draft: Save updates the file, Save As creates and opens a new one,
Reset changes only the draft, and leaving a changed draft offers Save, Don’t Save, and Cancel.
Diffractor writes JPEG, PNG, WebP, JPEG XL and AVIF, so a photo in one of those saves over itself,
keeping its bits and its HDR. Any other - a RAW, a HEIC - saves beside it under a name of its own:
`<name>-edit.jpg`, numbered past names already taken and never the same name with `.jpg`, which beside a
RAW is the camera's own JPEG; or `<name>-edit.avif` for a HEIC holding more than a JPEG keeps - more than
eight bits, HDR, or a gain map. A HEIC's turn alone saves over it, as Rotate turns one. An HDR photo too
large to edit in HDR is never replaced by the standard-range picture its edit was made on: the save is a
copy beside it. A copy in a format that cannot hold a photo's HDR or its gain map says so before it is
written, and a save with pixel changes backs up the file it replaces when that option is on
([File I/O](file-io.md#writing) says how precision, HDR and gain maps are kept). While source pixels are
loading, unreadable, or refused as too large, Edit shows that state and withholds pixel operations
rather than editing a placeholder.

Adjustment rows put their labels above their controls when a narrow pane would otherwise leave no usable
slider and numeric field; resizing never changes a value. Above the colour sliders a histogram shows the
photo's red, green and blue as the edit leaves them, counted from a sample of the preview as it is
graded. Under it a handle each for the darks, midtones and lights stands where its slider has moved that
point of the tone curve - a fifth, a half and four fifths along, a tenth either way at most; dragging
one, or the histogram above it, moves the slider, Escape takes a drag back, and a double click returns
the handle to 0. A round handle beside the photo's left edge, clear of the photo where there is room,
compares the edit with the original: held, its line follows the pointer, with the original left of it as
the file shows it - whole, upright and in its own colour - and the photo right of it as the edit will
save it - cropped, turned, straightened and coloured - each fitted to the view, so the two sides are a
true before and after. The crop's frame and handles wait meanwhile; let go, the handle slides back and
they return. Preview shows the two side by side instead.

Edit also opens a video or a sound file and edits it losslessly: a save re-muxes the file, copying its
packets into a new one with nothing re-encoded, as re-encoding is the [Movie mixer](movie.md)'s work.
A file in a format Edit writes back - MP4, QuickTime, M4V, 3GP and M4A, Matroska and WebM, MPEG-TS, AVI,
WMV and WMA, FLV, MP3, AAC, FLAC, Ogg and Opus, WAV and AIFF - is copied in its own format; one in any
other format FFmpeg reads, such as an MPEG program stream, is saved as an MP4 beside it (`<name>-edit.mp4`,
chosen in the dialog Save As shows). A SID tune or a tracker module has no packets to copy, so Edit does
not open one. The draft is a trim - the handle at each end of the scrubber, or I and O at the frame shown -
the sound taken out of a video, and, in the MP4 and QuickTime family, a quarter turn players take from the
track. The player shows the draft as it will be saved: a dragged handle shows its frame until let go,
the scrubber dims what the trim leaves out, playback pauses where it ends, and a video plays silent or
turned as it will be; the status bar lists the trim's start, end and length, the sound removed and the
turn. Each end keeps the frame shown, every picture stream copied from a key frame of its own: an
MP4-family copy starts on the chosen frame, hiding with its edit list what it keeps from the key frame
before, while any other starts at that key frame, as the panel says.

Save As offers the file's own format, under each of its extensions, and MP4 wherever an MP4 holds the
file, the panel naming what an MP4 would leave out. A save rebuilds the file's index, sizes and
timestamps, so Save As with no changes repairs a file other players will not play; a file FFmpeg cannot
read is said to be so, and is neither edited nor repaired. A stream the format cannot hold - an Apple
metadata track, a GoPro data track, a QuickTime file's cover - is left out, as the panel says before
anything is saved. The copy keeps the file's tags, the chapters inside the trim, its timecode, its cover
art where the format holds one, its XMP packet, rating and tags, and a backup of the original when that
option is on; a subtitle file beside it is left as it is. Two save options shape the copy: an MP4-family
file can have its index written before its media, to play while it downloads - one that had it there
keeps it there - and a damaged file can be repaired, the copy reading on past what cannot be read,
leaving out packets whose timing cannot be mended, and saying what it left out and where it ends. A save
can be cancelled until its file is replaced, and fails, leaving the file as it was, on a source that
cannot be read to its end unless repairing, or whose timing starts again part way - a recording, or clips
joined end to end - saying where. A repaired copy that left part of the file out replaces it only behind
a backup, and otherwise says to use Save As or turn the backup on ([File I/O](file-io.md#writing)).

Metadata and Tags operate on the complete visibly selected set. Optional metadata fields change only
when selected for the run. Date adjustment applies one shift while preserving relative offsets;
items without a known date receive the chosen start. Rotation changes selected originals and is
always reviewed for multiple items. Convert/Resize creates output and retains sources; its size limit
reduces what is larger than the limit and never enlarges what is not, as its review shows. The size
limit and conflict policy precede the save options, which show only the selected format's settings,
and switching formats keeps each one's settings. Share hands the complete visibly selected set to an app
chosen in the system share sheet, or to an email draft where the system has none. Files its options
leave unchanged go as themselves; converted, resized or zipped copies are staged in a temporary folder
and kept until the next share, since the receiving app may read them later, and a photo resized in a
format Diffractor does not write goes as a JPEG. Sources are never changed.

Collisions use Replace, Skip, Auto-rename, or Block Run; two plan rows claiming one destination are a
collision. Delete distinguishes recoverable Recycle from confirmed permanent delete, and asks first only
when its confirmation option is on - except on a network share, or a drive mapped to one, which has no
Recycle Bin: there Delete always asks, once, saying the items will be permanently deleted, and Windows,
recycling what it can, does not ask again. When a requested Recycle would destroy an item, Windows asks
before removing it, and Windows' own Recycle Bin confirmation, if on, may confirm ordinary recycles too.

What Delete or Move takes away leaves the listing at once, the rest keeping its place; when the
selection went, the cursor lands on the item that followed it, or the one before at the listing's end.
In a listing of copies the cursor passes over a copy whose others all went, which leaves the listing as
the index finds it has no copy left. The search is not run again for it, so a search the index answers
never lists a deleted file while the index catches up; the index learns of the change, and a moved file
the search still matches is listed again where it went.

### Recent changes

The session's changes stay in sight, so the items being worked on are never lost among the rest. Each
change made to files - a rating, label or tags, a location, a person renamed in the files naming them, a
turn, a saved edit, a batch of metadata or dates, a rename, a copy, move or paste, a conversion, an
import, a movie - is kept, newest first, under what it did: `Rating: 4`, `Tag: +beach`, `Rename`. The same
change to the same items again is one change, and the latest dozen are kept. Every item a change reached
is marked wherever it shows - a pencil badge beside the pin on its tile, its row icon tinted in details.
Recent changes, in Items' status bar and the Navigate menu, lists them with how many items each changed,
a sidecar counted with its item; picking one opens their folders as one search with those items
selected, wherever they now are, since a renamed or moved item - or folder - takes its changes and marks
along. The list lasts the session, as zoom and scroll do; the files hold what was done.

## Guided Operations

Import, Sync, Convert, Metadata, Date, and batch Rename follow one state grammar:

1. Scope and destination.
2. Complete visible target and count.
3. Options, collision policy, and recovery.
4. Analyze a snapshot.
5. Review actions, overwrites, skips, and permanent effects.
6. Revalidate and Run the approved snapshot.
7. Retain per-item Succeeded, Failed, Skipped, and Canceled results.

A finished run keeps every row and column of its review and adds a Status column - Success, Failed,
Skipped or Not run - dimming the rows that did nothing. Its status line says what it did, then why any
rows did nothing, and keeps the review's status when it reports neither.

Review rows are picked out as listing items are selected, by click and by the [keys every list
shares](#keyboard), Ctrl+A picking out every row. Copy copies the rows picked out as text, a line a row
with tabs between columns, and Escape lets them go before it closes the task. Picking out rows changes
nothing the task acts on.

The review's columns fit their content, shrinking to the view when they cannot all fit, and the last
takes the rest of the width, so the table runs from edge to edge. Dragging the divider after a column
resizes it, down to a column's least width, the columns before it staying put and the last giving or
taking the room; Escape abandons the drag, and a double-click fits the column to its content. Sizes and
sort last until the task is opened again, and a fresh analysis arrives in the user's sort order.
Markdown and CSV documents and other display tables share these divider gestures, a document keeping
its column sizes while open and a display table for its lifetime.

A display table is headed where headings say what a column holds - an archive's listing, a file's
streams, a module's sample and instrument banks - and not where a row reads by itself, as a summary's
or a bubble's rows do; an unheaded table draws no dividers on its first row, so it never reads as
headings. Clicking a heading sorts by its column - names and sequences from the start, sizes, lengths
and dates largest or latest first - a second click reverses it, and a third restores the table's own
order; rows with nothing in the column come last. Each table of the selection panel keeps its sort and
sizes while the selection stays, however the panel is rebuilt around it. Every headed table - a display
table, a task's review, a detail group in Items - draws its headings alike, the sorted column bright
with a chevron and the rest dimmed, shades every other row, and ends a value too wide for its column in
an ellipsis.

A detail group in Items is headed the same way over the columns its rows show, the headings staying at
the top of the view while its rows scroll. A click sorts that group alone, the others keeping the
toolbar's order, and a third click gives it back; the listing stays where it is scrolled rather than
following the selection, and choosing a sort from the toolbar sets the groups' heading sorts aside. The
name column takes up the room: the divider before a column after the name, or after a column before it,
resizes that column, and a double-click fits it again. A group keeps its sort and sizes, by what it
groups, until Diffractor closes. Grouped by duplicates, each set of copies is shaded as a whole, tiles or
rows, rather than every other row.

Changing scope, target, options, destination, or relevant files invalidates the analysis. Cancellation
stops future work and reports partial completion without claiming rollback. Import history is not
duplicate proof; Sync names both endpoints and direction, and applies full-path collection exclusions to
the corresponding folders of both trees. Convert carries the reviewed collision decision through final
publication: a destination that appears or changes after review is refused rather than overwritten.

Import's sources are the selection, the OneDrive camera roll, each removable drive holding media, each
phone or camera a cable connects as a device rather than a drive, and another folder. A device has no
path to read, so Analyze first copies what its `DCIM`, `Pictures` and `Movies` folders hold into a
staging folder of Diffractor's own, keeping the device's folders, names and times and showing how far it
has got: what an import already took stays on the device, what an earlier Analyze staged is not copied
again, and what the device no longer holds goes. What is still to copy must fit, with room to spare, on
the drive staging is on, or Analyze copies none of it and says how much more room it needs. The staged
files are then analyzed and reviewed as a card's are. A file the device will not give up - kept only in
the cloud, or still being written - is named on the status line and the rest staged regardless; a
cancelled Analyze stages nothing. Import copies from a device whatever Move says, and removes the staged
copies once a run, leaving Import or choosing another source is done with them. Each running Diffractor
stages in a folder of its own, and opening Import clears what instances since ended left there. A device
offering nothing - locked, or asking whether to allow the computer in - is said to be so, with a request
to unlock it. Only Windows reaches such devices.

### Name templates

Rename's template and Import's folder structure name what they make from text, `#` for Rename's
numbering, and tokens in braces, read from each file; a file without a token's value gets `Unknown`
in its place. A token is a property's name, in any case:

- The file and when it was taken: `{name}` (also `{file}` and `{filename}`), `{created}` as `yyyy-mm-dd`,
  `{year}`, `{month}`, `{month.text}`, `{month.short}` and `{day}`.
- What it is given in Diffractor: `{title}`, `{rating}`, `{tags}`, `{label}`, `{description}` and
  `{comment}`.
- Where it was taken: `{place}`, `{state}` and `{country}`.
- What took it: `{camera}`, `{camera.manufacturer}` and `{lens}`.
- Music, films and series: `{artist}` (the album's artist where there is one), `{album}`, `{genre}`,
  `{composer}`, `{performer}`, `{publisher}`, `{show}`, and `{season}` and `{episode}` in two digits.
- The rest a file can hold: `{album.artist}`, `{synopsis}`, `{encoder}`, `{bitrate}`, `{audio.codec}`,
  `{video.codec}`, `{pixel.format}`, `{raw.file}` and the `{copyright.creator}`, `.credit`, `.notice`,
  `.source` and `.url` tokens.

## System States And Consistency

Surfaces distinguish loading, searching, empty folder, no results, filtered results, unavailable
scope, indexing, offline, download required, unsupported, and decode failed. Each state names its
scope and next action; blank content is not an error message. A review, a timeline or a pane with
nothing to show says so in faint text centred in its space, and a group's header in Items and a year's
in Explore's timeline are shaded alike. After bounded image-load retries fail, Items and Fullscreen show
the decode failure in a drawable media area, even when the image's dimensions are unknown, rather than a
blank area or a thumbnail presented as the loaded image. A changed source can be loaded again.

Long work shows operation, current item, completed/total counts, and Cancel. The taskbar button shows
the same progress, and flashes when work ends while another application is in front. Results and
actionable errors remain afterward. Progressive results apply only to the requesting scope and
generation. Visible state converges after source changes without unrelated input;
[implementation](implementation.md#view-invalidation) owns the invalidation ordering.

Animation carries no meaning and completes immediately in software rendering or when the system
disables client-area animation. Durable preferences persist; transient focus, selection, pin, zoom,
and operation progress do not.

After a crash, unattended indexing skips the media blamed for it, though the user can still open them,
and repeated unsettled launches start safely, keeping the collection and user data
([implementation](implementation.md#crash-loop-protection)).

Network capabilities independently disclose trigger, transmitted data, recipient, purpose, and
disable control. Aggregate feature-use diagnostics contain no stable user ID or file information.

## Where this lives

- [model.cpp](../src/model.cpp), [model.h](../src/model.h), and [model_display.h](../src/model_display.h):
  scope, navigation, selection, display, and view transitions.
- [app_toolbar.cpp](../src/app_toolbar.cpp)'s `app_frame::update_button_state` and
  `default_keyboard_accelerators`: each command's availability, disabled reason and keys;
  [app_commands.cpp](../src/app_commands.cpp) and [app_commands.h](../src/app_commands.h): the commands,
  their targets, handlers and menus; `bubble_delay` in [ui_view.h](../src/ui_view.h): how long bubbles wait.
- [app_search.h](../src/app_search.h), [model_search.cpp](../src/model_search.cpp), and
  [model_tokenizer.h](../src/model_tokenizer.h): address editing, parsing, and matching.
- [view_items.cpp](../src/view_items.cpp), [view_items.h](../src/view_items.h), and
  [view_media.h](../src/view_media.h): Items, selection presentation, and Fullscreen; the region drawn on
  a picture is `view_state::drawn_region` in [model.h](../src/model.h), drawn by `photo_control` in
  [ui_controls.h](../src/ui_controls.h) and moved by `region_controller` in
  [ui_elements.cpp](../src/ui_elements.cpp).
- [view_explore.cpp](../src/view_explore.cpp) and [view_explore.h](../src/view_explore.h): Explore's
  globe and timeline, and the searches its parts open, which
  [model_explore.cpp](../src/model_explore.cpp) makes from the predicates that counted them.
- [app.cpp](../src/app.cpp) and [app_toolbar.cpp](../src/app_toolbar.cpp): the title bar and status
  bar, and the commands each view declares for them in [ui_view.h](../src/ui_view.h).
- [app.cpp](../src/app.cpp) and [ui_elements.cpp](../src/ui_elements.cpp): the order Tab visits the
  window in, and the parts the keyboard finds, steps between and presses among drawn elements, which a
  view names in [ui_view.h](../src/ui_view.h).
- [view_import.cpp](../src/view_import.cpp), [view_sync.cpp](../src/view_sync.cpp),
  [view_rename.cpp](../src/view_rename.cpp), and [view_batch.cpp](../src/view_batch.cpp): guided
  operations, and [view_list.h](../src/view_list.h) the review list they share. Name templates are
  `prop::replace_tokens` in [model_property.cpp](../src/model_property.cpp).
- [view_edit.cpp](../src/view_edit.cpp): Edit's photo draft, and a video's or a sound file's, which
  [av_format.cpp](../src/av_format.cpp)'s `av_remux` copies in the formats `av_edit_muxer` names.
- [av_sid.cpp](../src/av_sid.cpp) and [ui_sid.cpp](../src/ui_sid.cpp), [av_tracker.cpp](../src/av_tracker.cpp)
  and [ui_tracker.cpp](../src/ui_tracker.cpp), [av_c64.cpp](../src/av_c64.cpp) and
  [ui_c64.cpp](../src/ui_c64.cpp): SID tunes and how long each lasts, tracker modules, and the running
  Commodore 64, with their monitors, listing and cartridge; [av_subtitle.cpp](../src/av_subtitle.cpp):
  which subtitles pair with a video and show.
- [view_selector.cpp](../src/view_selector.cpp): the selector strip under a task view and the marks on
  its scroll bar. What a task's strip offers is that task's own test: `can_edit_in_edit_view` and
  `can_edit_metadata_of` in [model.cpp](../src/model.cpp), and `locate_offers` in
  [view_locate.cpp](../src/view_locate.cpp).
- [app_util.cpp](../src/app_util.cpp) and libdesktop's [platform.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform.h): file effects, collisions,
  shared copies, and the recycle and share capabilities; [app.cpp](../src/app.cpp)'s
  `app_frame::delete_items`: Delete's confirmation, and `app_frame::reuse_running_instance`: handing a
  file to the open window.
- [model.cpp](../src/model.cpp)'s `view_state::record_change` and its callers: the session's recent changes,
  recorded by the batch write in [app_util.cpp](../src/app_util.cpp) and by each operation that changes files;
  the changed mark is drawn in [model_items.cpp](../src/model_items.cpp).
- [app_settings.cpp](../src/app_settings.cpp): durable preferences and diagnostics settings, and
  [view_options.cpp](../src/view_options.cpp) the Options view that edits them.