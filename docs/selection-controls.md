# Selection Controls

This document owns the selection panel's current forms, content order, density, and responsive
behavior. [Design](design.md) owns selection and command targeting; source owns exact APIs and
control inventories.

## Forms

The selected set determines one form:

| Form | Condition | Purpose |
| --- | --- | --- |
| Singular media | One selected media file | Identify, view, grade, edit, and inspect the item |
| Singular file | One selected non-media file | Identify the file, show its contents read only, and offer applicable file actions |
| Comparison | Two comparable files | Align per-item actions and facts for comparison |
| Selection summary | Every other non-empty selection | Summarize the complete selected set and its batch actions |

Comparison requires compatible file traits, not cardinality alone. Image pairs and previewable
video pairs are comparable; mixed types, folders, and unsupported pairs use the selection summary.
An empty selection creates no panel.

## Singular Media

The regular panel presents applicable groups in this order:

1. Playback transport for playable audio or video, including SID tunes and tracker modules.
2. Identity: authored title or filename, plus sidecar and copy badges.
3. Viewing controls such as slideshow, pin, orientation, scale, and fullscreen.
4. Item actions such as label, rating, rotate, edit, open, and tools.
5. File facts: the containing folder, as a link that opens it, when it is not a folder the view
   shows; then filename, dates, and size.
6. Media facts: dimensions, codecs, camera, album, artist, and other populated properties.
7. Location, tags, and descriptive fields.

Missing or inapplicable groups consume no space. Presence remains distinct from duplicate identity:
the badge and its bubble can report related files and collection presence without adding those files
to the command target. Tags are bounded to six visible values plus a remainder count.

Description, synopsis, and comment form one descriptive section. A single value is shown directly;
multiple values use labeled rows, suppress repeated text, and share open, copy, and edit actions.
Comparison keeps the fields separate so differences remain aligned.

## Grading Controls

Rating, color label, reject, and to-do state are item grading controls. They show the current value,
remain attached to the item they target, and appear only where that target is unambiguous. Comparison
therefore gives each item its own controls; selected-set commands do not imply one shared grade.

## Verbose Metadata Blocks

Items may show stream details and raw metadata below the primary panel. The user opens or closes
this detail explicitly. It defaults to closed; an existing saved preference is respected.
Rows are grouped by their source and omitted when empty; late scan results
append below the stable primary block rather than changing its command order. Streams are hidden
until opened and never claim space in the primary media budget, including in landscape panes.

## Other Singular Files

A non-media file shows name, containing folder (a link, when the view is not showing it), size,
created and modified dates, label/reject, Pin, Open, and Tools where applicable. It does not claim
media navigation, playback, rotation, rating, tags, or editable media metadata.

Below them the panel shows the file's first MiB, read only, with a note when the file is longer.
Text, Markdown, CSV and dumps use the same inset document panel and scroll with the pane:

- Bytes that read as text are shown as text: UTF-8 or UTF-16 with a byte order mark, UTF-8 without
  one where the bytes are valid as such, and Windows-1252 otherwise. A NUL without a byte order mark,
  or control characters past one byte in a hundred, leave the file a hex dump. Source lines wrap at word
  boundaries to the pane's width in the code font; tabs reach stops of four, and a control character
  shows as a dot.
- C and C++, Rust, Python and PowerShell are coloured by their extension, with
  libdesktop's highlighters. Plain text is not coloured.
- Markdown uses libdesktop's document view for headings, links and pipe tables; CSV uses its aligned
  table view, including quoted commas and escaped quotes. Their header dividers use the
  [common column gestures](design.md#guided-operations); column sizes and selection survive the
  panel being rebuilt around the document. Web and mail links open on a click, not a selection drag.
- Anything else is the hex dump: offset, hex and ASCII columns, as many bytes a row, in fours up to
  32, as fit. A Commodore program's dump follows the panel, under the listing above it, and a raw
  metadata block's detail is a dump of the block.

A drag selects text, a double click selects a word, and Shift extends the selection; while the
document has the keyboard, Escape clears it. Copy takes the text as written, with Windows line
breaks, or a dump's rows as drawn; [design](design.md#selection-and-viewing) owns when Copy, Cut and
Select all target the document rather than the items. Nothing is written to the file, and nothing
about it is indexed.

An archive's contents are listed under headings - name, modified date, size, and the packed size where
the archive's reader knows it - and a file's streams under number, type, title, codec, FourCC and
format: a FourCC only where the tag is four printable letters, and a turned stream's rotation among its
format. In a narrow pane the title, codec and format wrap rather than squeeze one another out. The
headings sort the rows and size the columns as [display tables](design.md#guided-operations) do. A
file-group summary keeps its icons, colours and numeric alignment unheaded, and so draws no dividers on
its first row.

A Commodore 64 disk's, tape's or program file's listing stands above the panel, where media does, and
its rows run what they name in an emulated Commodore 64 that then stands there instead. A cartridge
is drawn there as one, its title on its label, with Start under it
([design](design.md#selection-and-viewing)).

## Comparison

The two names head the comparison's columns, and under each name are that item's own label/reject,
rating, Pin, Delete, and Unselect controls, so what keeps, grades or discards a file is read with
its name. Shared commands whose target would be ambiguous are omitted. Rows align sizes, dates,
media facts, location, tags, and descriptive fields, with the equality result above the table. The
folder row appears only when one of the two is away from the folders the view shows, as links. Rows
with no value on either side are omitted.

A fact the same in both is stated once: the second column shows `=`, so what differs between the
two is what stands out. Sizes, dates and dimensions always show both values. Copy counts stay
attached to each title. Rank color may distinguish larger dimensions or size and later dates, but
does not claim that larger or later is better.

## Selection Summary

The summary shows folder and file counts, a file-type and size breakdown, the folders the selected
items are in when they are not folders the view shows (as links, three at most and a count of the
rest), applicable selected-set commands, and the pinned item when one exists. Three or more files
may add a thumbnail collage, a panel above the summary bounded to 24 cells with an overflow count.
A failed thumbnail contributes to the remainder rather than creating a blank cell.

The summary describes the selected set; it is not a degraded comparison and does not infer common
metadata that the model has not computed.

## Presentation

Items and Fullscreen present a selection as one stack of panels, built once for both: what the
selection is shown as stands above the selection panel, the description follows it, and in Items
the content that can run on for screens stacks below.

| Selection | Above the panel | Below it, in Items |
| --- | --- | --- |
| Photo, video, running Commodore 64 | The picture | Description |
| Two comparable files | The two side by side | Description |
| Two files that do not compare | - | - |
| SID tune | The monitor's panels: tunes, voices, output, filter, registers | Description |
| Tracker module | The monitor's pattern panel | Description, sample and instrument bank |
| Other audio | The visualizer panel: cover art, if any, over the bars | Description |
| Cartridge | The cartridge, with Start | Description |
| Disk, tape, program file | The listing | Description, the program's dump |
| Archive | In Fullscreen, its listing, fitted | Description, the listing |
| Document, other file | - | Description, the document |
| Several files | The thumbnail collage | - |
| Folders | - | - |

Every part of the stack is a panel in libdesktop's common background and inner inset, as wide as
the column, with one uniform gap between panels and no spacer elements or panel-specific outside
margins; a monitor draws its own parts in that same background. A truncation note stays inside its
document panel.

Items centers the primary block - what stands above the panel, and the panel - while it fits; when
it overflows, the column starts at the top and scrolls. Media retains at least half of the pane where
possible, so metadata cannot reduce it to a strip. A listing or document that can run on for
screens holds nothing above the fold, and the column scrolls as one. Late detail is appended below
the primary block.

Fullscreen gives a picture - a photo, a video, a running Commodore 64, a comparison - the screen,
with the compact panel floating over it and its description beside it. The overlay height is
bounded; overlay controls stay visible for paused media and may fade while playback or image
viewing is active. Everything else is the stack, centred and no wider than a line is comfortably
read across, with the compact panel and its description, never fading. Fullscreen does not scroll:
a disk's listing and a document are left to Items, and what stands above the panel is fitted to the
screen.

A monitor or the visualizer fits the space the stack gives it rather than pushing the panels below
it away. The SID monitor is drawn smaller, then gives up its register grid, filter, output and ADSR
row in turn; the tracker's pattern shows fewer rows; the visualizer gives up cover art before its
bars. A panel that takes less than its slot leaves no gap: the stack closes up behind it.

Compact uses fewer facts: it keeps identity and subject facts and removes most container facts.
Compact and regular forms preserve the same command targets.

Layout uses natural-height regions, cohesive wrapping command groups, and equal comparison value
columns. Width changes may wrap groups or stack comparison values but must not change the chosen
form or command target.

## Where this lives

- [model.cpp](../src/model.cpp): `view_state::create_selection_controls` and form construction.
- [view_items.cpp](../src/view_items.cpp): `make_media_presentation`, which builds the stack for both
  views, the Items presentation and `layout_media_column`.
- [view_media.h](../src/view_media.h): fullscreen's picture overlay and `layout_media_stack`.
- [ui_sid.cpp](../src/ui_sid.cpp), [ui_tracker.cpp](../src/ui_tracker.cpp) and
  [ui_music.h](../src/ui_music.h): the monitors' panels; [ui_c64.cpp](../src/ui_c64.cpp): the
  listing and the cartridge; `audio_control` and `images_control2` in
  [ui_controls.h](../src/ui_controls.h): the visualizer and the collage.
- [model_property.cpp](../src/model_property.cpp): descriptive-field ordering and de-duplication.
- libdesktop's [ui/elements.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/ui/elements.h)
  and [flex.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/ui/flex.cpp): the element
  tree the panel is built from and its responsive layout; [ui_controls.h](../src/ui_controls.h): the
  panel's own elements, and `draw_stack_panel` in [ui_elements.h](../src/ui_elements.h): the common
  panel's background for an element that draws its own.
- [model_document.h](../src/model_document.h): a file's head decoded as text or kept as a dump, its
  rows, selection and copy; [ui_document.h](../src/ui_document.h): drawing it and selecting with the
  mouse, and adapting formatted documents to libdesktop's views and common document panel;
  `display_state_t::load_selected_item_data` in [model.cpp](../src/model.cpp) reads and
  decodes it off the UI thread.