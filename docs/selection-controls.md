# Selection Controls

This document owns the selection panel's current forms, content order, density, and responsive
behavior. [Design](design.md) owns selection and command targeting; source owns exact APIs and
control inventories.

## Forms

The selected set determines one form:

| Form | Condition | Purpose |
| --- | --- | --- |
| Singular media | One selected media file | Identify, view, grade, edit, and inspect the item |
| Singular file | One selected non-media file | Identify the file and offer applicable file actions |
| Comparison | Two comparable files | Align per-item actions and facts for comparison |
| Selection summary | Every other non-empty selection | Summarize the complete selected set and its batch actions |

Comparison requires compatible file traits, not cardinality alone. Image pairs and previewable
video pairs are comparable; mixed types, folders, and unsupported pairs use the selection summary.
An empty selection creates no panel.

## Singular Media

The regular panel presents applicable groups in this order:

1. Playback transport for playable audio or video.
2. Identity: authored title or filename, plus sidecar and copy badges.
3. Viewing controls such as slideshow, pin, orientation, scale, and fullscreen.
4. Item actions such as label, rating, rotate, edit, open, and tools.
5. File facts: containing folder when useful, filename, dates, and size.
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
this detail explicitly. Rows are grouped by their source and omitted when empty; late scan results
append below the stable primary block rather than changing its command order.

## Other Singular Files

A non-media file shows name, containing folder, size, created and modified dates, label/reject,
Pin, Open, and Tools where applicable. It does not claim media navigation, playback, rotation,
rating, tags, or editable media metadata.

## Comparison

Each item receives its own label/reject, rating, Pin, Delete, and Unselect controls. Shared commands
whose target would be ambiguous are omitted. Rows align names, folders, sizes, dates, media facts,
location, tags, descriptive fields, and equality results when those values are available. Rows with
no value on either side are omitted.

Copy counts stay attached to each title. Rank color may distinguish larger dimensions or size and
later dates, but does not claim that larger or later is better.

## Selection Summary

The summary shows folder and file counts, a file-type and size breakdown, applicable selected-set
commands, and the pinned item when one exists. Three or more files may add a thumbnail collage,
bounded to 24 cells with an overflow count. A failed thumbnail contributes to the remainder rather
than creating a blank cell.

The summary describes the selected set; it is not a degraded comparison and does not infer common
metadata that the model has not computed.

## Presentation

Items uses the regular panel and may place verbose stream and raw metadata below it. The media and
primary information center together while they fit; when they overflow, the column starts at the
top and scrolls. Media retains at least half of the pane where possible, so metadata cannot reduce
it to a strip. Late detail is appended below the primary block.

Fullscreen uses compact density. It keeps identity and subject facts, removes most container facts,
and bounds the overlay height. Overlay controls stay visible for paused media and for items whose
panel is the primary presentation; otherwise they may fade while playback or image viewing is
active. Compact and regular forms preserve the same command targets.

Layout uses natural-height regions, cohesive wrapping command groups, and equal comparison value
columns. Width changes may wrap groups or stack comparison values but must not change the chosen
form or command target.

## Where this lives

- [model.cpp](../src/model.cpp): `view_state::create_selection_controls` and form construction.
- [view_items.cpp](../src/view_items.cpp): Items presentation and `layout_media_column`.
- [view_media.h](../src/view_media.h): fullscreen media presentation.
- [model_property.cpp](../src/model_property.cpp): descriptive-field ordering and de-duplication.
- [ui_elements.h](../src/ui_elements.h) and [ui_flex.cpp](../src/ui_flex.cpp): panel elements and
  responsive layout.