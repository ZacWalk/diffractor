# Diffractor Faces

This document owns face detection, alignment, vectors, grouping, sample faces, the face palette, and
face search. Collection membership is owned by [collections](collections.md), background execution
and publication by [implementation](implementation.md), media decoding by [file I/O](file-io.md),
and dependency provenance by [third-party](third-party.md).

## 1. User contract

Face search finds pictures with similar-looking faces. It does not identify people: results can
include lookalikes and miss other photographs of the same person.

Nothing here is named. Faces are collected into groups of ones that look alike, and a group is
shown as, and identified by, one face out of it - its **sample**. Clicking a group asks for the
pictures whose faces resemble that sample. There is no person, no label, and no gesture that says
two groups are the same somebody: a name would be a claim about identity that a resemblance search
cannot support, and the moment one existed the collection would be half-labelled and the half
without labels would look like a job left undone.

Face search is off until the user enables it. While off, Diffractor computes, loads, stores, and
shows no detections or vectors. The sidebar and Options menu always show **Face search** as entries
to its settings, so the feature remains discoverable while off and inspectable while on. The face
palette appears below the sidebar entry while enabled.

The options dialog states before enabling:

- faces are found and grouped locally;
- only pictures in the indexed collection are scanned;
- every eligible picture is read again in the background;
- the number of eligible pictures;
- nothing is uploaded or written to a media file; and
- disabling deletes every detection, vector, and group, and enabling again finds them by reading the
  collection a second time. Nothing a user typed is lost, because face search holds nothing a user
  typed.

Disabling invalidates work already in flight before deleting derived rows. A result created before
that transition cannot republish biometric data, even if face search is enabled again immediately.

## 2. Face data

Everything face search holds is derived from the pictures and rebuildable by reading them again.

| Data | Meaning | Lifetime |
|---|---|---|
| Detection | One face in one file: bounds, five landmarks, and detector score | Derived |
| Face vector | The normalized 128-value embedding of an aligned detection | Derived |
| Group | Faces resembling one sample face, proposed by Diffractor | Derived, rebuilt by each pass |

A group is an appearance cluster, not a person, and it is identified by the face it shows rather
than by a number or a label. That identity survives regrouping for the same reason a file name
does: it names a face in a file, which the next pass will find again, rather than an internal id
whose meaning the next pass decides.

The faces in one file are stored in a **stable order** - upright left to right, then top to bottom,
then larger first - and that order is what the `:n` in a face term counts. Detector output order is
not that: it is a function of how the model happened to scan a frame, so a saved term would move to
a different face after a re-detection. Because the order is part of what a term means, it is part of
the stored model version.

Stored geometry is **upright**: the space the picture is displayed in, not the grid it was decoded
on. Most formats decode in the file's own orientation, so a portrait photograph detects on a
landscape surface; detection, alignment and embedding all run there, and only what is written down is
turned. Without this a box would be on its side the moment anything drew one, and a face locator
would mean a different part of the picture depending on the format it arrived in. This is the same
space a region the user draws is already kept in.

Derived data is one private binary cache property in the existing item's `properties` blob. Its
payload holds the model version, scan time, upright geometry, vectors, and a completed-empty marker.
There are no added tables, columns, or schema versions. Master preserves this opaque property during
ordinary metadata writes, without treating it as public media metadata. A value from another model
version, a malformed value, or one older than its source file is unscanned and rebuilt.

Groups are not stored. They are what the last grouping pass proposed over the vectors the index
currently holds. Cleaning expired item rows removes their faces automatically; an index reset or
clearing cached metadata discards face values too, and enabling the feature rebuilds them. A saved
search outlives all of it: a face term names a file and a position in it,
which is answered from whatever that file's current vectors are.

Each in-memory face set is immutable and published as one complete value. Publication verifies the
feature generation, collection membership, path, and source modified time. A face set carries only
what was detected in that file; nothing about a group is written onto it.

## 3. Background pass

Enabling loads the current face rows, then starts detection. The pass runs on its own worker,
independently of indexing and UI work.

The pass is incremental and resumable:

- each completed file is persisted;
- closing the application loses at most the active bounded batch;
- new or modified pictures wake the pass and only stale files are processed;
- a model-version change makes old vectors stale, and grouping simply runs again over what replaces
  them; and
- failed or faceless files receive an empty completed row so they are not retried forever.

Decode size, batch size, detector input size, and minimum face size are bounded. Decoding and model
inference never run on the UI thread.

The sidebar reports face progress separately from indexing, because the two passes have different
totals, different costs and different completion times, and one bar reporting both would be wrong
for the whole of the longer one. It reports it **once**: the **Face search** entry row carries the
percentage while a pass runs and reverts to its name when the pass finishes, rather than a second
row appearing beneath it to say the same thing. A pass starting or finishing is therefore a repaint
of one row and never a re-layout of the rows below it.

Saying it once is what it means: the tiles below the row carry faces and nothing else. They report
neither how far the pass has got nor how many groups they are not showing, because the row already
answers the first and the face view answers the second. A panel whose value is being glanceable
cannot afford a line of prose under every grid in it.

Progress is reported against every eligible picture in the collection, not against the work left to
do. Because each completed file is persisted, a session that resumes a half-finished pass reports
where the collection actually is rather than restarting the bar at nothing.

Opening a picture asks for that one picture out of turn, so what is on screen is answered for now
rather than whenever the collection pass happens to reach it. It is the same work under the same
eligibility — collection, pictures, and only when the stored row is missing or stale — so it neither
widens what face search reads nor repeats what has already been done.

Grouping reads a collection snapshot on a separate worker, so it cannot hold up detection or a
displayed-picture request. One grouping pass may be queued or running, with one coalesced follow-up.
The first results, periodic scan milestones, opening Faces, and scan completion request projections.
Unchanged snapshots do not repeat the calculation.

Detection may advance while grouping runs. A pass removes obsolete source rows before publishing
its complete snapshot and remains partial until it catches up. New detections do not repeatedly
cancel whole-collection work. Consent changes and shutdown cancel obsolete grouping; the final scan
requests a catch-up pass.

Palette entries, counts, and picture captions are built off-thread and published as one immutable
value. Nothing about a group is written to the database, so a pass that is cancelled or discarded
costs the calculation and leaves nothing behind to reconcile.

## 4. Pipeline

```mermaid
flowchart LR
    A[Bounded picture decode] --> B[Luminance reduction]
    B --> C[YuNet detection]
    C --> D[Five-point similarity alignment]
    D --> E[MobileFaceNet embedding]
    E --> F[Grouping around a sample face]
```

### Detect

YuNet receives a bounded luminance plane and returns bounds, a score, and eye, nose, and mouth
landmarks. Low-confidence and very small detections are discarded before embedding. Detector input
size and minimum admitted face size are one tuning decision because reducing the input also raises
the smallest detectable face.

Both packed RGB and NV12 feed full-range luminance. Limited-range NV12 is expanded after reduction;
packed RGB uses full-range coefficients. Preprocessing changes invalidate old derived face rows.

### Align

The five landmarks are fitted by least squares to the MobileFaceNet 112x112 reference template.
The transform permits rotation, uniform scale, and translation, but no shear. Alignment that would
sample substantially outside the decoded surface is rejected rather than padded.

### Embed

ncnn warps the aligned crop and runs the externally supplied, build-time MobileFaceNet model. Its output is normalized
once, making later cosine comparisons dot products. ncnn is single-threaded; Diffractor owns
parallelism and resource bounds. Model provenance and licence are recorded in
[third-party](third-party.md#ncnn).

### Group

Similarity is cosine similarity over normalized vectors. Grouping happens in two steps, and the
second is what makes a group answerable by a search.

Clusters are proposed first. Candidates are led in a deterministic quality order; each one either
joins the nearest existing cluster it is within the threshold of, or anchors a new one. Anchors
receive one mean-based refinement and then stay fixed while membership is evaluated. A face outside
all refined anchors gets a reference of its own, so refinement cannot make it unsearchable.

Each cluster then adopts a **sample face** and takes that face's own vector as its anchor, and
membership is measured again against it. A mean vector is nobody's face: it could not be shown, it
could not be spelled, and a count taken against it would not equal what a search for the sample
returns. Adopting a real face makes the tile, the identity and the query the same thing. Two
clusters that settle on one sample face are one group.

Re-anchoring can strand a face, and a stranded face gets a group of its own. Refinement only
promises that every member is within the threshold of its cluster's mean, so two members either side
of that mean can be twice as far apart as the threshold allows; a sample is a real face on one side
of its cluster, so a face on the far side can fall outside every sample there is. A face in no group
has no tile, no caption and no term — nothing in the product can reach it — which is why neither
step is allowed to leave one behind.

Published groups are overlapping resemblance searches, not an exclusive partition. Every member
must directly meet the sample's threshold. A face can appear in two searches without connecting
those searches transitively: A resembling B and B resembling C does not make A a match for C. The
palette counts and the query use this same predicate, against the same sample vector. A picture's
caption uses the group whose sample it resembles most.

The threshold trades missed matches against lookalikes; it does not separate identities. No media
file is moved or modified by grouping.

## 5. Groups, samples and the palette

A group is shown as its sample face and is identified by it. There is no other label: no name, no
generated `face12`, and no id a user is asked to recognise. What a group is called is what the
picture it came from is called, which is a thing the user can see, type and save.

The sample is chosen to be worth showing and worth searching for: square-on rather than in profile,
large enough not to be an upscale, and typical of the group rather than its most photogenic outlier.
A face turned too far from the camera is passed over entirely while any member is square-on, because
a profile makes a poor tile and a worse reference. Crop size counts in absolute pixels, because as a
fraction of the frame it barely varies and a small blurred face would win from a large sharp one.
Ties break on path and face index, so the tile does not reshuffle between sessions.

Where two members are equally good, a sample whose file name is unique in the collection is
preferred, because a face term spells a file name and a name shared by two files asks for both
faces. Preferring the unambiguous one keeps the count on the tile and the results of clicking it
the same number.

The sidebar palette is a grid of sample faces and nothing else: one square thumbnail per group,
largest first, with the file count and the term in its tooltip. The palette is a way in to the
collection, so the faces that account for most of it come first. It shows a fixed number of them and
says nothing about the rest — the **Face search** row above opens the face view, which lists every
group, because paging a column six tiles wide through thousands of them was never going to be how
anyone finds one.

Only a face is a target in the grid. There is no count to click and no page to advance, so the one
gesture the tiles offer is the one worth having.

Pixels come from the thumbnail cache. A missing thumbnail is requested once through the normal
bounded thumbnail pipeline; Paint never reads a file or SQLite.

Faces uses the same largest-first projection as the sidebar, and lists each group's sample, the file
it came from, how many pictures it accounts for, when they were taken and where. It does not
recluster groups to order the list. Thumbnail reads, crops, and retained surfaces cover the viewport
and nearby rows only; scrolling requests the next window. Crop reuse checks the source face set and
crop geometry.

Clicking has one meaning everywhere a face is shown: search for faces resembling this one. A sidebar
tile, a face view row, and a face box on a photograph all run the same term, so there is nothing to
learn twice and nothing a second click does instead.

## 6. Faces on the picture

While face search is on, the faces found in the displayed photograph are drawn on it as boxes. They
are chrome over the picture, not part of it: they are drawn half visible, they fade away with the
rest of the chrome once the pointer stops moving, and they are absent in zoom, where the picture is
being examined rather than surveyed. Boxes are drawn only where a face is something to point at: one
picture, laid out in its own space, so not a projected panorama and not a comparison.

The face the current query is asking for is drawn in the highlight colour and every other face in
the ordinary one, so a result says which face made it a result. Only a face term that references a
face singles one out: `@face` on its own asks for any face, and an exclusion names faces the results
do not contain, so neither highlights anything.

Pointing at a box brings that face fully up, captions it, and fades in its buttons. Nothing has to be
clicked first: a click is one of the things the buttons offer, so it cannot also be the price of
seeing them. The pointer stays on the face while it is anywhere between the box and the strip, so
reaching for a button never puts the buttons away. Only the face under the pointer is captioned —
a caption under every stranger is noise.

The caption says how much of the collection this face's group accounts for, which is what says
whether searching for it is worth doing. There is nothing to call it, so the caption says the one
thing a resemblance search can honestly report.

| Button | Effect |
|---|---|
| Zoom | Zooms the view to that face |
| Crop | Opens Edit with the crop set to that face |
| Search | Searches for that face's group, by its sample |

Clicking the box itself searches for that face, which is the one action worth having without aiming
at a button. The term is the group's sample rather than this particular face, so the results are the
count the caption just quoted rather than a near-miss of it. No file is read or written by any of the
three.

The boxes are laid out from the face set the index published for that picture, requested off the UI
thread and applied as one value, together with which of them the query is asking for and what each
of them would search for. Paint draws what has already arrived and never reads a file or the
database.

Where the boxes and the buttons are is computed from that published value whenever it is needed,
by drawing and by hit testing alike, rather than recorded by drawing and read back by hit testing.
Faces arrive while the picture is already on screen, and a pointer move is followed by a fresh hit
test before anything repaints, so geometry left over from the last paint is geometry that does not
describe either moment. For the same reason, a click on a palette tile carries the entry it was
aimed at: the palette is rebuilt on every face-content change, which is continuously while the pass
runs, so an index recorded when the pointer arrived names something else by the time the click does.

## 7. Limits

Faces intentionally does not do these things, and their absence is a decision rather than an
oversight:

- Only pictures are scanned. Video is deferred because "which frame" is a real question, and
  answering it badly produces faces nobody can find again.
- There is no manually drawn face box. Every detection comes from the detector, so a face it did not
  find cannot be searched for.
- Nothing is named. Faces are grouped by resemblance and shown by example.
- Face terms cover the indexed collection only.
- Nothing about a face is exported, and no face data is written to a media file or a sidecar.

## 8. Searching

`@face` means "has at least one detected face."

`@face:IMG_1234.jpg:2` means "has a face resembling the second face in IMG_1234.jpg". The position
counts the file's stable face order and `:1` is the default, so `@face:IMG_1234.jpg` is the first
face — and for the great majority of pictures, which hold one face, the whole of what there is to
say. `face:IMG_1234.jpg:2` is accepted for the same thing and canonicalizes to the `@` form, exactly
as `loc:` does.

A term spells a face, not a group, which is why it outlives grouping. Nothing in it refers to a
cluster, an id or a label the next pass is free to change: it names a picture and a position, both
of which mean the same thing a year later. A face that no pass has grouped is still searchable, and
so is one that grouping has since moved.

The file name alone locates the picture, without its folder, because a term has to be readable and
typeable to be worth having. Two files with one name therefore spell the same term, and it asks for
every face that spells that way — a superset of either group, never the wrong one. Answering from
only one of them would make a saved search mean a different face after a restart, because which one
was found first is an accident of how the index was walked. The number that answer is bounded and
taken in file-path order, so a name repeated across a whole collection cannot make one term cost the
collection. Sample selection prefers an unambiguous file so this stays a corner rather than the
common case.

A term whose file is no longer in the indexed collection, or no longer has a face in that position,
matches nothing. It is not an error and nothing is invented in its place.

Face terms apply only to the indexed collection. In another scope the query returns no items and the
empty-results surface explains that face search covers the indexed collection.

## 9. Privacy, ownership, and recovery

- Detection, embedding, grouping, and searches remain on the local machine.
- No face information is written to EXIF, IPTC, XMP, sidecars, or media files.
- UI code schedules work and consumes detached results; it performs no decoding, file access, or
  database access.
- Workers publish only complete, current values. Generation and source checks reject stale work.
- Everything face search holds is derived, so everything it removes is recoverable by scanning
  again. There is no user data here to lose and no separate confirmation to ask for.
- Disabling removes only the private face property, preserving every other cached property and the
  item's playback and hash state. Older experimental face tables are not created, migrated, or dropped.

## Experiment inputs

This branch contains code, not face photographs, derived face records, or embedding weights.
Without an external model, face search reports itself unavailable; the model-independent suite
still exercises reduction, alignment, grouping, storage, search and UI behavior.

Set the CMake option `DIFFRACTOR_FACE_MODEL_DIR` to a directory outside the checkout containing
a licensed `mobilefacenet.param` and `mobilefacenet.bin` pair. The graph must accept the engine's
112-square input and produce a 128-component vector. The application embeds those files for the
experiment; they are not added to Git. The model-content fingerprint is part of the stored version,
so changing either file invalidates old vectors. Changing the detector or alignment still requires
changing the foundation version in [model_faces.h](../src/model_faces.h).

The photograph-based tests are registered only when a model is available and
`DIFFRACTOR_FACE_TEST_DIR` explicitly names an external directory containing the cases those tests
expect. No photograph corpus is bundled. The degraded-photograph check adds one Release-only test
when those inputs are present. The read-only face report accepts an external list of pictures;
keep its output outside the checkout too.

## Where this lives

| Concern | Source |
|---|---|
| Face values, alignment, similarity, grouping, sample selection, face terms, and storage encoding | [model_faces.h](../src/model_faces.h), [model_faces.cpp](../src/model_faces.cpp) |
| Detection and embedding | [model_faces_engine.cpp](../src/model_faces_engine.cpp) |
| Background pass, publication, palette projection, and term resolution | [model_index.h](../src/model_index.h), [model_index.cpp](../src/model_index.cpp) |
| Existing item property storage, loading, cleaning, reset, and writes | [model_db.h](../src/model_db.h), [model_db.cpp](../src/model_db.cpp), [model_db_pack.h](../src/model_db_pack.h), [model_property.h](../src/model_property.h) |
| Sidebar entry row, progress, palette, and options | [app_sidebar.h](../src/app_sidebar.h), [app_commands.cpp](../src/app_commands.cpp) |
| The face view: every group, its sample, count, dates and places, and the settings beside them | [view_faces.h](../src/view_faces.h), [view_faces.cpp](../src/view_faces.cpp) |
| Read-only measurement of what the pipeline embeds and how it groups, over a named list of pictures | [app_face_report.cpp](../src/app_face_report.cpp) |
| Face boxes on the displayed picture | [ui_controls.h](../src/ui_controls.h), [ui_elements.cpp](../src/ui_elements.cpp), [model.h](../src/model.h) |
| Search parsing, matching, and completion | [model_search.h](../src/model_search.h), [model_search.cpp](../src/model_search.cpp), [app_search.cpp](../src/app_search.cpp) |
| Dependency builds, external model input, resources, provenance, and licences | [third-party](third-party.md), [CMakeLists.txt](../CMakeLists.txt), [app_resources.cpp](../src/app_resources.cpp), [facedetection.cmake](../cmake/vendored/facedetection.cmake), [ncnn.cmake](../cmake/vendored/ncnn.cmake) |
| Tests | [test_faces.cpp](../src/test_faces.cpp), [test_index.cpp](../src/test_index.cpp), [test_search.cpp](../src/test_search.cpp) |